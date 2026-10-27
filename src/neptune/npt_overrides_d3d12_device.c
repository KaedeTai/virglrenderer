/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Device dispatch overrides, and the table that installs them.
 * The shared-HANDLE entry points are rejected under the policy in
 * npt_overrides_shared_handle.c; they live here so the table they share
 * with CheckFeatureSupport needs no cross-file declaration.
 *
 * CheckFeatureSupport travels as an opaque in/out byte blob, which works
 * only for pointer-free feature structs.  FEATURE_LEVELS embeds a guest
 * pointer, so it gets a wire convention -- the requested-levels array is
 * appended after the struct, the pointer field travels as NULL -- and
 * this override rebuilds the pointer for the call and scrubs it from the
 * reply.  The remaining pointer-bearing features have no such convention
 * and are rejected: their pointer would arrive holding whatever the
 * guest left in it, and the backend dereferences it -- for
 * PROTECTED_RESOURCE_SESSION_TYPES, writes through it -- inside the
 * render server.
 */

#include "npt_common.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"

static HRESULT
reject_ID3D12Device_CreateSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CreateSharedHandle *args,
   UNUSED PFN_ID3D12Device_CreateSharedHandle original)
{
   if (args->pHandle)
      *args->pHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D12Device_OpenSharedHandle(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_OpenSharedHandle *args,
   UNUSED PFN_ID3D12Device_OpenSharedHandle original)
{
   if (args->ppvObj)
      *args->ppvObj = NULL;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
reject_ID3D12Device_OpenSharedHandleByName(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_OpenSharedHandleByName *args,
   UNUSED PFN_ID3D12Device_OpenSharedHandleByName original)
{
   if (args->pNTHandle)
      *args->pNTHandle = (HANDLE)0;
   args->ret = NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device_CheckFeatureSupport(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CheckFeatureSupport *args,
   PFN_ID3D12Device_CheckFeatureSupport original)
{
   if (args->Feature == D3D12_FEATURE_FEATURE_LEVELS) {
      D3D12_FEATURE_DATA_FEATURE_LEVELS *fl = args->pFeatureSupportData;
      if (!fl || args->FeatureSupportDataSize < sizeof(*fl)) {
         args->ret = NPT_E_INVALIDARG;
         return args->ret;
      }
      const size_t appended =
         args->FeatureSupportDataSize - sizeof(*fl);
      if ((size_t)fl->NumFeatureLevels * sizeof(D3D_FEATURE_LEVEL) !=
          appended) {
         args->ret = NPT_E_INVALIDARG;
         return args->ret;
      }
      fl->pFeatureLevelsRequested =
         (const D3D_FEATURE_LEVEL *)((uint8_t *)fl + sizeof(*fl));
      args->ret = original(args->_self, args->Feature, fl,
                           (UINT)sizeof(*fl));
      /* Never leak a host pointer back through the reply blob. */
      fl->pFeatureLevelsRequested = NULL;
      return args->ret;
   }

   if (args->Feature == D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES ||
       args->Feature == D3D12_FEATURE_QUERY_META_COMMAND) {
      npt_log("CheckFeatureSupport: pointer-bearing feature %u has no wire "
              "convention; rejected", (unsigned)args->Feature);
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   args->ret = original(args->_self, args->Feature,
                        args->pFeatureSupportData,
                        args->FeatureSupportDataSize);
   return args->ret;
}

struct npt_dispatch_id3d12device_overrides npt_id3d12device_overrides = {
   .CreateSharedHandle = reject_ID3D12Device_CreateSharedHandle,
   .OpenSharedHandle = reject_ID3D12Device_OpenSharedHandle,
   .OpenSharedHandleByName = reject_ID3D12Device_OpenSharedHandleByName,
   .CheckFeatureSupport = npt_override_ID3D12Device_CheckFeatureSupport,
   .CreateGraphicsPipelineState =
      npt_override_ID3D12Device_CreateGraphicsPipelineState,
   .CreateComputePipelineState =
      npt_override_ID3D12Device_CreateComputePipelineState,
};
