/*
 * Copyright (C) 2020-2021 Collabora, Ltd.
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

#include <math.h>
#include <stdio.h>
#include "pan_blend.h"
#include "pan_blitter.h"
#include "pan_cs.h"
#include "pan_encoder.h"
#include "pan_pool.h"
#include "pan_shader.h"
#include "pan_scoreboard.h"
#include "pan_texture.h"
#include "panfrost-quirks.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_math.h"

/* On Midgard, the native blit infrastructure (via MFBD preloads) is broken or
 * missing in many cases. We instead use software paths as fallbacks to
 * implement blits, which are done as TILER jobs. No vertex shader is
 * necessary since we can supply screen-space coordinates directly.
 *
 * This is primarily designed as a fallback for preloads but could be extended
 * for other clears/blits if needed in the future. */

static enum mali_bifrost_register_file_format
blit_type_to_reg_fmt(nir_alu_type in)
{
        switch (in) {
        case nir_type_float32:
                return MALI_BIFROST_REGISTER_FILE_FORMAT_F32;
        case nir_type_int32:
                return MALI_BIFROST_REGISTER_FILE_FORMAT_I32;
        case nir_type_uint32:
                return MALI_BIFROST_REGISTER_FILE_FORMAT_U32;
        default:
                unreachable("Invalid blit type");
        }
}

struct pan_blit_surface {
        gl_frag_result loc : 4;
        nir_alu_type type : 8;
        enum mali_texture_dimension dim : 2;
        bool array : 1;
        unsigned src_samples: 5;
        unsigned dst_samples: 5;
};

struct pan_blit_shader_key {
        struct pan_blit_surface surfaces[8];
};

struct pan_blit_shader_data {
        struct pan_blit_shader_key key;
        mali_ptr address;
        unsigned blend_ret_offsets[8];
        nir_alu_type blend_types[8];
};

struct pan_blit_blend_shader_key {
        enum pipe_format format;
        nir_alu_type type;
        unsigned rt : 3;
        unsigned nr_samples : 5;
};

struct pan_blit_blend_shader_data {
        struct pan_blit_blend_shader_key key;
        mali_ptr address;
};

struct pan_blit_rsd_key {
        struct {
                enum pipe_format format;
                nir_alu_type type : 8;
                unsigned src_samples : 5;
                unsigned dst_samples : 5;
                enum mali_texture_dimension dim : 2;
                bool array : 1;
        } rts[8], z, s;
};

struct pan_blit_rsd_data {
        struct pan_blit_rsd_key key;
        mali_ptr address;
};

static void
pan_blitter_prepare_midgard_rsd(const struct panfrost_device *dev,
                                const struct pan_image_view **rts,
                                mali_ptr *blend_shaders, bool zs,
                                struct MALI_RENDERER_STATE *rsd)
{
        mali_ptr blend_shader = blend_shaders ? blend_shaders[0] : 0;

        rsd->properties.midgard.work_register_count = 4;
        rsd->properties.midgard.force_early_z = !zs;
        rsd->stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;
        if (!(dev->quirks & MIDGARD_SFBD)) {
                rsd->sfbd_blend_shader = blend_shader;
                return;
        }

        rsd->stencil_mask_misc.sfbd_write_enable = true;
        rsd->stencil_mask_misc.sfbd_dither_disable = true;
        rsd->multisample_misc.sfbd_blend_shader = !!blend_shader;
        rsd->sfbd_blend_shader = blend_shader;
        if (rsd->multisample_misc.sfbd_blend_shader)
                return;

        rsd->sfbd_blend_equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
        rsd->sfbd_blend_equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
        rsd->sfbd_blend_equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
        rsd->sfbd_blend_equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
        rsd->sfbd_blend_equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
        rsd->sfbd_blend_equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
        rsd->sfbd_blend_constant = 0;

        if (rts && rts[0]) {
                rsd->stencil_mask_misc.sfbd_srgb =
                        util_format_is_srgb(rts[0]->format);
                rsd->sfbd_blend_equation.color_mask = 0xf;
        }
}

static void
pan_blitter_prepare_bifrost_rsd(const struct panfrost_device *dev,
                                bool zs, bool ms,
                                struct MALI_RENDERER_STATE *rsd)
{
        if (zs) {
                rsd->properties.bifrost.zs_update_operation =
                        MALI_PIXEL_KILL_FORCE_LATE;
                rsd->properties.bifrost.pixel_kill_operation =
                        MALI_PIXEL_KILL_FORCE_LATE;
        } else {
                rsd->properties.bifrost.zs_update_operation =
                        MALI_PIXEL_KILL_STRONG_EARLY;
                rsd->properties.bifrost.pixel_kill_operation =
                        MALI_PIXEL_KILL_FORCE_EARLY;
        }

        /* We can only allow blit shader fragments to kill if they write all
         * colour outputs. This is true for our colour (non-Z/S) blit shaders,
         * but obviously not true for Z/S shaders. However, blit shaders
         * otherwise lack side effects, so other fragments may kill them. */

        rsd->properties.bifrost.allow_forward_pixel_to_kill = !zs;
        rsd->properties.bifrost.allow_forward_pixel_to_be_killed = true;

        rsd->preload.fragment.coverage = true;
        rsd->preload.fragment.sample_mask_id = ms;
}

static void
pan_blitter_emit_midgard_blend(const struct panfrost_device *dev,
                               unsigned rt,
                               const struct pan_image_view *iview,
                               mali_ptr blend_shader,
                               void *out)
{
        assert(!(dev->quirks & MIDGARD_SFBD));

        pan_pack(out, BLEND, cfg) {
                if (!iview) {
                        cfg.midgard.equation.color_mask = 0xf;
                        cfg.midgard.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
                        cfg.midgard.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
                        cfg.midgard.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
                        cfg.midgard.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
                        cfg.midgard.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
                        cfg.midgard.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
                        continue;
                }

                cfg.round_to_fb_precision = true;
                cfg.srgb = util_format_is_srgb(iview->format);

                if (!blend_shader) {
                        cfg.midgard.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
                        cfg.midgard.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
                        cfg.midgard.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
                        cfg.midgard.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
                        cfg.midgard.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
                        cfg.midgard.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
                        cfg.midgard.equation.color_mask = 0xf;
                } else {
                        cfg.midgard.blend_shader = true;
                        cfg.midgard.shader_pc = blend_shader;
                }
        }
}

static void
pan_blitter_emit_bifrost_blend(const struct panfrost_device *dev,
                               unsigned rt,
                               const struct pan_image_view *iview,
                               const struct pan_blit_shader_data *blit_shader,
                               mali_ptr blend_shader,
                               void *out)
{
        pan_pack(out, BLEND, cfg) {
                if (!iview) {
                        cfg.enable = false;
                        cfg.bifrost.internal.mode = MALI_BIFROST_BLEND_MODE_OFF;
                        return;
                }

                nir_alu_type type = blit_shader->key.surfaces[rt].type;

                cfg.round_to_fb_precision = true;
                cfg.srgb = util_format_is_srgb(iview->format);
                cfg.bifrost.internal.mode = blend_shader ?
                                            MALI_BIFROST_BLEND_MODE_SHADER :
                                            MALI_BIFROST_BLEND_MODE_OPAQUE;
                if (blend_shader) {
                        cfg.bifrost.internal.shader.pc = blend_shader;
                        if (blit_shader->blend_ret_offsets[rt]) {
                                cfg.bifrost.internal.shader.return_value =
                                        blit_shader->address +
                                        blit_shader->blend_ret_offsets[rt];
                        }
                } else {
                        cfg.bifrost.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
                        cfg.bifrost.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
                        cfg.bifrost.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
                        cfg.bifrost.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
                        cfg.bifrost.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
                        cfg.bifrost.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
                        cfg.bifrost.equation.color_mask = 0xf;
                        cfg.bifrost.internal.fixed_function.num_comps = 4;
                        cfg.bifrost.internal.fixed_function.conversion.memory_format =
                                panfrost_format_to_bifrost_blend(dev, iview->format);
                        cfg.bifrost.internal.fixed_function.conversion.register_format =
                                blit_type_to_reg_fmt(type);

                        cfg.bifrost.internal.fixed_function.rt = rt;
                }
        }
}

