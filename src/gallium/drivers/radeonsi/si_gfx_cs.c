/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 * Copyright 2018 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "si_build_pm4.h"
#include "si_pipe.h"
#include "sid.h"
#include "util/os_time.h"
#include "util/u_log.h"
#include "util/u_upload_mgr.h"
#include "ac_debug.h"

/* initialize */
void si_need_gfx_cs_space(struct si_context *ctx, unsigned num_draws)
{
   struct radeon_cmdbuf *cs = &ctx->gfx_cs;

   /* There are two memory usage counters in the winsys for all buffers
    * that have been added (cs_add_buffer) and two counters in the pipe
    * driver for those that haven't been added yet.
    */
   if (unlikely(!radeon_cs_memory_below_limit(ctx->screen, &ctx->gfx_cs, ctx->vram_kb, ctx->gtt_kb))) {
      ctx->gtt_kb = 0;
      ctx->vram_kb = 0;
      si_flush_gfx_cs(ctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
      return;
   }
   ctx->gtt_kb = 0;
   ctx->vram_kb = 0;

   unsigned need_dwords = si_get_minimum_num_gfx_cs_dwords(ctx, num_draws);
   if (!ctx->ws->cs_check_space(cs, need_dwords, false))
      si_flush_gfx_cs(ctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
}

void si_flush_gfx_cs(struct si_context *ctx, unsigned flags, struct pipe_fence_handle **fence)
{
   struct radeon_cmdbuf *cs = &ctx->gfx_cs;
   struct radeon_winsys *ws = ctx->ws;
   struct si_screen *sscreen = ctx->screen;
   const unsigned wait_ps_cs = SI_CONTEXT_PS_PARTIAL_FLUSH | SI_CONTEXT_CS_PARTIAL_FLUSH;
   unsigned wait_flags = 0;

   if (ctx->gfx_flush_in_progress)
      return;

   /* The amdgpu kernel driver synchronizes execution for shared DMABUFs between
    * processes on DRM >= 3.39.0, so we don't have to wait at the end of IBs to
    * make sure everything is idle.
    *
    * The amdgpu winsys synchronizes execution for buffers shared by different
    * contexts within the same process.
    *
    * Interop with AMDVLK, RADV, or OpenCL within the same process requires
    * explicit fences or glFinish.
    */
   if (sscreen->info.is_amdgpu && sscreen->info.drm_minor >= 39)
      flags |= RADEON_FLUSH_START_NEXT_GFX_IB_NOW;

   if (!sscreen->info.kernel_flushes_tc_l2_after_ib) {
      wait_flags |= wait_ps_cs | SI_CONTEXT_INV_L2;
   } else if (ctx->chip_class == GFX6) {
      /* The kernel flushes L2 before shaders are finished. */
      wait_flags |= wait_ps_cs;
   } else if (!(flags & RADEON_FLUSH_START_NEXT_GFX_IB_NOW) ||
              ((flags & RADEON_FLUSH_TOGGLE_SECURE_SUBMISSION) &&
                !ws->cs_is_secure(cs))) {
      /* TODO: this workaround fixes subtitles rendering with mpv -vo=vaapi and
       * tmz but shouldn't be necessary.
       */
      wait_flags |= wait_ps_cs;
   }

   /* Drop this flush if it's a no-op. */
   if (!radeon_emitted(cs, ctx->initial_gfx_cs_size) &&
       (!wait_flags || !ctx->gfx_last_ib_is_busy) &&
       !(flags & RADEON_FLUSH_TOGGLE_SECURE_SUBMISSION)) {
      tc_driver_internal_flush_notify(ctx->tc);
      return;
   }

   /* Non-aux contexts must set up no-op API dispatch on GPU resets. This is
    * similar to si_get_reset_status but here we can ignore soft-recoveries,
    * while si_get_reset_status can't. */
   if (!(ctx->context_flags & SI_CONTEXT_FLAG_AUX) &&
       ctx->device_reset_callback.reset) {
      enum pipe_reset_status status = ctx->ws->ctx_query_reset_status(ctx->ctx, true, NULL);
      if (status != PIPE_NO_RESET)
         ctx->device_reset_callback.reset(ctx->device_reset_callback.data, status);
   }

   if (sscreen->debug_flags & DBG(CHECK_VM))
      flags &= ~PIPE_FLUSH_ASYNC;

   ctx->gfx_flush_in_progress = true;

   if (radeon_emitted(&ctx->prim_discard_compute_cs, 0)) {
      struct radeon_cmdbuf *compute_cs = &ctx->prim_discard_compute_cs;
      si_compute_signal_gfx(ctx);

      /* Make sure compute shaders are idle before leaving the IB, so that
       * the next IB doesn't overwrite GDS that might be in use. */
      radeon_begin(compute_cs);
      radeon_emit(compute_cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(compute_cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH) | EVENT_INDEX(4));
      radeon_end();

      /* Save the GDS prim restart counter if needed. */
      if (ctx->preserve_prim_restart_gds_at_flush) {
         si_cp_copy_data(ctx, compute_cs, COPY_DATA_DST_MEM, ctx->wait_mem_scratch, 4,
                         COPY_DATA_GDS, NULL, 4);
      }
   }

   if (ctx->has_graphics) {
      if (!list_is_empty(&ctx->active_queries))
         si_suspend_queries(ctx);

      ctx->streamout.suspended = false;
      if (ctx->streamout.begin_emitted) {
         si_emit_streamout_end(ctx);
         ctx->streamout.suspended = true;

         /* Since NGG streamout uses GDS, we need to make GDS
          * idle when we leave the IB, otherwise another process
          * might overwrite it while our shaders are busy.
          */
         if (sscreen->use_ngg_streamout)
            wait_flags |= SI_CONTEXT_PS_PARTIAL_FLUSH;
      }
   }

   /* Make sure CP DMA is idle at the end of IBs after L2 prefetches
    * because the kernel doesn't wait for it. */
   if (ctx->chip_class >= GFX7)
      si_cp_dma_wait_for_idle(ctx, &ctx->gfx_cs);

   /* Wait for draw calls to finish if needed. */
   if (wait_flags) {
      ctx->flags |= wait_flags;
      ctx->emit_cache_flush(ctx, &ctx->gfx_cs);
   }
   ctx->gfx_last_ib_is_busy = (wait_flags & wait_ps_cs) != wait_ps_cs;

   if (ctx->current_saved_cs) {
      si_trace_emit(ctx);

      /* Save the IB for debug contexts. */
      si_save_cs(ws, cs, &ctx->current_saved_cs->gfx, true);
      ctx->current_saved_cs->flushed = true;
      ctx->current_saved_cs->time_flush = os_time_get_nano();

      si_log_hw_flush(ctx);
   }

   if (si_compute_prim_discard_enabled(ctx)) {
      /* The compute IB can start after the previous gfx IB starts. */
      if (radeon_emitted(&ctx->prim_discard_compute_cs, 0) && ctx->last_gfx_fence) {
         ctx->ws->cs_add_fence_dependency(
            &ctx->gfx_cs, ctx->last_gfx_fence,
            RADEON_DEPENDENCY_PARALLEL_COMPUTE_ONLY | RADEON_DEPENDENCY_START_FENCE);
      }

      /* Remember the last execution barrier. It's in the IB.
       * It will signal the start of the next compute IB.
       */
      if (flags & RADEON_FLUSH_START_NEXT_GFX_IB_NOW && ctx->last_pkt3_write_data) {
         *ctx->last_pkt3_write_data = PKT3(PKT3_WRITE_DATA, 3, 0);
         ctx->last_pkt3_write_data = NULL;

         si_resource_reference(&ctx->last_ib_barrier_buf, ctx->barrier_buf);
         ctx->last_ib_barrier_buf_offset = ctx->barrier_buf_offset;
         si_resource_reference(&ctx->barrier_buf, NULL);

         ws->fence_reference(&ctx->last_ib_barrier_fence, NULL);
      }
   }

   if (ctx->is_noop)
      flags |= RADEON_FLUSH_NOOP;

   /* Flush the CS. */
   ws->cs_flush(cs, flags, &ctx->last_gfx_fence);

   tc_driver_internal_flush_notify(ctx->tc);
   if (fence)
      ws->fence_reference(fence, ctx->last_gfx_fence);

   ctx->num_gfx_cs_flushes++;

   if (si_compute_prim_discard_enabled(ctx)) {
      /* Remember the last execution barrier, which is the last fence
       * in this case.
       */
      if (!(flags & RADEON_FLUSH_START_NEXT_GFX_IB_NOW)) {
         ctx->last_pkt3_write_data = NULL;
         si_resource_reference(&ctx->last_ib_barrier_buf, NULL);
         ws->fence_reference(&ctx->last_ib_barrier_fence, ctx->last_gfx_fence);
      }
   }

   /* Check VM faults if needed. */
   if (sscreen->debug_flags & DBG(CHECK_VM)) {
      /* Use conservative timeout 800ms, after which we won't wait any
       * longer and assume the GPU is hung.
       */
      ctx->ws->fence_wait(ctx->ws, ctx->last_gfx_fence, 800 * 1000 * 1000);

      si_check_vm_faults(ctx, &ctx->current_saved_cs->gfx, RING_GFX);
   }

   if (unlikely(ctx->thread_trace &&
                (flags & PIPE_FLUSH_END_OF_FRAME))) {
      si_handle_thread_trace(ctx, &ctx->gfx_cs);
   }

   if (ctx->current_saved_cs)
      si_saved_cs_reference(&ctx->current_saved_cs, NULL);

   si_begin_new_gfx_cs(ctx, false);
   ctx->gfx_flush_in_progress = false;
}

static void si_begin_gfx_cs_debug(struct si_context *ctx)
{
   static const uint32_t zeros[1];
   assert(!ctx->current_saved_cs);

   ctx->current_saved_cs = calloc(1, sizeof(*ctx->current_saved_cs));
   if (!ctx->current_saved_cs)
      return;

   pipe_reference_init(&ctx->current_saved_cs->reference, 1);

   ctx->current_saved_cs->trace_buf =
      si_resource(pipe_buffer_create(ctx->b.screen, 0, PIPE_USAGE_STAGING, 8));
   if (!ctx->current_saved_cs->trace_buf) {
      free(ctx->current_saved_cs);
      ctx->current_saved_cs = NULL;
      return;
   }

   pipe_buffer_write_nooverlap(&ctx->b, &ctx->current_saved_cs->trace_buf->b.b, 0, sizeof(zeros),
                               zeros);
   ctx->current_saved_cs->trace_id = 0;

   si_trace_emit(ctx);

   radeon_add_to_buffer_list(ctx, &ctx->gfx_cs, ctx->current_saved_cs->trace_buf,
                             RADEON_USAGE_READWRITE, RADEON_PRIO_TRACE);
}

static void si_add_gds_to_buffer_list(struct si_context *sctx)
{
   if (sctx->gds) {
      sctx->ws->cs_add_buffer(&sctx->gfx_cs, sctx->gds, RADEON_USAGE_READWRITE, 0, 0);
      if (sctx->gds_oa) {
         sctx->ws->cs_add_buffer(&sctx->gfx_cs, sctx->gds_oa, RADEON_USAGE_READWRITE, 0, 0);
      }
   }
}

void si_allocate_gds(struct si_context *sctx)
{
   struct radeon_winsys *ws = sctx->ws;

   if (sctx->gds)
      return;

   assert(sctx->screen->use_ngg_streamout);

   /* 4 streamout GDS counters.
    * We need 256B (64 dw) of GDS, otherwise streamout hangs.
    */
   sctx->gds = ws->buffer_create(ws, 256, 4, RADEON_DOMAIN_GDS, RADEON_FLAG_DRIVER_INTERNAL);
   sctx->gds_oa = ws->buffer_create(ws, 4, 1, RADEON_DOMAIN_OA, RADEON_FLAG_DRIVER_INTERNAL);

   assert(sctx->gds && sctx->gds_oa);
   si_add_gds_to_buffer_list(sctx);
}

void si_set_tracked_regs_to_clear_state(struct si_context *ctx)
{
   STATIC_ASSERT(SI_NUM_TRACKED_REGS <= sizeof(ctx->tracked_regs.reg_saved) * 8);

   ctx->tracked_regs.reg_value[SI_TRACKED_DB_RENDER_CONTROL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_DB_COUNT_CONTROL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_DB_RENDER_OVERRIDE2] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_DB_SHADER_CONTROL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_CB_TARGET_MASK] = 0xffffffff;
   ctx->tracked_regs.reg_value[SI_TRACKED_CB_DCC_CONTROL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SX_PS_DOWNCONVERT] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SX_BLEND_OPT_EPSILON] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SX_BLEND_OPT_CONTROL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_LINE_CNTL] = 0x00001000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_AA_CONFIG] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_DB_EQAA] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_MODE_CNTL_1] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_PRIM_FILTER_CNTL] = 0;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_SMALL_PRIM_FILTER_CNTL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_VS_OUT_CNTL__VS] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_VS_OUT_CNTL__CL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_CLIP_CNTL] = 0x00090000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_BINNER_CNTL_0] = 0x00000003;
   ctx->tracked_regs.reg_value[SI_TRACKED_DB_VRS_OVERRIDE_CNTL] = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_VERT_CLIP_ADJ] = 0x3f800000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_VERT_DISC_ADJ] = 0x3f800000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_HORZ_CLIP_ADJ] = 0x3f800000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_GB_HORZ_DISC_ADJ] = 0x3f800000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_HARDWARE_SCREEN_OFFSET] = 0;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SU_VTX_CNTL] = 0x00000005;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_CLIPRECT_RULE] = 0xffff;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_SC_LINE_STIPPLE] = 0;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_ESGS_RING_ITEMSIZE]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_OFFSET_1]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_OFFSET_2]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_OFFSET_3]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GSVS_RING_ITEMSIZE]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_MAX_VERT_OUT]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE_1]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE_2]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_VERT_ITEMSIZE_3]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_INSTANCE_CNT]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_ONCHIP_CNTL]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_MAX_PRIMS_PER_SUBGROUP]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_GS_MODE]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_PRIMITIVEID_EN]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_REUSE_OFF]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_VS_OUT_CONFIG]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_GE_NGG_SUBGRP_CNTL]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_IDX_FORMAT]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_POS_FORMAT]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_VTE_CNTL]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_PA_CL_NGG_CNTL]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_PS_INPUT_ENA]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_PS_INPUT_ADDR]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_BARYC_CNTL]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_PS_IN_CONTROL]  = 0x00000002;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_Z_FORMAT]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_SPI_SHADER_COL_FORMAT]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_CB_SHADER_MASK]  = 0xffffffff;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_TF_PARAM]  = 0x00000000;
   ctx->tracked_regs.reg_value[SI_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL]  = 0x0000001e; /* From GFX8 */

   /* Set all cleared context registers to saved. */
   ctx->tracked_regs.reg_saved = ~(1ull << SI_TRACKED_GE_PC_ALLOC); /* uconfig reg */
   ctx->last_gs_out_prim = 0; /* cleared by CLEAR_STATE */
}

