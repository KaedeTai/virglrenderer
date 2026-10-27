/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Pipeline-state creation off the ring thread.
 *
 * The guest sends every pipeline Create without waiting for it, and the
 * backend compiles the shaders inside the call: tens of milliseconds a
 * pipeline when its shader cache is cold.  Run on the ring thread, an
 * engine's startup batch of thousands of pipelines holds every command
 * behind it, so a synchronous call from the render thread waits for the
 * whole batch -- long enough for Unreal to declare its render thread
 * hung.  Each Create instead becomes a job (npt_context_create_async)
 * owning a deep copy of the description, and the ring moves on; a later
 * command naming the pipeline waits for it, or runs it at once when no
 * worker has started it yet.
 */

#include "npt_common.h"
#include "npt_com.h"
#include "npt_context.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"

enum npt_pso_kind {
   NPT_PSO_GRAPHICS,
   NPT_PSO_COMPUTE,
   NPT_PSO_STREAM,
};

struct npt_pso_job {
   struct npt_async_create base;
   enum npt_pso_kind kind;
   void *device;
   IID riid;
   /* Every allocation the copied description points into. */
   void **blocks;
   uint32_t block_count;
   uint32_t block_cap;
   bool copy_failed;
   /* The references the job holds on root signatures the description
    * names (a stream may carry one record). */
   void *root_signature;
   union {
      struct {
         PFN_ID3D12Device_CreateGraphicsPipelineState fn;
         D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
      } graphics;
      struct {
         PFN_ID3D12Device_CreateComputePipelineState fn;
         D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
      } compute;
      struct {
         PFN_ID3D12Device2_CreatePipelineState fn;
         D3D12_PIPELINE_STATE_STREAM_DESC desc;
      } stream;
   };
};

/* A copy of `size` bytes owned by the job; NULL for an empty range. */
static void *
pso_dup(struct npt_pso_job *job, const void *src, size_t size)
{
   if (!src || !size)
      return NULL;
   if (job->block_count == job->block_cap) {
      const uint32_t cap = job->block_cap ? job->block_cap * 2 : 16;
      void **blocks = realloc(job->blocks, cap * sizeof(*blocks));
      if (!blocks) {
         job->copy_failed = true;
         return NULL;
      }
      job->blocks = blocks;
      job->block_cap = cap;
   }
   void *p = malloc(size);
   if (!p) {
      job->copy_failed = true;
      return NULL;
   }
   memcpy(p, src, size);
   job->blocks[job->block_count++] = p;
   return p;
}

static const CHAR *
pso_dup_str(struct npt_pso_job *job, const CHAR *s)
{
   return s ? pso_dup(job, s, strlen((const char *)s) + 1) : NULL;
}

static void
pso_copy_bytecode(struct npt_pso_job *job, D3D12_SHADER_BYTECODE *bc)
{
   bc->pShaderBytecode = pso_dup(job, bc->pShaderBytecode, bc->BytecodeLength);
}

static void
pso_copy_stream_output(struct npt_pso_job *job, D3D12_STREAM_OUTPUT_DESC *so)
{
   D3D12_SO_DECLARATION_ENTRY *entries =
      pso_dup(job, so->pSODeclaration,
              (size_t)so->NumEntries * sizeof(*entries));
   for (uint32_t i = 0; entries && i < so->NumEntries; i++)
      entries[i].SemanticName = pso_dup_str(job, entries[i].SemanticName);
   so->pSODeclaration = entries;
   so->pBufferStrides = pso_dup(job, so->pBufferStrides,
                                (size_t)so->NumStrides * sizeof(UINT));
}

static void
pso_copy_input_layout(struct npt_pso_job *job, D3D12_INPUT_LAYOUT_DESC *il)
{
   D3D12_INPUT_ELEMENT_DESC *elements =
      pso_dup(job, il->pInputElementDescs,
              (size_t)il->NumElements * sizeof(*elements));
   for (uint32_t i = 0; elements && i < il->NumElements; i++)
      elements[i].SemanticName = pso_dup_str(job, elements[i].SemanticName);
   il->pInputElementDescs = elements;
}