static void
pan_blitter_emit_rsd(const struct panfrost_device *dev,
                     const struct pan_blit_shader_data *blit_shader,
                     unsigned rt_count,
                     const struct pan_image_view **rts,
                     mali_ptr *blend_shaders,
                     const struct pan_image_view *z,
                     const struct pan_image_view *s,
                     void *out)
{
        unsigned tex_count = 0;
        bool zs = (z || s);
        bool ms = false;

        for (unsigned i = 0; i < rt_count; i++) {
                if (rts[i]) {
                        tex_count++;
                        if (rts[i]->nr_samples > 1)
                                ms = true;
                }
        }

        if (z) {
                if (z->image->layout.nr_samples > 1)
                        ms = true;
                tex_count++;
        }

        if (s) {
                if (s->image->layout.nr_samples > 1)
                        ms = true;
                tex_count++;
        }

        pan_pack(out, RENDERER_STATE, cfg) {
                assert(blit_shader->address);
                cfg.shader.shader = blit_shader->address;
                cfg.shader.varying_count = 1;
                cfg.shader.texture_count = tex_count;
                cfg.shader.sampler_count = 1;

                cfg.properties.stencil_from_shader = s != NULL;
                cfg.properties.depth_source =
                        z ?
                        MALI_DEPTH_SOURCE_SHADER :
                        MALI_DEPTH_SOURCE_FIXED_FUNCTION;

                cfg.multisample_misc.sample_mask = 0xFFFF;
                cfg.multisample_misc.multisample_enable = ms;
                cfg.multisample_misc.evaluate_per_sample = ms;
                cfg.multisample_misc.depth_write_mask = z != NULL;
                cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;

                cfg.stencil_mask_misc.stencil_enable = s != NULL;
                cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
                cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
                cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
                cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
                cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
                cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
                cfg.stencil_front.mask = 0xFF;
                cfg.stencil_back = cfg.stencil_front;

                if (pan_is_bifrost(dev)) {
                        pan_blitter_prepare_bifrost_rsd(dev, zs, ms, &cfg);
                } else {
                        pan_blitter_prepare_midgard_rsd(dev, rts,
                                                        blend_shaders, zs,
                                                        &cfg);
                }
        }

        if (dev->quirks & MIDGARD_SFBD)
                return;

        for (unsigned i = 0; i < MAX2(rt_count, 1); ++i) {
                void *dest = out + MALI_RENDERER_STATE_LENGTH + MALI_BLEND_LENGTH * i;
                const struct pan_image_view *rt_view = rts ? rts[i] : NULL;
                mali_ptr blend_shader = blend_shaders ? blend_shaders[i] : 0;

                if (pan_is_bifrost(dev)) {
                        pan_blitter_emit_bifrost_blend(dev, i, rt_view, blit_shader,
                                                       blend_shader, dest);
                } else {
                        pan_blitter_emit_midgard_blend(dev, i, rt_view,
                                                       blend_shader, dest);
                }
        }
}

static void
pan_blitter_get_blend_shaders(struct panfrost_device *dev,
                              unsigned rt_count,
                              const struct pan_image_view **rts,
                              const struct pan_blit_shader_data *blit_shader,
                              mali_ptr *blend_shaders)
{
        if (!rt_count)
                return;

        struct pan_blend_state blend_state = {
                .rt_count = rt_count,
        };

        for (unsigned i = 0; i < rt_count; i++) {
                if (!rts[i] || panfrost_blendable_formats[rts[i]->format].internal)
                        continue;

                struct pan_blit_blend_shader_key key = {
                        .format = rts[i]->format,
                        .rt = i,
                        .nr_samples = rts[i]->image->layout.nr_samples,
                        .type = blit_shader->blend_types[i],
                };

                pthread_mutex_lock(&dev->blitter.shaders.lock);
                struct hash_entry *he =
                        _mesa_hash_table_search(dev->blitter.shaders.blend, &key);
                struct pan_blit_blend_shader_data *blend_shader = he ? he->data : NULL;
                if (blend_shader) {
                         blend_shaders[i] = blend_shader->address;
                         pthread_mutex_unlock(&dev->blitter.shaders.lock);
                         continue;
                }

                blend_shader = rzalloc(dev->blitter.shaders.blend,
                                       struct pan_blit_blend_shader_data);
                blend_shader->key = key;

                blend_state.rts[i] = (struct pan_blend_rt_state) {
                        .format = rts[i]->format,
                        .nr_samples = rts[i]->image->layout.nr_samples,
                        .equation = {
                                .blend_enable = true,
                                .rgb_src_factor = BLEND_FACTOR_ZERO,
                                .rgb_invert_src_factor = true,
                                .rgb_dst_factor = BLEND_FACTOR_ZERO,
                                .rgb_func = BLEND_FUNC_ADD,
                                .alpha_src_factor = BLEND_FACTOR_ZERO,
                                .alpha_invert_src_factor = true,
                                .alpha_dst_factor = BLEND_FACTOR_ZERO,
                                .alpha_func = BLEND_FUNC_ADD,
                                .color_mask = 0xf,
                        },
                };

                pthread_mutex_lock(&dev->blend_shaders.lock);
                struct pan_blend_shader_variant *b =
                        pan_blend_get_shader_locked(dev, &blend_state,
                                        blit_shader->blend_types[i],
                                        nir_type_float32, /* unused */
                                        i);

                assert(b->work_reg_count <= 4);
                struct panfrost_ptr bin =
                        panfrost_pool_alloc_aligned(&dev->blitter.shaders.pool,
                                                    b->binary.size,
                                                    pan_is_bifrost(dev) ? 128 : 64);
                memcpy(bin.cpu, b->binary.data, b->binary.size);

                blend_shader->address = bin.gpu | b->first_tag;
                pthread_mutex_unlock(&dev->blend_shaders.lock);
                _mesa_hash_table_insert(dev->blitter.shaders.blend,
                                        &blend_shader->key, blend_shader);
                pthread_mutex_unlock(&dev->blitter.shaders.lock);
                blend_shaders[i] = blend_shader->address;
        }
}