void si_install_draw_wrapper(struct si_context *sctx, pipe_draw_vbo_func wrapper)
{
   if (wrapper) {
      if (wrapper != sctx->b.draw_vbo) {
         assert (!sctx->real_draw_vbo);
         sctx->real_draw_vbo = sctx->b.draw_vbo;
         sctx->b.draw_vbo = wrapper;
      }
   } else if (sctx->real_draw_vbo) {
      sctx->real_draw_vbo = NULL;
      si_select_draw_vbo(sctx);
   }
}

static void si_draw_vbo_tmz_preamble(struct pipe_context *ctx,
                                     const struct pipe_draw_info *info,
                                     unsigned drawid_offset,
                                     const struct pipe_draw_indirect_info *indirect,
                                     const struct pipe_draw_start_count_bias *draws,
                                     unsigned num_draws) {
   struct si_context *sctx = (struct si_context *)ctx;

   bool secure = si_gfx_resources_check_encrypted(sctx);
   if (secure != sctx->ws->cs_is_secure(&sctx->gfx_cs)) {
      si_flush_gfx_cs(sctx, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW |
                            RADEON_FLUSH_TOGGLE_SECURE_SUBMISSION, NULL);
   }

   sctx->real_draw_vbo(ctx, info, drawid_offset, indirect, draws, num_draws);
}

