/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <time.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/debug.h"
#include "util/ralloc.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/u_upload_mgr.h"
#include "drm-uapi/i915_drm.h"
#include "iris_context.h"
#include "iris_resource.h"
#include "iris_screen.h"
#include "common/intel_defines.h"
#include "common/intel_sample_positions.h"

/**
 * The pipe->set_debug_callback() driver hook.
 */
static void
iris_set_debug_callback(struct pipe_context *ctx,
                        const struct pipe_debug_callback *cb)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (cb)
      ice->dbg = *cb;
   else
      memset(&ice->dbg, 0, sizeof(ice->dbg));
}

/**
 * Called from the batch module when it detects a GPU hang.
 *
 * In this case, we've lost our GEM context, and can't rely on any existing
 * state on the GPU.  We must mark everything dirty and wipe away any saved
 * assumptions about the last known state of the GPU.
 */
void
iris_lost_context_state(struct iris_batch *batch)
{
   struct iris_context *ice = batch->ice;

   if (batch->name == IRIS_BATCH_RENDER) {
      batch->screen->vtbl.init_render_context(batch);
   } else if (batch->name == IRIS_BATCH_COMPUTE) {
      batch->screen->vtbl.init_compute_context(batch);
   } else {
      unreachable("unhandled batch reset");
   }

   ice->state.dirty = ~0ull;
   ice->state.stage_dirty = ~0ull;
   ice->state.current_hash_scale = 0;
   memset(&ice->shaders.urb, 0, sizeof(ice->shaders.urb));
   memset(ice->state.last_block, 0, sizeof(ice->state.last_block));
   memset(ice->state.last_grid, 0, sizeof(ice->state.last_grid));
   batch->last_surface_base_address = ~0ull;
   batch->last_aux_map_state = 0;
   batch->screen->vtbl.lost_genx_state(ice, batch);
}

static enum pipe_reset_status
iris_get_device_reset_status(struct pipe_context *ctx)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   enum pipe_reset_status worst_reset = PIPE_NO_RESET;

   /* Check the reset status of each batch's hardware context, and take the
    * worst status (if one was guilty, proclaim guilt).
    */
   for (int i = 0; i < IRIS_BATCH_COUNT; i++) {
      /* This will also recreate the hardware contexts as necessary, so any
       * future queries will show no resets.  We only want to report once.
       */
      enum pipe_reset_status batch_reset =
         iris_batch_check_for_reset(&ice->batches[i]);

      if (batch_reset == PIPE_NO_RESET)
         continue;

      if (worst_reset == PIPE_NO_RESET) {
         worst_reset = batch_reset;
      } else {
         /* GUILTY < INNOCENT < UNKNOWN */
         worst_reset = MIN2(worst_reset, batch_reset);
      }
   }

   if (worst_reset != PIPE_NO_RESET && ice->reset.reset)
      ice->reset.reset(ice->reset.data, worst_reset);

   return worst_reset;
}

static void
iris_set_device_reset_callback(struct pipe_context *ctx,
                               const struct pipe_device_reset_callback *cb)
{
   struct iris_context *ice = (struct iris_context *)ctx;

   if (cb)
      ice->reset = *cb;
   else
      memset(&ice->reset, 0, sizeof(ice->reset));
}

static void
iris_get_sample_position(struct pipe_context *ctx,
                         unsigned sample_count,
                         unsigned sample_index,
                         float *out_value)
{
   union {
      struct {
         float x[16];
         float y[16];
      } a;
      struct {
         float  _0XOffset,  _1XOffset,  _2XOffset,  _3XOffset,
                _4XOffset,  _5XOffset,  _6XOffset,  _7XOffset,
                _8XOffset,  _9XOffset, _10XOffset, _11XOffset,
               _12XOffset, _13XOffset, _14XOffset, _15XOffset;
         float  _0YOffset,  _1YOffset,  _2YOffset,  _3YOffset,
                _4YOffset,  _5YOffset,  _6YOffset,  _7YOffset,
                _8YOffset,  _9YOffset, _10YOffset, _11YOffset,
               _12YOffset, _13YOffset, _14YOffset, _15YOffset;
      } v;
   } u;
   switch (sample_count) {
   case 1:  INTEL_SAMPLE_POS_1X(u.v._);  break;
   case 2:  INTEL_SAMPLE_POS_2X(u.v._);  break;
   case 4:  INTEL_SAMPLE_POS_4X(u.v._);  break;
   case 8:  INTEL_SAMPLE_POS_8X(u.v._);  break;
   case 16: INTEL_SAMPLE_POS_16X(u.v._); break;
   default: unreachable("invalid sample count");
   }

   out_value[0] = u.a.x[sample_index];
   out_value[1] = u.a.y[sample_index];
}