static const struct pan_blit_shader_data *
pan_blitter_get_blit_shader(struct panfrost_device *dev,
                            const struct pan_blit_shader_key *key)
{
        pthread_mutex_lock(&dev->blitter.shaders.lock);
        struct hash_entry *he = _mesa_hash_table_search(dev->blitter.shaders.blit, key);
        struct pan_blit_shader_data *shader = he ? he->data : NULL;

        if (shader)
                goto out;

        unsigned coord_comps = 0;
        unsigned sig_offset = 0;
        char sig[256];
        bool first = true;
        for (unsigned i = 0; i < ARRAY_SIZE(key->surfaces); i++) {
                const char *type_str, *dim_str;
                if (key->surfaces[i].type == nir_type_invalid)
                        continue;

                switch (key->surfaces[i].type) {
                case nir_type_float32: type_str = "float"; break;
                case nir_type_uint32: type_str = "uint"; break;
                case nir_type_int32: type_str = "int"; break;
                default: unreachable("Invalid type\n");
                }

                switch (key->surfaces[i].dim) {
                case MALI_TEXTURE_DIMENSION_CUBE: dim_str = "cube"; break;
                case MALI_TEXTURE_DIMENSION_1D: dim_str = "1D"; break;
                case MALI_TEXTURE_DIMENSION_2D: dim_str = "2D"; break;
                case MALI_TEXTURE_DIMENSION_3D: dim_str = "3D"; break;
                default: unreachable("Invalid dim\n");
                }

                coord_comps = MAX2(coord_comps,
                                   (key->surfaces[i].dim ? : 3) +
                                   (key->surfaces[i].array ? 1 : 0));
                first = false;

                if (sig_offset >= sizeof(sig))
                        continue;

                sig_offset += snprintf(sig + sig_offset, sizeof(sig) - sig_offset,
                                       "%s[%s;%s;%s%s;src_samples=%d,dst_samples=%d]",
                                       first ? "" : ",",
                                       gl_frag_result_name(key->surfaces[i].loc),
                                       type_str, dim_str,
                                       key->surfaces[i].array ? "[]" : "",
                                       key->surfaces[i].src_samples,
                                       key->surfaces[i].dst_samples);
        }

        nir_builder b =
                nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                               pan_shader_get_compiler_options(dev),
                                               "pan_blit(%s)", sig);
        b.shader->info.internal = true;

        nir_variable *coord_var =
                nir_variable_create(b.shader, nir_var_shader_in,
                                    glsl_vector_type(GLSL_TYPE_FLOAT, coord_comps),
                                    "coord");
        coord_var->data.location = VARYING_SLOT_TEX0;

        nir_ssa_def *coord = nir_load_var(&b, coord_var);

        unsigned active_count = 0;
        for (unsigned i = 0; i < ARRAY_SIZE(key->surfaces); i++) {
                if (key->surfaces[i].type == nir_type_invalid)
                        continue;

                /* Resolve operations only work for N -> 1 samples. */
                assert(key->surfaces[i].dst_samples == 1 ||
                       key->surfaces[i].src_samples == key->surfaces[i].dst_samples);

                static const char *out_names[] = {
                        "out0", "out1", "out2", "out3", "out4", "out5", "out6", "out7",
                };

                unsigned ncomps = key->surfaces[i].loc >= FRAG_RESULT_DATA0 ? 4 : 1;
                nir_variable *out =
                        nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vector_type(GLSL_TYPE_FLOAT, ncomps),
                                            out_names[active_count]);
                out->data.location = key->surfaces[i].loc;
                out->data.driver_location = active_count;

                bool resolve = key->surfaces[i].src_samples > key->surfaces[i].dst_samples;
                bool ms = key->surfaces[i].src_samples > 1;
                enum glsl_sampler_dim sampler_dim;

                switch (key->surfaces[i].dim) {
                case MALI_TEXTURE_DIMENSION_1D:
                        sampler_dim = GLSL_SAMPLER_DIM_1D;
                        break;
                case MALI_TEXTURE_DIMENSION_2D:
                        sampler_dim = ms ?
                                      GLSL_SAMPLER_DIM_MS :
                                      GLSL_SAMPLER_DIM_2D;
                        break;
                case MALI_TEXTURE_DIMENSION_3D:
                        sampler_dim = GLSL_SAMPLER_DIM_3D;
                        break;
                case MALI_TEXTURE_DIMENSION_CUBE:
                        sampler_dim = GLSL_SAMPLER_DIM_CUBE;
                        break;
                }

                nir_ssa_def *res = NULL;

                if (resolve) {
                        /* When resolving a float type, we need to calculate
                         * the average of all samples. For integer resolve, GL
                         * and Vulkan say that one sample should be chosen
                         * without telling which. Let's just pick the first one
                         * in that case.
                         */
                        nir_alu_type base_type =
                                nir_alu_type_get_base_type(key->surfaces[i].type);
                        unsigned nsamples = base_type == nir_type_float ?
                                            key->surfaces[i].src_samples : 1;

                        for (unsigned s = 0; s < nsamples; s++) {
                                nir_tex_instr *tex = nir_tex_instr_create(b.shader, 3);

                                tex->op = nir_texop_txf_ms;
                                tex->dest_type = key->surfaces[i].type;
                                tex->texture_index = active_count;
                                tex->is_array = key->surfaces[i].array;
                                tex->sampler_dim = sampler_dim;

                                tex->src[0].src_type = nir_tex_src_coord;
                                tex->src[0].src = nir_src_for_ssa(nir_f2i32(&b, coord));
                                tex->coord_components = coord_comps;

                                tex->src[1].src_type = nir_tex_src_ms_index;
                                tex->src[1].src = nir_src_for_ssa(nir_imm_int(&b, s));

                                tex->src[2].src_type = nir_tex_src_lod;
                                tex->src[2].src = nir_src_for_ssa(nir_imm_int(&b, 0));
                                nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32, NULL);
                                nir_builder_instr_insert(&b, &tex->instr);

                                res = res ? nir_fadd(&b, res, &tex->dest.ssa) : &tex->dest.ssa;
			}

                        if (base_type == nir_type_float) {
                                unsigned type_sz =
                                        nir_alu_type_get_type_size(key->surfaces[i].type);
                                res = nir_fmul(&b, res,
                                               nir_imm_floatN_t(&b, 1.0f / nsamples, type_sz));
                        }
                } else {
                        nir_tex_instr *tex =
                                nir_tex_instr_create(b.shader, ms ? 3 : 1);

                        tex->dest_type = key->surfaces[i].type;
                        tex->texture_index = active_count;
                        tex->is_array = key->surfaces[i].array;
                        tex->sampler_dim = sampler_dim;

                        if (ms) {
                                tex->op = nir_texop_txf_ms;

                                tex->src[0].src_type = nir_tex_src_coord;
                                tex->src[0].src = nir_src_for_ssa(nir_f2i32(&b, coord));
                                tex->coord_components = coord_comps;

                                tex->src[1].src_type = nir_tex_src_ms_index;
                                tex->src[1].src = nir_src_for_ssa(nir_load_sample_id(&b));

                                tex->src[2].src_type = nir_tex_src_lod;
                                tex->src[2].src = nir_src_for_ssa(nir_imm_int(&b, 0));
                        } else {
                                tex->op = nir_texop_tex;

                                tex->src[0].src_type = nir_tex_src_coord;
                                tex->src[0].src = nir_src_for_ssa(coord);
                                tex->coord_components = coord_comps;
                        }

                        nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32, NULL);
                        nir_builder_instr_insert(&b, &tex->instr);
                        res = &tex->dest.ssa;
                }

                assert(res);

                if (key->surfaces[i].loc >= FRAG_RESULT_DATA0) {
                        nir_store_var(&b, out, res, 0xFF);
                } else {
                        unsigned c = key->surfaces[i].loc == FRAG_RESULT_STENCIL ? 1 : 0;
                        nir_store_var(&b, out, nir_channel(&b, res, c), 0xFF);
                }
                active_count++;
        }

        struct panfrost_compile_inputs inputs = {
                .gpu_id = dev->gpu_id,
                .is_blit = true,
        };
        struct util_dynarray binary;
        struct pan_shader_info info;

        util_dynarray_init(&binary, NULL);

        pan_shader_compile(dev, b.shader, &inputs, &binary, &info);

        shader = rzalloc(dev->blitter.shaders.blit,
                         struct pan_blit_shader_data);
        shader->key = *key;
        shader->address =
                panfrost_pool_upload_aligned(&dev->blitter.shaders.pool,
                                             binary.data, binary.size,
                                             pan_is_bifrost(dev) ? 128 : 64);

        util_dynarray_fini(&binary);
        ralloc_free(b.shader);

        if (!pan_is_bifrost(dev))
                shader->address |= info.midgard.first_tag;

        if (pan_is_bifrost(dev)) {
                for (unsigned i = 0; i < ARRAY_SIZE(shader->blend_ret_offsets); i++) {
                        shader->blend_ret_offsets[i] = info.bifrost.blend[i].return_offset;
                        shader->blend_types[i] = info.bifrost.blend[i].type;
                }
        }

        _mesa_hash_table_insert(dev->blitter.shaders.blit, &shader->key, shader);