void si_begin_new_gfx_cs(struct si_context *ctx, bool first_cs)
{
   bool is_secure = false;

   if (unlikely(radeon_uses_secure_bos(ctx->ws))) {
      /* Disable features that don't work with TMZ:
       *   - primitive discard
       */
      ctx->prim_discard_vertex_count_threshold = UINT_MAX;

      is_secure = ctx->ws->cs_is_secure(&ctx->gfx_cs);

      si_install_draw_wrapper(ctx, si_draw_vbo_tmz_preamble);
   }

   if (ctx->is_debug)
      si_begin_gfx_cs_debug(ctx);

   si_add_gds_to_buffer_list(ctx);

   /* Always invalidate caches at the beginning of IBs, because external
    * users (e.g. BO evictions and SDMA/UVD/VCE IBs) can modify our
    * buffers.
    *
    * Note that the cache flush done by the kernel at the end of GFX IBs
    * isn't useful here, because that flush can finish after the following
    * IB starts drawing.
    *
    * TODO: Do we also need to invalidate CB & DB caches?
    */
   ctx->flags |= SI_CONTEXT_INV_ICACHE | SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE |
                 SI_CONTEXT_INV_L2 | SI_CONTEXT_START_PIPELINE_STATS;
   ctx->pipeline_stats_enabled = -1;

   /* We don't know if the last draw call used GS fast launch, so assume it didn't. */
   if (ctx->chip_class == GFX10 && ctx->ngg_culling & SI_NGG_CULL_GS_FAST_LAUNCH_ALL)
      ctx->flags |= SI_CONTEXT_VGT_FLUSH;

   if (ctx->border_color_buffer) {
      radeon_add_to_buffer_list(ctx, &ctx->gfx_cs, ctx->border_color_buffer,
                                RADEON_USAGE_READ, RADEON_PRIO_BORDER_COLORS);
   }
   if (ctx->shadowed_regs) {
      radeon_add_to_buffer_list(ctx, &ctx->gfx_cs, ctx->shadowed_regs,
                                RADEON_USAGE_READWRITE,
                                RADEON_PRIO_DESCRIPTORS);
   }

   si_add_all_descriptors_to_bo_list(ctx);

   if (first_cs || !ctx->shadowed_regs) {
      si_shader_pointers_mark_dirty(ctx);
      ctx->cs_shader_state.initialized = false;
   }

   if (!ctx->has_graphics) {
      ctx->initial_gfx_cs_size = ctx->gfx_cs.current.cdw;
      return;
   }

   if (ctx->tess_rings) {
      radeon_add_to_buffer_list(ctx, &ctx->gfx_cs,
                                unlikely(is_secure) ? si_resource(ctx->tess_rings_tmz) : si_resource(ctx->tess_rings),
                                RADEON_USAGE_READWRITE, RADEON_PRIO_SHADER_RINGS);
   }

   /* set all valid group as dirty so they get reemited on
    * next draw command
    */
   si_pm4_reset_emitted(ctx, first_cs);

   /* The CS initialization should be emitted before everything else. */
   if (ctx->cs_preamble_state)
      si_pm4_emit(ctx, ctx->cs_preamble_state);
   if (ctx->cs_preamble_tess_rings)
      si_pm4_emit(ctx, unlikely(is_secure) ? ctx->cs_preamble_tess_rings_tmz :
         ctx->cs_preamble_tess_rings);
   if (ctx->cs_preamble_gs_rings)
      si_pm4_emit(ctx, ctx->cs_preamble_gs_rings);

   if (ctx->queued.named.ls)
      ctx->prefetch_L2_mask |= SI_PREFETCH_LS;
   if (ctx->queued.named.hs)
      ctx->prefetch_L2_mask |= SI_PREFETCH_HS;
   if (ctx->queued.named.es)
      ctx->prefetch_L2_mask |= SI_PREFETCH_ES;
   if (ctx->queued.named.gs)
      ctx->prefetch_L2_mask |= SI_PREFETCH_GS;
   if (ctx->queued.named.vs)
      ctx->prefetch_L2_mask |= SI_PREFETCH_VS;
   if (ctx->queued.named.ps)
      ctx->prefetch_L2_mask |= SI_PREFETCH_PS;

   /* CLEAR_STATE disables all colorbuffers, so only enable bound ones. */
   bool has_clear_state = ctx->screen->info.has_clear_state;
   if (has_clear_state || ctx->shadowed_regs) {
      ctx->framebuffer.dirty_cbufs =
            u_bit_consecutive(0, ctx->framebuffer.state.nr_cbufs);
      /* CLEAR_STATE disables the zbuffer, so only enable it if it's bound. */
      ctx->framebuffer.dirty_zsbuf = ctx->framebuffer.state.zsbuf != NULL;
   } else {
      ctx->framebuffer.dirty_cbufs = u_bit_consecutive(0, 8);
      ctx->framebuffer.dirty_zsbuf = true;
   }

   /* Even with shadowed registers, we have to add buffers to the buffer list.
    * These atoms are the only ones that add buffers.
    */
   si_mark_atom_dirty(ctx, &ctx->atoms.s.framebuffer);
   si_mark_atom_dirty(ctx, &ctx->atoms.s.render_cond);
   if (ctx->screen->use_ngg_culling)
      si_mark_atom_dirty(ctx, &ctx->atoms.s.ngg_cull_state);

   if (first_cs || !ctx->shadowed_regs) {
      /* These don't add any buffers, so skip them with shadowing. */
      si_mark_atom_dirty(ctx, &ctx->atoms.s.clip_regs);
      /* CLEAR_STATE sets zeros. */
      if (!has_clear_state || ctx->clip_state_any_nonzeros)
         si_mark_atom_dirty(ctx, &ctx->atoms.s.clip_state);
      ctx->sample_locs_num_samples = 0;
      si_mark_atom_dirty(ctx, &ctx->atoms.s.msaa_sample_locs);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.msaa_config);
      /* CLEAR_STATE sets 0xffff. */
      if (!has_clear_state || ctx->sample_mask != 0xffff)
         si_mark_atom_dirty(ctx, &ctx->atoms.s.sample_mask);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.cb_render_state);
      /* CLEAR_STATE sets zeros. */
      if (!has_clear_state || ctx->blend_color_any_nonzeros)
         si_mark_atom_dirty(ctx, &ctx->atoms.s.blend_color);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.db_render_state);
      if (ctx->chip_class >= GFX9)
         si_mark_atom_dirty(ctx, &ctx->atoms.s.dpbb_state);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.stencil_ref);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.spi_map);
      if (!ctx->screen->use_ngg_streamout)
         si_mark_atom_dirty(ctx, &ctx->atoms.s.streamout_enable);
      /* CLEAR_STATE disables all window rectangles. */
      if (!has_clear_state || ctx->num_window_rectangles > 0)
         si_mark_atom_dirty(ctx, &ctx->atoms.s.window_rectangles);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.guardband);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.scissors);
      si_mark_atom_dirty(ctx, &ctx->atoms.s.viewports);

      /* Invalidate various draw states so that they are emitted before
       * the first draw call. */
      si_invalidate_draw_constants(ctx);
      ctx->last_index_size = -1;
      ctx->last_primitive_restart_en = -1;
      ctx->last_restart_index = SI_RESTART_INDEX_UNKNOWN;
      ctx->last_prim = -1;
      ctx->last_multi_vgt_param = -1;
      ctx->last_vs_state = ~0;
      ctx->last_ls = NULL;
      ctx->last_tcs = NULL;
      ctx->last_tes_sh_base = -1;
      ctx->last_num_tcs_input_cp = -1;
      ctx->last_ls_hs_config = -1; /* impossible value */
      ctx->last_binning_enabled = -1;

      if (has_clear_state) {
         si_set_tracked_regs_to_clear_state(ctx);
      } else {
         /* Set all register values to unknown. */
         ctx->tracked_regs.reg_saved = 0;
         ctx->last_gs_out_prim = -1; /* unknown */
      }

      /* 0xffffffff is an impossible value to register SPI_PS_INPUT_CNTL_n */
      memset(ctx->tracked_regs.spi_ps_input_cntl, 0xff, sizeof(uint32_t) * 32);
   }

   si_mark_atom_dirty(ctx, &ctx->atoms.s.scratch_state);
   if (ctx->scratch_buffer) {
      si_context_add_resource_size(ctx, &ctx->scratch_buffer->b.b);
   }

   if (ctx->streamout.suspended) {
      ctx->streamout.append_bitmask = ctx->streamout.enabled_mask;
      si_streamout_buffers_dirty(ctx);
   }

   if (!list_is_empty(&ctx->active_queries))
      si_resume_queries(ctx);

   assert(!ctx->gfx_cs.prev_dw);
   ctx->initial_gfx_cs_size = ctx->gfx_cs.current.cdw;
   ctx->prim_discard_compute_ib_initialized = false;

   /* Compute-based primitive discard:
    *   The index ring is divided into 2 halves. Switch between the halves
    *   in the same fashion as doublebuffering.
    */
   if (ctx->index_ring_base)
      ctx->index_ring_base = 0;
   else
      ctx->index_ring_base = ctx->index_ring_size_per_ib;

   ctx->index_ring_offset = 0;

   /* All buffer references are removed on a flush, so si_check_needs_implicit_sync
    * cannot determine if si_make_CB_shader_coherent() needs to be called.
    * ctx->force_cb_shader_coherent will be cleared by the first call to
    * si_make_CB_shader_coherent.
    */
   ctx->force_cb_shader_coherent = true;
}

