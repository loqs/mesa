/*
 * Copyright (C) 2021 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *   Boris Brezillon <boris.brezillon@collabora.com>
 */

#include "util/macros.h"

#include "panfrost-quirks.h"

#include "pan_cs.h"
#include "pan_encoder.h"
#include "pan_texture.h"

static void
pan_prepare_crc(const struct panfrost_device *dev,
                const struct pan_fb_info *fb, int rt_crc,
                struct MALI_ZS_CRC_EXTENSION *ext)
{
        if (rt_crc < 0)
                return;

        assert(rt_crc < fb->rt_count);

        const struct pan_image_view *rt = fb->rts[rt_crc].view;
        const struct pan_image_slice_layout *slice = &rt->image->layout.slices[rt->first_level];
        ext->crc_base = (rt->image->layout.crc_mode == PAN_IMAGE_CRC_INBAND ?
                         (rt->image->data.bo->ptr.gpu + rt->image->data.offset) :
                         (rt->image->crc.bo->ptr.gpu + rt->image->crc.offset)) +
                        slice->crc.offset;
        ext->crc_row_stride = slice->crc.stride;

        if (dev->arch == 7)
                ext->crc_render_target = rt_crc;

        if (fb->rts[rt_crc].clear) {
                uint32_t clear_val = fb->rts[rt_crc].clear_value[0];
                ext->crc_clear_color = clear_val | 0xc000000000000000 |
                                       (((uint64_t)clear_val & 0xffff) << 32);
        }
}

static enum mali_block_format_v7
mod_to_block_fmt_v7(uint64_t mod)
{
        switch (mod) {
        case DRM_FORMAT_MOD_LINEAR:
                return MALI_BLOCK_FORMAT_V7_LINEAR;
	case DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED:
                return MALI_BLOCK_FORMAT_V7_TILED_U_INTERLEAVED;
        default:
                if (drm_is_afbc(mod))
                        return MALI_BLOCK_FORMAT_V7_AFBC;

                unreachable("Unsupported modifer");
        }
}

static enum mali_block_format
mod_to_block_fmt(uint64_t mod)
{
        switch (mod) {
        case DRM_FORMAT_MOD_LINEAR:
                return MALI_BLOCK_FORMAT_LINEAR;
	case DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED:
                return MALI_BLOCK_FORMAT_TILED_U_INTERLEAVED;
        default:
                if (drm_is_afbc(mod))
                        return MALI_BLOCK_FORMAT_AFBC;

                unreachable("Unsupported modifer");
        }
}

static enum mali_zs_format
translate_zs_format(enum pipe_format in)
{
        switch (in) {
        case PIPE_FORMAT_Z16_UNORM: return MALI_ZS_FORMAT_D16;
        case PIPE_FORMAT_Z24_UNORM_S8_UINT: return MALI_ZS_FORMAT_D24S8;
        case PIPE_FORMAT_Z24X8_UNORM: return MALI_ZS_FORMAT_D24X8;
        case PIPE_FORMAT_Z32_FLOAT: return MALI_ZS_FORMAT_D32;
        case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT: return MALI_ZS_FORMAT_D32_S8X24;
        default: unreachable("Unsupported depth/stencil format.");
        }
}

static enum mali_s_format
translate_s_format(enum pipe_format in)
{
        switch (in) {
        case PIPE_FORMAT_S8_UINT: return MALI_S_FORMAT_S8;
        case PIPE_FORMAT_S8_UINT_Z24_UNORM:
        case PIPE_FORMAT_S8X24_UINT:
                return MALI_S_FORMAT_S8X24;
        case PIPE_FORMAT_Z24_UNORM_S8_UINT:
        case PIPE_FORMAT_X24S8_UINT:
                return MALI_S_FORMAT_X24S8;
        case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
                return MALI_S_FORMAT_X32_S8X24;
        default:
                unreachable("Unsupported stencil format.");
        }
}

static enum mali_msaa
mali_sampling_mode(const struct pan_image_view *view)
{
        if (view->image->layout.nr_samples > 1) {
                assert(view->nr_samples == view->image->layout.nr_samples);
                assert(view->image->layout.slices[0].surface_stride != 0);
                return MALI_MSAA_LAYERED;
        }

        if (view->nr_samples > view->image->layout.nr_samples) {
                assert(view->image->layout.nr_samples == 1);
                return MALI_MSAA_AVERAGE;
        }

        assert(view->nr_samples == view->image->layout.nr_samples);
        assert(view->nr_samples == 1);

        return MALI_MSAA_SINGLE;
}