out:
        pthread_mutex_unlock(&dev->blitter.shaders.lock);
        return shader;
}

static mali_ptr
pan_blitter_get_rsd(struct panfrost_device *dev,
                    unsigned rt_count,
                    const struct pan_image_view **src_rts,
                    const struct pan_image_view **dst_rts,
                    const struct pan_image_view *src_z,
                    const struct pan_image_view *dst_z,
                    const struct pan_image_view *src_s,
                    const struct pan_image_view *dst_s)
{
        struct pan_blit_rsd_key rsd_key = { 0 };

        assert(!rt_count || (!src_z && !src_s));

        struct pan_blit_shader_key blit_key = { 0 };

        if (src_z) {
                assert(dst_z);
                rsd_key.z.format = dst_z->format;
                blit_key.surfaces[0].loc = FRAG_RESULT_DEPTH;
                rsd_key.z.type = blit_key.surfaces[0].type = nir_type_float32;
                rsd_key.z.src_samples = blit_key.surfaces[0].src_samples = src_z->image->layout.nr_samples;
                rsd_key.z.dst_samples = blit_key.surfaces[0].dst_samples = dst_z->image->layout.nr_samples;
                rsd_key.z.dim = blit_key.surfaces[0].dim = src_z->dim;
                rsd_key.z.array = blit_key.surfaces[0].array = src_z->first_layer != src_z->last_layer;
        }

        if (src_s) {
                assert(dst_s);
                rsd_key.s.format = dst_s->format;
                blit_key.surfaces[1].loc = FRAG_RESULT_STENCIL;
                rsd_key.s.type = blit_key.surfaces[1].type = nir_type_uint32;
                rsd_key.s.src_samples = blit_key.surfaces[1].src_samples = src_s->image->layout.nr_samples;
                rsd_key.s.dst_samples = blit_key.surfaces[1].dst_samples = dst_s->image->layout.nr_samples;
                rsd_key.s.dim = blit_key.surfaces[1].dim = src_s->dim;
                rsd_key.s.array = blit_key.surfaces[1].array = src_s->first_layer != src_s->last_layer;
        }

        for (unsigned i = 0; i < rt_count; i++) {
                if (!src_rts[i])
                        continue;

                assert(dst_rts[i]);
                rsd_key.rts[i].format = dst_rts[i]->format;
                blit_key.surfaces[i].loc = FRAG_RESULT_DATA0 + i;
                rsd_key.rts[i].type = blit_key.surfaces[i].type =
                        util_format_is_pure_uint(src_rts[i]->format) ? nir_type_uint32 :
                        util_format_is_pure_sint(src_rts[i]->format) ? nir_type_int32 :
                        nir_type_float32;
                rsd_key.rts[i].src_samples = blit_key.surfaces[i].src_samples = src_rts[i]->image->layout.nr_samples;
                rsd_key.rts[i].dst_samples = blit_key.surfaces[i].dst_samples = dst_rts[i]->image->layout.nr_samples;
                rsd_key.rts[i].dim = blit_key.surfaces[i].dim = src_rts[i]->dim;
                rsd_key.rts[i].array = blit_key.surfaces[i].array = src_rts[i]->first_layer != src_rts[i]->last_layer;
        }

        pthread_mutex_lock(&dev->blitter.rsds.lock);
        struct hash_entry *he =
                _mesa_hash_table_search(dev->blitter.rsds.rsds, &rsd_key);
        struct pan_blit_rsd_data *rsd = he ? he->data : NULL;
        if (rsd)
                goto out;

        rsd = rzalloc(dev->blitter.rsds.rsds, struct pan_blit_rsd_data);
        rsd->key = rsd_key;

        struct panfrost_ptr rsd_ptr =
                (dev->quirks & MIDGARD_SFBD) ?
                panfrost_pool_alloc_desc(&dev->blitter.rsds.pool, RENDERER_STATE) :
                panfrost_pool_alloc_desc_aggregate(&dev->blitter.rsds.pool,
                                                   PAN_DESC(RENDERER_STATE),
                                                   PAN_DESC_ARRAY(MAX2(rt_count, 1), BLEND));

        mali_ptr blend_shaders[8] = { 0 };

        const struct pan_blit_shader_data *blit_shader =
                pan_blitter_get_blit_shader(dev, &blit_key);

        pan_blitter_get_blend_shaders(dev, rt_count, dst_rts,
                                      blit_shader, blend_shaders);

        pan_blitter_emit_rsd(dev, blit_shader,
                             MAX2(rt_count, 1), dst_rts, blend_shaders,
                             dst_z, dst_s, rsd_ptr.cpu);
        rsd->address = rsd_ptr.gpu;
        _mesa_hash_table_insert(dev->blitter.rsds.rsds, &rsd->key, rsd);

out:
        pthread_mutex_unlock(&dev->blitter.rsds.lock);
        return rsd->address;
}

static mali_ptr
pan_preload_get_rsd(struct panfrost_device *dev,
                    const struct pan_fb_info *fb,
                    bool zs)
{
        const struct pan_image_view *rts[8] = { NULL };
        const struct pan_image_view *z = NULL, *s = NULL;
        struct pan_image_view patched_s_view;
        unsigned rt_count = 0;

        if (zs) {
                if (fb->zs.preload.z)
                        z = fb->zs.view.zs;

                if (fb->zs.preload.s) {
                        const struct pan_image_view *view = fb->zs.view.s ? : fb->zs.view.zs;
                        enum pipe_format fmt = util_format_get_depth_only(view->format);

                        switch (view->format) {
                        case PIPE_FORMAT_Z24_UNORM_S8_UINT: fmt = PIPE_FORMAT_X24S8_UINT; break;
                        case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT: fmt = PIPE_FORMAT_X32_S8X24_UINT; break;
                        default: fmt = view->format; break;
                        }

                        if (fmt != view->format) {
                                patched_s_view = *view;
                                patched_s_view.format = fmt;
                                s = &patched_s_view;
                        } else {
                                s = view;
                        }
                }
        } else {
                for (unsigned i = 0; i < fb->rt_count; i++) {
                        if (fb->rts[i].preload)
                                rts[i] = fb->rts[i].view;
                }

                rt_count = fb->rt_count;
        }

        return pan_blitter_get_rsd(dev, rt_count, rts, rts, z, z, s, s);
}

static mali_ptr
pan_blit_get_rsd(struct panfrost_device *dev,
                 const struct pan_image_view *src_views,
                 const struct pan_image_view *dst_view)
{
        const struct util_format_description *desc =
                util_format_description(src_views[0].format);
        const struct pan_image_view *src_rt = NULL, *dst_rt = NULL;
        const struct pan_image_view *src_z = NULL, *dst_z = NULL;
        const struct pan_image_view *src_s = NULL, *dst_s = NULL;

        if (util_format_has_depth(desc)) {
                src_z = &src_views[0];
                dst_z = dst_view;
        }

        if (src_views[1].format) {
                src_s = &src_views[1];
                dst_s = dst_view;
        } else if (util_format_has_stencil(desc)) {
                src_s = &src_views[0];
                dst_s = dst_view;
        }

        if (!src_z && !src_s) {
                src_rt = &src_views[0];
                dst_rt = dst_view;
        }

        return pan_blitter_get_rsd(dev, src_rt ? 1 : 0, &src_rt, &dst_rt,
                                   src_z, dst_z, src_s, dst_s);
}