void si_trace_emit(struct si_context *sctx)
{
   struct radeon_cmdbuf *cs = &sctx->gfx_cs;
   uint32_t trace_id = ++sctx->current_saved_cs->trace_id;

   si_cp_write_data(sctx, sctx->current_saved_cs->trace_buf, 0, 4, V_370_MEM, V_370_ME, &trace_id);

   radeon_begin(cs);
   radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
   radeon_emit(cs, AC_ENCODE_TRACE_POINT(trace_id));
   radeon_end();

   if (sctx->log)
      u_log_flush(sctx->log);
}

void si_prim_discard_signal_next_compute_ib_start(struct si_context *sctx)
{
   if (!si_compute_prim_discard_enabled(sctx))
      return;

   if (!sctx->barrier_buf) {
      u_suballocator_alloc(&sctx->allocator_zeroed_memory, 4, 4, &sctx->barrier_buf_offset,
                           (struct pipe_resource **)&sctx->barrier_buf);
   }

   /* Emit a placeholder to signal the next compute IB to start.
    * See si_compute_prim_discard.c for explanation.
    */
   uint32_t signal = 1;
   si_cp_write_data(sctx, sctx->barrier_buf, sctx->barrier_buf_offset, 4, V_370_MEM, V_370_ME,
                    &signal);

   sctx->last_pkt3_write_data = &sctx->gfx_cs.current.buf[sctx->gfx_cs.current.cdw - 5];

   /* Only the last occurrence of WRITE_DATA will be executed.
    * The packet will be enabled in si_flush_gfx_cs.
    */
   *sctx->last_pkt3_write_data = PKT3(PKT3_NOP, 3, 0);
}