static void
pan_prepare_s(const struct panfrost_device *dev,
              const struct pan_fb_info *fb,
              struct MALI_ZS_CRC_EXTENSION *ext)
{
        const struct pan_image_view *s = fb->zs.view.s;

        if (!s)
                return;

        unsigned level = s->first_level;

        if (dev->arch < 7)
                ext->s_msaa = mali_sampling_mode(s);
        else
                ext->s_msaa_v7 = mali_sampling_mode(s);

        struct pan_surface surf;
        pan_iview_get_surface(s, 0, 0, 0, &surf);

        assert(s->image->layout.modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
               s->image->layout.modifier == DRM_FORMAT_MOD_LINEAR);
        ext->s_writeback_base = surf.data;
        ext->s_writeback_row_stride = s->image->layout.slices[level].row_stride;
        ext->s_writeback_surface_stride =
                (s->image->layout.nr_samples > 1) ?
                s->image->layout.slices[level].surface_stride : 0;

        if (dev->arch >= 7)
                ext->s_block_format_v7 = mod_to_block_fmt_v7(s->image->layout.modifier);
        else
                ext->s_block_format = mod_to_block_fmt(s->image->layout.modifier);

        ext->s_write_format = translate_s_format(s->format);
}

static void
pan_prepare_zs(const struct panfrost_device *dev,
               const struct pan_fb_info *fb,
               struct MALI_ZS_CRC_EXTENSION *ext)
{
        const struct pan_image_view *zs = fb->zs.view.zs;

        if (!zs)
                return;

        unsigned level = zs->first_level;

        if (dev->arch < 7)
                ext->zs_msaa = mali_sampling_mode(zs);
        else
                ext->zs_msaa_v7 = mali_sampling_mode(zs);

        struct pan_surface surf;
        pan_iview_get_surface(zs, 0, 0, 0, &surf);

        if (drm_is_afbc(zs->image->layout.modifier)) {
                const struct pan_image_slice_layout *slice = &zs->image->layout.slices[level];

                ext->zs_afbc_header = surf.afbc.header;
                ext->zs_afbc_body = surf.afbc.body;

                if (pan_is_bifrost(dev)) {
                        ext->zs_afbc_row_stride = slice->afbc.row_stride /
                                                  AFBC_HEADER_BYTES_PER_TILE;
                } else {
                        ext->zs_block_format = MALI_BLOCK_FORMAT_AFBC;
                        ext->zs_afbc_body_size = 0x1000;
                        ext->zs_afbc_chunk_size = 9;
                        ext->zs_afbc_sparse = true;
                }
        } else {
                assert(zs->image->layout.modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
                       zs->image->layout.modifier == DRM_FORMAT_MOD_LINEAR);

                /* TODO: Z32F(S8) support, which is always linear */

                ext->zs_writeback_base = surf.data;
                ext->zs_writeback_row_stride =
                        zs->image->layout.slices[level].row_stride;
                ext->zs_writeback_surface_stride =
                        (zs->image->layout.nr_samples > 1) ?
                        zs->image->layout.slices[level].surface_stride : 0;
        }

        if (dev->arch >= 7)
                ext->zs_block_format_v7 = mod_to_block_fmt_v7(zs->image->layout.modifier);
        else
                ext->zs_block_format = mod_to_block_fmt(zs->image->layout.modifier);

        ext->zs_write_format = translate_zs_format(zs->format);
        if (ext->zs_write_format == MALI_ZS_FORMAT_D24S8)
                ext->s_writeback_base = ext->zs_writeback_base;
}

static void
pan_emit_zs_crc_ext(const struct panfrost_device *dev,
                    const struct pan_fb_info *fb, int rt_crc,
                    void *zs_crc_ext)
{
        pan_pack(zs_crc_ext, ZS_CRC_EXTENSION, cfg) {
                pan_prepare_crc(dev, fb, rt_crc, &cfg);
                cfg.zs_clean_pixel_write_enable = fb->zs.clear.z || fb->zs.clear.s;
                pan_prepare_zs(dev, fb, &cfg);
                pan_prepare_s(dev, fb, &cfg);
        }
}

/* Measure format as it appears in the tile buffer */

static unsigned
pan_bytes_per_pixel_tib(enum pipe_format format)
{
        if (panfrost_blendable_formats[format].internal) {
                /* Blendable formats are always 32-bits in the tile buffer,
                 * extra bits are used as padding or to dither */
                return 4;
        } else {
                /* Non-blendable formats are raw, rounded up to the nearest
                 * power-of-two size */
                unsigned bytes = util_format_get_blocksize(format);
                return util_next_power_of_two(bytes);
        }
}