static bool
pan_preload_needed(const struct pan_fb_info *fb, bool zs)
{
        if (zs) {
                if (fb->zs.preload.z || fb->zs.preload.s)
                        return true;
        } else {
                for (unsigned i = 0; i < fb->rt_count; i++) {
                        if (fb->rts[i].preload)
                                return true;
                }
        }

        return false;
}

static void
pan_blitter_emit_varying(struct pan_pool *pool,
                         mali_ptr coordinates,
                         struct MALI_DRAW *draw)
{
        /* Bifrost needs an empty desc to mark end of prefetching */
        bool padding_buffer = pan_is_bifrost(pool->dev);

        struct panfrost_ptr varying =
                panfrost_pool_alloc_desc(pool, ATTRIBUTE);
        struct panfrost_ptr varying_buffer =
                panfrost_pool_alloc_desc_array(pool, (padding_buffer ? 2 : 1),
                                               ATTRIBUTE_BUFFER);

        pan_pack(varying_buffer.cpu, ATTRIBUTE_BUFFER, cfg) {
                cfg.pointer = coordinates;
                cfg.stride = 4 * sizeof(float);
                cfg.size = cfg.stride * 4;
        }

        if (padding_buffer) {
                pan_pack(varying_buffer.cpu + MALI_ATTRIBUTE_BUFFER_LENGTH,
                         ATTRIBUTE_BUFFER, cfg);
        }

        pan_pack(varying.cpu, ATTRIBUTE, cfg) {
                cfg.buffer_index = 0;
                cfg.offset_enable = !pan_is_bifrost(pool->dev);
                cfg.format = pool->dev->formats[PIPE_FORMAT_R32G32B32_FLOAT].hw;
        }

        draw->varyings = varying.gpu;
        draw->varying_buffers = varying_buffer.gpu;
}

static mali_ptr
pan_blitter_emit_bifrost_sampler(struct pan_pool *pool,
                                 bool nearest_filter)
{
        struct panfrost_ptr sampler =
                 panfrost_pool_alloc_desc(pool, BIFROST_SAMPLER);

        pan_pack(sampler.cpu, BIFROST_SAMPLER, cfg) {
                cfg.seamless_cube_map = false;
                cfg.normalized_coordinates = false;
                cfg.point_sample_minify = nearest_filter;
                cfg.point_sample_magnify = nearest_filter;
        }

        return sampler.gpu;
}

static mali_ptr
pan_blitter_emit_midgard_sampler(struct pan_pool *pool,
                                 bool nearest_filter)
{
        struct panfrost_ptr sampler =
                 panfrost_pool_alloc_desc(pool, MIDGARD_SAMPLER);

        pan_pack(sampler.cpu, MIDGARD_SAMPLER, cfg) {
                cfg.normalized_coordinates = false;
                cfg.magnify_nearest = nearest_filter;
                cfg.minify_nearest = nearest_filter;
        }

        return sampler.gpu;
}

static mali_ptr
pan_blitter_emit_bifrost_textures(struct pan_pool *pool,
                                  unsigned tex_count,
                                  const struct pan_image_view **views)
{
        struct panfrost_ptr textures =
                panfrost_pool_alloc_desc_array(pool, tex_count, BIFROST_TEXTURE);

        for (unsigned i = 0; i < tex_count; i++) {
                void *texture = textures.cpu + (MALI_BIFROST_TEXTURE_LENGTH * i);
                size_t payload_size =
                        panfrost_estimate_texture_payload_size(pool->dev, views[i]);
                struct panfrost_ptr surfaces =
                        panfrost_pool_alloc_aligned(pool, payload_size,
                                                    MALI_SURFACE_WITH_STRIDE_ALIGN);

                panfrost_new_texture(pool->dev, views[i], texture, &surfaces);
        }

        return textures.gpu;
}

static mali_ptr
pan_blitter_emit_midgard_textures(struct pan_pool *pool,
                                  unsigned tex_count,
                                  const struct pan_image_view **views)
{
        mali_ptr textures[8] = { 0 };

        for (unsigned i = 0; i < tex_count; i++) {
                size_t sz = MALI_MIDGARD_TEXTURE_LENGTH +
                            panfrost_estimate_texture_payload_size(pool->dev, views[i]);
                struct panfrost_ptr texture =
                        panfrost_pool_alloc_aligned(pool, sz, MALI_MIDGARD_TEXTURE_ALIGN);
                struct panfrost_ptr surfaces = {
                        .cpu = texture.cpu + MALI_MIDGARD_TEXTURE_LENGTH,
                        .gpu = texture.gpu + MALI_MIDGARD_TEXTURE_LENGTH,
                };

                panfrost_new_texture(pool->dev, views[i], texture.cpu, &surfaces);
                textures[i] = texture.gpu;
        }

        return panfrost_pool_upload_aligned(pool, textures,
                                            tex_count * sizeof(mali_ptr),
                                            sizeof(mali_ptr));
}

static void
pan_preload_emit_textures(struct pan_pool *pool,
                          const struct pan_fb_info *fb, bool zs,
                          struct MALI_DRAW *draw)
{
        const struct pan_image_view *views[8];
        struct pan_image_view patched_s_view;
        unsigned tex_count = 0;

        if (zs) {
                if (fb->zs.preload.z)
                        views[tex_count++] = fb->zs.view.zs;

                if (fb->zs.preload.s) {
                        const struct pan_image_view *view = fb->zs.view.s ? : fb->zs.view.zs;
                        enum pipe_format fmt = util_format_get_depth_only(view->format);

                        switch (view->format) {
                        case PIPE_FORMAT_Z24_UNORM_S8_UINT: fmt = PIPE_FORMAT_X24S8_UINT; break;
                        case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT: fmt = PIPE_FORMAT_X32_S8X24_UINT; break;
                        default: fmt = view->format; break;
                        }

                        if (fmt != view->format) {
                                patched_s_view = *view;
                                patched_s_view.format = fmt;
                                view = &patched_s_view;
                        }
                        views[tex_count++] = view;
                }
        } else {
                for (unsigned i = 0; i < fb->rt_count; i++) {
                        if (fb->rts[i].preload)
                                views[tex_count++] = fb->rts[i].view;
                }

        }

        if (pan_is_bifrost(pool->dev))
                draw->textures = pan_blitter_emit_bifrost_textures(pool, tex_count, views);
        else
                draw->textures = pan_blitter_emit_midgard_textures(pool, tex_count, views);
}

static mali_ptr
pan_blitter_emit_viewport(struct pan_pool *pool,
                          uint16_t minx, uint16_t miny,
                          uint16_t maxx, uint16_t maxy)
{
        struct panfrost_ptr vp = panfrost_pool_alloc_desc(pool, VIEWPORT);

        pan_pack(vp.cpu, VIEWPORT, cfg) {
                cfg.scissor_minimum_x = minx;
                cfg.scissor_minimum_y = miny;
                cfg.scissor_maximum_x = maxx;
                cfg.scissor_maximum_y = maxy;
        }

        return vp.gpu;
}