void si_emit_surface_sync(struct si_context *sctx, struct radeon_cmdbuf *cs, unsigned cp_coher_cntl)
{
   bool compute_ib = !sctx->has_graphics || cs == &sctx->prim_discard_compute_cs;

   assert(sctx->chip_class <= GFX9);

   cp_coher_cntl |= 1u << 31; /* don't sync PFP, i.e. execute the sync in ME */

   radeon_begin(cs);

   if (sctx->chip_class == GFX9 || compute_ib) {
      /* Flush caches and wait for the caches to assert idle. */
      radeon_emit(cs, PKT3(PKT3_ACQUIRE_MEM, 5, 0));
      radeon_emit(cs, cp_coher_cntl); /* CP_COHER_CNTL */
      radeon_emit(cs, 0xffffffff);    /* CP_COHER_SIZE */
      radeon_emit(cs, 0xffffff);      /* CP_COHER_SIZE_HI */
      radeon_emit(cs, 0);             /* CP_COHER_BASE */
      radeon_emit(cs, 0);             /* CP_COHER_BASE_HI */
      radeon_emit(cs, 0x0000000A);    /* POLL_INTERVAL */
   } else {
      /* ACQUIRE_MEM is only required on a compute ring. */
      radeon_emit(cs, PKT3(PKT3_SURFACE_SYNC, 3, 0));
      radeon_emit(cs, cp_coher_cntl); /* CP_COHER_CNTL */
      radeon_emit(cs, 0xffffffff);    /* CP_COHER_SIZE */
      radeon_emit(cs, 0);             /* CP_COHER_BASE */
      radeon_emit(cs, 0x0000000A);    /* POLL_INTERVAL */
   }
   radeon_end();

   /* ACQUIRE_MEM has an implicit context roll if the current context
    * is busy. */
   if (!compute_ib)
      sctx->context_roll = true;
}