static void
pso_copy_cached(struct npt_pso_job *job, D3D12_CACHED_PIPELINE_STATE *cached)
{
   cached->pCachedBlob = pso_dup(job, cached->pCachedBlob,
                                 cached->CachedBlobSizeInBytes);
}

static void
pso_hold_root_signature(struct npt_pso_job *job, void *rs)
{
   /* The guest may release the root signature as soon as the Create is
    * sent; the job keeps it alive until the compile is done. */
   if (rs) {
      npt_com_add_ref(rs);
      job->root_signature = rs;
   }
}

static void
pso_copy_stream(struct npt_pso_job *job, D3D12_PIPELINE_STATE_STREAM_DESC *sd)
{
   uint8_t *base = pso_dup(job, sd->pPipelineStateSubobjectStream,
                           sd->SizeInBytes);
   if (!base) {
      job->copy_failed = true;
      return;
   }
   sd->pPipelineStateSubobjectStream = base;
   size_t off = 0;
   struct npt_pss_record rec;
   int r;
   while ((r = npt_pss_next(sd, &off, &rec)) > 0) {
      void *p = base + rec.payload_off;
      switch (rec.type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
         void *rs;
         memcpy(&rs, p, sizeof(rs));
         if (job->root_signature && rs != job->root_signature) {
            job->copy_failed = true;
            return;
         }
         if (!job->root_signature)
            pso_hold_root_signature(job, rs);
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
         pso_copy_bytecode(job, p);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
         pso_copy_stream_output(job, p);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
         pso_copy_input_layout(job, p);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
         pso_copy_cached(job, p);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
         D3D12_VIEW_INSTANCING_DESC *vi = p;
         vi->pViewInstanceLocations =
            pso_dup(job, vi->pViewInstanceLocations,
                    (size_t)vi->ViewInstanceCount *
                       sizeof(*vi->pViewInstanceLocations));
         break;
      }
      default:
         /* Every other record is plain data. */
         break;
      }
   }
   if (r < 0)
      job->copy_failed = true;
}

static void *
pso_job_run(struct npt_async_create *base)
{
   struct npt_pso_job *job = (struct npt_pso_job *)base;
   void *out = NULL;
   HRESULT hr;
   switch (job->kind) {
   case NPT_PSO_GRAPHICS:
      hr = job->graphics.fn(job->device, &job->graphics.desc, &job->riid, &out);
      break;
   case NPT_PSO_COMPUTE:
      hr = job->compute.fn(job->device, &job->compute.desc, &job->riid, &out);
      break;
   default:
      hr = job->stream.fn(job->device, &job->stream.desc, &job->riid, &out);
      break;
   }
   if (!NPT_SUCCEEDED(hr) && out) {
      npt_com_release(out);
      out = NULL;
   }
   return out;
}

static void
pso_job_destroy(struct npt_async_create *base)
{
   struct npt_pso_job *job = (struct npt_pso_job *)base;
   if (job->root_signature)
      npt_com_release(job->root_signature);
   npt_com_release(job->device);
   for (uint32_t i = 0; i < job->block_count; i++)
      free(job->blocks[i]);
   free(job->blocks);
   free(job);
}

static struct npt_pso_job *
pso_job_new(enum npt_pso_kind kind, void *device, const IID *riid,
            uint64_t guest_id)
{
   struct npt_pso_job *job = calloc(1, sizeof(*job));
   if (!job)
      return NULL;
   job->kind = kind;
   job->device = device;
   npt_com_add_ref(device);
   job->riid = *riid;
   job->base.guest_id = guest_id;
   job->base.type = npt_object_type_from_iid(riid);
   job->base.run = pso_job_run;
   job->base.destroy = pso_job_destroy;
   return job;
}