static unsigned
pan_internal_cbuf_size(const struct pan_fb_info *fb,
                       unsigned *tile_size)
{
        unsigned total_size = 0;

        *tile_size = 16 * 16;
        for (int cb = 0; cb < fb->rt_count; ++cb) {
                const struct pan_image_view *rt = fb->rts[cb].view;

                if (!rt)
                        continue;

                total_size += pan_bytes_per_pixel_tib(rt->format) *
                              rt->nr_samples * (*tile_size);
        }

        /* We have a 4KB budget, let's reduce the tile size until it fits. */
        while (total_size > 4096) {
                total_size >>= 1;
                *tile_size >>= 1;
        }

        /* Align on 1k. */
        total_size = ALIGN_POT(total_size, 1024);

        /* Minimum tile size is 4x4. */
        assert(*tile_size >= 4 * 4);
        return total_size;
}

static inline enum mali_sample_pattern
pan_sample_pattern(unsigned samples)
{
        switch (samples) {
        case 1:  return MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
        case 4:  return MALI_SAMPLE_PATTERN_ROTATED_4X_GRID;
        case 8:  return MALI_SAMPLE_PATTERN_D3D_8X_GRID;
        case 16: return MALI_SAMPLE_PATTERN_D3D_16X_GRID;
        default: unreachable("Unsupported sample count");
        }
}

int
pan_select_crc_rt(const struct panfrost_device *dev, const struct pan_fb_info *fb)
{
        if (dev->arch < 7) {
                if (fb->rt_count == 1 && fb->rts[0].view && !fb->rts[0].discard &&
                    fb->rts[0].view->image->layout.crc_mode != PAN_IMAGE_CRC_NONE)
                        return 0;

                return -1;
        }

        bool best_rt_valid = false;
        int best_rt = -1;

        for (unsigned i = 0; i < fb->rt_count; i++) {
		if (!fb->rts[i].view || fb->rts[0].discard ||
                    fb->rts[i].view->image->layout.crc_mode == PAN_IMAGE_CRC_NONE)
                        continue;

                bool valid = *(fb->rts[i].crc_valid);
                bool full = !fb->extent.minx && !fb->extent.miny &&
                            fb->extent.maxx == (fb->width - 1) &&
                            fb->extent.maxy == (fb->height - 1);
                if (!full && !valid)
                        continue;

                if (best_rt < 0 || (valid && !best_rt_valid)) {
                        best_rt = i;
                        best_rt_valid = valid;
                }

                if (valid)
                        break;
        }

        return best_rt;
}

bool
pan_fbd_has_zs_crc_ext(const struct panfrost_device *dev,
                       const struct pan_fb_info *fb)
{
        if (dev->quirks & MIDGARD_SFBD)
                return false;

        return fb->zs.view.zs || fb->zs.view.s || pan_select_crc_rt(dev, fb) >= 0;
}

static enum mali_mfbd_color_format
pan_mfbd_raw_format(unsigned bits)
{
        switch (bits) {
        case    8: return MALI_MFBD_COLOR_FORMAT_RAW8;
        case   16: return MALI_MFBD_COLOR_FORMAT_RAW16;
        case   24: return MALI_MFBD_COLOR_FORMAT_RAW24;
        case   32: return MALI_MFBD_COLOR_FORMAT_RAW32;
        case   48: return MALI_MFBD_COLOR_FORMAT_RAW48;
        case   64: return MALI_MFBD_COLOR_FORMAT_RAW64;
        case   96: return MALI_MFBD_COLOR_FORMAT_RAW96;
        case  128: return MALI_MFBD_COLOR_FORMAT_RAW128;
        case  192: return MALI_MFBD_COLOR_FORMAT_RAW192;
        case  256: return MALI_MFBD_COLOR_FORMAT_RAW256;
        case  384: return MALI_MFBD_COLOR_FORMAT_RAW384;
        case  512: return MALI_MFBD_COLOR_FORMAT_RAW512;
        case  768: return MALI_MFBD_COLOR_FORMAT_RAW768;
        case 1024: return MALI_MFBD_COLOR_FORMAT_RAW1024;
        case 1536: return MALI_MFBD_COLOR_FORMAT_RAW1536;
        case 2048: return MALI_MFBD_COLOR_FORMAT_RAW2048;
        default: unreachable("invalid raw bpp");
        }
}

static void
pan_rt_init_format(const struct panfrost_device *dev,
                   const struct pan_image_view *rt,
                   struct MALI_RENDER_TARGET *cfg)
{
        /* Explode details on the format */