static void
pan_preload_emit_dcd(struct pan_pool *pool,
                     struct pan_fb_info *fb, bool zs,
                     mali_ptr coordinates,
                     mali_ptr tsd, mali_ptr rsd,
                     void *out, bool always_write)
{
        pan_pack(out, DRAW, cfg) {
                cfg.four_components_per_vertex = true;
                cfg.draw_descriptor_is_64b = true;
                cfg.thread_storage = tsd;
                cfg.state = rsd;

                cfg.position = coordinates;
                pan_blitter_emit_varying(pool, coordinates, &cfg);
                uint16_t minx = 0, miny = 0, maxx, maxy;
                if (pool->dev->quirks & MIDGARD_SFBD) {
                        maxx = fb->width - 1;
                        maxy = fb->height - 1;
                } else {
                        /* Align on 32x32 tiles */
                        minx = fb->extent.minx & ~31;
                        miny = fb->extent.miny & ~31;
                        maxx = MIN2(ALIGN_POT(fb->extent.maxx + 1, 32), fb->width) - 1;
                        maxy = MIN2(ALIGN_POT(fb->extent.maxy + 1, 32), fb->height) - 1;
                }

                cfg.viewport =
                        pan_blitter_emit_viewport(pool, minx, miny, maxx, maxy);

                pan_preload_emit_textures(pool, fb, zs, &cfg);

                if (pan_is_bifrost(pool->dev)) {
                        cfg.samplers = pan_blitter_emit_bifrost_sampler(pool, true);

                        /* Tiles updated by blit shaders are still considered
                         * clean (separate for colour and Z/S), allowing us to
                         * suppress unnecessary writeback */
                        cfg.clean_fragment_write = !always_write;
                } else {
                        cfg.samplers = pan_blitter_emit_midgard_sampler(pool, true);
                        cfg.texture_descriptor_is_64b = true;
                }
        }
}

static void
pan_blit_emit_dcd(struct pan_pool *pool,
                  mali_ptr src_coords, mali_ptr dst_coords,
                  mali_ptr textures, mali_ptr samplers,
                  mali_ptr vpd, mali_ptr tsd, mali_ptr rsd,
                  void *out)
{
        pan_pack(out, DRAW, cfg) {
                cfg.four_components_per_vertex = true;
                cfg.draw_descriptor_is_64b = true;
                cfg.thread_storage = tsd;
                cfg.state = rsd;

                cfg.position = dst_coords;
                pan_blitter_emit_varying(pool, src_coords, &cfg);
                cfg.viewport = vpd;
                cfg.texture_descriptor_is_64b = !pan_is_bifrost(pool->dev);
                cfg.textures = textures;
                cfg.samplers = samplers;
        }
}

static void
pan_preload_fb_bifrost_alloc_pre_post_dcds(struct pan_pool *desc_pool,
                                           struct pan_fb_info *fb)
{
        assert(pan_is_bifrost(desc_pool->dev));

        if (fb->bifrost.pre_post.dcds.gpu)
                return;

        fb->bifrost.pre_post.dcds =
                panfrost_pool_alloc_desc_aggregate(desc_pool,
                                                   PAN_DESC(DRAW),
                                                   PAN_DESC(DRAW_PADDING),
                                                   PAN_DESC(DRAW),
                                                   PAN_DESC(DRAW_PADDING),
                                                   PAN_DESC(DRAW),
                                                   PAN_DESC(DRAW_PADDING));
}

static void
pan_preload_emit_midgard_tiler_job(struct pan_pool *desc_pool,
                                   struct pan_scoreboard *scoreboard,
                                   struct pan_fb_info *fb, bool zs,
                                   mali_ptr coords, mali_ptr rsd, mali_ptr tsd)
{
        struct panfrost_ptr job =
                panfrost_pool_alloc_desc(desc_pool, MIDGARD_TILER_JOB);

        pan_preload_emit_dcd(desc_pool, fb, zs, coords, tsd, rsd,
                             pan_section_ptr(job.cpu, MIDGARD_TILER_JOB, DRAW),
                             false);

        pan_section_pack(job.cpu, MIDGARD_TILER_JOB, PRIMITIVE, cfg) {
                cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
                cfg.index_count = 4;
                cfg.job_task_split = 6;
        }

        pan_section_pack(job.cpu, MIDGARD_TILER_JOB, PRIMITIVE_SIZE, cfg) {
                cfg.constant = 1.0f;
        }

        void *invoc = pan_section_ptr(job.cpu,
                                      MIDGARD_TILER_JOB,
                                      INVOCATION);
        panfrost_pack_work_groups_compute(invoc, 1, 4,
                                          1, 1, 1, 1, true, false);

        panfrost_add_job(desc_pool, scoreboard, MALI_JOB_TYPE_TILER,
                         false, false, 0, 0, &job, true);
}

static void
pan_blit_emit_midgard_tiler_job(struct pan_pool *desc_pool,
                                struct pan_scoreboard *scoreboard,
                                mali_ptr src_coords, mali_ptr dst_coords,
                                mali_ptr textures, mali_ptr samplers,
                                mali_ptr vpd, mali_ptr rsd, mali_ptr tsd)
{
        struct panfrost_ptr job =
                panfrost_pool_alloc_desc(desc_pool, MIDGARD_TILER_JOB);

        pan_blit_emit_dcd(desc_pool,
                          src_coords, dst_coords, textures, samplers,
                          vpd, tsd, rsd,
                          pan_section_ptr(job.cpu, MIDGARD_TILER_JOB, DRAW));

        pan_section_pack(job.cpu, MIDGARD_TILER_JOB, PRIMITIVE, cfg) {
                cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
                cfg.index_count = 4;
                cfg.job_task_split = 6;
        }

        pan_section_pack(job.cpu, MIDGARD_TILER_JOB, PRIMITIVE_SIZE, cfg) {
                cfg.constant = 1.0f;
        }

        void *invoc = pan_section_ptr(job.cpu,
                                      MIDGARD_TILER_JOB,
                                      INVOCATION);
        panfrost_pack_work_groups_compute(invoc, 1, 4,
                                          1, 1, 1, 1, true, false);

        panfrost_add_job(desc_pool, scoreboard, MALI_JOB_TYPE_TILER,
                         false, false, 0, 0, &job, false);
}

static void
pan_blit_emit_bifrost_tiler_job(struct pan_pool *desc_pool,
                                struct pan_scoreboard *scoreboard,
                                mali_ptr src_coords, mali_ptr dst_coords,
                                mali_ptr textures, mali_ptr samplers,
                                mali_ptr vpd, mali_ptr rsd,
                                mali_ptr tsd, mali_ptr tiler)
{
        struct panfrost_ptr job =
                panfrost_pool_alloc_desc(desc_pool, BIFROST_TILER_JOB);

        pan_blit_emit_dcd(desc_pool,
                          src_coords, dst_coords, textures, samplers,
                          vpd, tsd, rsd,
                          pan_section_ptr(job.cpu, BIFROST_TILER_JOB, DRAW));

        pan_section_pack(job.cpu, BIFROST_TILER_JOB, PRIMITIVE, cfg) {
                cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
                cfg.index_count = 4;
                cfg.job_task_split = 6;
        }

        pan_section_pack(job.cpu, BIFROST_TILER_JOB, PRIMITIVE_SIZE, cfg) {
                cfg.constant = 1.0f;
        }

        void *invoc = pan_section_ptr(job.cpu,
                                      BIFROST_TILER_JOB,
                                      INVOCATION);
        panfrost_pack_work_groups_compute(invoc, 1, 4,
                                          1, 1, 1, 1, true, false);

        pan_section_pack(job.cpu, BIFROST_TILER_JOB, PADDING, cfg);
        pan_section_pack(job.cpu, BIFROST_TILER_JOB, TILER, cfg) {
                cfg.address = tiler;
        }

        panfrost_add_job(desc_pool, scoreboard, MALI_JOB_TYPE_TILER,
                         false, false, 0, 0, &job, false);
}