/* Queue `job`; false (and the job freed) when the Create has to run
 * inline instead. */
static bool
pso_job_submit(struct npt_dispatch_context *ctx, struct npt_pso_job *job)
{
   if (!job)
      return false;
   if (job->copy_failed ||
       !npt_context_create_async(npt_context_from_dispatch(ctx), &job->base)) {
      pso_job_destroy(&job->base);
      return false;
   }
   return true;
}

HRESULT
npt_override_ID3D12Device_CreateGraphicsPipelineState(
   struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateGraphicsPipelineState *args,
   PFN_ID3D12Device_CreateGraphicsPipelineState original)
{
   if (args->ppPipelineState && args->_guest_id_ppPipelineState) {
      struct npt_pso_job *job =
         pso_job_new(NPT_PSO_GRAPHICS, args->_self, args->riid,
                     args->_guest_id_ppPipelineState);
      if (job) {
         D3D12_GRAPHICS_PIPELINE_STATE_DESC *d = &job->graphics.desc;
         job->graphics.fn = original;
         *d = *args->pDesc;
         pso_hold_root_signature(job, d->pRootSignature);
         pso_copy_bytecode(job, &d->VS);
         pso_copy_bytecode(job, &d->PS);
         pso_copy_bytecode(job, &d->DS);
         pso_copy_bytecode(job, &d->HS);
         pso_copy_bytecode(job, &d->GS);
         pso_copy_stream_output(job, &d->StreamOutput);
         pso_copy_input_layout(job, &d->InputLayout);
         pso_copy_cached(job, &d->CachedPSO);
      }
      if (pso_job_submit(ctx, job)) {
         *args->ppPipelineState = NULL;
         args->ret = NPT_S_OK;
         return args->ret;
      }
   }
   args->ret = original(args->_self, args->pDesc, args->riid,
                        args->ppPipelineState);
   return args->ret;
}

HRESULT
npt_override_ID3D12Device_CreateComputePipelineState(
   struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateComputePipelineState *args,
   PFN_ID3D12Device_CreateComputePipelineState original)
{
   if (args->ppPipelineState && args->_guest_id_ppPipelineState) {
      struct npt_pso_job *job =
         pso_job_new(NPT_PSO_COMPUTE, args->_self, args->riid,
                     args->_guest_id_ppPipelineState);
      if (job) {
         D3D12_COMPUTE_PIPELINE_STATE_DESC *d = &job->compute.desc;
         job->compute.fn = original;
         *d = *args->pDesc;
         pso_hold_root_signature(job, d->pRootSignature);
         pso_copy_bytecode(job, &d->CS);
         pso_copy_cached(job, &d->CachedPSO);
      }
      if (pso_job_submit(ctx, job)) {
         *args->ppPipelineState = NULL;
         args->ret = NPT_S_OK;
         return args->ret;
      }
   }
   args->ret = original(args->_self, args->pDesc, args->riid,
                        args->ppPipelineState);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device2_CreatePipelineState(
   struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device2_CreatePipelineState *args,
   PFN_ID3D12Device2_CreatePipelineState original)
{
   if (args->ppPipelineState && args->_guest_id_ppPipelineState) {
      struct npt_pso_job *job =
         pso_job_new(NPT_PSO_STREAM, args->_self, args->riid,
                     args->_guest_id_ppPipelineState);
      if (job) {
         job->stream.fn = original;
         job->stream.desc = *args->pDesc;
         pso_copy_stream(job, &job->stream.desc);
      }
      if (pso_job_submit(ctx, job)) {
         *args->ppPipelineState = NULL;
         args->ret = NPT_S_OK;
         return args->ret;
      }
   }
   args->ret = original(args->_self, args->pDesc, args->riid,
                        args->ppPipelineState);
   return args->ret;
}

struct npt_dispatch_id3d12device2_overrides npt_id3d12device2_overrides = {
   .CreatePipelineState = npt_override_ID3D12Device2_CreatePipelineState,
};