        const struct util_format_description *desc =
                util_format_description(rt->format);

        /* The swizzle for rendering is inverted from texturing */

        unsigned char swizzle[4];
        panfrost_invert_swizzle(desc->swizzle, swizzle);

        cfg->swizzle = panfrost_translate_swizzle_4(swizzle);

        /* Fill in accordingly, defaulting to 8-bit UNORM */

        if (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB)
                cfg->srgb = true;

        struct pan_blendable_format fmt = panfrost_blendable_formats[rt->format];

        if (fmt.internal) {
                cfg->internal_format = fmt.internal;
                cfg->writeback_format = fmt.writeback;
        } else {
                /* Construct RAW internal/writeback, where internal is
                 * specified logarithmically (round to next power-of-two).
                 * Offset specified from RAW8, where 8 = 2^3 */

                unsigned bits = desc->block.bits;
                unsigned offset = util_logbase2_ceil(bits) - 3;
                assert(offset <= 4);

                cfg->internal_format =
                        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW8 + offset;

                cfg->writeback_format = pan_mfbd_raw_format(bits);
        }
}

static void
pan_prepare_rt(const struct panfrost_device *dev,
               const struct pan_fb_info *fb, unsigned idx,
               unsigned cbuf_offset,
               struct MALI_RENDER_TARGET *cfg)
{
        cfg->clean_pixel_write_enable = fb->rts[idx].clear;
        cfg->internal_buffer_offset = cbuf_offset;
        if (fb->rts[idx].clear) {
                cfg->clear.color_0 = fb->rts[idx].clear_value[0];
                cfg->clear.color_1 = fb->rts[idx].clear_value[1];
                cfg->clear.color_2 = fb->rts[idx].clear_value[2];
                cfg->clear.color_3 = fb->rts[idx].clear_value[3];
        }

        const struct pan_image_view *rt = fb->rts[idx].view;
        if (!rt || fb->rts[idx].discard) {
                cfg->internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
                cfg->internal_buffer_offset = cbuf_offset;
                if (dev->arch >= 7) {
                        cfg->bifrost_v7.writeback_block_format = MALI_BLOCK_FORMAT_V7_TILED_U_INTERLEAVED;
                        cfg->dithering_enable = true;
                }

                return;
        }

        cfg->write_enable = true;
        cfg->dithering_enable = true;

        unsigned level = rt->first_level;
        assert(rt->last_level == rt->first_level);
        assert(rt->last_layer == rt->first_layer);

        int row_stride = rt->image->layout.slices[level].row_stride;

        /* Only set layer_stride for layered MSAA rendering  */

        unsigned layer_stride =
                (rt->image->layout.nr_samples > 1) ?
                        rt->image->layout.slices[level].surface_stride : 0;

        cfg->writeback_msaa = mali_sampling_mode(rt);

        pan_rt_init_format(dev, rt, cfg);

        if (dev->arch >= 7)
                cfg->bifrost_v7.writeback_block_format = mod_to_block_fmt_v7(rt->image->layout.modifier);
        else
                cfg->midgard.writeback_block_format = mod_to_block_fmt(rt->image->layout.modifier);

        struct pan_surface surf;
        pan_iview_get_surface(rt, 0, 0, 0, &surf);

        if (drm_is_afbc(rt->image->layout.modifier)) {
                const struct pan_image_slice_layout *slice = &rt->image->layout.slices[level];

                if (pan_is_bifrost(dev)) {
                        cfg->afbc.row_stride = slice->afbc.row_stride /
                                               AFBC_HEADER_BYTES_PER_TILE;
                        cfg->bifrost_afbc.afbc_wide_block_enable =
                                panfrost_block_dim(rt->image->layout.modifier, true, 0) > 16;
                } else {
                        cfg->afbc.chunk_size = 9;
                        cfg->midgard_afbc.sparse = true;
                        cfg->afbc.body_size = slice->afbc.body_size;
                }

                cfg->afbc.header = surf.afbc.header;
                cfg->afbc.body = surf.afbc.body;

                if (rt->image->layout.modifier & AFBC_FORMAT_MOD_YTR)
                        cfg->afbc.yuv_transform_enable = true;
        } else {
                assert(rt->image->layout.modifier == DRM_FORMAT_MOD_LINEAR ||
                       rt->image->layout.modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED);
                cfg->rgb.base = surf.data;
                cfg->rgb.row_stride = row_stride;
                cfg->rgb.surface_stride = layer_stride;
        }
}