static void
pan_preload_emit_bifrost_pre_frame_dcd(struct pan_pool *desc_pool,
                                       struct pan_fb_info *fb, bool zs,
                                       mali_ptr coords, mali_ptr rsd,
                                       mali_ptr tsd)
{
        struct panfrost_device *dev = desc_pool->dev;

        unsigned dcd_idx = zs ? 0 : 1;
        pan_preload_fb_bifrost_alloc_pre_post_dcds(desc_pool, fb);
        assert(fb->bifrost.pre_post.dcds.cpu);
        void *dcd = fb->bifrost.pre_post.dcds.cpu +
                    (dcd_idx * (MALI_DRAW_LENGTH + MALI_DRAW_PADDING_LENGTH));

        int crc_rt = pan_select_crc_rt(dev, fb);

        bool always_write = false;

        /* If CRC data is currently invalid and this batch will make it valid,
         * write even clean tiles to make sure CRC data is updated. */
        if (crc_rt >= 0) {
                bool *valid = fb->rts[crc_rt].crc_valid;
                bool full = !fb->extent.minx && !fb->extent.miny &&
                        fb->extent.maxx == (fb->width - 1) &&
                        fb->extent.maxy == (fb->height - 1);

                if (full && !(*valid))
                        always_write = true;
        }

        pan_preload_emit_dcd(desc_pool, fb, zs, coords, tsd, rsd, dcd, always_write);
        if (zs) {
                enum pipe_format fmt = fb->zs.view.zs->image->layout.format;
                bool always = false;

                /* If we're dealing with a combined ZS resource and only one
                 * component is cleared, we need to reload the whole surface
                 * because the zs_clean_pixel_write_enable flag is set in that
                 * case.
                 */
                if (util_format_is_depth_and_stencil(fmt) &&
                    fb->zs.clear.z != fb->zs.clear.s)
                        always = true;

                /* We could use INTERSECT on Bifrost v7 too, but
                 * EARLY_ZS_ALWAYS has the advantage of reloading the ZS tile
                 * buffer one or more tiles ahead, making ZS data immediately
                 * available for any ZS tests taking place in other shaders.
                 * Thing's haven't been benchmarked to determine what's
                 * preferable (saving bandwidth vs having ZS preloaded
                 * earlier), so let's leave it like that for now.
                 */
                fb->bifrost.pre_post.modes[dcd_idx] =
                        desc_pool->dev->arch > 6 ?
                        MALI_PRE_POST_FRAME_SHADER_MODE_EARLY_ZS_ALWAYS :
                        always ? MALI_PRE_POST_FRAME_SHADER_MODE_ALWAYS :
                        MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT;
        } else {
                fb->bifrost.pre_post.modes[dcd_idx] =
                        always_write ? MALI_PRE_POST_FRAME_SHADER_MODE_ALWAYS :
                        MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT;
        }
}

static void
pan_preload_fb_part(struct pan_pool *pool,
                    struct pan_scoreboard *scoreboard,
                    struct pan_fb_info *fb, bool zs,
                    mali_ptr coords, mali_ptr tsd, mali_ptr tiler)
{
        struct panfrost_device *dev = pool->dev;
        mali_ptr rsd = pan_preload_get_rsd(dev, fb, zs);

        if (pan_is_bifrost(dev)) {
                pan_preload_emit_bifrost_pre_frame_dcd(pool, fb, zs,
                                                       coords, rsd, tsd);
        } else {
                pan_preload_emit_midgard_tiler_job(pool, scoreboard,
                                                   fb, zs, coords, rsd, tsd);
        }
}

void
pan_preload_fb(struct pan_pool *pool,
               struct pan_scoreboard *scoreboard,
               struct pan_fb_info *fb,
               mali_ptr tsd, mali_ptr tiler)
{
        bool preload_zs = pan_preload_needed(fb, true);
        bool preload_rts = pan_preload_needed(fb, false);
        mali_ptr coords;

        if (!preload_zs && !preload_rts)
                return;

        float rect[] = {
                0.0, 0.0, 0.0, 1.0,
                fb->width, 0.0, 0.0, 1.0,
                0.0, fb->height, 0.0, 1.0,
                fb->width, fb->height, 0.0, 1.0,
        };

        coords = panfrost_pool_upload_aligned(pool, rect,
                                              sizeof(rect), 64);

        if (preload_zs)
                pan_preload_fb_part(pool, scoreboard, fb, true, coords,
                                    tsd, tiler);

        if (preload_rts)
                pan_preload_fb_part(pool, scoreboard, fb, false, coords,
                                    tsd, tiler);
}

void
pan_blit_ctx_init(struct panfrost_device *dev,
                  const struct pan_blit_info *info,
                  struct pan_blit_context *ctx)
{
        memset(ctx, 0, sizeof(*ctx));
        panfrost_pool_init(&ctx->pool, NULL, dev, 0, 65536, "Blitter pool", false, true);

        ctx->z_scale = (float)(info->dst.end.z - info->dst.start.z + 1) /
                       (info->src.end.z - info->src.start.z + 1);

        struct pan_image_view sviews[2] = {
                {
                        .format = info->src.planes[0].format,
                        .image = info->src.planes[0].image,
                        .dim = info->src.planes[0].image->layout.dim == MALI_TEXTURE_DIMENSION_CUBE ?
                               MALI_TEXTURE_DIMENSION_2D : info->src.planes[0].image->layout.dim,
                        .first_level = info->src.level,
                        .last_level = info->src.level,
                        .first_layer = info->src.start.layer,
                        .last_layer = info->src.end.layer,
                        .swizzle = {
                                PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
                                PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W,
                        },
                },
        };

        struct pan_image_view dview = {
                .format = info->dst.planes[0].format,
                .image = info->dst.planes[0].image,
                .dim = info->dst.planes[0].image->layout.dim == MALI_TEXTURE_DIMENSION_1D ?
                       MALI_TEXTURE_DIMENSION_1D : MALI_TEXTURE_DIMENSION_2D,
                .first_level = info->dst.level,
                .last_level = info->dst.level,
                .first_layer = info->dst.start.layer,
                .last_layer = info->dst.start.layer,
                .swizzle = {
                        PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
                        PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W,
                },
        };

        ctx->src.start.x = info->src.start.x;
        ctx->src.start.y = info->src.start.y;
        ctx->src.end.x = info->src.end.x;
        ctx->src.end.y = info->src.end.y;
        ctx->src.dim = sviews[0].dim;
        if (sviews[0].dim == MALI_TEXTURE_DIMENSION_3D)
                ctx->src.z_offset = info->src.start.z;
        else
                ctx->src.layer_offset = info->src.start.layer;

        if (info->dst.planes[0].image->layout.dim == MALI_TEXTURE_DIMENSION_3D) {
                ctx->dst.layer_offset = info->dst.start.z;
                ctx->dst.cur_layer = info->dst.start.z;
                ctx->dst.last_layer = info->dst.end.z;
        } else {
                ctx->dst.layer_offset = info->dst.start.layer;
                ctx->dst.cur_layer = info->dst.start.layer;
                ctx->dst.last_layer = info->dst.end.layer;
        }

        /* Split depth and stencil */
        if (util_format_is_depth_and_stencil(sviews[0].format)) {
                sviews[1] = sviews[0];
                sviews[0].format = util_format_get_depth_only(sviews[0].format);
                sviews[1].format = util_format_stencil_only(sviews[1].format);
        } else if (info->src.planes[1].format) {
                sviews[1] = sviews[0];
                sviews[1].format = info->src.planes[1].format;
                sviews[1].image = info->src.planes[1].image;
        }

        ctx->rsd = pan_blit_get_rsd(dev, sviews, &dview);

        ASSERTED unsigned nlayers = info->src.end.layer - info->src.start.layer + 1;

        assert(nlayers == (info->dst.end.layer - info->dst.start.layer + 1));

        unsigned dst_w = u_minify(info->dst.planes[0].image->layout.width, info->dst.level);
        unsigned dst_h = u_minify(info->dst.planes[0].image->layout.height, info->dst.level);
        unsigned minx = MAX2(info->dst.start.x, 0);
        unsigned miny = MAX2(info->dst.start.y, 0);
        unsigned maxx = MIN2(info->dst.end.x, dst_w - 1);
        unsigned maxy = MIN2(info->dst.end.y, dst_h - 1);