void gfx10_emit_cache_flush(struct si_context *ctx, struct radeon_cmdbuf *cs)
{
   uint32_t gcr_cntl = 0;
   unsigned cb_db_event = 0;
   unsigned flags = ctx->flags;

   if (!ctx->has_graphics) {
      /* Only process compute flags. */
      flags &= SI_CONTEXT_INV_ICACHE | SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE |
               SI_CONTEXT_INV_L2 | SI_CONTEXT_WB_L2 | SI_CONTEXT_INV_L2_METADATA |
               SI_CONTEXT_CS_PARTIAL_FLUSH;
   }

   /* We don't need these. */
   assert(!(flags & (SI_CONTEXT_VGT_STREAMOUT_SYNC | SI_CONTEXT_FLUSH_AND_INV_DB_META)));

   radeon_begin(cs);

   if (flags & SI_CONTEXT_VGT_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
   }

   if (flags & SI_CONTEXT_FLUSH_AND_INV_CB)
      ctx->num_cb_cache_flushes++;
   if (flags & SI_CONTEXT_FLUSH_AND_INV_DB)
      ctx->num_db_cache_flushes++;

   if (flags & SI_CONTEXT_INV_ICACHE)
      gcr_cntl |= S_586_GLI_INV(V_586_GLI_ALL);
   if (flags & SI_CONTEXT_INV_SCACHE) {
      /* TODO: When writing to the SMEM L1 cache, we need to set SEQ
       * to FORWARD when both L1 and L2 are written out (WB or INV).
       */
      gcr_cntl |= S_586_GL1_INV(1) | S_586_GLK_INV(1);
   }
   if (flags & SI_CONTEXT_INV_VCACHE)
      gcr_cntl |= S_586_GL1_INV(1) | S_586_GLV_INV(1);

   /* The L2 cache ops are:
    * - INV: - invalidate lines that reflect memory (were loaded from memory)
    *        - don't touch lines that were overwritten (were stored by gfx clients)
    * - WB: - don't touch lines that reflect memory
    *       - write back lines that were overwritten
    * - WB | INV: - invalidate lines that reflect memory
    *             - write back lines that were overwritten
    *
    * GLM doesn't support WB alone. If WB is set, INV must be set too.
    */
   if (flags & SI_CONTEXT_INV_L2) {
      /* Writeback and invalidate everything in L2. */
      gcr_cntl |= S_586_GL2_INV(1) | S_586_GL2_WB(1) | S_586_GLM_INV(1) | S_586_GLM_WB(1);
      ctx->num_L2_invalidates++;
   } else if (flags & SI_CONTEXT_WB_L2) {
      gcr_cntl |= S_586_GL2_WB(1) | S_586_GLM_WB(1) | S_586_GLM_INV(1);
   } else if (flags & SI_CONTEXT_INV_L2_METADATA) {
      gcr_cntl |= S_586_GLM_INV(1) | S_586_GLM_WB(1);
   }

   if (flags & (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB)) {
      if (flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
         /* Flush CMASK/FMASK/DCC. Will wait for idle later. */
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));
      }
      if (flags & SI_CONTEXT_FLUSH_AND_INV_DB) {
         /* Flush HTILE. Will wait for idle later. */
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));
      }

      /* First flush CB/DB, then L1/L2. */
      gcr_cntl |= S_586_SEQ(V_586_SEQ_FORWARD);

      if ((flags & (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB)) ==
          (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB)) {
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
      } else if (flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
      } else if (flags & SI_CONTEXT_FLUSH_AND_INV_DB) {
         cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
      } else {
         assert(0);
      }
   } else {
      /* Wait for graphics shaders to go idle if requested. */
      if (flags & SI_CONTEXT_PS_PARTIAL_FLUSH) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
         /* Only count explicit shader flushes, not implicit ones. */
         ctx->num_vs_flushes++;
         ctx->num_ps_flushes++;
      } else if (flags & SI_CONTEXT_VS_PARTIAL_FLUSH) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
         ctx->num_vs_flushes++;
      }
   }

   if (flags & SI_CONTEXT_CS_PARTIAL_FLUSH && ctx->compute_is_busy) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH | EVENT_INDEX(4)));
      ctx->num_cs_flushes++;
      ctx->compute_is_busy = false;
   }
   radeon_end();

   if (cb_db_event) {
      struct si_resource* wait_mem_scratch = unlikely(ctx->ws->cs_is_secure(cs)) ?
        ctx->wait_mem_scratch_tmz : ctx->wait_mem_scratch;
      /* CB/DB flush and invalidate (or possibly just a wait for a
       * meta flush) via RELEASE_MEM.
       *
       * Combine this with other cache flushes when possible; this
       * requires affected shaders to be idle, so do it after the
       * CS_PARTIAL_FLUSH before (VS/PS partial flushes are always
       * implied).
       */
      uint64_t va;

      /* Do the flush (enqueue the event and wait for it). */
      va = wait_mem_scratch->gpu_address;
      ctx->wait_mem_number++;

      /* Get GCR_CNTL fields, because the encoding is different in RELEASE_MEM. */
      unsigned glm_wb = G_586_GLM_WB(gcr_cntl);
      unsigned glm_inv = G_586_GLM_INV(gcr_cntl);
      unsigned glv_inv = G_586_GLV_INV(gcr_cntl);
      unsigned gl1_inv = G_586_GL1_INV(gcr_cntl);
      assert(G_586_GL2_US(gcr_cntl) == 0);
      assert(G_586_GL2_RANGE(gcr_cntl) == 0);
      assert(G_586_GL2_DISCARD(gcr_cntl) == 0);
      unsigned gl2_inv = G_586_GL2_INV(gcr_cntl);
      unsigned gl2_wb = G_586_GL2_WB(gcr_cntl);
      unsigned gcr_seq = G_586_SEQ(gcr_cntl);

      gcr_cntl &= C_586_GLM_WB & C_586_GLM_INV & C_586_GLV_INV & C_586_GL1_INV & C_586_GL2_INV &
                  C_586_GL2_WB; /* keep SEQ */

      si_cp_release_mem(ctx, cs, cb_db_event,
                        S_490_GLM_WB(glm_wb) | S_490_GLM_INV(glm_inv) | S_490_GLV_INV(glv_inv) |
                           S_490_GL1_INV(gl1_inv) | S_490_GL2_INV(gl2_inv) | S_490_GL2_WB(gl2_wb) |
                           S_490_SEQ(gcr_seq),
                        EOP_DST_SEL_MEM, EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM,
                        EOP_DATA_SEL_VALUE_32BIT, wait_mem_scratch, va, ctx->wait_mem_number,
                        SI_NOT_QUERY);

      if (unlikely(ctx->thread_trace_enabled)) {
         si_sqtt_describe_barrier_start(ctx, &ctx->gfx_cs);
      }

      si_cp_wait_mem(ctx, cs, va, ctx->wait_mem_number, 0xffffffff, WAIT_REG_MEM_EQUAL);

      if (unlikely(ctx->thread_trace_enabled)) {
         si_sqtt_describe_barrier_end(ctx, &ctx->gfx_cs, flags);
      }
   }

   radeon_begin_again(cs);

   /* Ignore fields that only modify the behavior of other fields. */
   if (gcr_cntl & C_586_GL1_RANGE & C_586_GL2_RANGE & C_586_SEQ) {
      unsigned dont_sync_pfp = (!(flags & SI_CONTEXT_PFP_SYNC_ME)) << 31;

      /* Flush caches and wait for the caches to assert idle.
       * The cache flush is executed in the ME, but the PFP waits
       * for completion.
       */
      radeon_emit(cs, PKT3(PKT3_ACQUIRE_MEM, 6, 0));
      radeon_emit(cs, dont_sync_pfp); /* CP_COHER_CNTL */
      radeon_emit(cs, 0xffffffff); /* CP_COHER_SIZE */
      radeon_emit(cs, 0xffffff);   /* CP_COHER_SIZE_HI */
      radeon_emit(cs, 0);          /* CP_COHER_BASE */
      radeon_emit(cs, 0);          /* CP_COHER_BASE_HI */
      radeon_emit(cs, 0x0000000A); /* POLL_INTERVAL */
      radeon_emit(cs, gcr_cntl);   /* GCR_CNTL */
   } else if (flags & SI_CONTEXT_PFP_SYNC_ME) {
      /* Synchronize PFP with ME. (this stalls PFP) */
      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);
   }

   if (flags & SI_CONTEXT_START_PIPELINE_STATS && ctx->pipeline_stats_enabled != 1) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_START) | EVENT_INDEX(0));
      ctx->pipeline_stats_enabled = 1;
   } else if (flags & SI_CONTEXT_STOP_PIPELINE_STATS && ctx->pipeline_stats_enabled != 0) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_STOP) | EVENT_INDEX(0));
      ctx->pipeline_stats_enabled = 0;
   }
   radeon_end();

   ctx->flags = 0;
}