static void
pan_emit_rt(const struct panfrost_device *dev,
            const struct pan_fb_info *fb,
            unsigned idx, unsigned cbuf_offset, void *out)
{
        pan_pack(out, RENDER_TARGET, cfg) {
                pan_prepare_rt(dev, fb, idx, cbuf_offset, &cfg);
        }
}

static unsigned
pan_wls_instances(const struct pan_compute_dim *dim)
{
        return util_next_power_of_two(dim->x) *
               util_next_power_of_two(dim->y) *
               util_next_power_of_two(dim->z);
}

static unsigned
pan_wls_adjust_size(unsigned wls_size)
{
        return util_next_power_of_two(MAX2(wls_size, 128));
}

unsigned
pan_wls_mem_size(const struct panfrost_device *dev,
                 const struct pan_compute_dim *dim,
                 unsigned wls_size)
{
        unsigned instances = pan_wls_instances(dim);

        return pan_wls_adjust_size(wls_size) * instances * dev->core_count;
}

void
pan_emit_tls(const struct panfrost_device *dev,
             const struct pan_tls_info *info,
             void *out)
{
        pan_pack(out, LOCAL_STORAGE, cfg) {
                if (info->tls.size) {
                        unsigned shift =
                                panfrost_get_stack_shift(info->tls.size);

                        /* TODO: Why do we need to make the stack bigger than other platforms? */
	                if (dev->quirks & MIDGARD_SFBD)
                                shift = MAX2(shift, 512);

                        cfg.tls_size = shift;
                        cfg.tls_base_pointer = info->tls.ptr;
                }

                if (info->wls.size) {
                        assert(!(info->wls.ptr & 4095));
                        assert((info->wls.ptr & 0xffffffff00000000ULL) == ((info->wls.ptr + info->wls.size - 1) & 0xffffffff00000000ULL));
                        cfg.wls_base_pointer = info->wls.ptr;
                        unsigned wls_size = pan_wls_adjust_size(info->wls.size);
                        cfg.wls_instances = pan_wls_instances(&info->wls.dim);
                        cfg.wls_size_scale = util_logbase2(wls_size) + 1;
                } else {
                        cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
                }
        }
}

static void
pan_emit_bifrost_mfbd_params(const struct panfrost_device *dev,
                             const struct pan_fb_info *fb,
                             void *fbd)
{
        pan_section_pack(fbd, MULTI_TARGET_FRAMEBUFFER, BIFROST_PARAMETERS, params) {
                params.sample_locations =
                        panfrost_sample_positions(dev, pan_sample_pattern(fb->nr_samples));
                params.pre_frame_0 = fb->bifrost.pre_post.modes[0];
                params.pre_frame_1 = fb->bifrost.pre_post.modes[1];
                params.post_frame = fb->bifrost.pre_post.modes[2];
                params.frame_shader_dcds = fb->bifrost.pre_post.dcds.gpu;
        }
}

static void
pan_emit_mfbd_bifrost_tiler(const struct pan_tiler_context *ctx, void *fbd)
{
        pan_section_pack(fbd, MULTI_TARGET_FRAMEBUFFER, BIFROST_TILER_POINTER, cfg) {
                cfg.address = ctx->bifrost;
        }
        pan_section_pack(fbd, MULTI_TARGET_FRAMEBUFFER, BIFROST_PADDING, padding);
}

static void
pan_emit_midgard_tiler(const struct panfrost_device *dev,
                       const struct pan_fb_info *fb,
                       const struct pan_tiler_context *tiler_ctx,
                       void *out)
{
        bool hierarchy = !(dev->quirks & MIDGARD_NO_HIER_TILING);

        assert(tiler_ctx->midgard.polygon_list->ptr.gpu);

        pan_pack(out, MIDGARD_TILER, cfg) {
                unsigned header_size;

                if (tiler_ctx->midgard.disable) {
                        cfg.hierarchy_mask =
                                hierarchy ?
                                MALI_MIDGARD_TILER_DISABLED :
                                MALI_MIDGARD_TILER_USER;
                        header_size = MALI_MIDGARD_TILER_MINIMUM_HEADER_SIZE;
                        cfg.polygon_list_size = header_size + (hierarchy ? 0 : 4);
                        cfg.heap_start = tiler_ctx->midgard.polygon_list->ptr.gpu;
                        cfg.heap_end = tiler_ctx->midgard.polygon_list->ptr.gpu;
		} else {
                        cfg.hierarchy_mask =
                                panfrost_choose_hierarchy_mask(fb->width,
                                                               fb->height,
                                                               1, hierarchy);
                        header_size = panfrost_tiler_header_size(fb->width,
                                                                 fb->height,
                                                                 cfg.hierarchy_mask,
                                                                 hierarchy);
                        cfg.polygon_list_size =
                                panfrost_tiler_full_size(fb->width, fb->height,
                                                         cfg.hierarchy_mask,
                                                         hierarchy);
                        cfg.heap_start = dev->tiler_heap->ptr.gpu;
                        cfg.heap_end = dev->tiler_heap->ptr.gpu + dev->tiler_heap->size;
                }

                cfg.polygon_list = tiler_ctx->midgard.polygon_list->ptr.gpu;
                cfg.polygon_list_body = cfg.polygon_list + header_size;
        }
}