        if (info->scissor.enable) {
                minx = MAX2(minx, info->scissor.minx);
                miny = MAX2(miny, info->scissor.miny);
                maxx = MIN2(maxx, info->scissor.maxx);
                maxy = MIN2(maxy, info->scissor.maxy);
        }

        const struct pan_image_view *sview_ptrs[] = { &sviews[0], &sviews[1] };
        unsigned nviews = sviews[1].format ? 2 : 1;

        if (pan_is_bifrost(dev)) {
                ctx->textures =
                        pan_blitter_emit_bifrost_textures(&ctx->pool, nviews, sview_ptrs);
                ctx->samplers =
                        pan_blitter_emit_bifrost_sampler(&ctx->pool, info->nearest);
        } else {
                ctx->textures =
                        pan_blitter_emit_midgard_textures(&ctx->pool, nviews, sview_ptrs);
                ctx->samplers =
                        pan_blitter_emit_midgard_sampler(&ctx->pool, info->nearest);
        }

        ctx->vpd = pan_blitter_emit_viewport(&ctx->pool,
                                             minx, miny, maxx, maxy);

        float dst_rect[] = {
                info->dst.start.x, info->dst.start.y, 0.0, 1.0,
                info->dst.end.x + 1, info->dst.start.y, 0.0, 1.0,
                info->dst.start.x, info->dst.end.y + 1, 0.0, 1.0,
                info->dst.end.x + 1, info->dst.end.y + 1, 0.0, 1.0,
        };

        ctx->position =
                panfrost_pool_upload_aligned(&ctx->pool, dst_rect,
                                             sizeof(dst_rect), 64);
}

void
pan_blit_ctx_cleanup(struct pan_blit_context *ctx)
{
        panfrost_pool_cleanup(&ctx->pool);
}

bool
pan_blit_next_surface(struct pan_blit_context *ctx)
{
        if (ctx->dst.cur_layer >= ctx->dst.last_layer)
                return false;

        ctx->dst.cur_layer++;
        return true;
}

void
pan_blit(struct pan_blit_context *ctx,
         struct pan_pool *pool,
         struct pan_scoreboard *scoreboard,
         mali_ptr tsd, mali_ptr tiler)
{
        if (ctx->dst.cur_layer < 0 || ctx->dst.cur_layer > ctx->dst.last_layer)
                return;

        int32_t layer = ctx->dst.cur_layer - ctx->dst.layer_offset;
        float src_z;
        if (ctx->src.dim == MALI_TEXTURE_DIMENSION_3D)
                src_z = (ctx->z_scale * layer) + ctx->src.z_offset;
        else
                src_z = ctx->src.layer_offset + layer;

        float src_rect[] = {
                ctx->src.start.x, ctx->src.start.y, src_z, 1.0,
                ctx->src.end.x + 1, ctx->src.start.y, src_z, 1.0,
                ctx->src.start.x, ctx->src.end.y + 1, src_z, 1.0,
                ctx->src.end.x + 1, ctx->src.end.y + 1, src_z, 1.0,
        };

        mali_ptr src_coords =
                panfrost_pool_upload_aligned(pool, src_rect,
                                             sizeof(src_rect), 64);

        if (pan_is_bifrost(pool->dev)) {
                pan_blit_emit_bifrost_tiler_job(pool, scoreboard,
                                                src_coords, ctx->position,
                                                ctx->textures, ctx->samplers,
                                                ctx->vpd, ctx->rsd, tsd, tiler);
        } else {
                pan_blit_emit_midgard_tiler_job(pool, scoreboard,
                                                src_coords, ctx->position,
                                                ctx->textures, ctx->samplers,
                                                ctx->vpd, ctx->rsd, tsd);
        }
}

static uint32_t pan_blit_shader_key_hash(const void *key)
{
        return _mesa_hash_data(key, sizeof(struct pan_blit_shader_key));
}

static bool pan_blit_shader_key_equal(const void *a, const void *b)
{
        return !memcmp(a, b, sizeof(struct pan_blit_shader_key));
}

static uint32_t pan_blit_blend_shader_key_hash(const void *key)
{
        return _mesa_hash_data(key, sizeof(struct pan_blit_blend_shader_key));
}

static bool pan_blit_blend_shader_key_equal(const void *a, const void *b)
{
        return !memcmp(a, b, sizeof(struct pan_blit_blend_shader_key));
}

static uint32_t pan_blit_rsd_key_hash(const void *key)
{
        return _mesa_hash_data(key, sizeof(struct pan_blit_rsd_key));
}

static bool pan_blit_rsd_key_equal(const void *a, const void *b)
{
        return !memcmp(a, b, sizeof(struct pan_blit_rsd_key));
}

static void
pan_blitter_prefill_blit_shader_cache(struct panfrost_device *dev)
{
        static const struct pan_blit_shader_key prefill[] = {
                {
                        .surfaces[0] = {
                                .loc = FRAG_RESULT_DEPTH,
                                .type = nir_type_float32,
                                .dim = MALI_TEXTURE_DIMENSION_2D,
                                .src_samples = 1,
                                .dst_samples = 1,
                        },
                },
                {
                        .surfaces[1] = {
                                .loc = FRAG_RESULT_STENCIL,
                                .type = nir_type_uint32,
                                .dim = MALI_TEXTURE_DIMENSION_2D,
                                .src_samples = 1,
                                .dst_samples = 1,
                        },
                },
                {
                        .surfaces[0] = {
                                .loc = FRAG_RESULT_DATA0,
                                .type = nir_type_float32,
                                .dim = MALI_TEXTURE_DIMENSION_2D,
                                .src_samples = 1,
                                .dst_samples = 1,
                        },
                },
        };

        for (unsigned i = 0; i < ARRAY_SIZE(prefill); i++)
                pan_blitter_get_blit_shader(dev, &prefill[i]);
}

void
pan_blitter_init(struct panfrost_device *dev)
{
        dev->blitter.shaders.blit =
                _mesa_hash_table_create(NULL, pan_blit_shader_key_hash,
                                        pan_blit_shader_key_equal);
        dev->blitter.shaders.blend =
                _mesa_hash_table_create(NULL, pan_blit_blend_shader_key_hash,
                                        pan_blit_blend_shader_key_equal);
        panfrost_pool_init(&dev->blitter.shaders.pool, NULL, dev,
                           PAN_BO_EXECUTE, 4096, "Blitter shaders", false, true);
        pthread_mutex_init(&dev->blitter.shaders.lock, NULL);
        pan_blitter_prefill_blit_shader_cache(dev);

        panfrost_pool_init(&dev->blitter.rsds.pool, NULL, dev, 0, 65536, "Blitter RSDs", false, true);
        dev->blitter.rsds.rsds =
                _mesa_hash_table_create(NULL, pan_blit_rsd_key_hash,
                                        pan_blit_rsd_key_equal);
        pthread_mutex_init(&dev->blitter.rsds.lock, NULL);
}

void
pan_blitter_cleanup(struct panfrost_device *dev)
{
        _mesa_hash_table_destroy(dev->blitter.shaders.blit, NULL);
        _mesa_hash_table_destroy(dev->blitter.shaders.blend, NULL);
        panfrost_pool_cleanup(&dev->blitter.shaders.pool);
        pthread_mutex_destroy(&dev->blitter.shaders.lock);
        _mesa_hash_table_destroy(dev->blitter.rsds.rsds, NULL);
        panfrost_pool_cleanup(&dev->blitter.rsds.pool);
        pthread_mutex_destroy(&dev->blitter.rsds.lock);
}