static bool
create_dirty_dmabuf_set(struct iris_context *ice)
{
   assert(ice->dirty_dmabufs == NULL);

   ice->dirty_dmabufs = _mesa_pointer_set_create(ice);
   return ice->dirty_dmabufs != NULL;
}

void
iris_mark_dirty_dmabuf(struct iris_context *ice,
                       struct pipe_resource *res)
{
   if (!_mesa_set_search(ice->dirty_dmabufs, res)) {
      _mesa_set_add(ice->dirty_dmabufs, res);
      pipe_reference(NULL, &res->reference);
   }
}

static void
clear_dirty_dmabuf_set(struct iris_context *ice)
{
   set_foreach(ice->dirty_dmabufs, entry) {
      struct pipe_resource *res = (struct pipe_resource *)entry->key;
      if (pipe_reference(&res->reference, NULL))
         res->screen->resource_destroy(res->screen, res);
   }

   _mesa_set_clear(ice->dirty_dmabufs, NULL);
}

void
iris_flush_dirty_dmabufs(struct iris_context *ice)
{
   set_foreach(ice->dirty_dmabufs, entry) {
      struct pipe_resource *res = (struct pipe_resource *)entry->key;
      ice->ctx.flush_resource(&ice->ctx, res);
   }

   clear_dirty_dmabuf_set(ice);
}


/**
 * Destroy a context, freeing any associated memory.
 */
static void
iris_destroy_context(struct pipe_context *ctx)
{
   struct iris_context *ice = (struct iris_context *)ctx;
   struct iris_screen *screen = (struct iris_screen *)ctx->screen;

   if (ctx->stream_uploader)
      u_upload_destroy(ctx->stream_uploader);
   if (ctx->const_uploader)
      u_upload_destroy(ctx->const_uploader);

   clear_dirty_dmabuf_set(ice);

   screen->vtbl.destroy_state(ice);

   for (unsigned i = 0; i < ARRAY_SIZE(ice->shaders.scratch_surfs); i++)
      pipe_resource_reference(&ice->shaders.scratch_surfs[i].res, NULL);

   iris_destroy_program_cache(ice);
   iris_destroy_border_color_pool(ice);
   if (screen->measure.config)
      iris_destroy_ctx_measure(ice);

   u_upload_destroy(ice->state.surface_uploader);
   u_upload_destroy(ice->state.bindless_uploader);
   u_upload_destroy(ice->state.dynamic_uploader);
   u_upload_destroy(ice->query_buffer_uploader);

   iris_batch_free(&ice->batches[IRIS_BATCH_RENDER]);
   iris_batch_free(&ice->batches[IRIS_BATCH_COMPUTE]);
   iris_destroy_binder(&ice->state.binder);

   slab_destroy_child(&ice->transfer_pool);
   slab_destroy_child(&ice->transfer_pool_unsync);

   ralloc_free(ice);
}

#define genX_call(devinfo, func, ...)             \
   switch ((devinfo)->verx10) {                   \
   case 125:                                      \
      gfx125_##func(__VA_ARGS__);                 \
      break;                                      \
   case 120:                                      \
      gfx12_##func(__VA_ARGS__);                  \
      break;                                      \
   case 110:                                      \
      gfx11_##func(__VA_ARGS__);                  \
      break;                                      \
   case 90:                                       \
      gfx9_##func(__VA_ARGS__);                   \
      break;                                      \
   case 80:                                       \
      gfx8_##func(__VA_ARGS__);                   \
      break;                                      \
   default:                                       \
      unreachable("Unknown hardware generation"); \
   }

/**
 * Create a context.
 *
 * This is where each context begins.
 */