static void
pan_emit_mfbd_midgard_tiler(const struct panfrost_device *dev,
                            const struct pan_fb_info *fb,
                            const struct pan_tiler_context *ctx,
                            void *fbd)
{
       pan_emit_midgard_tiler(dev, fb, ctx,
                              pan_section_ptr(fbd, MULTI_TARGET_FRAMEBUFFER, TILER));

        /* All weights set to 0, nothing to do here */
        pan_section_pack(fbd, MULTI_TARGET_FRAMEBUFFER, TILER_WEIGHTS, w);
}

static void
pan_emit_sfbd_tiler(const struct panfrost_device *dev,
                    const struct pan_fb_info *fb,
                    const struct pan_tiler_context *ctx,
                    void *fbd)
{
       pan_emit_midgard_tiler(dev, fb, ctx,
                              pan_section_ptr(fbd, SINGLE_TARGET_FRAMEBUFFER, TILER));

        /* All weights set to 0, nothing to do here */
        pan_section_pack(fbd, SINGLE_TARGET_FRAMEBUFFER, PADDING_1, padding);
        pan_section_pack(fbd, SINGLE_TARGET_FRAMEBUFFER, TILER_WEIGHTS, w);
}

static unsigned
pan_emit_mfbd(const struct panfrost_device *dev,
              const struct pan_fb_info *fb,
              const struct pan_tls_info *tls,
              const struct pan_tiler_context *tiler_ctx,
              void *out)
{
        unsigned tags = MALI_FBD_TAG_IS_MFBD;
        void *fbd = out;
        void *rtd = out + MALI_MULTI_TARGET_FRAMEBUFFER_LENGTH;

        if (pan_is_bifrost(dev)) {
                pan_emit_bifrost_mfbd_params(dev, fb, fbd);
        } else {
                pan_emit_tls(dev, tls,
                             pan_section_ptr(fbd, MULTI_TARGET_FRAMEBUFFER,
                                             LOCAL_STORAGE));
        }

        unsigned tile_size;
        unsigned internal_cbuf_size = pan_internal_cbuf_size(fb, &tile_size);
        int crc_rt = pan_select_crc_rt(dev, fb);
        bool has_zs_crc_ext = pan_fbd_has_zs_crc_ext(dev, fb);

        pan_section_pack(fbd, MULTI_TARGET_FRAMEBUFFER, PARAMETERS, cfg) {
                cfg.width = fb->width;
                cfg.height = fb->height;
                cfg.bound_min_x = fb->extent.minx;
                cfg.bound_min_y = fb->extent.miny;
                cfg.bound_max_x = fb->extent.maxx;
                cfg.bound_max_y = fb->extent.maxy;
                cfg.effective_tile_size = tile_size;
                cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
                cfg.render_target_count = MAX2(fb->rt_count, 1);

                /* Default to 24 bit depth if there's no surface. */
                cfg.z_internal_format =
                        fb->zs.view.zs ?
                        panfrost_get_z_internal_format(fb->zs.view.zs->format) :
                        MALI_Z_INTERNAL_FORMAT_D24;

                cfg.z_clear = fb->zs.clear_value.depth;
                cfg.s_clear = fb->zs.clear_value.stencil;
                cfg.color_buffer_allocation = internal_cbuf_size;
                cfg.sample_count = fb->nr_samples;
                cfg.sample_pattern = pan_sample_pattern(fb->nr_samples);
                cfg.z_write_enable = (fb->zs.view.zs && !fb->zs.discard.z);
                cfg.s_write_enable = (fb->zs.view.s && !fb->zs.discard.s);
                cfg.has_zs_crc_extension = has_zs_crc_ext;

                if (crc_rt >= 0) {
                        bool *valid = fb->rts[crc_rt].crc_valid;
                        bool full = !fb->extent.minx && !fb->extent.miny &&
                                    fb->extent.maxx == (fb->width - 1) &&
                                    fb->extent.maxy == (fb->height - 1);

                        cfg.crc_read_enable = *valid;

                        /* If the data is currently invalid, still write CRC
                         * data if we are doing a full write, so that it is
                         * valid for next time. */
                        cfg.crc_write_enable = *valid || full;

                        *valid = full;
                }
        }

        if (pan_is_bifrost(dev))
                pan_emit_mfbd_bifrost_tiler(tiler_ctx, fbd);
        else
                pan_emit_mfbd_midgard_tiler(dev, fb, tiler_ctx, fbd);

        if (has_zs_crc_ext) {
                pan_emit_zs_crc_ext(dev, fb, crc_rt,
                                    out + MALI_MULTI_TARGET_FRAMEBUFFER_LENGTH);
                rtd += MALI_ZS_CRC_EXTENSION_LENGTH;
                tags |= MALI_FBD_TAG_HAS_ZS_RT;
        }

        unsigned rt_count = MAX2(fb->rt_count, 1);
        unsigned cbuf_offset = 0;
        for (unsigned i = 0; i < rt_count; i++) {
                pan_emit_rt(dev, fb, i, cbuf_offset, rtd);
                rtd += MALI_RENDER_TARGET_LENGTH;
                if (!fb->rts[i].view)
                        continue;

                cbuf_offset += pan_bytes_per_pixel_tib(fb->rts[i].view->format) *
                               tile_size * fb->rts[i].view->image->layout.nr_samples;

                if (i != crc_rt)
                        *(fb->rts[i].crc_valid) = false;
        }
        tags |= MALI_POSITIVE(MAX2(fb->rt_count, 1)) << 2;

        return tags;
}