void si_emit_cache_flush(struct si_context *sctx, struct radeon_cmdbuf *cs)
{
   uint32_t flags = sctx->flags;

   if (!sctx->has_graphics) {
      /* Only process compute flags. */
      flags &= SI_CONTEXT_INV_ICACHE | SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE |
               SI_CONTEXT_INV_L2 | SI_CONTEXT_WB_L2 | SI_CONTEXT_INV_L2_METADATA |
               SI_CONTEXT_CS_PARTIAL_FLUSH;
   }

   uint32_t cp_coher_cntl = 0;
   const uint32_t flush_cb_db = flags & (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB);
   const bool is_barrier =
      flush_cb_db ||
      /* INV_ICACHE == beginning of gfx IB. Checking
       * INV_ICACHE fixes corruption for DeusExMD with
       * compute-based culling, but I don't know why.
       */
      flags & (SI_CONTEXT_INV_ICACHE | SI_CONTEXT_PS_PARTIAL_FLUSH | SI_CONTEXT_VS_PARTIAL_FLUSH) ||
      (flags & SI_CONTEXT_CS_PARTIAL_FLUSH && sctx->compute_is_busy);

   assert(sctx->chip_class <= GFX9);

   if (flags & SI_CONTEXT_FLUSH_AND_INV_CB)
      sctx->num_cb_cache_flushes++;
   if (flags & SI_CONTEXT_FLUSH_AND_INV_DB)
      sctx->num_db_cache_flushes++;

   /* GFX6 has a bug that it always flushes ICACHE and KCACHE if either
    * bit is set. An alternative way is to write SQC_CACHES, but that
    * doesn't seem to work reliably. Since the bug doesn't affect
    * correctness (it only does more work than necessary) and
    * the performance impact is likely negligible, there is no plan
    * to add a workaround for it.
    */

   if (flags & SI_CONTEXT_INV_ICACHE)
      cp_coher_cntl |= S_0085F0_SH_ICACHE_ACTION_ENA(1);
   if (flags & SI_CONTEXT_INV_SCACHE)
      cp_coher_cntl |= S_0085F0_SH_KCACHE_ACTION_ENA(1);

   if (sctx->chip_class <= GFX8) {
      if (flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
         cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1) | S_0085F0_CB0_DEST_BASE_ENA(1) |
                          S_0085F0_CB1_DEST_BASE_ENA(1) | S_0085F0_CB2_DEST_BASE_ENA(1) |
                          S_0085F0_CB3_DEST_BASE_ENA(1) | S_0085F0_CB4_DEST_BASE_ENA(1) |
                          S_0085F0_CB5_DEST_BASE_ENA(1) | S_0085F0_CB6_DEST_BASE_ENA(1) |
                          S_0085F0_CB7_DEST_BASE_ENA(1);

         /* Necessary for DCC */
         if (sctx->chip_class == GFX8)
            si_cp_release_mem(sctx, cs, V_028A90_FLUSH_AND_INV_CB_DATA_TS, 0, EOP_DST_SEL_MEM,
                              EOP_INT_SEL_NONE, EOP_DATA_SEL_DISCARD, NULL, 0, 0, SI_NOT_QUERY);
      }
      if (flags & SI_CONTEXT_FLUSH_AND_INV_DB)
         cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1) | S_0085F0_DB_DEST_BASE_ENA(1);
   }

   radeon_begin(cs);

   if (flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
      /* Flush CMASK/FMASK/DCC. SURFACE_SYNC will wait for idle. */
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));
   }
   if (flags & (SI_CONTEXT_FLUSH_AND_INV_DB | SI_CONTEXT_FLUSH_AND_INV_DB_META)) {
      /* Flush HTILE. SURFACE_SYNC will wait for idle. */
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));
   }

   /* Wait for shader engines to go idle.
    * VS and PS waits are unnecessary if SURFACE_SYNC is going to wait
    * for everything including CB/DB cache flushes.
    */
   if (!flush_cb_db) {
      if (flags & SI_CONTEXT_PS_PARTIAL_FLUSH) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
         /* Only count explicit shader flushes, not implicit ones
          * done by SURFACE_SYNC.
          */
         sctx->num_vs_flushes++;
         sctx->num_ps_flushes++;
      } else if (flags & SI_CONTEXT_VS_PARTIAL_FLUSH) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
         sctx->num_vs_flushes++;
      }
   }

   if (flags & SI_CONTEXT_CS_PARTIAL_FLUSH && sctx->compute_is_busy) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH) | EVENT_INDEX(4));
      sctx->num_cs_flushes++;
      sctx->compute_is_busy = false;
   }

   /* VGT state synchronization. */
   if (flags & SI_CONTEXT_VGT_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
   }
   if (flags & SI_CONTEXT_VGT_STREAMOUT_SYNC) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_STREAMOUT_SYNC) | EVENT_INDEX(0));
   }

   radeon_end();

   /* GFX9: Wait for idle if we're flushing CB or DB. ACQUIRE_MEM doesn't
    * wait for idle on GFX9. We have to use a TS event.
    */
   if (sctx->chip_class == GFX9 && flush_cb_db) {
      uint64_t va;
      unsigned tc_flags, cb_db_event;

      /* Set the CB/DB flush event. */
      switch (flush_cb_db) {
      case SI_CONTEXT_FLUSH_AND_INV_CB:
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
         break;
      case SI_CONTEXT_FLUSH_AND_INV_DB:
         cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
         break;
      default:
         /* both CB & DB */
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
      }

      /* These are the only allowed combinations. If you need to
       * do multiple operations at once, do them separately.
       * All operations that invalidate L2 also seem to invalidate
       * metadata. Volatile (VOL) and WC flushes are not listed here.
       *
       * TC    | TC_WB         = writeback & invalidate L2 & L1
       * TC    | TC_WB | TC_NC = writeback & invalidate L2 for MTYPE == NC
       *         TC_WB | TC_NC = writeback L2 for MTYPE == NC
       * TC            | TC_NC = invalidate L2 for MTYPE == NC
       * TC    | TC_MD         = writeback & invalidate L2 metadata (DCC, etc.)
       * TCL1                  = invalidate L1
       */
      tc_flags = 0;

      if (flags & SI_CONTEXT_INV_L2_METADATA) {
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_MD_ACTION_ENA;
      }

      /* Ideally flush TC together with CB/DB. */
      if (flags & SI_CONTEXT_INV_L2) {
         /* Writeback and invalidate everything in L2 & L1. */
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_WB_ACTION_ENA;

         /* Clear the flags. */
         flags &= ~(SI_CONTEXT_INV_L2 | SI_CONTEXT_WB_L2 | SI_CONTEXT_INV_VCACHE);
         sctx->num_L2_invalidates++;
      }

      /* Do the flush (enqueue the event and wait for it). */
      struct si_resource* wait_mem_scratch = unlikely(sctx->ws->cs_is_secure(cs)) ?
        sctx->wait_mem_scratch_tmz : sctx->wait_mem_scratch;
      va = wait_mem_scratch->gpu_address;
      sctx->wait_mem_number++;

      si_cp_release_mem(sctx, cs, cb_db_event, tc_flags, EOP_DST_SEL_MEM,
                        EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM, EOP_DATA_SEL_VALUE_32BIT,
                        wait_mem_scratch, va, sctx->wait_mem_number, SI_NOT_QUERY);

      if (unlikely(sctx->thread_trace_enabled)) {
         si_sqtt_describe_barrier_start(sctx, &sctx->gfx_cs);
      }

      si_cp_wait_mem(sctx, cs, va, sctx->wait_mem_number, 0xffffffff, WAIT_REG_MEM_EQUAL);

      if (unlikely(sctx->thread_trace_enabled)) {
         si_sqtt_describe_barrier_end(sctx, &sctx->gfx_cs, sctx->flags);
      }
   }

   /* GFX6-GFX8 only:
    *   When one of the CP_COHER_CNTL.DEST_BASE flags is set, SURFACE_SYNC
    *   waits for idle, so it should be last. SURFACE_SYNC is done in PFP.
    *
    * cp_coher_cntl should contain all necessary flags except TC and PFP flags
    * at this point.
    *
    * GFX6-GFX7 don't support L2 write-back.
    */
   if (flags & SI_CONTEXT_INV_L2 || (sctx->chip_class <= GFX7 && (flags & SI_CONTEXT_WB_L2))) {
      /* Invalidate L1 & L2. (L1 is always invalidated on GFX6)
       * WB must be set on GFX8+ when TC_ACTION is set.
       */
      si_emit_surface_sync(sctx, cs,
                           cp_coher_cntl | S_0085F0_TC_ACTION_ENA(1) | S_0085F0_TCL1_ACTION_ENA(1) |
                              S_0301F0_TC_WB_ACTION_ENA(sctx->chip_class >= GFX8));
      cp_coher_cntl = 0;
      sctx->num_L2_invalidates++;
   } else {
      /* L1 invalidation and L2 writeback must be done separately,
       * because both operations can't be done together.
       */
      if (flags & SI_CONTEXT_WB_L2) {
         /* WB = write-back
          * NC = apply to non-coherent MTYPEs
          *      (i.e. MTYPE <= 1, which is what we use everywhere)
          *
          * WB doesn't work without NC.
          */
         si_emit_surface_sync(
            sctx, cs,
            cp_coher_cntl | S_0301F0_TC_WB_ACTION_ENA(1) | S_0301F0_TC_NC_ACTION_ENA(1));
         cp_coher_cntl = 0;
         sctx->num_L2_writebacks++;
      }
      if (flags & SI_CONTEXT_INV_VCACHE) {
         /* Invalidate per-CU VMEM L1. */
         si_emit_surface_sync(sctx, cs, cp_coher_cntl | S_0085F0_TCL1_ACTION_ENA(1));
         cp_coher_cntl = 0;
      }
   }

   /* If TC flushes haven't cleared this... */
   if (cp_coher_cntl)
      si_emit_surface_sync(sctx, cs, cp_coher_cntl);

   if (flags & SI_CONTEXT_PFP_SYNC_ME) {
      radeon_begin(cs);
      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);
      radeon_end();
   }

   if (is_barrier)
      si_prim_discard_signal_next_compute_ib_start(sctx);

   if (flags & SI_CONTEXT_START_PIPELINE_STATS && sctx->pipeline_stats_enabled != 1) {
      radeon_begin(cs);
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_START) | EVENT_INDEX(0));
      radeon_end();
      sctx->pipeline_stats_enabled = 1;
   } else if (flags & SI_CONTEXT_STOP_PIPELINE_STATS && sctx->pipeline_stats_enabled != 0) {
      radeon_begin(cs);
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_STOP) | EVENT_INDEX(0));
      radeon_end();
      sctx->pipeline_stats_enabled = 0;
   }

   sctx->flags = 0;
}