struct pipe_context *
iris_create_context(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct iris_screen *screen = (struct iris_screen*)pscreen;
   const struct intel_device_info *devinfo = &screen->devinfo;
   struct iris_context *ice = rzalloc(NULL, struct iris_context);

   if (!ice)
      return NULL;

   struct pipe_context *ctx = &ice->ctx;

   ctx->screen = pscreen;
   ctx->priv = priv;

   ctx->stream_uploader = u_upload_create_default(ctx);
   if (!ctx->stream_uploader) {
      free(ctx);
      return NULL;
   }
   ctx->const_uploader = u_upload_create(ctx, 1024 * 1024,
                                         PIPE_BIND_CONSTANT_BUFFER,
                                         PIPE_USAGE_IMMUTABLE, 0);
   if (!ctx->const_uploader) {
      u_upload_destroy(ctx->stream_uploader);
      free(ctx);
      return NULL;
   }

   if (!create_dirty_dmabuf_set(ice)) {
      ralloc_free(ice);
      return NULL;
   }

   ctx->destroy = iris_destroy_context;
   ctx->set_debug_callback = iris_set_debug_callback;
   ctx->set_device_reset_callback = iris_set_device_reset_callback;
   ctx->get_device_reset_status = iris_get_device_reset_status;
   ctx->get_sample_position = iris_get_sample_position;

   iris_init_context_fence_functions(ctx);
   iris_init_blit_functions(ctx);
   iris_init_clear_functions(ctx);
   iris_init_program_functions(ctx);
   iris_init_resource_functions(ctx);
   iris_init_flush_functions(ctx);
   iris_init_perfquery_functions(ctx);

   iris_init_program_cache(ice);
   iris_init_border_color_pool(ice);
   iris_init_binder(ice);

   slab_create_child(&ice->transfer_pool, &screen->transfer_pool);
   slab_create_child(&ice->transfer_pool_unsync, &screen->transfer_pool);

   ice->state.surface_uploader =
      u_upload_create(ctx, 64 * 1024, PIPE_BIND_CUSTOM, PIPE_USAGE_IMMUTABLE,
                      IRIS_RESOURCE_FLAG_SURFACE_MEMZONE);
   ice->state.bindless_uploader =
      u_upload_create(ctx, 64 * 1024, PIPE_BIND_CUSTOM, PIPE_USAGE_IMMUTABLE,
                      IRIS_RESOURCE_FLAG_BINDLESS_MEMZONE);
   ice->state.dynamic_uploader =
      u_upload_create(ctx, 64 * 1024, PIPE_BIND_CUSTOM, PIPE_USAGE_IMMUTABLE,
                      IRIS_RESOURCE_FLAG_DYNAMIC_MEMZONE);

   ice->query_buffer_uploader =
      u_upload_create(ctx, 16 * 1024, PIPE_BIND_CUSTOM, PIPE_USAGE_STAGING,
                      0);

   genX_call(devinfo, init_state, ice);
   genX_call(devinfo, init_blorp, ice);
   genX_call(devinfo, init_query, ice);

   int priority = 0;
   if (flags & PIPE_CONTEXT_HIGH_PRIORITY)
      priority = INTEL_CONTEXT_HIGH_PRIORITY;
   if (flags & PIPE_CONTEXT_LOW_PRIORITY)
      priority = INTEL_CONTEXT_LOW_PRIORITY;

   if (INTEL_DEBUG & DEBUG_BATCH)
      ice->state.sizes = _mesa_hash_table_u64_create(ice);

   for (int i = 0; i < IRIS_BATCH_COUNT; i++) {
      iris_init_batch(ice, (enum iris_batch_name) i, priority);
   }

   screen->vtbl.init_render_context(&ice->batches[IRIS_BATCH_RENDER]);
   screen->vtbl.init_compute_context(&ice->batches[IRIS_BATCH_COMPUTE]);

   if (!(flags & PIPE_CONTEXT_PREFER_THREADED))
      return ctx;

   /* Clover doesn't support u_threaded_context */
   if (flags & PIPE_CONTEXT_COMPUTE_ONLY)
      return ctx;

   return threaded_context_create(ctx, &screen->transfer_pool,
                                  iris_replace_buffer_storage,
                                  NULL, /* TODO: asynchronous flushes? */
                                  NULL,
                                  false,
                                  &ice->thrctx);
}