static void
pan_emit_sfbd(const struct panfrost_device *dev,
              const struct pan_fb_info *fb,
              const struct pan_tls_info *tls,
              const struct pan_tiler_context *tiler_ctx,
              void *fbd)
{
        pan_emit_tls(dev, tls,
                     pan_section_ptr(fbd, SINGLE_TARGET_FRAMEBUFFER,
                                     LOCAL_STORAGE));
        pan_section_pack(fbd, SINGLE_TARGET_FRAMEBUFFER, PARAMETERS, cfg) {
                cfg.bound_max_x = fb->width - 1;
                cfg.bound_max_y = fb->height - 1;
                cfg.dithering_enable = true;
                cfg.clean_pixel_write_enable = true;
                cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
                if (fb->rts[0].clear) {
                        cfg.clear_color_0 = fb->rts[0].clear_value[0];
                        cfg.clear_color_1 = fb->rts[0].clear_value[1];
                        cfg.clear_color_2 = fb->rts[0].clear_value[2];
                        cfg.clear_color_3 = fb->rts[0].clear_value[3];
                }

                if (fb->zs.clear.z)
                        cfg.z_clear = fb->zs.clear_value.depth;

                if (fb->zs.clear.s)
                        cfg.s_clear = fb->zs.clear_value.stencil;

                if (fb->rt_count && fb->rts[0].view) {
                        const struct pan_image_view *rt = fb->rts[0].view;

                        const struct util_format_description *desc =
                                util_format_description(rt->format);

                        /* The swizzle for rendering is inverted from texturing */
                        unsigned char swizzle[4];
                        panfrost_invert_swizzle(desc->swizzle, swizzle);
                        cfg.swizzle = panfrost_translate_swizzle_4(swizzle);

                        struct pan_blendable_format fmt = panfrost_blendable_formats[rt->format];
                        if (fmt.internal) {
                                cfg.internal_format = fmt.internal;
                                cfg.color_writeback_format = fmt.writeback;
                        } else {
                                unreachable("raw formats not finished for SFBD");
                        }

                        unsigned level = rt->first_level;
                        struct pan_surface surf;

                        pan_iview_get_surface(rt, 0, 0, 0, &surf);

                        cfg.color_write_enable = !fb->rts[0].discard;
                        cfg.color_writeback.base = surf.data;
                        cfg.color_writeback.row_stride =
	                        rt->image->layout.slices[level].row_stride;

                        cfg.color_block_format = mod_to_block_fmt(rt->image->layout.modifier);
                        assert(cfg.color_block_format == MALI_BLOCK_FORMAT_LINEAR ||
                               cfg.color_block_format == MALI_BLOCK_FORMAT_TILED_U_INTERLEAVED);

                        if (rt->image->layout.crc_mode != PAN_IMAGE_CRC_NONE) {
                                const struct pan_image_slice_layout *slice =
                                        &rt->image->layout.slices[level];

                                cfg.crc_buffer.row_stride = slice->crc.stride;
                                if (rt->image->layout.crc_mode == PAN_IMAGE_CRC_INBAND) {
                                        cfg.crc_buffer.base = rt->image->data.bo->ptr.gpu +
                                                              rt->image->data.offset +
                                                              slice->crc.offset;
                                } else {
                                        cfg.crc_buffer.base = rt->image->crc.bo->ptr.gpu +
                                                              rt->image->crc.offset +
                                                              slice->crc.offset;
                                }
                        }
                }

                if (fb->zs.view.zs) {
                        const struct pan_image_view *zs = fb->zs.view.zs;
                        unsigned level = zs->first_level;
                        struct pan_surface surf;

                        pan_iview_get_surface(zs, 0, 0, 0, &surf);

                        cfg.zs_write_enable = !fb->zs.discard.z;
                        cfg.zs_writeback.base = surf.data;
                        cfg.zs_writeback.row_stride =
                                zs->image->layout.slices[level].row_stride;
                        cfg.zs_block_format = mod_to_block_fmt(zs->image->layout.modifier);
                        assert(cfg.zs_block_format == MALI_BLOCK_FORMAT_LINEAR ||
                               cfg.zs_block_format == MALI_BLOCK_FORMAT_TILED_U_INTERLEAVED);

                        cfg.zs_format = translate_zs_format(zs->format);
                }

                cfg.sample_count = fb->nr_samples;

                /* XXX: different behaviour from MFBD and probably wrong... */
                cfg.msaa = mali_sampling_mode(fb->rts[0].view);
        }
        pan_emit_sfbd_tiler(dev, fb, tiler_ctx, fbd);
        pan_section_pack(fbd, SINGLE_TARGET_FRAMEBUFFER, PADDING_2, padding);
}

