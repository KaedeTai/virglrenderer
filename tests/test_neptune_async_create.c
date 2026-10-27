/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * npt_context_create_async: a Create handed to a worker registers its id
 * as pending, and every use of the id sees the finished object (or the
 * failure) exactly as if the Create had run inline.  Drives the object
 * table directly with fake COM objects; no backend library is loaded.
 */

#include "config.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "neptune/npt_context.h"

static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(fn)                                     \
   do {                                                  \
      tests_run++;                                       \
      printf("  %-50s ", #fn);                           \
      fflush(stdout);                                    \
      if (fn()) {                                        \
         printf("PASS\n");                               \
      } else {                                           \
         printf("FAIL\n");                               \
         tests_failed++;                                 \
      }                                                  \
   } while (0)

#define EXPECT(cond)                                     \
   do {                                                  \
      if (!(cond)) {                                     \
         fprintf(stderr, "    EXPECT failed: %s:%d: %s\n", \
                 __FILE__, __LINE__, #cond);             \
         return 0;                                       \
      }                                                  \
   } while (0)

#define TYPE NPT_OBJECT_TYPE_ID3D12PIPELINESTATE

/* A COM object that answers every QueryInterface with itself. */
struct fake_obj {
   void *const *vtbl;
   atomic_int refs;
};

static HRESULT NPT_STDMETHODCALLTYPE
fake_qi(void *self, const GUID *riid, void **out)
{
   (void)riid;
   atomic_fetch_add(&((struct fake_obj *)self)->refs, 1);
   *out = self;
   return 0;
}

static uint32_t NPT_STDMETHODCALLTYPE
fake_add_ref(void *self)
{
   return atomic_fetch_add(&((struct fake_obj *)self)->refs, 1) + 1;
}

static uint32_t NPT_STDMETHODCALLTYPE
fake_release(void *self)
{
   return atomic_fetch_sub(&((struct fake_obj *)self)->refs, 1) - 1;
}

static void *const fake_vtbl[] = {
   (void *)fake_qi, (void *)fake_add_ref, (void *)fake_release,
};

static void
fake_init(struct fake_obj *o)
{
   o->vtbl = fake_vtbl;
   atomic_init(&o->refs, 0);
}

/* A gate jobs can be held at until the test opens it. */
struct gate {
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   bool open;
};

static void
gate_init(struct gate *g)
{
   pthread_mutex_init(&g->mutex, NULL);
   pthread_cond_init(&g->cond, NULL);
   g->open = false;
}

static void
gate_wait(struct gate *g)
{
   pthread_mutex_lock(&g->mutex);
   while (!g->open)
      pthread_cond_wait(&g->cond, &g->mutex);
   pthread_mutex_unlock(&g->mutex);
}

static void
gate_open(struct gate *g)
{
   pthread_mutex_lock(&g->mutex);
   g->open = true;
   pthread_cond_broadcast(&g->cond);
   pthread_mutex_unlock(&g->mutex);
}

static atomic_int jobs_ran;
static atomic_int jobs_destroyed;
static atomic_int jobs_started;

struct test_job {
   struct npt_async_create base;
   struct fake_obj *result;   /* NULL: the Create fails */
   struct gate *gate;         /* held here before finishing, if set */
};

static void *
test_job_run(struct npt_async_create *base)
{
   struct test_job *job = (struct test_job *)base;
   atomic_fetch_add(&jobs_started, 1);
   if (job->gate)
      gate_wait(job->gate);
   atomic_fetch_add(&jobs_ran, 1);
   if (!job->result)
      return NULL;
   /* The ref the registration holds. */
   atomic_fetch_add(&job->result->refs, 1);
   return job->result;
}

static void
test_job_destroy(struct npt_async_create *base)
{
   atomic_fetch_add(&jobs_destroyed, 1);
   free(base);
}

static struct test_job *
test_job_new(uint64_t id, struct fake_obj *result, struct gate *gate)
{
   struct test_job *job = calloc(1, sizeof(*job));
   job->base.guest_id = id;
   job->base.type = TYPE;
   job->base.run = test_job_run;
   job->base.destroy = test_job_destroy;
   job->result = result;
   job->gate = gate;
   return job;
}

static void
retire_fence(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
   (void)ctx_id;
   (void)ring_idx;
   (void)fence_id;
}

static struct npt_context *
make_context(void)
{
   atomic_store(&jobs_ran, 0);
   atomic_store(&jobs_destroyed, 0);
   atomic_store(&jobs_started, 0);
   return npt_context_create(1, retire_fence, 4, "test");
}

static void
wait_started(int n)
{
   while (atomic_load(&jobs_started) < n)
      usleep(1000);
}

static int
test_lookup_sees_created_object(void)
{
   struct npt_context *ctx = make_context();
   EXPECT(ctx);
   struct fake_obj obj;
   fake_init(&obj);

   EXPECT(npt_context_create_async(ctx, &test_job_new(10, &obj, NULL)->base));
   EXPECT(npt_context_lookup_object(ctx, NULL, 10, TYPE) == &obj);
   EXPECT(atomic_load(&jobs_ran) == 1);
   EXPECT(atomic_load(&obj.refs) == 1);

   /* The generated dispatch's post-call pass sees a NULL out-pointer
    * and records a failure; the pending registration must win. */
   npt_context_register_failed_object(ctx, 10);
   EXPECT(npt_context_lookup_object(ctx, NULL, 10, TYPE) == &obj);

   npt_context_release_object(ctx, 10);
   EXPECT(atomic_load(&obj.refs) == 0);
   EXPECT(npt_context_lookup_object(ctx, NULL, 10,
                                    NPT_OBJECT_TYPE_IUNKNOWN) == NULL);

   npt_context_destroy(ctx);
   EXPECT(atomic_load(&jobs_destroyed) == 1);
   return 1;
}

struct lookup_arg {
   struct npt_context *ctx;
   uint64_t id;
   void *result;
   atomic_bool done;
};

static void *
lookup_thread(void *p)
{
   struct lookup_arg *a = p;
   a->result = npt_context_lookup_object(a->ctx, NULL, a->id, TYPE);
   atomic_store(&a->done, true);
   return NULL;
}

static int
test_lookup_waits_for_running_job(void)
{
   struct npt_context *ctx = make_context();
   struct fake_obj obj;
   fake_init(&obj);
   struct gate gate;
   gate_init(&gate);

   EXPECT(npt_context_create_async(ctx, &test_job_new(11, &obj, &gate)->base));
   wait_started(1);

   struct lookup_arg a = { .ctx = ctx, .id = 11 };
   atomic_init(&a.done, false);
   pthread_t t;
   pthread_create(&t, NULL, lookup_thread, &a);
   usleep(50 * 1000);
   EXPECT(!atomic_load(&a.done));

   gate_open(&gate);
   pthread_join(t, NULL);
   EXPECT(a.result == &obj);

   npt_context_release_object(ctx, 11);
   EXPECT(atomic_load(&obj.refs) == 0);
   npt_context_destroy(ctx);
   return 1;
}

static int
test_failed_create_is_a_miss(void)
{
   struct npt_context *ctx = make_context();

   EXPECT(npt_context_create_async(ctx, &test_job_new(12, NULL, NULL)->base));
   EXPECT(!npt_context_object_is(ctx, 12, TYPE));
   EXPECT(npt_context_lookup_object(ctx, NULL, 12,
                                    NPT_OBJECT_TYPE_IUNKNOWN) == NULL);
   npt_context_release_object(ctx, 12);

   npt_context_destroy(ctx);
   EXPECT(atomic_load(&jobs_ran) == 1);
   EXPECT(atomic_load(&jobs_destroyed) == 1);
   return 1;
}

static int
test_release_waits_for_pending(void)
{
   struct npt_context *ctx = make_context();
   struct fake_obj obj;
   fake_init(&obj);
   struct gate gate;
   gate_init(&gate);

   EXPECT(npt_context_create_async(ctx, &test_job_new(13, &obj, &gate)->base));
   wait_started(1);
   gate_open(&gate);
   npt_context_release_object(ctx, 13);
   EXPECT(atomic_load(&jobs_ran) == 1);
   EXPECT(atomic_load(&obj.refs) == 0);

   npt_context_destroy(ctx);
   return 1;
}

/* With every worker held, a use of a queued job runs it on the calling
 * thread instead of waiting behind the queue. */
static int
test_waiting_use_runs_queued_job(void)
{
   struct npt_context *ctx = make_context();
   struct gate gate;
   gate_init(&gate);
   struct fake_obj held[NPT_ASYNC_CREATE_THREADS], obj;
   fake_init(&obj);

   for (int i = 0; i < NPT_ASYNC_CREATE_THREADS; i++) {
      fake_init(&held[i]);
      EXPECT(npt_context_create_async(
         ctx, &test_job_new(100 + i, &held[i], &gate)->base));
   }
   wait_started(NPT_ASYNC_CREATE_THREADS);

   EXPECT(npt_context_create_async(ctx, &test_job_new(200, &obj, NULL)->base));
   EXPECT(npt_context_lookup_object(ctx, NULL, 200, TYPE) == &obj);
   /* Only the held workers have started, so it ran here. */
   EXPECT(atomic_load(&jobs_ran) == 1);

   gate_open(&gate);
   for (int i = 0; i < NPT_ASYNC_CREATE_THREADS; i++)
      EXPECT(npt_context_lookup_object(ctx, NULL, 100 + i, TYPE) == &held[i]);
   npt_context_destroy(ctx);
   EXPECT(atomic_load(&jobs_destroyed) == NPT_ASYNC_CREATE_THREADS + 1);
   return 1;
}

static void *
destroy_thread(void *p)
{
   npt_context_destroy(p);
   return NULL;
}

/* Teardown with jobs still queued: each job is destroyed exactly once,
 * whether it ran or not. */
static int
test_teardown_with_queued_jobs(void)
{
   struct npt_context *ctx = make_context();
   struct gate gate;
   gate_init(&gate);
   static struct fake_obj objs[NPT_ASYNC_CREATE_THREADS + 3];
   const int total = NPT_ASYNC_CREATE_THREADS + 3;

   for (int i = 0; i < total; i++) {
      fake_init(&objs[i]);
      EXPECT(npt_context_create_async(
         ctx, &test_job_new(300 + i, &objs[i], &gate)->base));
   }
   wait_started(NPT_ASYNC_CREATE_THREADS);

   pthread_t t;
   pthread_create(&t, NULL, destroy_thread, ctx);
   usleep(20 * 1000);
   gate_open(&gate);
   pthread_join(t, NULL);

   EXPECT(atomic_load(&jobs_destroyed) == total);
   EXPECT(atomic_load(&jobs_ran) >= NPT_ASYNC_CREATE_THREADS);
   EXPECT(atomic_load(&jobs_ran) <= total);
   return 1;
}

/* An id the table already holds is not taken over. */
static int
test_registered_id_is_refused(void)
{
   struct npt_context *ctx = make_context();
   struct fake_obj obj;
   fake_init(&obj);
   atomic_store(&obj.refs, 1);
   npt_context_register_object(ctx, 14, &obj, TYPE);

   struct test_job *job = test_job_new(14, &obj, NULL);
   EXPECT(!npt_context_create_async(ctx, &job->base));
   free(job);

   npt_context_release_object(ctx, 14);
   EXPECT(atomic_load(&obj.refs) == 0);
   npt_context_destroy(ctx);
   return 1;
}

int
main(void)
{
   printf("Neptune async create tests:\n");

   RUN_TEST(test_lookup_sees_created_object);
   RUN_TEST(test_lookup_waits_for_running_job);
   RUN_TEST(test_failed_create_is_a_miss);
   RUN_TEST(test_release_waits_for_pending);
   RUN_TEST(test_waiting_use_runs_queued_job);
   RUN_TEST(test_teardown_with_queued_jobs);
   RUN_TEST(test_registered_id_is_refused);

   printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
   return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