unsigned
pan_emit_fbd(const struct panfrost_device *dev,
             const struct pan_fb_info *fb,
             const struct pan_tls_info *tls,
             const struct pan_tiler_context *tiler_ctx,
             void *out)
{
        if (dev->quirks & MIDGARD_SFBD) {
                assert(fb->rt_count <= 1);
                pan_emit_sfbd(dev, fb, tls, tiler_ctx, out);
                return 0;
        } else {
                return pan_emit_mfbd(dev, fb, tls, tiler_ctx, out);
        }
}

void
pan_emit_bifrost_tiler_heap(const struct panfrost_device *dev,
                            void *out)
{
        pan_pack(out, BIFROST_TILER_HEAP, heap) {
                heap.size = dev->tiler_heap->size;
                heap.base = dev->tiler_heap->ptr.gpu;
                heap.bottom = dev->tiler_heap->ptr.gpu;
                heap.top = dev->tiler_heap->ptr.gpu + dev->tiler_heap->size;
        }
}

void
pan_emit_bifrost_tiler(const struct panfrost_device *dev,
                       unsigned fb_width, unsigned fb_height,
                       unsigned nr_samples,
                       mali_ptr heap,
                       void *out)
{
        pan_pack(out, BIFROST_TILER, tiler) {
                tiler.hierarchy_mask = 0x28;
                tiler.fb_width = fb_width;
                tiler.fb_height = fb_height;
                tiler.heap = heap;
                tiler.sample_pattern = pan_sample_pattern(nr_samples);
        }
}

void
pan_emit_fragment_job(const struct panfrost_device *dev,
                      const struct pan_fb_info *fb,
                      mali_ptr fbd,
                      void *out)
{
        pan_section_pack(out, FRAGMENT_JOB, HEADER, header) {
                header.type = MALI_JOB_TYPE_FRAGMENT;
                header.index = 1;
        }

        pan_section_pack(out, FRAGMENT_JOB, PAYLOAD, payload) {
                payload.bound_min_x = fb->extent.minx >> MALI_TILE_SHIFT;
                payload.bound_min_y = fb->extent.miny >> MALI_TILE_SHIFT;
                payload.bound_max_x = fb->extent.maxx >> MALI_TILE_SHIFT;
                payload.bound_max_y = fb->extent.maxy >> MALI_TILE_SHIFT;
                payload.framebuffer = fbd;

                if (fb->tile_map.base) {
                        payload.has_tile_enable_map = true;
                        payload.tile_enable_map = fb->tile_map.base;
                        payload.tile_enable_map_row_stride = fb->tile_map.stride;
                }
        }
}
