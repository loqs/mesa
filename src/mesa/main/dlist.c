/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009  VMware, Inc.  All Rights Reserved.
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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/**
 * \file dlist.c
 * Display lists management functions.
 */

#include "c99_math.h"
#include "glheader.h"

#include "api_arrayelt.h"
#include "api_exec.h"
#include "draw_validate.h"
#include "atifragshader.h"
#include "config.h"
#include "bufferobj.h"
#include "arrayobj.h"
#include "context.h"
#include "dlist.h"
#include "enums.h"
#include "eval.h"
#include "fbobject.h"
#include "framebuffer.h"
#include "glapi/glapi.h"
#include "glformats.h"
#include "hash.h"
#include "image.h"
#include "light.h"
#include "macros.h"
#include "pack.h"
#include "pbo.h"
#include "queryobj.h"
#include "samplerobj.h"
#include "shaderapi.h"
#include "syncobj.h"
#include "teximage.h"
#include "texstorage.h"
#include "mtypes.h"
#include "varray.h"
#include "arbprogram.h"
#include "transformfeedback.h"
#include "glthread_marshal.h"

#include "math/m_matrix.h"

#include "main/dispatch.h"

#include "vbo/vbo.h"
#include "vbo/vbo_util.h"
#include "vbo/vbo_save.h"
#include "util/format_r11g11b10f.h"

#include "util/u_memory.h"

#define USE_BITMAP_ATLAS 1

/**
 * Flush vertices.
 *
 * \param ctx GL context.
 *
 * Checks if dd_function_table::SaveNeedFlush is marked to flush
 * stored (save) vertices, and calls vbo_save_SaveFlushVertices if so.
 */
#define SAVE_FLUSH_VERTICES(ctx)                     \
   do {                                              \
      if (ctx->Driver.SaveNeedFlush)                 \
         vbo_save_SaveFlushVertices(ctx);            \
   } while (0)


/**
 * Macro to assert that the API call was made outside the
 * glBegin()/glEnd() pair, with return value.
 *
 * \param ctx GL context.
 * \param retval value to return value in case the assertion fails.
 */
#define ASSERT_OUTSIDE_SAVE_BEGIN_END_WITH_RETVAL(ctx, retval)          \
   do {                                                                 \
      if (ctx->Driver.CurrentSavePrimitive <= PRIM_MAX) {               \
         _mesa_compile_error( ctx, GL_INVALID_OPERATION, "glBegin/End" ); \
         return retval;                                                 \
      }                                                                 \
   } while (0)

/**
 * Macro to assert that the API call was made outside the
 * glBegin()/glEnd() pair.
 *
 * \param ctx GL context.
 */
#define ASSERT_OUTSIDE_SAVE_BEGIN_END(ctx)                              \
   do {                                                                 \
      if (ctx->Driver.CurrentSavePrimitive <= PRIM_MAX) {               \
         _mesa_compile_error( ctx, GL_INVALID_OPERATION, "glBegin/End" ); \
         return;                                                        \
      }                                                                 \
   } while (0)

/**
 * Macro to assert that the API call was made outside the
 * glBegin()/glEnd() pair and flush the vertices.
 *
 * \param ctx GL context.
 */
#define ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx)                    \
   do {                                                                 \
      ASSERT_OUTSIDE_SAVE_BEGIN_END(ctx);                               \
      SAVE_FLUSH_VERTICES(ctx);                                         \
   } while (0)

/**
 * Macro to assert that the API call was made outside the
 * glBegin()/glEnd() pair and flush the vertices, with return value.
 *
 * \param ctx GL context.
 * \param retval value to return value in case the assertion fails.
 */
#define ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH_WITH_RETVAL(ctx, retval) \
   do {                                                                 \
      ASSERT_OUTSIDE_SAVE_BEGIN_END_WITH_RETVAL(ctx, retval);           \
      SAVE_FLUSH_VERTICES(ctx);                                         \
   } while (0)


/**
 * Display list opcodes.
 *
 * The fact that these identifiers are assigned consecutive
 * integer values starting at 0 is very important, see InstSize array usage)
 */
typedef enum
{
   OPCODE_INVALID = -1,         /* Force signed enum */
   OPCODE_ACCUM,
   OPCODE_ALPHA_FUNC,
   OPCODE_BIND_TEXTURE,
   OPCODE_BITMAP,
   OPCODE_BLEND_COLOR,
   OPCODE_BLEND_EQUATION,
   OPCODE_BLEND_EQUATION_SEPARATE,
   OPCODE_BLEND_FUNC_SEPARATE,

   OPCODE_BLEND_EQUATION_I,
   OPCODE_BLEND_EQUATION_SEPARATE_I,
   OPCODE_BLEND_FUNC_I,
   OPCODE_BLEND_FUNC_SEPARATE_I,

   OPCODE_CALL_LIST,
   OPCODE_CALL_LISTS,
   OPCODE_CLEAR,
   OPCODE_CLEAR_ACCUM,
   OPCODE_CLEAR_COLOR,
   OPCODE_CLEAR_DEPTH,
   OPCODE_CLEAR_INDEX,
   OPCODE_CLEAR_STENCIL,
   OPCODE_CLEAR_BUFFER_IV,
   OPCODE_CLEAR_BUFFER_UIV,
   OPCODE_CLEAR_BUFFER_FV,
   OPCODE_CLEAR_BUFFER_FI,
   OPCODE_CLIP_PLANE,
   OPCODE_COLOR_MASK,
   OPCODE_COLOR_MASK_INDEXED,
   OPCODE_COLOR_MATERIAL,
   OPCODE_COPY_PIXELS,
   OPCODE_COPY_TEX_IMAGE1D,
   OPCODE_COPY_TEX_IMAGE2D,
   OPCODE_COPY_TEX_SUB_IMAGE1D,
   OPCODE_COPY_TEX_SUB_IMAGE2D,
   OPCODE_COPY_TEX_SUB_IMAGE3D,
   OPCODE_CULL_FACE,
   OPCODE_DEPTH_FUNC,
   OPCODE_DEPTH_MASK,
   OPCODE_DEPTH_RANGE,
   OPCODE_DISABLE,
   OPCODE_DISABLE_INDEXED,
   OPCODE_DRAW_BUFFER,
   OPCODE_DRAW_PIXELS,
   OPCODE_ENABLE,
   OPCODE_ENABLE_INDEXED,
   OPCODE_EVALMESH1,
   OPCODE_EVALMESH2,
   OPCODE_FOG,
   OPCODE_FRONT_FACE,
   OPCODE_FRUSTUM,
   OPCODE_HINT,
   OPCODE_INDEX_MASK,
   OPCODE_INIT_NAMES,
   OPCODE_LIGHT,
   OPCODE_LIGHT_MODEL,
   OPCODE_LINE_STIPPLE,
   OPCODE_LINE_WIDTH,
   OPCODE_LIST_BASE,
   OPCODE_LOAD_IDENTITY,
   OPCODE_LOAD_MATRIX,
   OPCODE_LOAD_NAME,
   OPCODE_LOGIC_OP,
   OPCODE_MAP1,
   OPCODE_MAP2,
   OPCODE_MAPGRID1,
   OPCODE_MAPGRID2,
   OPCODE_MATRIX_MODE,
   OPCODE_MULT_MATRIX,
   OPCODE_ORTHO,
   OPCODE_PASSTHROUGH,
   OPCODE_PIXEL_MAP,
   OPCODE_PIXEL_TRANSFER,
   OPCODE_PIXEL_ZOOM,
   OPCODE_POINT_SIZE,
   OPCODE_POINT_PARAMETERS,
   OPCODE_POLYGON_MODE,
   OPCODE_POLYGON_STIPPLE,
   OPCODE_POLYGON_OFFSET,
   OPCODE_POP_ATTRIB,
   OPCODE_POP_MATRIX,
   OPCODE_POP_NAME,
   OPCODE_PRIORITIZE_TEXTURE,
   OPCODE_PUSH_ATTRIB,
   OPCODE_PUSH_MATRIX,
   OPCODE_PUSH_NAME,
   OPCODE_RASTER_POS,
   OPCODE_READ_BUFFER,
   OPCODE_ROTATE,
   OPCODE_SCALE,
   OPCODE_SCISSOR,
   OPCODE_SELECT_TEXTURE_SGIS,
   OPCODE_SELECT_TEXTURE_COORD_SET,
   OPCODE_SHADE_MODEL,
   OPCODE_STENCIL_FUNC,
   OPCODE_STENCIL_MASK,
   OPCODE_STENCIL_OP,
   OPCODE_TEXENV,
   OPCODE_TEXGEN,
   OPCODE_TEXPARAMETER,
   OPCODE_TEX_IMAGE1D,
   OPCODE_TEX_IMAGE2D,
   OPCODE_TEX_IMAGE3D,
   OPCODE_TEX_SUB_IMAGE1D,
   OPCODE_TEX_SUB_IMAGE2D,
   OPCODE_TEX_SUB_IMAGE3D,
   OPCODE_TRANSLATE,
   OPCODE_VIEWPORT,
   OPCODE_WINDOW_POS,
   /* ARB_viewport_array */
   OPCODE_VIEWPORT_ARRAY_V,
   OPCODE_VIEWPORT_INDEXED_F,
   OPCODE_VIEWPORT_INDEXED_FV,
   OPCODE_SCISSOR_ARRAY_V,
   OPCODE_SCISSOR_INDEXED,
   OPCODE_SCISSOR_INDEXED_V,
   OPCODE_DEPTH_ARRAY_V,
   OPCODE_DEPTH_INDEXED,
   /* GL_ARB_multitexture */
   OPCODE_ACTIVE_TEXTURE,
   /* GL_ARB_texture_compression */
   OPCODE_COMPRESSED_TEX_IMAGE_1D,
   OPCODE_COMPRESSED_TEX_IMAGE_2D,
   OPCODE_COMPRESSED_TEX_IMAGE_3D,
   OPCODE_COMPRESSED_TEX_SUB_IMAGE_1D,
   OPCODE_COMPRESSED_TEX_SUB_IMAGE_2D,
   OPCODE_COMPRESSED_TEX_SUB_IMAGE_3D,
   /* GL_ARB_multisample */
   OPCODE_SAMPLE_COVERAGE,
   /* GL_ARB_window_pos */
   OPCODE_WINDOW_POS_ARB,
   /* GL_ARB_vertex_program */
   OPCODE_BIND_PROGRAM_ARB,
   OPCODE_PROGRAM_LOCAL_PARAMETER_ARB,
   /* GL_EXT_stencil_two_side */
   OPCODE_ACTIVE_STENCIL_FACE_EXT,
   /* GL_EXT_depth_bounds_test */
   OPCODE_DEPTH_BOUNDS_EXT,
   /* GL_ARB_vertex/fragment_program */
   OPCODE_PROGRAM_STRING_ARB,
   OPCODE_PROGRAM_ENV_PARAMETER_ARB,
   /* GL_ARB_occlusion_query */
   OPCODE_BEGIN_QUERY_ARB,
   OPCODE_END_QUERY_ARB,
   /* GL_ARB_draw_buffers */
   OPCODE_DRAW_BUFFERS_ARB,
   /* GL_ATI_fragment_shader */
   OPCODE_BIND_FRAGMENT_SHADER_ATI,
   OPCODE_SET_FRAGMENT_SHADER_CONSTANTS_ATI,
   /* OpenGL 2.0 */
   OPCODE_STENCIL_FUNC_SEPARATE,
   OPCODE_STENCIL_OP_SEPARATE,
   OPCODE_STENCIL_MASK_SEPARATE,
   /* GL_NV_primitive_restart */
   OPCODE_PRIMITIVE_RESTART_NV,
   /* GL_ARB_shader_objects */
   OPCODE_USE_PROGRAM,
   OPCODE_UNIFORM_1F,
   OPCODE_UNIFORM_2F,
   OPCODE_UNIFORM_3F,
   OPCODE_UNIFORM_4F,
   OPCODE_UNIFORM_1FV,
   OPCODE_UNIFORM_2FV,
   OPCODE_UNIFORM_3FV,
   OPCODE_UNIFORM_4FV,
   OPCODE_UNIFORM_1I,
   OPCODE_UNIFORM_2I,
   OPCODE_UNIFORM_3I,
   OPCODE_UNIFORM_4I,
   OPCODE_UNIFORM_1IV,
   OPCODE_UNIFORM_2IV,
   OPCODE_UNIFORM_3IV,
   OPCODE_UNIFORM_4IV,
   OPCODE_UNIFORM_MATRIX22,
   OPCODE_UNIFORM_MATRIX33,
   OPCODE_UNIFORM_MATRIX44,
   OPCODE_UNIFORM_MATRIX23,
   OPCODE_UNIFORM_MATRIX32,
   OPCODE_UNIFORM_MATRIX24,
   OPCODE_UNIFORM_MATRIX42,
   OPCODE_UNIFORM_MATRIX34,
   OPCODE_UNIFORM_MATRIX43,

   /* OpenGL 3.0 */
   OPCODE_UNIFORM_1UI,
   OPCODE_UNIFORM_2UI,
   OPCODE_UNIFORM_3UI,
   OPCODE_UNIFORM_4UI,
   OPCODE_UNIFORM_1UIV,
   OPCODE_UNIFORM_2UIV,
   OPCODE_UNIFORM_3UIV,
   OPCODE_UNIFORM_4UIV,

   /* GL_ARB_gpu_shader_fp64 */
   OPCODE_UNIFORM_1D,
   OPCODE_UNIFORM_2D,
   OPCODE_UNIFORM_3D,
   OPCODE_UNIFORM_4D,
   OPCODE_UNIFORM_1DV,
   OPCODE_UNIFORM_2DV,
   OPCODE_UNIFORM_3DV,
   OPCODE_UNIFORM_4DV,
   OPCODE_UNIFORM_MATRIX22D,
   OPCODE_UNIFORM_MATRIX33D,
   OPCODE_UNIFORM_MATRIX44D,
   OPCODE_UNIFORM_MATRIX23D,
   OPCODE_UNIFORM_MATRIX32D,
   OPCODE_UNIFORM_MATRIX24D,
   OPCODE_UNIFORM_MATRIX42D,
   OPCODE_UNIFORM_MATRIX34D,
   OPCODE_UNIFORM_MATRIX43D,

   /* GL_ARB_gpu_shader_int64 */
   OPCODE_UNIFORM_1I64,
   OPCODE_UNIFORM_2I64,
   OPCODE_UNIFORM_3I64,
   OPCODE_UNIFORM_4I64,
   OPCODE_UNIFORM_1I64V,
   OPCODE_UNIFORM_2I64V,
   OPCODE_UNIFORM_3I64V,
   OPCODE_UNIFORM_4I64V,
   OPCODE_UNIFORM_1UI64,
   OPCODE_UNIFORM_2UI64,
   OPCODE_UNIFORM_3UI64,
   OPCODE_UNIFORM_4UI64,
   OPCODE_UNIFORM_1UI64V,
   OPCODE_UNIFORM_2UI64V,
   OPCODE_UNIFORM_3UI64V,
   OPCODE_UNIFORM_4UI64V,
   OPCODE_PROGRAM_UNIFORM_1I64,
   OPCODE_PROGRAM_UNIFORM_2I64,
   OPCODE_PROGRAM_UNIFORM_3I64,
   OPCODE_PROGRAM_UNIFORM_4I64,
   OPCODE_PROGRAM_UNIFORM_1I64V,
   OPCODE_PROGRAM_UNIFORM_2I64V,
   OPCODE_PROGRAM_UNIFORM_3I64V,
   OPCODE_PROGRAM_UNIFORM_4I64V,
   OPCODE_PROGRAM_UNIFORM_1UI64,
   OPCODE_PROGRAM_UNIFORM_2UI64,
   OPCODE_PROGRAM_UNIFORM_3UI64,
   OPCODE_PROGRAM_UNIFORM_4UI64,
   OPCODE_PROGRAM_UNIFORM_1UI64V,
   OPCODE_PROGRAM_UNIFORM_2UI64V,
   OPCODE_PROGRAM_UNIFORM_3UI64V,
   OPCODE_PROGRAM_UNIFORM_4UI64V,

   /* OpenGL 4.0 / GL_ARB_tessellation_shader */
   OPCODE_PATCH_PARAMETER_I,
   OPCODE_PATCH_PARAMETER_FV_INNER,
   OPCODE_PATCH_PARAMETER_FV_OUTER,

   /* OpenGL 4.2 / GL_ARB_separate_shader_objects */
   OPCODE_USE_PROGRAM_STAGES,
   OPCODE_PROGRAM_UNIFORM_1F,
   OPCODE_PROGRAM_UNIFORM_2F,
   OPCODE_PROGRAM_UNIFORM_3F,
   OPCODE_PROGRAM_UNIFORM_4F,
   OPCODE_PROGRAM_UNIFORM_1FV,
   OPCODE_PROGRAM_UNIFORM_2FV,
   OPCODE_PROGRAM_UNIFORM_3FV,
   OPCODE_PROGRAM_UNIFORM_4FV,
   OPCODE_PROGRAM_UNIFORM_1D,
   OPCODE_PROGRAM_UNIFORM_2D,
   OPCODE_PROGRAM_UNIFORM_3D,
   OPCODE_PROGRAM_UNIFORM_4D,
   OPCODE_PROGRAM_UNIFORM_1DV,
   OPCODE_PROGRAM_UNIFORM_2DV,
   OPCODE_PROGRAM_UNIFORM_3DV,
   OPCODE_PROGRAM_UNIFORM_4DV,
   OPCODE_PROGRAM_UNIFORM_1I,
   OPCODE_PROGRAM_UNIFORM_2I,
   OPCODE_PROGRAM_UNIFORM_3I,
   OPCODE_PROGRAM_UNIFORM_4I,
   OPCODE_PROGRAM_UNIFORM_1IV,
   OPCODE_PROGRAM_UNIFORM_2IV,
   OPCODE_PROGRAM_UNIFORM_3IV,
   OPCODE_PROGRAM_UNIFORM_4IV,
   OPCODE_PROGRAM_UNIFORM_1UI,
   OPCODE_PROGRAM_UNIFORM_2UI,
   OPCODE_PROGRAM_UNIFORM_3UI,
   OPCODE_PROGRAM_UNIFORM_4UI,
   OPCODE_PROGRAM_UNIFORM_1UIV,
   OPCODE_PROGRAM_UNIFORM_2UIV,
   OPCODE_PROGRAM_UNIFORM_3UIV,
   OPCODE_PROGRAM_UNIFORM_4UIV,
   OPCODE_PROGRAM_UNIFORM_MATRIX22F,
   OPCODE_PROGRAM_UNIFORM_MATRIX33F,
   OPCODE_PROGRAM_UNIFORM_MATRIX44F,
   OPCODE_PROGRAM_UNIFORM_MATRIX23F,
   OPCODE_PROGRAM_UNIFORM_MATRIX32F,
   OPCODE_PROGRAM_UNIFORM_MATRIX24F,
   OPCODE_PROGRAM_UNIFORM_MATRIX42F,
   OPCODE_PROGRAM_UNIFORM_MATRIX34F,
   OPCODE_PROGRAM_UNIFORM_MATRIX43F,
   OPCODE_PROGRAM_UNIFORM_MATRIX22D,
   OPCODE_PROGRAM_UNIFORM_MATRIX33D,
   OPCODE_PROGRAM_UNIFORM_MATRIX44D,
   OPCODE_PROGRAM_UNIFORM_MATRIX23D,
   OPCODE_PROGRAM_UNIFORM_MATRIX32D,
   OPCODE_PROGRAM_UNIFORM_MATRIX24D,
   OPCODE_PROGRAM_UNIFORM_MATRIX42D,
   OPCODE_PROGRAM_UNIFORM_MATRIX34D,
   OPCODE_PROGRAM_UNIFORM_MATRIX43D,

   /* GL_ARB_clip_control */
   OPCODE_CLIP_CONTROL,

   /* GL_ARB_color_buffer_float */
   OPCODE_CLAMP_COLOR,

   /* GL_EXT_framebuffer_blit */
   OPCODE_BLIT_FRAMEBUFFER,

   /* Vertex attributes -- fallback for when optimized display
    * list build isn't active.
    */
   OPCODE_ATTR_1F_NV,
   OPCODE_ATTR_2F_NV,
   OPCODE_ATTR_3F_NV,
   OPCODE_ATTR_4F_NV,
   OPCODE_ATTR_1F_ARB,
   OPCODE_ATTR_2F_ARB,
   OPCODE_ATTR_3F_ARB,
   OPCODE_ATTR_4F_ARB,
   OPCODE_ATTR_1I,
   OPCODE_ATTR_2I,
   OPCODE_ATTR_3I,
   OPCODE_ATTR_4I,
   OPCODE_ATTR_1D,
   OPCODE_ATTR_2D,
   OPCODE_ATTR_3D,
   OPCODE_ATTR_4D,
   OPCODE_ATTR_1UI64,
   OPCODE_MATERIAL,
   OPCODE_BEGIN,
   OPCODE_END,
   OPCODE_EVAL_C1,
   OPCODE_EVAL_C2,
   OPCODE_EVAL_P1,
   OPCODE_EVAL_P2,

   /* GL_EXT_provoking_vertex */
   OPCODE_PROVOKING_VERTEX,

   /* GL_EXT_transform_feedback */
   OPCODE_BEGIN_TRANSFORM_FEEDBACK,
   OPCODE_END_TRANSFORM_FEEDBACK,
   OPCODE_BIND_TRANSFORM_FEEDBACK,
   OPCODE_PAUSE_TRANSFORM_FEEDBACK,
   OPCODE_RESUME_TRANSFORM_FEEDBACK,
   OPCODE_DRAW_TRANSFORM_FEEDBACK,

   /* GL_EXT_texture_integer */
   OPCODE_CLEARCOLOR_I,
   OPCODE_CLEARCOLOR_UI,
   OPCODE_TEXPARAMETER_I,
   OPCODE_TEXPARAMETER_UI,

   /* GL_ARB_instanced_arrays */
   OPCODE_VERTEX_ATTRIB_DIVISOR,

   /* GL_NV_texture_barrier */
   OPCODE_TEXTURE_BARRIER_NV,

   /* GL_ARB_sampler_object */
   OPCODE_BIND_SAMPLER,
   OPCODE_SAMPLER_PARAMETERIV,
   OPCODE_SAMPLER_PARAMETERFV,
   OPCODE_SAMPLER_PARAMETERIIV,
   OPCODE_SAMPLER_PARAMETERUIV,

   /* ARB_compute_shader */
   OPCODE_DISPATCH_COMPUTE,

   /* GL_ARB_sync */
   OPCODE_WAIT_SYNC,

   /* GL_NV_conditional_render */
   OPCODE_BEGIN_CONDITIONAL_RENDER,
   OPCODE_END_CONDITIONAL_RENDER,

   /* ARB_timer_query */
   OPCODE_QUERY_COUNTER,

   /* ARB_transform_feedback3 */
   OPCODE_BEGIN_QUERY_INDEXED,
   OPCODE_END_QUERY_INDEXED,
   OPCODE_DRAW_TRANSFORM_FEEDBACK_STREAM,

   /* ARB_transform_feedback_instanced */
   OPCODE_DRAW_TRANSFORM_FEEDBACK_INSTANCED,
   OPCODE_DRAW_TRANSFORM_FEEDBACK_STREAM_INSTANCED,

   /* ARB_uniform_buffer_object */
   OPCODE_UNIFORM_BLOCK_BINDING,

   /* ARB_shader_subroutines */
   OPCODE_UNIFORM_SUBROUTINES,

   /* EXT_polygon_offset_clamp */
   OPCODE_POLYGON_OFFSET_CLAMP,

   /* EXT_window_rectangles */
   OPCODE_WINDOW_RECTANGLES,

   /* NV_conservative_raster */
   OPCODE_SUBPIXEL_PRECISION_BIAS,

   /* NV_conservative_raster_dilate */
   OPCODE_CONSERVATIVE_RASTER_PARAMETER_F,

   /* NV_conservative_raster_pre_snap_triangles */
   OPCODE_CONSERVATIVE_RASTER_PARAMETER_I,

   /* EXT_direct_state_access */
   OPCODE_MATRIX_LOAD,
   OPCODE_MATRIX_MULT,
   OPCODE_MATRIX_ROTATE,
   OPCODE_MATRIX_SCALE,
   OPCODE_MATRIX_TRANSLATE,
   OPCODE_MATRIX_LOAD_IDENTITY,
   OPCODE_MATRIX_ORTHO,
   OPCODE_MATRIX_FRUSTUM,
   OPCODE_MATRIX_PUSH,
   OPCODE_MATRIX_POP,
   OPCODE_TEXTUREPARAMETER_F,
   OPCODE_TEXTUREPARAMETER_I,
   OPCODE_TEXTUREPARAMETER_II,
   OPCODE_TEXTUREPARAMETER_IUI,
   OPCODE_TEXTURE_IMAGE1D,
   OPCODE_TEXTURE_IMAGE2D,
   OPCODE_TEXTURE_IMAGE3D,
   OPCODE_TEXTURE_SUB_IMAGE1D,
   OPCODE_TEXTURE_SUB_IMAGE2D,
   OPCODE_TEXTURE_SUB_IMAGE3D,
   OPCODE_COPY_TEXTURE_IMAGE1D,
   OPCODE_COPY_TEXTURE_IMAGE2D,
   OPCODE_COPY_TEXTURE_SUB_IMAGE1D,
   OPCODE_COPY_TEXTURE_SUB_IMAGE2D,
   OPCODE_COPY_TEXTURE_SUB_IMAGE3D,
   OPCODE_BIND_MULTITEXTURE,
   OPCODE_MULTITEXPARAMETER_F,
   OPCODE_MULTITEXPARAMETER_I,
   OPCODE_MULTITEXPARAMETER_II,
   OPCODE_MULTITEXPARAMETER_IUI,
   OPCODE_MULTITEX_IMAGE1D,
   OPCODE_MULTITEX_IMAGE2D,
   OPCODE_MULTITEX_IMAGE3D,
   OPCODE_MULTITEX_SUB_IMAGE1D,
   OPCODE_MULTITEX_SUB_IMAGE2D,
   OPCODE_MULTITEX_SUB_IMAGE3D,
   OPCODE_COPY_MULTITEX_IMAGE1D,
   OPCODE_COPY_MULTITEX_IMAGE2D,
   OPCODE_COPY_MULTITEX_SUB_IMAGE1D,
   OPCODE_COPY_MULTITEX_SUB_IMAGE2D,
   OPCODE_COPY_MULTITEX_SUB_IMAGE3D,
   OPCODE_MULTITEXENV,
   OPCODE_COMPRESSED_TEXTURE_IMAGE_1D,
   OPCODE_COMPRESSED_TEXTURE_IMAGE_2D,
   OPCODE_COMPRESSED_TEXTURE_IMAGE_3D,
   OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_1D,
   OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_2D,
   OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_3D,
   OPCODE_COMPRESSED_MULTITEX_IMAGE_1D,
   OPCODE_COMPRESSED_MULTITEX_IMAGE_2D,
   OPCODE_COMPRESSED_MULTITEX_IMAGE_3D,
   OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_1D,
   OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_2D,
   OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_3D,
   OPCODE_NAMED_PROGRAM_STRING,
   OPCODE_NAMED_PROGRAM_LOCAL_PARAMETER,

   OPCODE_VERTEX_LIST,

   /* The following three are meta instructions */
   OPCODE_ERROR,                /* raise compiled-in error */
   OPCODE_CONTINUE,
   OPCODE_NOP,                  /* No-op (used for 8-byte alignment */
   OPCODE_END_OF_LIST,
   OPCODE_EXT_0
} OpCode;



/**
 * Display list node.
 *
 * Display list instructions are stored as sequences of "nodes".  Nodes
 * are allocated in blocks.  Each block has BLOCK_SIZE nodes.  Blocks
 * are linked together with a pointer.
 *
 * Each instruction in the display list is stored as a sequence of
 * contiguous nodes in memory.
 * Each node is the union of a variety of data types.
 *
 * Note, all of these members should be 4 bytes in size or less for the
 * sake of compact display lists.  We store 8-byte pointers in a pair of
 * these nodes using the save/get_pointer() functions below.
 */
union gl_dlist_node
{
   OpCode opcode;
   GLboolean b;
   GLbitfield bf;
   GLubyte ub;
   GLshort s;
   GLushort us;
   GLint i;
   GLuint ui;
   GLenum e;
   GLfloat f;
   GLsizei si;
};


typedef union gl_dlist_node Node;


/** How many 4-byte dwords to store a pointer */
#define POINTER_DWORDS (sizeof(void *) / 4)

/* We want to keep sizeof(union gl_dlist_node) == 4 to minimize
 * space for display lists.  The following types and functions are
 * used to help store 4- and 8-byte pointers in 1 or 2 dlist_nodes.
 */
union pointer
{
   void *ptr;
   GLuint dwords[POINTER_DWORDS];
};


/**
 * Save a 4 or 8-byte pointer at dest (and dest+1).
 */
static inline void
save_pointer(Node *dest, void *src)
{
   union pointer p;
   unsigned i;

   STATIC_ASSERT(POINTER_DWORDS == 1 || POINTER_DWORDS == 2);
   STATIC_ASSERT(sizeof(Node) == 4);

   p.ptr = src;

   for (i = 0; i < POINTER_DWORDS; i++)
      dest[i].ui = p.dwords[i];
}


/**
 * Retrieve a 4 or 8-byte pointer from node (node+1).
 */
static inline void *
get_pointer(const Node *node)
{
   union pointer p;
   unsigned i;

   for (i = 0; i < POINTER_DWORDS; i++)
      p.dwords[i] = node[i].ui;

   return p.ptr;
}


/**
 * Used to store a 64-bit uint in a pair of "Nodes" for the sake of 32-bit
 * environment.
 */
union uint64_pair
{
   GLuint64 uint64;
   GLuint uint32[2];
};


union float64_pair
{
   GLdouble d;
   GLuint uint32[2];
};

union int64_pair
{
   GLint64 int64;
   GLint int32[2];
};

#define ASSIGN_DOUBLE_TO_NODES(n, idx, value)                              \
   do {                                                                    \
      union float64_pair tmp;                                              \
      tmp.d = value;                                                       \
      n[idx].ui = tmp.uint32[0];                                           \
      n[idx+1].ui = tmp.uint32[1];                                         \
   } while (0)

#define ASSIGN_UINT64_TO_NODES(n, idx, value)                              \
   do {                                                                    \
      union uint64_pair tmp;                                               \
      tmp.uint64 = value;                                                  \
      n[idx].ui = tmp.uint32[0];                                           \
      n[idx+1].ui = tmp.uint32[1];                                         \
   } while (0)

#define ASSIGN_INT64_TO_NODES(n, idx, value)                               \
   do {                                                                    \
      union int64_pair tmp;                                                \
      tmp.int64 = value;                                                   \
      n[idx].i = tmp.int32[0];                                             \
      n[idx+1].i = tmp.int32[1];                                           \
   } while (0)

/**
 * How many nodes to allocate at a time.  Note that bulk vertex data
 * from glBegin/glVertex/glEnd primitives will typically wind up in
 * a VBO, and not directly in the display list itself.
 */
#define BLOCK_SIZE 256



/**
 * Number of nodes of storage needed for each instruction.
 * Sizes for dynamically allocated opcodes are stored in the context struct.
 */
static GLuint InstSize[OPCODE_END_OF_LIST + 1];


void mesa_print_display_list(GLuint list);


/**
 * Called by display list code when a display list is being deleted.
 */
static void
vbo_destroy_vertex_list(struct gl_context *ctx, struct vbo_save_vertex_list *node)
{
   for (gl_vertex_processing_mode vpm = VP_MODE_FF; vpm < VP_MODE_MAX; ++vpm)
      _mesa_reference_vao(ctx, &node->VAO[vpm], NULL);

   if (--node->prim_store->refcount == 0) {
      free(node->prim_store->prims);
      free(node->prim_store);
   }

   free(node->merged.mode);
   free(node->merged.start_count);

   _mesa_reference_buffer_object(ctx, &node->merged.ib.obj, NULL);
   free(node->current_data);
   node->current_data = NULL;
}

static void
vbo_print_vertex_list(struct gl_context *ctx, struct vbo_save_vertex_list *node, FILE *f)
{
   GLuint i;
   struct gl_buffer_object *buffer = node->VAO[0]->BufferBinding[0].BufferObj;
   const GLuint vertex_size = _vbo_save_get_stride(node)/sizeof(GLfloat);
   (void) ctx;

   fprintf(f, "VBO-VERTEX-LIST, %u vertices, %d primitives, %d vertsize, "
           "buffer %p\n",
           node->vertex_count, node->prim_count, vertex_size,
           buffer);

   for (i = 0; i < node->prim_count; i++) {
      struct _mesa_prim *prim = &node->prims[i];
      fprintf(f, "   prim %d: %s %d..%d %s %s\n",
             i,
             _mesa_lookup_prim_by_nr(prim->mode),
             prim->start,
             prim->start + prim->count,
             (prim->begin) ? "BEGIN" : "(wrap)",
             (prim->end) ? "END" : "(wrap)");
   }
}


/**
 * Does the given display list only contain a single glBitmap call?
 */
static bool
is_bitmap_list(const struct gl_display_list *dlist)
{
   const Node *n = dlist->Head;
   if (n[0].opcode == OPCODE_BITMAP) {
      n += InstSize[OPCODE_BITMAP];
      if (n[0].opcode == OPCODE_END_OF_LIST)
         return true;
   }
   return false;
}


/**
 * Is the given display list an empty list?
 */
static bool
is_empty_list(const struct gl_display_list *dlist)
{
   const Node *n = dlist->Head;
   return n[0].opcode == OPCODE_END_OF_LIST;
}


/**
 * Delete/free a gl_bitmap_atlas.  Called during context tear-down.
 */
void
_mesa_delete_bitmap_atlas(struct gl_context *ctx, struct gl_bitmap_atlas *atlas)
{
   if (atlas->texObj) {
      ctx->Driver.DeleteTexture(ctx, atlas->texObj);
   }
   free(atlas->glyphs);
   free(atlas);
}


/**
 * Lookup a gl_bitmap_atlas by listBase ID.
 */
static struct gl_bitmap_atlas *
lookup_bitmap_atlas(struct gl_context *ctx, GLuint listBase)
{
   struct gl_bitmap_atlas *atlas;

   assert(listBase > 0);
   atlas = _mesa_HashLookup(ctx->Shared->BitmapAtlas, listBase);
   return atlas;
}


/**
 * Create new bitmap atlas and insert into hash table.
 */
static struct gl_bitmap_atlas *
alloc_bitmap_atlas(struct gl_context *ctx, GLuint listBase, bool isGenName)
{
   struct gl_bitmap_atlas *atlas;

   assert(listBase > 0);
   assert(_mesa_HashLookup(ctx->Shared->BitmapAtlas, listBase) == NULL);

   atlas = calloc(1, sizeof(*atlas));
   if (atlas) {
      _mesa_HashInsert(ctx->Shared->BitmapAtlas, listBase, atlas, isGenName);
      atlas->Id = listBase;
   }

   return atlas;
}


/**
 * Try to build a bitmap atlas.  This involves examining a sequence of
 * display lists which contain glBitmap commands and putting the bitmap
 * images into a texture map (the atlas).
 * If we succeed, gl_bitmap_atlas::complete will be set to true.
 * If we fail, gl_bitmap_atlas::incomplete will be set to true.
 */
static void
build_bitmap_atlas(struct gl_context *ctx, struct gl_bitmap_atlas *atlas,
                   GLuint listBase)
{
   unsigned i, row_height = 0, xpos = 0, ypos = 0;
   GLubyte *map;
   GLint map_stride;

   assert(atlas);
   assert(!atlas->complete);
   assert(atlas->numBitmaps > 0);

   /* We use a rectangle texture (non-normalized coords) for the atlas */
   assert(ctx->Extensions.NV_texture_rectangle);
   assert(ctx->Const.MaxTextureRectSize >= 1024);

   atlas->texWidth = 1024;
   atlas->texHeight = 0;  /* determined below */

   atlas->glyphs = malloc(atlas->numBitmaps * sizeof(atlas->glyphs[0]));
   if (!atlas->glyphs) {
      /* give up */
      atlas->incomplete = true;
      return;
   }

   /* Loop over the display lists.  They should all contain a single glBitmap
    * call.  If not, bail out.  Also, compute the position and sizes of each
    * bitmap in the atlas to determine the texture atlas size.
    */
   for (i = 0; i < atlas->numBitmaps; i++) {
      const struct gl_display_list *list = _mesa_lookup_list(ctx, listBase + i);
      const Node *n;
      struct gl_bitmap_glyph *g = &atlas->glyphs[i];
      unsigned bitmap_width, bitmap_height;
      float bitmap_xmove, bitmap_ymove, bitmap_xorig, bitmap_yorig;

      if (!list || is_empty_list(list)) {
         /* stop here */
         atlas->numBitmaps = i;
         break;
      }

      if (!is_bitmap_list(list)) {
         /* This list does not contain exactly one glBitmap command. Give up. */
         atlas->incomplete = true;
         return;
      }

      /* get bitmap info from the display list command */
      n = list->Head;
      assert(n[0].opcode == OPCODE_BITMAP);
      bitmap_width = n[1].i;
      bitmap_height = n[2].i;
      bitmap_xorig = n[3].f;
      bitmap_yorig = n[4].f;
      bitmap_xmove = n[5].f;
      bitmap_ymove = n[6].f;

      if (xpos + bitmap_width > atlas->texWidth) {
         /* advance to the next row of the texture */
         xpos = 0;
         ypos += row_height;
         row_height = 0;
      }

      /* save the bitmap's position in the atlas */
      g->x = xpos;
      g->y = ypos;
      g->w = bitmap_width;
      g->h = bitmap_height;
      g->xorig = bitmap_xorig;
      g->yorig = bitmap_yorig;
      g->xmove = bitmap_xmove;
      g->ymove = bitmap_ymove;

      xpos += bitmap_width;

      /* keep track of tallest bitmap in the row */
      row_height = MAX2(row_height, bitmap_height);
   }

   /* Now we know the texture height */
   atlas->texHeight = ypos + row_height;

   if (atlas->texHeight == 0) {
      /* no glyphs found, give up */
      goto fail;
   }
   else if (atlas->texHeight > ctx->Const.MaxTextureRectSize) {
      /* too large, give up */
      goto fail;
   }

   /* Create atlas texture (texture ID is irrelevant) */
   atlas->texObj = ctx->Driver.NewTextureObject(ctx, 999, GL_TEXTURE_RECTANGLE);
   if (!atlas->texObj) {
      goto out_of_memory;
   }

   atlas->texObj->Sampler.Attrib.MinFilter = GL_NEAREST;
   atlas->texObj->Sampler.Attrib.MagFilter = GL_NEAREST;
   atlas->texObj->Sampler.Attrib.state.min_img_filter = PIPE_TEX_FILTER_NEAREST;
   atlas->texObj->Sampler.Attrib.state.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   atlas->texObj->Sampler.Attrib.state.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
   atlas->texObj->Attrib.MaxLevel = 0;
   atlas->texObj->Immutable = GL_TRUE;

   atlas->texImage = _mesa_get_tex_image(ctx, atlas->texObj,
                                         GL_TEXTURE_RECTANGLE, 0);
   if (!atlas->texImage) {
      goto out_of_memory;
   }

   if (ctx->Const.BitmapUsesRed)
      _mesa_init_teximage_fields(ctx, atlas->texImage,
                                 atlas->texWidth, atlas->texHeight, 1, 0,
                                 GL_RED, MESA_FORMAT_R_UNORM8);
   else
      _mesa_init_teximage_fields(ctx, atlas->texImage,
                                 atlas->texWidth, atlas->texHeight, 1, 0,
                                 GL_ALPHA, MESA_FORMAT_A_UNORM8);

   /* alloc image storage */
   if (!ctx->Driver.AllocTextureImageBuffer(ctx, atlas->texImage)) {
      goto out_of_memory;
   }

   /* map teximage, load with bitmap glyphs */
   ctx->Driver.MapTextureImage(ctx, atlas->texImage, 0,
                               0, 0, atlas->texWidth, atlas->texHeight,
                               GL_MAP_WRITE_BIT, &map, &map_stride);
   if (!map) {
      goto out_of_memory;
   }

   /* Background/clear pixels are 0xff, foreground/set pixels are 0x0 */
   memset(map, 0xff, map_stride * atlas->texHeight);

   for (i = 0; i < atlas->numBitmaps; i++) {
      const struct gl_display_list *list = _mesa_lookup_list(ctx, listBase + i);
      const Node *n = list->Head;

      assert(n[0].opcode == OPCODE_BITMAP ||
             n[0].opcode == OPCODE_END_OF_LIST);

      if (n[0].opcode == OPCODE_BITMAP) {
         unsigned bitmap_width = n[1].i;
         unsigned bitmap_height = n[2].i;
         unsigned xpos = atlas->glyphs[i].x;
         unsigned ypos = atlas->glyphs[i].y;
         const void *bitmap_image = get_pointer(&n[7]);

         assert(atlas->glyphs[i].w == bitmap_width);
         assert(atlas->glyphs[i].h == bitmap_height);

         /* put the bitmap image into the texture image */
         _mesa_expand_bitmap(bitmap_width, bitmap_height,
                             &ctx->DefaultPacking, bitmap_image,
                             map + map_stride * ypos + xpos, /* dest addr */
                             map_stride, 0x0);
      }
   }

   ctx->Driver.UnmapTextureImage(ctx, atlas->texImage, 0);

   atlas->complete = true;

   return;

out_of_memory:
   _mesa_error(ctx, GL_OUT_OF_MEMORY, "Display list bitmap atlas");
fail:
   if (atlas->texObj) {
      ctx->Driver.DeleteTexture(ctx, atlas->texObj);
   }
   free(atlas->glyphs);
   atlas->glyphs = NULL;
   atlas->incomplete = true;
}


/**
 * Allocate a gl_display_list object with an initial block of storage.
 * \param count  how many display list nodes/tokens to allocate
 */
static struct gl_display_list *
make_list(GLuint name, GLuint count)
{
   struct gl_display_list *dlist = CALLOC_STRUCT(gl_display_list);
   dlist->Name = name;
   dlist->Head = malloc(sizeof(Node) * count);
   dlist->Head[0].opcode = OPCODE_END_OF_LIST;
   /* All InstSize[] entries must be non-zero */
   InstSize[OPCODE_END_OF_LIST] = 1;
   return dlist;
}


/**
 * Lookup function to just encapsulate casting.
 */
struct gl_display_list *
_mesa_lookup_list(struct gl_context *ctx, GLuint list)
{
   return (struct gl_display_list *)
      _mesa_HashLookup(ctx->Shared->DisplayList, list);
}


/**
 * Delete the named display list, but don't remove from hash table.
 * \param dlist - display list pointer
 */
void
_mesa_delete_list(struct gl_context *ctx, struct gl_display_list *dlist)
{
   Node *n, *block;

   if (!dlist->Head) {
      free(dlist->Label);
      free(dlist);
      return;
   }

   n = block = dlist->Head;

   while (1) {
      const OpCode opcode = n[0].opcode;

      switch (opcode) {
            /* for some commands, we need to free malloc'd memory */
         case OPCODE_MAP1:
            free(get_pointer(&n[6]));
            break;
         case OPCODE_MAP2:
            free(get_pointer(&n[10]));
            break;
         case OPCODE_CALL_LISTS:
            free(get_pointer(&n[3]));
            break;
         case OPCODE_DRAW_PIXELS:
            free(get_pointer(&n[5]));
            break;
         case OPCODE_BITMAP:
            free(get_pointer(&n[7]));
            break;
         case OPCODE_POLYGON_STIPPLE:
            free(get_pointer(&n[1]));
            break;
         case OPCODE_TEX_IMAGE1D:
            free(get_pointer(&n[8]));
            break;
         case OPCODE_TEX_IMAGE2D:
            free(get_pointer(&n[9]));
            break;
         case OPCODE_TEX_IMAGE3D:
            free(get_pointer(&n[10]));
            break;
         case OPCODE_TEX_SUB_IMAGE1D:
            free(get_pointer(&n[7]));
            break;
         case OPCODE_TEX_SUB_IMAGE2D:
            free(get_pointer(&n[9]));
            break;
         case OPCODE_TEX_SUB_IMAGE3D:
            free(get_pointer(&n[11]));
            break;
         case OPCODE_COMPRESSED_TEX_IMAGE_1D:
            free(get_pointer(&n[7]));
            break;
         case OPCODE_COMPRESSED_TEX_IMAGE_2D:
            free(get_pointer(&n[8]));
            break;
         case OPCODE_COMPRESSED_TEX_IMAGE_3D:
            free(get_pointer(&n[9]));
            break;
         case OPCODE_COMPRESSED_TEX_SUB_IMAGE_1D:
            free(get_pointer(&n[7]));
            break;
         case OPCODE_COMPRESSED_TEX_SUB_IMAGE_2D:
            free(get_pointer(&n[9]));
            break;
         case OPCODE_COMPRESSED_TEX_SUB_IMAGE_3D:
            free(get_pointer(&n[11]));
            break;
         case OPCODE_PROGRAM_STRING_ARB:
            free(get_pointer(&n[4]));      /* program string */
            break;
         case OPCODE_UNIFORM_1FV:
         case OPCODE_UNIFORM_2FV:
         case OPCODE_UNIFORM_3FV:
         case OPCODE_UNIFORM_4FV:
         case OPCODE_UNIFORM_1DV:
         case OPCODE_UNIFORM_2DV:
         case OPCODE_UNIFORM_3DV:
         case OPCODE_UNIFORM_4DV:
         case OPCODE_UNIFORM_1IV:
         case OPCODE_UNIFORM_2IV:
         case OPCODE_UNIFORM_3IV:
         case OPCODE_UNIFORM_4IV:
         case OPCODE_UNIFORM_1UIV:
         case OPCODE_UNIFORM_2UIV:
         case OPCODE_UNIFORM_3UIV:
         case OPCODE_UNIFORM_4UIV:
         case OPCODE_UNIFORM_1I64V:
         case OPCODE_UNIFORM_2I64V:
         case OPCODE_UNIFORM_3I64V:
         case OPCODE_UNIFORM_4I64V:
         case OPCODE_UNIFORM_1UI64V:
         case OPCODE_UNIFORM_2UI64V:
         case OPCODE_UNIFORM_3UI64V:
         case OPCODE_UNIFORM_4UI64V:
            free(get_pointer(&n[3]));
            break;
         case OPCODE_UNIFORM_MATRIX22:
         case OPCODE_UNIFORM_MATRIX33:
         case OPCODE_UNIFORM_MATRIX44:
         case OPCODE_UNIFORM_MATRIX24:
         case OPCODE_UNIFORM_MATRIX42:
         case OPCODE_UNIFORM_MATRIX23:
         case OPCODE_UNIFORM_MATRIX32:
         case OPCODE_UNIFORM_MATRIX34:
         case OPCODE_UNIFORM_MATRIX43:
         case OPCODE_UNIFORM_MATRIX22D:
         case OPCODE_UNIFORM_MATRIX33D:
         case OPCODE_UNIFORM_MATRIX44D:
         case OPCODE_UNIFORM_MATRIX24D:
         case OPCODE_UNIFORM_MATRIX42D:
         case OPCODE_UNIFORM_MATRIX23D:
         case OPCODE_UNIFORM_MATRIX32D:
         case OPCODE_UNIFORM_MATRIX34D:
         case OPCODE_UNIFORM_MATRIX43D:
            free(get_pointer(&n[4]));
            break;
         case OPCODE_PROGRAM_UNIFORM_1FV:
         case OPCODE_PROGRAM_UNIFORM_2FV:
         case OPCODE_PROGRAM_UNIFORM_3FV:
         case OPCODE_PROGRAM_UNIFORM_4FV:
         case OPCODE_PROGRAM_UNIFORM_1DV:
         case OPCODE_PROGRAM_UNIFORM_2DV:
         case OPCODE_PROGRAM_UNIFORM_3DV:
         case OPCODE_PROGRAM_UNIFORM_4DV:
         case OPCODE_PROGRAM_UNIFORM_1IV:
         case OPCODE_PROGRAM_UNIFORM_2IV:
         case OPCODE_PROGRAM_UNIFORM_3IV:
         case OPCODE_PROGRAM_UNIFORM_4IV:
         case OPCODE_PROGRAM_UNIFORM_1UIV:
         case OPCODE_PROGRAM_UNIFORM_2UIV:
         case OPCODE_PROGRAM_UNIFORM_3UIV:
         case OPCODE_PROGRAM_UNIFORM_4UIV:
         case OPCODE_PROGRAM_UNIFORM_1I64V:
         case OPCODE_PROGRAM_UNIFORM_2I64V:
         case OPCODE_PROGRAM_UNIFORM_3I64V:
         case OPCODE_PROGRAM_UNIFORM_4I64V:
         case OPCODE_PROGRAM_UNIFORM_1UI64V:
         case OPCODE_PROGRAM_UNIFORM_2UI64V:
         case OPCODE_PROGRAM_UNIFORM_3UI64V:
         case OPCODE_PROGRAM_UNIFORM_4UI64V:
            free(get_pointer(&n[4]));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX22F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX33F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX44F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX24F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX42F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX23F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX32F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX34F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX43F:
         case OPCODE_PROGRAM_UNIFORM_MATRIX22D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX33D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX44D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX24D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX42D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX23D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX32D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX34D:
         case OPCODE_PROGRAM_UNIFORM_MATRIX43D:
            free(get_pointer(&n[5]));
            break;
         case OPCODE_PIXEL_MAP:
            free(get_pointer(&n[3]));
            break;
         case OPCODE_VIEWPORT_ARRAY_V:
         case OPCODE_SCISSOR_ARRAY_V:
         case OPCODE_DEPTH_ARRAY_V:
         case OPCODE_UNIFORM_SUBROUTINES:
         case OPCODE_WINDOW_RECTANGLES:
            free(get_pointer(&n[3]));
            break;
         case OPCODE_TEXTURE_IMAGE1D:
         case OPCODE_MULTITEX_IMAGE1D:
            free(get_pointer(&n[9]));
            break;
         case OPCODE_TEXTURE_IMAGE2D:
         case OPCODE_MULTITEX_IMAGE2D:
            free(get_pointer(&n[10]));
            break;
         case OPCODE_TEXTURE_IMAGE3D:
         case OPCODE_MULTITEX_IMAGE3D:
            free(get_pointer(&n[11]));
            break;
         case OPCODE_TEXTURE_SUB_IMAGE1D:
         case OPCODE_MULTITEX_SUB_IMAGE1D:
         case OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_1D:
         case OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_1D:
            free(get_pointer(&n[8]));
            break;
         case OPCODE_TEXTURE_SUB_IMAGE2D:
         case OPCODE_MULTITEX_SUB_IMAGE2D:
         case OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_2D:
         case OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_2D:
            free(get_pointer(&n[10]));
            break;
         case OPCODE_TEXTURE_SUB_IMAGE3D:
         case OPCODE_MULTITEX_SUB_IMAGE3D:
         case OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_3D:
         case OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_3D:
            free(get_pointer(&n[12]));
            break;
         case OPCODE_COMPRESSED_TEXTURE_IMAGE_1D:
         case OPCODE_COMPRESSED_MULTITEX_IMAGE_1D:
            free(get_pointer(&n[8]));
            break;
         case OPCODE_COMPRESSED_TEXTURE_IMAGE_2D:
         case OPCODE_COMPRESSED_MULTITEX_IMAGE_2D:
            free(get_pointer(&n[9]));
            break;
         case OPCODE_COMPRESSED_TEXTURE_IMAGE_3D:
         case OPCODE_COMPRESSED_MULTITEX_IMAGE_3D:
            free(get_pointer(&n[10]));
            break;
         case OPCODE_NAMED_PROGRAM_STRING:
            free(get_pointer(&n[5]));
            break;
         case OPCODE_VERTEX_LIST:
            vbo_destroy_vertex_list(ctx, (struct vbo_save_vertex_list *) &n[1]);
            break;
         case OPCODE_CONTINUE:
            n = (Node *) get_pointer(&n[1]);
            free(block);
            block = n;
            continue;
         case OPCODE_END_OF_LIST:
            free(block);
            free(dlist->Label);
            free(dlist);
            return;
         default:
            /* just increment 'n' pointer, below */
            ;
      }

      assert(InstSize[opcode] > 0);
      n += InstSize[opcode];
   }
}


/**
 * Called by _mesa_HashWalk() to check if a display list which is being
 * deleted belongs to a bitmap texture atlas.
 */
static void
check_atlas_for_deleted_list(void *data, void *userData)
{
   struct gl_bitmap_atlas *atlas = (struct gl_bitmap_atlas *) data;
   GLuint list_id = *((GLuint *) userData);  /* the list being deleted */
   const GLuint atlas_id = atlas->Id;

   /* See if the list_id falls in the range contained in this texture atlas */
   if (atlas->complete &&
       list_id >= atlas_id &&
       list_id < atlas_id + atlas->numBitmaps) {
      /* Mark the atlas as incomplete so it doesn't get used.  But don't
       * delete it yet since we don't want to try to recreate it in the next
       * glCallLists.
       */
      atlas->complete = false;
      atlas->incomplete = true;
   }
}


/**
 * Destroy a display list and remove from hash table.
 * \param list - display list number
 */
static void
destroy_list(struct gl_context *ctx, GLuint list)
{
   struct gl_display_list *dlist;

   if (list == 0)
      return;

   dlist = _mesa_lookup_list(ctx, list);
   if (!dlist)
      return;

   if (is_bitmap_list(dlist)) {
      /* If we're destroying a simple glBitmap display list, there's a
       * chance that we're destroying a bitmap image that's in a texture
       * atlas.  Examine all atlases to see if that's the case.  There's
       * usually few (if any) atlases so this isn't expensive.
       */
      _mesa_HashWalk(ctx->Shared->BitmapAtlas,
                     check_atlas_for_deleted_list, &list);
   }

   _mesa_delete_list(ctx, dlist);
   _mesa_HashRemove(ctx->Shared->DisplayList, list);
}


/**
 * Wrapper for _mesa_unpack_image/bitmap() that handles pixel buffer objects.
 * If width < 0 or height < 0 or format or type are invalid we'll just
 * return NULL.  We will not generate an error since OpenGL command
 * arguments aren't error-checked until the command is actually executed
 * (not when they're compiled).
 * But if we run out of memory, GL_OUT_OF_MEMORY will be recorded.
 */
static GLvoid *
unpack_image(struct gl_context *ctx, GLuint dimensions,
             GLsizei width, GLsizei height, GLsizei depth,
             GLenum format, GLenum type, const GLvoid * pixels,
             const struct gl_pixelstore_attrib *unpack)
{
   if (width <= 0 || height <= 0) {
      return NULL;
   }

   if (_mesa_bytes_per_pixel(format, type) < 0) {
      /* bad format and/or type */
      return NULL;
   }

   if (!unpack->BufferObj) {
      /* no PBO */
      GLvoid *image;

      image = _mesa_unpack_image(dimensions, width, height, depth,
                                 format, type, pixels, unpack);
      if (pixels && !image) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "display list construction");
      }
      return image;
   }
   else if (_mesa_validate_pbo_access(dimensions, unpack, width, height,
                                      depth, format, type, INT_MAX, pixels)) {
      const GLubyte *map, *src;
      GLvoid *image;

      map = (GLubyte *)
         ctx->Driver.MapBufferRange(ctx, 0, unpack->BufferObj->Size,
                                    GL_MAP_READ_BIT, unpack->BufferObj,
                                    MAP_INTERNAL);
      if (!map) {
         /* unable to map src buffer! */
         _mesa_error(ctx, GL_INVALID_OPERATION, "unable to map PBO");
         return NULL;
      }

      src = ADD_POINTERS(map, pixels);
      image = _mesa_unpack_image(dimensions, width, height, depth,
                                 format, type, src, unpack);

      ctx->Driver.UnmapBuffer(ctx, unpack->BufferObj, MAP_INTERNAL);

      if (!image) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "display list construction");
      }
      return image;
   }

   /* bad access! */
   _mesa_error(ctx, GL_INVALID_OPERATION, "invalid PBO access");
   return NULL;
}


/** Return copy of memory */
static void *
memdup(const void *src, GLsizei bytes)
{
   void *b = bytes >= 0 ? malloc(bytes) : NULL;
   if (b)
      memcpy(b, src, bytes);
   return b;
}


/**
 * Allocate space for a display list instruction (opcode + payload space).
 * \param opcode  the instruction opcode (OPCODE_* value)
 * \param bytes   instruction payload size (not counting opcode)
 * \param align8  does the payload need to be 8-byte aligned?
 *                This is only relevant in 64-bit environments.
 * \return pointer to allocated memory (the payload will be at pointer+1)
 */
static Node *
dlist_alloc(struct gl_context *ctx, OpCode opcode, GLuint bytes, bool align8)
{
   const GLuint numNodes = 1 + (bytes + sizeof(Node) - 1) / sizeof(Node);
   const GLuint contNodes = 1 + POINTER_DWORDS;  /* size of continue info */
   GLuint nopNode;
   Node *n;

   assert(bytes <= BLOCK_SIZE * sizeof(Node));

   if (opcode < OPCODE_EXT_0) {
      if (InstSize[opcode] == 0) {
         /* save instruction size now */
         InstSize[opcode] = numNodes;
      }
      else {
         /* make sure instruction size agrees */
         assert(numNodes == InstSize[opcode]);
      }
   }

   if (sizeof(void *) > sizeof(Node) && align8
       && ctx->ListState.CurrentPos % 2 == 0) {
      /* The opcode would get placed at node[0] and the payload would start
       * at node[1].  But the payload needs to be at an even offset (8-byte
       * multiple).
       */
      nopNode = 1;
   }
   else {
      nopNode = 0;
   }

   if (ctx->ListState.CurrentPos + nopNode + numNodes + contNodes
       > BLOCK_SIZE) {
      /* This block is full.  Allocate a new block and chain to it */
      Node *newblock;
      n = ctx->ListState.CurrentBlock + ctx->ListState.CurrentPos;
      n[0].opcode = OPCODE_CONTINUE;
      newblock = malloc(sizeof(Node) * BLOCK_SIZE);
      if (!newblock) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "Building display list");
         return NULL;
      }

      /* a fresh block should be 8-byte aligned on 64-bit systems */
      assert(((GLintptr) newblock) % sizeof(void *) == 0);

      save_pointer(&n[1], newblock);
      ctx->ListState.CurrentBlock = newblock;
      ctx->ListState.CurrentPos = 0;

      /* Display list nodes are always 4 bytes.  If we need 8-byte alignment
       * we have to insert a NOP so that the payload of the real opcode lands
       * on an even location:
       *   node[0] = OPCODE_NOP
       *   node[1] = OPCODE_x;
       *   node[2] = start of payload
       */
      nopNode = sizeof(void *) > sizeof(Node) && align8;
   }

   n = ctx->ListState.CurrentBlock + ctx->ListState.CurrentPos;
   if (nopNode) {
      assert(ctx->ListState.CurrentPos % 2 == 0); /* even value */
      n[0].opcode = OPCODE_NOP;
      n++;
      /* The "real" opcode will now be at an odd location and the payload
       * will be at an even location.
       */
   }
   ctx->ListState.CurrentPos += nopNode + numNodes;

   n[0].opcode = opcode;

   return n;
}



/**
 * Allocate space for a display list instruction.  Used by callers outside
 * this file for things like VBO vertex data.
 *
 * \param opcode  the instruction opcode (OPCODE_* value)
 * \param bytes   instruction size in bytes, not counting opcode.
 * \return pointer to the usable data area (not including the internal
 *         opcode).
 */
void *
_mesa_dlist_alloc(struct gl_context *ctx, GLuint opcode, GLuint bytes)
{
   Node *n = dlist_alloc(ctx, (OpCode) opcode, bytes, false);
   if (n)
      return n + 1;  /* return pointer to payload area, after opcode */
   else
      return NULL;
}


/**
 * Same as _mesa_dlist_alloc(), but return a pointer which is 8-byte
 * aligned in 64-bit environments, 4-byte aligned otherwise.
 */
void *
_mesa_dlist_alloc_aligned(struct gl_context *ctx, GLuint opcode, GLuint bytes)
{
   Node *n = dlist_alloc(ctx, (OpCode) opcode, bytes, true);
   if (n)
      return n + 1;  /* return pointer to payload area, after opcode */
   else
      return NULL;
}


void *
_mesa_dlist_alloc_vertex_list(struct gl_context *ctx)
{
   return _mesa_dlist_alloc_aligned(ctx, OPCODE_VERTEX_LIST,
                                    sizeof(struct vbo_save_vertex_list));
}


/**
 * Allocate space for a display list instruction.  The space is basically
 * an array of Nodes where node[0] holds the opcode, node[1] is the first
 * function parameter, node[2] is the second parameter, etc.
 *
 * \param opcode  one of OPCODE_x
 * \param nparams  number of function parameters
 * \return  pointer to start of instruction space
 */
static inline Node *
alloc_instruction(struct gl_context *ctx, OpCode opcode, GLuint nparams)
{
   return dlist_alloc(ctx, opcode, nparams * sizeof(Node), false);
}


/**
 * Called by EndList to try to reduce memory used for the list.
 */
static void
trim_list(struct gl_context *ctx)
{
   /* If the list we're ending only has one allocated block of nodes/tokens
    * and its size isn't a full block size, realloc the block to use less
    * memory.  This is important for apps that create many small display
    * lists and apps that use glXUseXFont (many lists each containing one
    * glBitmap call).
    * Note: we currently only trim display lists that allocated one block
    * of tokens.  That hits the short list case which is what we're mainly
    * concerned with.  Trimming longer lists would involve traversing the
    * linked list of blocks.
    */
   struct gl_dlist_state *list = &ctx->ListState;

   if ((list->CurrentList->Head == list->CurrentBlock) &&
       (list->CurrentPos < BLOCK_SIZE)) {
      /* There's only one block and it's not full, so realloc */
      GLuint newSize = list->CurrentPos * sizeof(Node);
      list->CurrentList->Head =
      list->CurrentBlock = realloc(list->CurrentBlock, newSize);
      if (!list->CurrentBlock) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "glEndList");
      }
   }
}



/*
 * Display List compilation functions
 */
static void GLAPIENTRY
save_Accum(GLenum op, GLfloat value)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ACCUM, 2);
   if (n) {
      n[1].e = op;
      n[2].f = value;
   }
   if (ctx->ExecuteFlag) {
      CALL_Accum(ctx->Exec, (op, value));
   }
}


static void GLAPIENTRY
save_AlphaFunc(GLenum func, GLclampf ref)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ALPHA_FUNC, 2);
   if (n) {
      n[1].e = func;
      n[2].f = (GLfloat) ref;
   }
   if (ctx->ExecuteFlag) {
      CALL_AlphaFunc(ctx->Exec, (func, ref));
   }
}


static void GLAPIENTRY
save_BindTexture(GLenum target, GLuint texture)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BIND_TEXTURE, 2);
   if (n) {
      n[1].e = target;
      n[2].ui = texture;
   }
   if (ctx->ExecuteFlag) {
      CALL_BindTexture(ctx->Exec, (target, texture));
   }
}


static void GLAPIENTRY
save_Bitmap(GLsizei width, GLsizei height,
            GLfloat xorig, GLfloat yorig,
            GLfloat xmove, GLfloat ymove, const GLubyte * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BITMAP, 6 + POINTER_DWORDS);
   if (n) {
      n[1].i = (GLint) width;
      n[2].i = (GLint) height;
      n[3].f = xorig;
      n[4].f = yorig;
      n[5].f = xmove;
      n[6].f = ymove;
      save_pointer(&n[7],
                   unpack_image(ctx, 2, width, height, 1, GL_COLOR_INDEX,
                                GL_BITMAP, pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_Bitmap(ctx->Exec, (width, height,
                              xorig, yorig, xmove, ymove, pixels));
   }
}


static void GLAPIENTRY
save_BlendEquation(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_EQUATION, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendEquation(ctx->Exec, (mode));
   }
}


static void GLAPIENTRY
save_BlendEquationSeparateEXT(GLenum modeRGB, GLenum modeA)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_EQUATION_SEPARATE, 2);
   if (n) {
      n[1].e = modeRGB;
      n[2].e = modeA;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendEquationSeparate(ctx->Exec, (modeRGB, modeA));
   }
}


static void GLAPIENTRY
save_BlendFuncSeparateEXT(GLenum sfactorRGB, GLenum dfactorRGB,
                          GLenum sfactorA, GLenum dfactorA)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_FUNC_SEPARATE, 4);
   if (n) {
      n[1].e = sfactorRGB;
      n[2].e = dfactorRGB;
      n[3].e = sfactorA;
      n[4].e = dfactorA;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendFuncSeparate(ctx->Exec,
                                (sfactorRGB, dfactorRGB, sfactorA, dfactorA));
   }
}


static void GLAPIENTRY
save_BlendFunc(GLenum srcfactor, GLenum dstfactor)
{
   save_BlendFuncSeparateEXT(srcfactor, dstfactor, srcfactor, dstfactor);
}


static void GLAPIENTRY
save_BlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_COLOR, 4);
   if (n) {
      n[1].f = red;
      n[2].f = green;
      n[3].f = blue;
      n[4].f = alpha;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendColor(ctx->Exec, (red, green, blue, alpha));
   }
}

/* GL_ARB_draw_buffers_blend */
static void GLAPIENTRY
save_BlendFuncSeparatei(GLuint buf, GLenum sfactorRGB, GLenum dfactorRGB,
                        GLenum sfactorA, GLenum dfactorA)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_FUNC_SEPARATE_I, 5);
   if (n) {
      n[1].ui = buf;
      n[2].e = sfactorRGB;
      n[3].e = dfactorRGB;
      n[4].e = sfactorA;
      n[5].e = dfactorA;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendFuncSeparateiARB(ctx->Exec, (buf, sfactorRGB, dfactorRGB,
                                             sfactorA, dfactorA));
   }
}

/* GL_ARB_draw_buffers_blend */
static void GLAPIENTRY
save_BlendFunci(GLuint buf, GLenum sfactor, GLenum dfactor)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_FUNC_I, 3);
   if (n) {
      n[1].ui = buf;
      n[2].e = sfactor;
      n[3].e = dfactor;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendFunciARB(ctx->Exec, (buf, sfactor, dfactor));
   }
}

/* GL_ARB_draw_buffers_blend */
static void GLAPIENTRY
save_BlendEquationi(GLuint buf, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_EQUATION_I, 2);
   if (n) {
      n[1].ui = buf;
      n[2].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendEquationiARB(ctx->Exec, (buf, mode));
   }
}

/* GL_ARB_draw_buffers_blend */
static void GLAPIENTRY
save_BlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeA)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLEND_EQUATION_SEPARATE_I, 3);
   if (n) {
      n[1].ui = buf;
      n[2].e = modeRGB;
      n[3].e = modeA;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlendEquationSeparateiARB(ctx->Exec, (buf, modeRGB, modeA));
   }
}


/* GL_ARB_draw_instanced. */
static void GLAPIENTRY
save_DrawArraysInstancedARB(UNUSED GLenum mode,
                            UNUSED GLint first,
                            UNUSED GLsizei count,
                            UNUSED GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawArraysInstanced() during display list compile");
}

static void GLAPIENTRY
save_DrawElementsInstancedARB(UNUSED GLenum mode,
                              UNUSED GLsizei count,
                              UNUSED GLenum type,
                              UNUSED const GLvoid *indices,
                              UNUSED GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawElementsInstanced() during display list compile");
}

static void GLAPIENTRY
save_DrawElementsInstancedBaseVertexARB(UNUSED GLenum mode,
                                        UNUSED GLsizei count,
                                        UNUSED GLenum type,
                                        UNUSED const GLvoid *indices,
                                        UNUSED GLsizei primcount,
                                        UNUSED GLint basevertex)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawElementsInstancedBaseVertex() during display list compile");
}

/* GL_ARB_base_instance. */
static void GLAPIENTRY
save_DrawArraysInstancedBaseInstance(UNUSED GLenum mode,
                                     UNUSED GLint first,
                                     UNUSED GLsizei count,
                                     UNUSED GLsizei primcount,
                                     UNUSED GLuint baseinstance)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawArraysInstancedBaseInstance() during display list compile");
}

static void APIENTRY
save_DrawElementsInstancedBaseInstance(UNUSED GLenum mode,
                                       UNUSED GLsizei count,
                                       UNUSED GLenum type,
                                       UNUSED const void *indices,
                                       UNUSED GLsizei primcount,
                                       UNUSED GLuint baseinstance)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawElementsInstancedBaseInstance() during display list compile");
}

static void APIENTRY
save_DrawElementsInstancedBaseVertexBaseInstance(UNUSED GLenum mode,
                                                 UNUSED GLsizei count,
                                                 UNUSED GLenum type,
                                                 UNUSED const void *indices,
                                                 UNUSED GLsizei primcount,
                                                 UNUSED GLint basevertex,
                                                 UNUSED GLuint baseinstance)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawElementsInstancedBaseVertexBaseInstance() during display list compile");
}

static void APIENTRY
save_DrawArraysIndirect(UNUSED GLenum mode,
                        UNUSED const void *indirect)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawArraysIndirect() during display list compile");
}

static void APIENTRY
save_DrawElementsIndirect(UNUSED GLenum mode,
                          UNUSED GLenum type,
                          UNUSED const void *indirect)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDrawElementsIndirect() during display list compile");
}

static void APIENTRY
save_MultiDrawArraysIndirect(UNUSED GLenum mode,
                             UNUSED const void *indirect,
                             UNUSED GLsizei primcount,
                             UNUSED GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glMultiDrawArraysIndirect() during display list compile");
}

static void APIENTRY
save_MultiDrawElementsIndirect(UNUSED GLenum mode,
                               UNUSED GLenum type,
                               UNUSED const void *indirect,
                               UNUSED GLsizei primcount,
                               UNUSED GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glMultiDrawElementsIndirect() during display list compile");
}

/**
 * While building a display list we cache some OpenGL state.
 * Under some circumstances we need to invalidate that state (immediately
 * when we start compiling a list, or after glCallList(s)).
 */
static void
invalidate_saved_current_state(struct gl_context *ctx)
{
   GLint i;

   for (i = 0; i < VERT_ATTRIB_MAX; i++)
      ctx->ListState.ActiveAttribSize[i] = 0;

   for (i = 0; i < MAT_ATTRIB_MAX; i++)
      ctx->ListState.ActiveMaterialSize[i] = 0;

   memset(&ctx->ListState.Current, 0, sizeof ctx->ListState.Current);

   ctx->Driver.CurrentSavePrimitive = PRIM_UNKNOWN;
}


static void GLAPIENTRY
save_CallList(GLuint list)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);

   n = alloc_instruction(ctx, OPCODE_CALL_LIST, 1);
   if (n) {
      n[1].ui = list;
   }

   /* After this, we don't know what state we're in.  Invalidate all
    * cached information previously gathered:
    */
   invalidate_saved_current_state( ctx );

   if (ctx->ExecuteFlag) {
      _mesa_CallList(list);
   }
}


static void GLAPIENTRY
save_CallLists(GLsizei num, GLenum type, const GLvoid * lists)
{
   GET_CURRENT_CONTEXT(ctx);
   unsigned type_size;
   Node *n;
   void *lists_copy;

   SAVE_FLUSH_VERTICES(ctx);

   switch (type) {
   case GL_BYTE:
   case GL_UNSIGNED_BYTE:
      type_size = 1;
      break;
   case GL_SHORT:
   case GL_UNSIGNED_SHORT:
   case GL_2_BYTES:
      type_size = 2;
      break;
   case GL_3_BYTES:
      type_size = 3;
      break;
   case GL_INT:
   case GL_UNSIGNED_INT:
   case GL_FLOAT:
   case GL_4_BYTES:
      type_size = 4;
      break;
   default:
      type_size = 0;
   }

   if (num > 0 && type_size > 0) {
      /* create a copy of the array of list IDs to save in the display list */
      lists_copy = memdup(lists, num * type_size);
   } else {
      lists_copy = NULL;
   }

   n = alloc_instruction(ctx, OPCODE_CALL_LISTS, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = num;
      n[2].e = type;
      save_pointer(&n[3], lists_copy);
   }

   /* After this, we don't know what state we're in.  Invalidate all
    * cached information previously gathered:
    */
   invalidate_saved_current_state( ctx );

   if (ctx->ExecuteFlag) {
      CALL_CallLists(ctx->Exec, (num, type, lists));
   }
}


static void GLAPIENTRY
save_Clear(GLbitfield mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR, 1);
   if (n) {
      n[1].bf = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_Clear(ctx->Exec, (mask));
   }
}


static void GLAPIENTRY
save_ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint *value)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_BUFFER_IV, 6);
   if (n) {
      n[1].e = buffer;
      n[2].i = drawbuffer;
      n[3].i = value[0];
      if (buffer == GL_COLOR) {
         n[4].i = value[1];
         n[5].i = value[2];
         n[6].i = value[3];
      }
      else {
         n[4].i = 0;
         n[5].i = 0;
         n[6].i = 0;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearBufferiv(ctx->Exec, (buffer, drawbuffer, value));
   }
}


static void GLAPIENTRY
save_ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint *value)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_BUFFER_UIV, 6);
   if (n) {
      n[1].e = buffer;
      n[2].i = drawbuffer;
      n[3].ui = value[0];
      if (buffer == GL_COLOR) {
         n[4].ui = value[1];
         n[5].ui = value[2];
         n[6].ui = value[3];
      }
      else {
         n[4].ui = 0;
         n[5].ui = 0;
         n[6].ui = 0;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearBufferuiv(ctx->Exec, (buffer, drawbuffer, value));
   }
}


static void GLAPIENTRY
save_ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_BUFFER_FV, 6);
   if (n) {
      n[1].e = buffer;
      n[2].i = drawbuffer;
      n[3].f = value[0];
      if (buffer == GL_COLOR) {
         n[4].f = value[1];
         n[5].f = value[2];
         n[6].f = value[3];
      }
      else {
         n[4].f = 0.0F;
         n[5].f = 0.0F;
         n[6].f = 0.0F;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearBufferfv(ctx->Exec, (buffer, drawbuffer, value));
   }
}


static void GLAPIENTRY
save_ClearBufferfi(GLenum buffer, GLint drawbuffer,
                   GLfloat depth, GLint stencil)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_BUFFER_FI, 4);
   if (n) {
      n[1].e = buffer;
      n[2].i = drawbuffer;
      n[3].f = depth;
      n[4].i = stencil;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearBufferfi(ctx->Exec, (buffer, drawbuffer, depth, stencil));
   }
}


static void GLAPIENTRY
save_ClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_ACCUM, 4);
   if (n) {
      n[1].f = red;
      n[2].f = green;
      n[3].f = blue;
      n[4].f = alpha;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearAccum(ctx->Exec, (red, green, blue, alpha));
   }
}


static void GLAPIENTRY
save_ClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_COLOR, 4);
   if (n) {
      n[1].f = red;
      n[2].f = green;
      n[3].f = blue;
      n[4].f = alpha;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearColor(ctx->Exec, (red, green, blue, alpha));
   }
}


static void GLAPIENTRY
save_ClearDepth(GLclampd depth)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_DEPTH, 1);
   if (n) {
      n[1].f = (GLfloat) depth;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearDepth(ctx->Exec, (depth));
   }
}


static void GLAPIENTRY
save_ClearIndex(GLfloat c)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_INDEX, 1);
   if (n) {
      n[1].f = c;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearIndex(ctx->Exec, (c));
   }
}


static void GLAPIENTRY
save_ClearStencil(GLint s)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEAR_STENCIL, 1);
   if (n) {
      n[1].i = s;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearStencil(ctx->Exec, (s));
   }
}


static void GLAPIENTRY
save_ClipPlane(GLenum plane, const GLdouble * equ)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLIP_PLANE, 5);
   if (n) {
      n[1].e = plane;
      n[2].f = (GLfloat) equ[0];
      n[3].f = (GLfloat) equ[1];
      n[4].f = (GLfloat) equ[2];
      n[5].f = (GLfloat) equ[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_ClipPlane(ctx->Exec, (plane, equ));
   }
}



static void GLAPIENTRY
save_ColorMask(GLboolean red, GLboolean green,
               GLboolean blue, GLboolean alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COLOR_MASK, 4);
   if (n) {
      n[1].b = red;
      n[2].b = green;
      n[3].b = blue;
      n[4].b = alpha;
   }
   if (ctx->ExecuteFlag) {
      CALL_ColorMask(ctx->Exec, (red, green, blue, alpha));
   }
}


static void GLAPIENTRY
save_ColorMaskIndexed(GLuint buf, GLboolean red, GLboolean green,
                      GLboolean blue, GLboolean alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COLOR_MASK_INDEXED, 5);
   if (n) {
      n[1].ui = buf;
      n[2].b = red;
      n[3].b = green;
      n[4].b = blue;
      n[5].b = alpha;
   }
   if (ctx->ExecuteFlag) {
      /*CALL_ColorMaski(ctx->Exec, (buf, red, green, blue, alpha));*/
   }
}


static void GLAPIENTRY
save_ColorMaterial(GLenum face, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COLOR_MATERIAL, 2);
   if (n) {
      n[1].e = face;
      n[2].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_ColorMaterial(ctx->Exec, (face, mode));
   }
}


static void GLAPIENTRY
save_CopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_PIXELS, 5);
   if (n) {
      n[1].i = x;
      n[2].i = y;
      n[3].i = (GLint) width;
      n[4].i = (GLint) height;
      n[5].e = type;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyPixels(ctx->Exec, (x, y, width, height, type));
   }
}



static void GLAPIENTRY
save_CopyTexImage1D(GLenum target, GLint level, GLenum internalformat,
                    GLint x, GLint y, GLsizei width, GLint border)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEX_IMAGE1D, 7);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].e = internalformat;
      n[4].i = x;
      n[5].i = y;
      n[6].i = width;
      n[7].i = border;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTexImage1D(ctx->Exec, (target, level, internalformat,
                                      x, y, width, border));
   }
}


static void GLAPIENTRY
save_CopyTexImage2D(GLenum target, GLint level,
                    GLenum internalformat,
                    GLint x, GLint y, GLsizei width,
                    GLsizei height, GLint border)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEX_IMAGE2D, 8);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].e = internalformat;
      n[4].i = x;
      n[5].i = y;
      n[6].i = width;
      n[7].i = height;
      n[8].i = border;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTexImage2D(ctx->Exec, (target, level, internalformat,
                                      x, y, width, height, border));
   }
}



static void GLAPIENTRY
save_CopyTexSubImage1D(GLenum target, GLint level,
                       GLint xoffset, GLint x, GLint y, GLsizei width)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEX_SUB_IMAGE1D, 6);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = x;
      n[5].i = y;
      n[6].i = width;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTexSubImage1D(ctx->Exec,
                             (target, level, xoffset, x, y, width));
   }
}


static void GLAPIENTRY
save_CopyTexSubImage2D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset,
                       GLint x, GLint y, GLsizei width, GLint height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEX_SUB_IMAGE2D, 8);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = yoffset;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
      n[8].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTexSubImage2D(ctx->Exec, (target, level, xoffset, yoffset,
                                         x, y, width, height));
   }
}


static void GLAPIENTRY
save_CopyTexSubImage3D(GLenum target, GLint level,
                       GLint xoffset, GLint yoffset, GLint zoffset,
                       GLint x, GLint y, GLsizei width, GLint height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEX_SUB_IMAGE3D, 9);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = yoffset;
      n[5].i = zoffset;
      n[6].i = x;
      n[7].i = y;
      n[8].i = width;
      n[9].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTexSubImage3D(ctx->Exec, (target, level,
                                         xoffset, yoffset, zoffset,
                                         x, y, width, height));
   }
}


static void GLAPIENTRY
save_CullFace(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CULL_FACE, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_CullFace(ctx->Exec, (mode));
   }
}


static void GLAPIENTRY
save_DepthFunc(GLenum func)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DEPTH_FUNC, 1);
   if (n) {
      n[1].e = func;
   }
   if (ctx->ExecuteFlag) {
      CALL_DepthFunc(ctx->Exec, (func));
   }
}


static void GLAPIENTRY
save_DepthMask(GLboolean mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DEPTH_MASK, 1);
   if (n) {
      n[1].b = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_DepthMask(ctx->Exec, (mask));
   }
}


static void GLAPIENTRY
save_DepthRange(GLclampd nearval, GLclampd farval)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DEPTH_RANGE, 2);
   if (n) {
      n[1].f = (GLfloat) nearval;
      n[2].f = (GLfloat) farval;
   }
   if (ctx->ExecuteFlag) {
      CALL_DepthRange(ctx->Exec, (nearval, farval));
   }
}


static void GLAPIENTRY
save_Disable(GLenum cap)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DISABLE, 1);
   if (n) {
      n[1].e = cap;
   }
   if (ctx->ExecuteFlag) {
      CALL_Disable(ctx->Exec, (cap));
   }
}


static void GLAPIENTRY
save_DisableIndexed(GLuint index, GLenum cap)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DISABLE_INDEXED, 2);
   if (n) {
      n[1].ui = index;
      n[2].e = cap;
   }
   if (ctx->ExecuteFlag) {
      CALL_Disablei(ctx->Exec, (index, cap));
   }
}


static void GLAPIENTRY
save_DrawBuffer(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DRAW_BUFFER, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawBuffer(ctx->Exec, (mode));
   }
}


static void GLAPIENTRY
save_DrawPixels(GLsizei width, GLsizei height,
                GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_DRAW_PIXELS, 4 + POINTER_DWORDS);
   if (n) {
      n[1].i = width;
      n[2].i = height;
      n[3].e = format;
      n[4].e = type;
      save_pointer(&n[5],
                   unpack_image(ctx, 2, width, height, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawPixels(ctx->Exec, (width, height, format, type, pixels));
   }
}



static void GLAPIENTRY
save_Enable(GLenum cap)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ENABLE, 1);
   if (n) {
      n[1].e = cap;
   }
   if (ctx->ExecuteFlag) {
      CALL_Enable(ctx->Exec, (cap));
   }
}



static void GLAPIENTRY
save_EnableIndexed(GLuint index, GLenum cap)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ENABLE_INDEXED, 2);
   if (n) {
      n[1].ui = index;
      n[2].e = cap;
   }
   if (ctx->ExecuteFlag) {
      CALL_Enablei(ctx->Exec, (index, cap));
   }
}



static void GLAPIENTRY
save_EvalMesh1(GLenum mode, GLint i1, GLint i2)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_EVALMESH1, 3);
   if (n) {
      n[1].e = mode;
      n[2].i = i1;
      n[3].i = i2;
   }
   if (ctx->ExecuteFlag) {
      CALL_EvalMesh1(ctx->Exec, (mode, i1, i2));
   }
}


static void GLAPIENTRY
save_EvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_EVALMESH2, 5);
   if (n) {
      n[1].e = mode;
      n[2].i = i1;
      n[3].i = i2;
      n[4].i = j1;
      n[5].i = j2;
   }
   if (ctx->ExecuteFlag) {
      CALL_EvalMesh2(ctx->Exec, (mode, i1, i2, j1, j2));
   }
}




static void GLAPIENTRY
save_Fogfv(GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_FOG, 5);
   if (n) {
      n[1].e = pname;
      n[2].f = params[0];
      n[3].f = params[1];
      n[4].f = params[2];
      n[5].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_Fogfv(ctx->Exec, (pname, params));
   }
}


static void GLAPIENTRY
save_Fogf(GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_Fogfv(pname, parray);
}


static void GLAPIENTRY
save_Fogiv(GLenum pname, const GLint *params)
{
   GLfloat p[4];
   switch (pname) {
   case GL_FOG_MODE:
   case GL_FOG_DENSITY:
   case GL_FOG_START:
   case GL_FOG_END:
   case GL_FOG_INDEX:
   case GL_FOG_COORDINATE_SOURCE:
      p[0] = (GLfloat) *params;
      p[1] = 0.0f;
      p[2] = 0.0f;
      p[3] = 0.0f;
      break;
   case GL_FOG_COLOR:
      p[0] = INT_TO_FLOAT(params[0]);
      p[1] = INT_TO_FLOAT(params[1]);
      p[2] = INT_TO_FLOAT(params[2]);
      p[3] = INT_TO_FLOAT(params[3]);
      break;
   default:
      /* Error will be caught later in gl_Fogfv */
      ASSIGN_4V(p, 0.0F, 0.0F, 0.0F, 0.0F);
   }
   save_Fogfv(pname, p);
}


static void GLAPIENTRY
save_Fogi(GLenum pname, GLint param)
{
   GLint parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0;
   save_Fogiv(pname, parray);
}


static void GLAPIENTRY
save_FrontFace(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_FRONT_FACE, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_FrontFace(ctx->Exec, (mode));
   }
}


static void GLAPIENTRY
save_Frustum(GLdouble left, GLdouble right,
             GLdouble bottom, GLdouble top, GLdouble nearval, GLdouble farval)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_FRUSTUM, 6);
   if (n) {
      n[1].f = (GLfloat) left;
      n[2].f = (GLfloat) right;
      n[3].f = (GLfloat) bottom;
      n[4].f = (GLfloat) top;
      n[5].f = (GLfloat) nearval;
      n[6].f = (GLfloat) farval;
   }
   if (ctx->ExecuteFlag) {
      CALL_Frustum(ctx->Exec, (left, right, bottom, top, nearval, farval));
   }
}


static void GLAPIENTRY
save_Hint(GLenum target, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_HINT, 2);
   if (n) {
      n[1].e = target;
      n[2].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_Hint(ctx->Exec, (target, mode));
   }
}


static void GLAPIENTRY
save_IndexMask(GLuint mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_INDEX_MASK, 1);
   if (n) {
      n[1].ui = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_IndexMask(ctx->Exec, (mask));
   }
}


static void GLAPIENTRY
save_InitNames(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_INIT_NAMES, 0);
   if (ctx->ExecuteFlag) {
      CALL_InitNames(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_Lightfv(GLenum light, GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LIGHT, 6);
   if (n) {
      GLint i, nParams;
      n[1].e = light;
      n[2].e = pname;
      switch (pname) {
      case GL_AMBIENT:
         nParams = 4;
         break;
      case GL_DIFFUSE:
         nParams = 4;
         break;
      case GL_SPECULAR:
         nParams = 4;
         break;
      case GL_POSITION:
         nParams = 4;
         break;
      case GL_SPOT_DIRECTION:
         nParams = 3;
         break;
      case GL_SPOT_EXPONENT:
         nParams = 1;
         break;
      case GL_SPOT_CUTOFF:
         nParams = 1;
         break;
      case GL_CONSTANT_ATTENUATION:
         nParams = 1;
         break;
      case GL_LINEAR_ATTENUATION:
         nParams = 1;
         break;
      case GL_QUADRATIC_ATTENUATION:
         nParams = 1;
         break;
      default:
         nParams = 0;
      }
      for (i = 0; i < nParams; i++) {
         n[3 + i].f = params[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_Lightfv(ctx->Exec, (light, pname, params));
   }
}


static void GLAPIENTRY
save_Lightf(GLenum light, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_Lightfv(light, pname, parray);
}


static void GLAPIENTRY
save_Lightiv(GLenum light, GLenum pname, const GLint *params)
{
   GLfloat fparam[4];
   switch (pname) {
   case GL_AMBIENT:
   case GL_DIFFUSE:
   case GL_SPECULAR:
      fparam[0] = INT_TO_FLOAT(params[0]);
      fparam[1] = INT_TO_FLOAT(params[1]);
      fparam[2] = INT_TO_FLOAT(params[2]);
      fparam[3] = INT_TO_FLOAT(params[3]);
      break;
   case GL_POSITION:
      fparam[0] = (GLfloat) params[0];
      fparam[1] = (GLfloat) params[1];
      fparam[2] = (GLfloat) params[2];
      fparam[3] = (GLfloat) params[3];
      break;
   case GL_SPOT_DIRECTION:
      fparam[0] = (GLfloat) params[0];
      fparam[1] = (GLfloat) params[1];
      fparam[2] = (GLfloat) params[2];
      break;
   case GL_SPOT_EXPONENT:
   case GL_SPOT_CUTOFF:
   case GL_CONSTANT_ATTENUATION:
   case GL_LINEAR_ATTENUATION:
   case GL_QUADRATIC_ATTENUATION:
      fparam[0] = (GLfloat) params[0];
      break;
   default:
      /* error will be caught later in gl_Lightfv */
      ;
   }
   save_Lightfv(light, pname, fparam);
}


static void GLAPIENTRY
save_Lighti(GLenum light, GLenum pname, GLint param)
{
   GLint parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0;
   save_Lightiv(light, pname, parray);
}


static void GLAPIENTRY
save_LightModelfv(GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LIGHT_MODEL, 5);
   if (n) {
      n[1].e = pname;
      n[2].f = params[0];
      n[3].f = params[1];
      n[4].f = params[2];
      n[5].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_LightModelfv(ctx->Exec, (pname, params));
   }
}


static void GLAPIENTRY
save_LightModelf(GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_LightModelfv(pname, parray);
}


static void GLAPIENTRY
save_LightModeliv(GLenum pname, const GLint *params)
{
   GLfloat fparam[4];
   switch (pname) {
   case GL_LIGHT_MODEL_AMBIENT:
      fparam[0] = INT_TO_FLOAT(params[0]);
      fparam[1] = INT_TO_FLOAT(params[1]);
      fparam[2] = INT_TO_FLOAT(params[2]);
      fparam[3] = INT_TO_FLOAT(params[3]);
      break;
   case GL_LIGHT_MODEL_LOCAL_VIEWER:
   case GL_LIGHT_MODEL_TWO_SIDE:
   case GL_LIGHT_MODEL_COLOR_CONTROL:
      fparam[0] = (GLfloat) params[0];
      fparam[1] = 0.0F;
      fparam[2] = 0.0F;
      fparam[3] = 0.0F;
      break;
   default:
      /* Error will be caught later in gl_LightModelfv */
      ASSIGN_4V(fparam, 0.0F, 0.0F, 0.0F, 0.0F);
   }
   save_LightModelfv(pname, fparam);
}


static void GLAPIENTRY
save_LightModeli(GLenum pname, GLint param)
{
   GLint parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0;
   save_LightModeliv(pname, parray);
}


static void GLAPIENTRY
save_LineStipple(GLint factor, GLushort pattern)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LINE_STIPPLE, 2);
   if (n) {
      n[1].i = factor;
      n[2].us = pattern;
   }
   if (ctx->ExecuteFlag) {
      CALL_LineStipple(ctx->Exec, (factor, pattern));
   }
}


static void GLAPIENTRY
save_LineWidth(GLfloat width)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LINE_WIDTH, 1);
   if (n) {
      n[1].f = width;
   }
   if (ctx->ExecuteFlag) {
      CALL_LineWidth(ctx->Exec, (width));
   }
}


static void GLAPIENTRY
save_ListBase(GLuint base)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LIST_BASE, 1);
   if (n) {
      n[1].ui = base;
   }
   if (ctx->ExecuteFlag) {
      CALL_ListBase(ctx->Exec, (base));
   }
}


static void GLAPIENTRY
save_LoadIdentity(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_LOAD_IDENTITY, 0);
   if (ctx->ExecuteFlag) {
      CALL_LoadIdentity(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_LoadMatrixf(const GLfloat * m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LOAD_MATRIX, 16);
   if (n) {
      GLuint i;
      for (i = 0; i < 16; i++) {
         n[1 + i].f = m[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_LoadMatrixf(ctx->Exec, (m));
   }
}


static void GLAPIENTRY
save_LoadMatrixd(const GLdouble * m)
{
   GLfloat f[16];
   GLint i;
   for (i = 0; i < 16; i++) {
      f[i] = (GLfloat) m[i];
   }
   save_LoadMatrixf(f);
}


static void GLAPIENTRY
save_LoadName(GLuint name)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LOAD_NAME, 1);
   if (n) {
      n[1].ui = name;
   }
   if (ctx->ExecuteFlag) {
      CALL_LoadName(ctx->Exec, (name));
   }
}


static void GLAPIENTRY
save_LogicOp(GLenum opcode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_LOGIC_OP, 1);
   if (n) {
      n[1].e = opcode;
   }
   if (ctx->ExecuteFlag) {
      CALL_LogicOp(ctx->Exec, (opcode));
   }
}


static void GLAPIENTRY
save_Map1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride,
           GLint order, const GLdouble * points)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MAP1, 5 + POINTER_DWORDS);
   if (n) {
      GLfloat *pnts = _mesa_copy_map_points1d(target, stride, order, points);
      n[1].e = target;
      n[2].f = (GLfloat) u1;
      n[3].f = (GLfloat) u2;
      n[4].i = _mesa_evaluator_components(target);      /* stride */
      n[5].i = order;
      save_pointer(&n[6], pnts);
   }
   if (ctx->ExecuteFlag) {
      CALL_Map1d(ctx->Exec, (target, u1, u2, stride, order, points));
   }
}

static void GLAPIENTRY
save_Map1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride,
           GLint order, const GLfloat * points)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MAP1, 5 + POINTER_DWORDS);
   if (n) {
      GLfloat *pnts = _mesa_copy_map_points1f(target, stride, order, points);
      n[1].e = target;
      n[2].f = u1;
      n[3].f = u2;
      n[4].i = _mesa_evaluator_components(target);      /* stride */
      n[5].i = order;
      save_pointer(&n[6], pnts);
   }
   if (ctx->ExecuteFlag) {
      CALL_Map1f(ctx->Exec, (target, u1, u2, stride, order, points));
   }
}


static void GLAPIENTRY
save_Map2d(GLenum target,
           GLdouble u1, GLdouble u2, GLint ustride, GLint uorder,
           GLdouble v1, GLdouble v2, GLint vstride, GLint vorder,
           const GLdouble * points)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MAP2, 9 + POINTER_DWORDS);
   if (n) {
      GLfloat *pnts = _mesa_copy_map_points2d(target, ustride, uorder,
                                              vstride, vorder, points);
      n[1].e = target;
      n[2].f = (GLfloat) u1;
      n[3].f = (GLfloat) u2;
      n[4].f = (GLfloat) v1;
      n[5].f = (GLfloat) v2;
      /* XXX verify these strides are correct */
      n[6].i = _mesa_evaluator_components(target) * vorder;     /*ustride */
      n[7].i = _mesa_evaluator_components(target);      /*vstride */
      n[8].i = uorder;
      n[9].i = vorder;
      save_pointer(&n[10], pnts);
   }
   if (ctx->ExecuteFlag) {
      CALL_Map2d(ctx->Exec, (target,
                             u1, u2, ustride, uorder,
                             v1, v2, vstride, vorder, points));
   }
}


static void GLAPIENTRY
save_Map2f(GLenum target,
           GLfloat u1, GLfloat u2, GLint ustride, GLint uorder,
           GLfloat v1, GLfloat v2, GLint vstride, GLint vorder,
           const GLfloat * points)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MAP2, 9 + POINTER_DWORDS);
   if (n) {
      GLfloat *pnts = _mesa_copy_map_points2f(target, ustride, uorder,
                                              vstride, vorder, points);
      n[1].e = target;
      n[2].f = u1;
      n[3].f = u2;
      n[4].f = v1;
      n[5].f = v2;
      /* XXX verify these strides are correct */
      n[6].i = _mesa_evaluator_components(target) * vorder;     /*ustride */
      n[7].i = _mesa_evaluator_components(target);      /*vstride */
      n[8].i = uorder;
      n[9].i = vorder;
      save_pointer(&n[10], pnts);
   }
   if (ctx->ExecuteFlag) {
      CALL_Map2f(ctx->Exec, (target, u1, u2, ustride, uorder,
                             v1, v2, vstride, vorder, points));
   }
}


static void GLAPIENTRY
save_MapGrid1f(GLint un, GLfloat u1, GLfloat u2)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MAPGRID1, 3);
   if (n) {
      n[1].i = un;
      n[2].f = u1;
      n[3].f = u2;
   }
   if (ctx->ExecuteFlag) {
      CALL_MapGrid1f(ctx->Exec, (un, u1, u2));
   }
}


static void GLAPIENTRY
save_MapGrid1d(GLint un, GLdouble u1, GLdouble u2)
{
   save_MapGrid1f(un, (GLfloat) u1, (GLfloat) u2);
}


static void GLAPIENTRY
save_MapGrid2f(GLint un, GLfloat u1, GLfloat u2,
               GLint vn, GLfloat v1, GLfloat v2)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MAPGRID2, 6);
   if (n) {
      n[1].i = un;
      n[2].f = u1;
      n[3].f = u2;
      n[4].i = vn;
      n[5].f = v1;
      n[6].f = v2;
   }
   if (ctx->ExecuteFlag) {
      CALL_MapGrid2f(ctx->Exec, (un, u1, u2, vn, v1, v2));
   }
}



static void GLAPIENTRY
save_MapGrid2d(GLint un, GLdouble u1, GLdouble u2,
               GLint vn, GLdouble v1, GLdouble v2)
{
   save_MapGrid2f(un, (GLfloat) u1, (GLfloat) u2,
                  vn, (GLfloat) v1, (GLfloat) v2);
}


static void GLAPIENTRY
save_MatrixMode(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_MODE, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixMode(ctx->Exec, (mode));
   }
}


static void GLAPIENTRY
save_MultMatrixf(const GLfloat * m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MULT_MATRIX, 16);
   if (n) {
      GLuint i;
      for (i = 0; i < 16; i++) {
         n[1 + i].f = m[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_MultMatrixf(ctx->Exec, (m));
   }
}


static void GLAPIENTRY
save_MultMatrixd(const GLdouble * m)
{
   GLfloat f[16];
   GLint i;
   for (i = 0; i < 16; i++) {
      f[i] = (GLfloat) m[i];
   }
   save_MultMatrixf(f);
}


static void GLAPIENTRY
save_NewList(GLuint name, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   /* It's an error to call this function while building a display list */
   _mesa_error(ctx, GL_INVALID_OPERATION, "glNewList");
   (void) name;
   (void) mode;
}



static void GLAPIENTRY
save_Ortho(GLdouble left, GLdouble right,
           GLdouble bottom, GLdouble top, GLdouble nearval, GLdouble farval)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ORTHO, 6);
   if (n) {
      n[1].f = (GLfloat) left;
      n[2].f = (GLfloat) right;
      n[3].f = (GLfloat) bottom;
      n[4].f = (GLfloat) top;
      n[5].f = (GLfloat) nearval;
      n[6].f = (GLfloat) farval;
   }
   if (ctx->ExecuteFlag) {
      CALL_Ortho(ctx->Exec, (left, right, bottom, top, nearval, farval));
   }
}


static void GLAPIENTRY
save_PatchParameteri(GLenum pname, const GLint value)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PATCH_PARAMETER_I, 2);
   if (n) {
      n[1].e = pname;
      n[2].i = value;
   }
   if (ctx->ExecuteFlag) {
      CALL_PatchParameteri(ctx->Exec, (pname, value));
   }
}


static void GLAPIENTRY
save_PatchParameterfv(GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   if (pname == GL_PATCH_DEFAULT_OUTER_LEVEL) {
      n = alloc_instruction(ctx, OPCODE_PATCH_PARAMETER_FV_OUTER, 5);
   } else {
      assert(pname == GL_PATCH_DEFAULT_INNER_LEVEL);
      n = alloc_instruction(ctx, OPCODE_PATCH_PARAMETER_FV_INNER, 3);
   }
   if (n) {
      n[1].e = pname;
      if (pname == GL_PATCH_DEFAULT_OUTER_LEVEL) {
         n[2].f = params[0];
         n[3].f = params[1];
         n[4].f = params[2];
         n[5].f = params[3];
      } else {
         n[2].f = params[0];
         n[3].f = params[1];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_PatchParameterfv(ctx->Exec, (pname, params));
   }
}


static void GLAPIENTRY
save_PixelMapfv(GLenum map, GLint mapsize, const GLfloat *values)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PIXEL_MAP, 2 + POINTER_DWORDS);
   if (n) {
      n[1].e = map;
      n[2].i = mapsize;
      save_pointer(&n[3], memdup(values, mapsize * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_PixelMapfv(ctx->Exec, (map, mapsize, values));
   }
}


static void GLAPIENTRY
save_PixelMapuiv(GLenum map, GLint mapsize, const GLuint *values)
{
   GLfloat fvalues[MAX_PIXEL_MAP_TABLE];
   GLint i;
   if (map == GL_PIXEL_MAP_I_TO_I || map == GL_PIXEL_MAP_S_TO_S) {
      for (i = 0; i < mapsize; i++) {
         fvalues[i] = (GLfloat) values[i];
      }
   }
   else {
      for (i = 0; i < mapsize; i++) {
         fvalues[i] = UINT_TO_FLOAT(values[i]);
      }
   }
   save_PixelMapfv(map, mapsize, fvalues);
}


static void GLAPIENTRY
save_PixelMapusv(GLenum map, GLint mapsize, const GLushort *values)
{
   GLfloat fvalues[MAX_PIXEL_MAP_TABLE];
   GLint i;
   if (map == GL_PIXEL_MAP_I_TO_I || map == GL_PIXEL_MAP_S_TO_S) {
      for (i = 0; i < mapsize; i++) {
         fvalues[i] = (GLfloat) values[i];
      }
   }
   else {
      for (i = 0; i < mapsize; i++) {
         fvalues[i] = USHORT_TO_FLOAT(values[i]);
      }
   }
   save_PixelMapfv(map, mapsize, fvalues);
}


static void GLAPIENTRY
save_PixelTransferf(GLenum pname, GLfloat param)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PIXEL_TRANSFER, 2);
   if (n) {
      n[1].e = pname;
      n[2].f = param;
   }
   if (ctx->ExecuteFlag) {
      CALL_PixelTransferf(ctx->Exec, (pname, param));
   }
}


static void GLAPIENTRY
save_PixelTransferi(GLenum pname, GLint param)
{
   save_PixelTransferf(pname, (GLfloat) param);
}


static void GLAPIENTRY
save_PixelZoom(GLfloat xfactor, GLfloat yfactor)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PIXEL_ZOOM, 2);
   if (n) {
      n[1].f = xfactor;
      n[2].f = yfactor;
   }
   if (ctx->ExecuteFlag) {
      CALL_PixelZoom(ctx->Exec, (xfactor, yfactor));
   }
}


static void GLAPIENTRY
save_PointParameterfvEXT(GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_POINT_PARAMETERS, 4);
   if (n) {
      n[1].e = pname;
      n[2].f = params[0];
      n[3].f = params[1];
      n[4].f = params[2];
   }
   if (ctx->ExecuteFlag) {
      CALL_PointParameterfv(ctx->Exec, (pname, params));
   }
}


static void GLAPIENTRY
save_PointParameterfEXT(GLenum pname, GLfloat param)
{
   GLfloat parray[3];
   parray[0] = param;
   parray[1] = parray[2] = 0.0F;
   save_PointParameterfvEXT(pname, parray);
}

static void GLAPIENTRY
save_PointParameteri(GLenum pname, GLint param)
{
   GLfloat parray[3];
   parray[0] = (GLfloat) param;
   parray[1] = parray[2] = 0.0F;
   save_PointParameterfvEXT(pname, parray);
}

static void GLAPIENTRY
save_PointParameteriv(GLenum pname, const GLint * param)
{
   GLfloat parray[3];
   parray[0] = (GLfloat) param[0];
   parray[1] = parray[2] = 0.0F;
   save_PointParameterfvEXT(pname, parray);
}


static void GLAPIENTRY
save_PointSize(GLfloat size)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_POINT_SIZE, 1);
   if (n) {
      n[1].f = size;
   }
   if (ctx->ExecuteFlag) {
      CALL_PointSize(ctx->Exec, (size));
   }
}


static void GLAPIENTRY
save_PolygonMode(GLenum face, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_POLYGON_MODE, 2);
   if (n) {
      n[1].e = face;
      n[2].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_PolygonMode(ctx->Exec, (face, mode));
   }
}


static void GLAPIENTRY
save_PolygonStipple(const GLubyte * pattern)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_POLYGON_STIPPLE, POINTER_DWORDS);
   if (n) {
      save_pointer(&n[1],
                   unpack_image(ctx, 2, 32, 32, 1, GL_COLOR_INDEX, GL_BITMAP,
                                pattern, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_PolygonStipple(ctx->Exec, ((GLubyte *) pattern));
   }
}


static void GLAPIENTRY
save_PolygonOffset(GLfloat factor, GLfloat units)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_POLYGON_OFFSET, 2);
   if (n) {
      n[1].f = factor;
      n[2].f = units;
   }
   if (ctx->ExecuteFlag) {
      CALL_PolygonOffset(ctx->Exec, (factor, units));
   }
}


static void GLAPIENTRY
save_PolygonOffsetClampEXT(GLfloat factor, GLfloat units, GLfloat clamp)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_POLYGON_OFFSET_CLAMP, 3);
   if (n) {
      n[1].f = factor;
      n[2].f = units;
      n[3].f = clamp;
   }
   if (ctx->ExecuteFlag) {
      CALL_PolygonOffsetClampEXT(ctx->Exec, (factor, units, clamp));
   }
}

static void GLAPIENTRY
save_PopAttrib(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_POP_ATTRIB, 0);
   if (ctx->ExecuteFlag) {
      CALL_PopAttrib(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_PopMatrix(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_POP_MATRIX, 0);
   if (ctx->ExecuteFlag) {
      CALL_PopMatrix(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_PopName(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_POP_NAME, 0);
   if (ctx->ExecuteFlag) {
      CALL_PopName(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_PrioritizeTextures(GLsizei num, const GLuint * textures,
                        const GLclampf * priorities)
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   for (i = 0; i < num; i++) {
      Node *n;
      n = alloc_instruction(ctx, OPCODE_PRIORITIZE_TEXTURE, 2);
      if (n) {
         n[1].ui = textures[i];
         n[2].f = priorities[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_PrioritizeTextures(ctx->Exec, (num, textures, priorities));
   }
}


static void GLAPIENTRY
save_PushAttrib(GLbitfield mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PUSH_ATTRIB, 1);
   if (n) {
      n[1].bf = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_PushAttrib(ctx->Exec, (mask));
   }
}


static void GLAPIENTRY
save_PushMatrix(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_PUSH_MATRIX, 0);
   if (ctx->ExecuteFlag) {
      CALL_PushMatrix(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_PushName(GLuint name)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PUSH_NAME, 1);
   if (n) {
      n[1].ui = name;
   }
   if (ctx->ExecuteFlag) {
      CALL_PushName(ctx->Exec, (name));
   }
}


static void GLAPIENTRY
save_RasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_RASTER_POS, 4);
   if (n) {
      n[1].f = x;
      n[2].f = y;
      n[3].f = z;
      n[4].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_RasterPos4f(ctx->Exec, (x, y, z, w));
   }
}

static void GLAPIENTRY
save_RasterPos2d(GLdouble x, GLdouble y)
{
   save_RasterPos4f((GLfloat) x, (GLfloat) y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos2f(GLfloat x, GLfloat y)
{
   save_RasterPos4f(x, y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos2i(GLint x, GLint y)
{
   save_RasterPos4f((GLfloat) x, (GLfloat) y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos2s(GLshort x, GLshort y)
{
   save_RasterPos4f(x, y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos3d(GLdouble x, GLdouble y, GLdouble z)
{
   save_RasterPos4f((GLfloat) x, (GLfloat) y, (GLfloat) z, 1.0F);
}

static void GLAPIENTRY
save_RasterPos3f(GLfloat x, GLfloat y, GLfloat z)
{
   save_RasterPos4f(x, y, z, 1.0F);
}

static void GLAPIENTRY
save_RasterPos3i(GLint x, GLint y, GLint z)
{
   save_RasterPos4f((GLfloat) x, (GLfloat) y, (GLfloat) z, 1.0F);
}

static void GLAPIENTRY
save_RasterPos3s(GLshort x, GLshort y, GLshort z)
{
   save_RasterPos4f(x, y, z, 1.0F);
}

static void GLAPIENTRY
save_RasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
   save_RasterPos4f((GLfloat) x, (GLfloat) y, (GLfloat) z, (GLfloat) w);
}

static void GLAPIENTRY
save_RasterPos4i(GLint x, GLint y, GLint z, GLint w)
{
   save_RasterPos4f((GLfloat) x, (GLfloat) y, (GLfloat) z, (GLfloat) w);
}

static void GLAPIENTRY
save_RasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w)
{
   save_RasterPos4f(x, y, z, w);
}

static void GLAPIENTRY
save_RasterPos2dv(const GLdouble * v)
{
   save_RasterPos4f((GLfloat) v[0], (GLfloat) v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos2fv(const GLfloat * v)
{
   save_RasterPos4f(v[0], v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos2iv(const GLint * v)
{
   save_RasterPos4f((GLfloat) v[0], (GLfloat) v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos2sv(const GLshort * v)
{
   save_RasterPos4f(v[0], v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_RasterPos3dv(const GLdouble * v)
{
   save_RasterPos4f((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2], 1.0F);
}

static void GLAPIENTRY
save_RasterPos3fv(const GLfloat * v)
{
   save_RasterPos4f(v[0], v[1], v[2], 1.0F);
}

static void GLAPIENTRY
save_RasterPos3iv(const GLint * v)
{
   save_RasterPos4f((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2], 1.0F);
}

static void GLAPIENTRY
save_RasterPos3sv(const GLshort * v)
{
   save_RasterPos4f(v[0], v[1], v[2], 1.0F);
}

static void GLAPIENTRY
save_RasterPos4dv(const GLdouble * v)
{
   save_RasterPos4f((GLfloat) v[0], (GLfloat) v[1],
                    (GLfloat) v[2], (GLfloat) v[3]);
}

static void GLAPIENTRY
save_RasterPos4fv(const GLfloat * v)
{
   save_RasterPos4f(v[0], v[1], v[2], v[3]);
}

static void GLAPIENTRY
save_RasterPos4iv(const GLint * v)
{
   save_RasterPos4f((GLfloat) v[0], (GLfloat) v[1],
                    (GLfloat) v[2], (GLfloat) v[3]);
}

static void GLAPIENTRY
save_RasterPos4sv(const GLshort * v)
{
   save_RasterPos4f(v[0], v[1], v[2], v[3]);
}


static void GLAPIENTRY
save_PassThrough(GLfloat token)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PASSTHROUGH, 1);
   if (n) {
      n[1].f = token;
   }
   if (ctx->ExecuteFlag) {
      CALL_PassThrough(ctx->Exec, (token));
   }
}


static void GLAPIENTRY
save_ReadBuffer(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_READ_BUFFER, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_ReadBuffer(ctx->Exec, (mode));
   }
}


static void GLAPIENTRY
save_Rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ROTATE, 4);
   if (n) {
      n[1].f = angle;
      n[2].f = x;
      n[3].f = y;
      n[4].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_Rotatef(ctx->Exec, (angle, x, y, z));
   }
}


static void GLAPIENTRY
save_Rotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z)
{
   save_Rotatef((GLfloat) angle, (GLfloat) x, (GLfloat) y, (GLfloat) z);
}


static void GLAPIENTRY
save_Scalef(GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SCALE, 3);
   if (n) {
      n[1].f = x;
      n[2].f = y;
      n[3].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_Scalef(ctx->Exec, (x, y, z));
   }
}


static void GLAPIENTRY
save_Scaled(GLdouble x, GLdouble y, GLdouble z)
{
   save_Scalef((GLfloat) x, (GLfloat) y, (GLfloat) z);
}


static void GLAPIENTRY
save_Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SCISSOR, 4);
   if (n) {
      n[1].i = x;
      n[2].i = y;
      n[3].i = width;
      n[4].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_Scissor(ctx->Exec, (x, y, width, height));
   }
}


static void GLAPIENTRY
save_ShadeModel(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END(ctx);

   if (ctx->ExecuteFlag) {
      CALL_ShadeModel(ctx->Exec, (mode));
   }

   /* Don't compile this call if it's a no-op.
    * By avoiding this state change we have a better chance of
    * coalescing subsequent drawing commands into one batch.
    */
   if (ctx->ListState.Current.ShadeModel == mode)
      return;

   SAVE_FLUSH_VERTICES(ctx);

   ctx->ListState.Current.ShadeModel = mode;

   n = alloc_instruction(ctx, OPCODE_SHADE_MODEL, 1);
   if (n) {
      n[1].e = mode;
   }
}


static void GLAPIENTRY
save_StencilFunc(GLenum func, GLint ref, GLuint mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_STENCIL_FUNC, 3);
   if (n) {
      n[1].e = func;
      n[2].i = ref;
      n[3].ui = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilFunc(ctx->Exec, (func, ref, mask));
   }
}


static void GLAPIENTRY
save_StencilMask(GLuint mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_STENCIL_MASK, 1);
   if (n) {
      n[1].ui = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilMask(ctx->Exec, (mask));
   }
}


static void GLAPIENTRY
save_StencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_STENCIL_OP, 3);
   if (n) {
      n[1].e = fail;
      n[2].e = zfail;
      n[3].e = zpass;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilOp(ctx->Exec, (fail, zfail, zpass));
   }
}


static void GLAPIENTRY
save_StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_STENCIL_FUNC_SEPARATE, 4);
   if (n) {
      n[1].e = face;
      n[2].e = func;
      n[3].i = ref;
      n[4].ui = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilFuncSeparate(ctx->Exec, (face, func, ref, mask));
   }
}


static void GLAPIENTRY
save_StencilFuncSeparateATI(GLenum frontfunc, GLenum backfunc, GLint ref,
                            GLuint mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   /* GL_FRONT */
   n = alloc_instruction(ctx, OPCODE_STENCIL_FUNC_SEPARATE, 4);
   if (n) {
      n[1].e = GL_FRONT;
      n[2].e = frontfunc;
      n[3].i = ref;
      n[4].ui = mask;
   }
   /* GL_BACK */
   n = alloc_instruction(ctx, OPCODE_STENCIL_FUNC_SEPARATE, 4);
   if (n) {
      n[1].e = GL_BACK;
      n[2].e = backfunc;
      n[3].i = ref;
      n[4].ui = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilFuncSeparate(ctx->Exec, (GL_FRONT, frontfunc, ref, mask));
      CALL_StencilFuncSeparate(ctx->Exec, (GL_BACK, backfunc, ref, mask));
   }
}


static void GLAPIENTRY
save_StencilMaskSeparate(GLenum face, GLuint mask)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_STENCIL_MASK_SEPARATE, 2);
   if (n) {
      n[1].e = face;
      n[2].ui = mask;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilMaskSeparate(ctx->Exec, (face, mask));
   }
}


static void GLAPIENTRY
save_StencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_STENCIL_OP_SEPARATE, 4);
   if (n) {
      n[1].e = face;
      n[2].e = fail;
      n[3].e = zfail;
      n[4].e = zpass;
   }
   if (ctx->ExecuteFlag) {
      CALL_StencilOpSeparate(ctx->Exec, (face, fail, zfail, zpass));
   }
}


static void GLAPIENTRY
save_TexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXENV, 6);
   if (n) {
      n[1].e = target;
      n[2].e = pname;
      if (pname == GL_TEXTURE_ENV_COLOR) {
         n[3].f = params[0];
         n[4].f = params[1];
         n[5].f = params[2];
         n[6].f = params[3];
      }
      else {
         n[3].f = params[0];
         n[4].f = n[5].f = n[6].f = 0.0F;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_TexEnvfv(ctx->Exec, (target, pname, params));
   }
}


static void GLAPIENTRY
save_TexEnvf(GLenum target, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = (GLfloat) param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_TexEnvfv(target, pname, parray);
}


static void GLAPIENTRY
save_TexEnvi(GLenum target, GLenum pname, GLint param)
{
   GLfloat p[4];
   p[0] = (GLfloat) param;
   p[1] = p[2] = p[3] = 0.0F;
   save_TexEnvfv(target, pname, p);
}


static void GLAPIENTRY
save_TexEnviv(GLenum target, GLenum pname, const GLint * param)
{
   GLfloat p[4];
   if (pname == GL_TEXTURE_ENV_COLOR) {
      p[0] = INT_TO_FLOAT(param[0]);
      p[1] = INT_TO_FLOAT(param[1]);
      p[2] = INT_TO_FLOAT(param[2]);
      p[3] = INT_TO_FLOAT(param[3]);
   }
   else {
      p[0] = (GLfloat) param[0];
      p[1] = p[2] = p[3] = 0.0F;
   }
   save_TexEnvfv(target, pname, p);
}


static void GLAPIENTRY
save_TexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXGEN, 6);
   if (n) {
      n[1].e = coord;
      n[2].e = pname;
      n[3].f = params[0];
      n[4].f = params[1];
      n[5].f = params[2];
      n[6].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TexGenfv(ctx->Exec, (coord, pname, params));
   }
}


static void GLAPIENTRY
save_TexGeniv(GLenum coord, GLenum pname, const GLint *params)
{
   GLfloat p[4];
   p[0] = (GLfloat) params[0];
   p[1] = (GLfloat) params[1];
   p[2] = (GLfloat) params[2];
   p[3] = (GLfloat) params[3];
   save_TexGenfv(coord, pname, p);
}


static void GLAPIENTRY
save_TexGend(GLenum coord, GLenum pname, GLdouble param)
{
   GLfloat parray[4];
   parray[0] = (GLfloat) param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_TexGenfv(coord, pname, parray);
}


static void GLAPIENTRY
save_TexGendv(GLenum coord, GLenum pname, const GLdouble *params)
{
   GLfloat p[4];
   p[0] = (GLfloat) params[0];
   p[1] = (GLfloat) params[1];
   p[2] = (GLfloat) params[2];
   p[3] = (GLfloat) params[3];
   save_TexGenfv(coord, pname, p);
}


static void GLAPIENTRY
save_TexGenf(GLenum coord, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_TexGenfv(coord, pname, parray);
}


static void GLAPIENTRY
save_TexGeni(GLenum coord, GLenum pname, GLint param)
{
   GLint parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0;
   save_TexGeniv(coord, pname, parray);
}


static void GLAPIENTRY
save_TexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXPARAMETER, 6);
   if (n) {
      n[1].e = target;
      n[2].e = pname;
      n[3].f = params[0];
      n[4].f = params[1];
      n[5].f = params[2];
      n[6].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TexParameterfv(ctx->Exec, (target, pname, params));
   }
}


static void GLAPIENTRY
save_TexParameterf(GLenum target, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_TexParameterfv(target, pname, parray);
}


static void GLAPIENTRY
save_TexParameteri(GLenum target, GLenum pname, GLint param)
{
   GLfloat fparam[4];
   fparam[0] = (GLfloat) param;
   fparam[1] = fparam[2] = fparam[3] = 0.0F;
   save_TexParameterfv(target, pname, fparam);
}


static void GLAPIENTRY
save_TexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
   GLfloat fparam[4];
   fparam[0] = (GLfloat) params[0];
   fparam[1] = fparam[2] = fparam[3] = 0.0F;
   save_TexParameterfv(target, pname, fparam);
}


static void GLAPIENTRY
save_TexImage1D(GLenum target,
                GLint level, GLint components,
                GLsizei width, GLint border,
                GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_1D) {
      /* don't compile, execute immediately */
      CALL_TexImage1D(ctx->Exec, (target, level, components, width,
                                  border, format, type, pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_TEX_IMAGE1D, 7 + POINTER_DWORDS);
      if (n) {
         n[1].e = target;
         n[2].i = level;
         n[3].i = components;
         n[4].i = (GLint) width;
         n[5].i = border;
         n[6].e = format;
         n[7].e = type;
         save_pointer(&n[8],
                      unpack_image(ctx, 1, width, 1, 1, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_TexImage1D(ctx->Exec, (target, level, components, width,
                                     border, format, type, pixels));
      }
   }
}


static void GLAPIENTRY
save_TexImage2D(GLenum target,
                GLint level, GLint components,
                GLsizei width, GLsizei height, GLint border,
                GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_2D) {
      /* don't compile, execute immediately */
      CALL_TexImage2D(ctx->Exec, (target, level, components, width,
                                  height, border, format, type, pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_TEX_IMAGE2D, 8 + POINTER_DWORDS);
      if (n) {
         n[1].e = target;
         n[2].i = level;
         n[3].i = components;
         n[4].i = (GLint) width;
         n[5].i = (GLint) height;
         n[6].i = border;
         n[7].e = format;
         n[8].e = type;
         save_pointer(&n[9],
                      unpack_image(ctx, 2, width, height, 1, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_TexImage2D(ctx->Exec, (target, level, components, width,
                                     height, border, format, type, pixels));
      }
   }
}


static void GLAPIENTRY
save_TexImage3D(GLenum target,
                GLint level, GLint internalFormat,
                GLsizei width, GLsizei height, GLsizei depth,
                GLint border,
                GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_3D) {
      /* don't compile, execute immediately */
      CALL_TexImage3D(ctx->Exec, (target, level, internalFormat, width,
                                  height, depth, border, format, type,
                                  pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_TEX_IMAGE3D, 9 + POINTER_DWORDS);
      if (n) {
         n[1].e = target;
         n[2].i = level;
         n[3].i = (GLint) internalFormat;
         n[4].i = (GLint) width;
         n[5].i = (GLint) height;
         n[6].i = (GLint) depth;
         n[7].i = border;
         n[8].e = format;
         n[9].e = type;
         save_pointer(&n[10],
                      unpack_image(ctx, 3, width, height, depth, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_TexImage3D(ctx->Exec, (target, level, internalFormat, width,
                                     height, depth, border, format, type,
                                     pixels));
      }
   }
}


static void GLAPIENTRY
save_TexSubImage1D(GLenum target, GLint level, GLint xoffset,
                   GLsizei width, GLenum format, GLenum type,
                   const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_TEX_SUB_IMAGE1D, 6 + POINTER_DWORDS);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = (GLint) width;
      n[5].e = format;
      n[6].e = type;
      save_pointer(&n[7],
                   unpack_image(ctx, 1, width, 1, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_TexSubImage1D(ctx->Exec, (target, level, xoffset, width,
                                     format, type, pixels));
   }
}


static void GLAPIENTRY
save_TexSubImage2D(GLenum target, GLint level,
                   GLint xoffset, GLint yoffset,
                   GLsizei width, GLsizei height,
                   GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_TEX_SUB_IMAGE2D, 8 + POINTER_DWORDS);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = yoffset;
      n[5].i = (GLint) width;
      n[6].i = (GLint) height;
      n[7].e = format;
      n[8].e = type;
      save_pointer(&n[9],
                   unpack_image(ctx, 2, width, height, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_TexSubImage2D(ctx->Exec, (target, level, xoffset, yoffset,
                                     width, height, format, type, pixels));
   }
}


static void GLAPIENTRY
save_TexSubImage3D(GLenum target, GLint level,
                   GLint xoffset, GLint yoffset, GLint zoffset,
                   GLsizei width, GLsizei height, GLsizei depth,
                   GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_TEX_SUB_IMAGE3D, 10 + POINTER_DWORDS);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = yoffset;
      n[5].i = zoffset;
      n[6].i = (GLint) width;
      n[7].i = (GLint) height;
      n[8].i = (GLint) depth;
      n[9].e = format;
      n[10].e = type;
      save_pointer(&n[11],
                   unpack_image(ctx, 3, width, height, depth, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_TexSubImage3D(ctx->Exec, (target, level,
                                     xoffset, yoffset, zoffset,
                                     width, height, depth, format, type,
                                     pixels));
   }
}


static void GLAPIENTRY
save_Translatef(GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TRANSLATE, 3);
   if (n) {
      n[1].f = x;
      n[2].f = y;
      n[3].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_Translatef(ctx->Exec, (x, y, z));
   }
}


static void GLAPIENTRY
save_Translated(GLdouble x, GLdouble y, GLdouble z)
{
   save_Translatef((GLfloat) x, (GLfloat) y, (GLfloat) z);
}



static void GLAPIENTRY
save_Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_VIEWPORT, 4);
   if (n) {
      n[1].i = x;
      n[2].i = y;
      n[3].i = (GLint) width;
      n[4].i = (GLint) height;
   }
   if (ctx->ExecuteFlag) {
      CALL_Viewport(ctx->Exec, (x, y, width, height));
   }
}

static void GLAPIENTRY
save_ViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat width,
                      GLfloat height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_VIEWPORT_INDEXED_F, 5);
   if (n) {
      n[1].ui = index;
      n[2].f = x;
      n[3].f = y;
      n[4].f = width;
      n[5].f = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_ViewportIndexedf(ctx->Exec, (index, x, y, width, height));
   }
}

static void GLAPIENTRY
save_ViewportIndexedfv(GLuint index, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_VIEWPORT_INDEXED_FV, 5);
   if (n) {
      n[1].ui = index;
      n[2].f = v[0];
      n[3].f = v[1];
      n[4].f = v[2];
      n[5].f = v[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_ViewportIndexedfv(ctx->Exec, (index, v));
   }
}

static void GLAPIENTRY
save_ViewportArrayv(GLuint first, GLsizei count, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_VIEWPORT_ARRAY_V, 2 + POINTER_DWORDS);
   if (n) {
      n[1].ui = first;
      n[2].si = count;
      save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ViewportArrayv(ctx->Exec, (first, count, v));
   }
}

static void GLAPIENTRY
save_ScissorIndexed(GLuint index, GLint left, GLint bottom, GLsizei width,
                    GLsizei height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SCISSOR_INDEXED, 5);
   if (n) {
      n[1].ui = index;
      n[2].i = left;
      n[3].i = bottom;
      n[4].si = width;
      n[5].si = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_ScissorIndexed(ctx->Exec, (index, left, bottom, width, height));
   }
}

static void GLAPIENTRY
save_ScissorIndexedv(GLuint index, const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SCISSOR_INDEXED_V, 5);
   if (n) {
      n[1].ui = index;
      n[2].i = v[0];
      n[3].i = v[1];
      n[4].si = v[2];
      n[5].si = v[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_ScissorIndexedv(ctx->Exec, (index, v));
   }
}

static void GLAPIENTRY
save_ScissorArrayv(GLuint first, GLsizei count, const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SCISSOR_ARRAY_V, 2 + POINTER_DWORDS);
   if (n) {
      n[1].ui = first;
      n[2].si = count;
      save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ScissorArrayv(ctx->Exec, (first, count, v));
   }
}

static void GLAPIENTRY
save_DepthRangeIndexed(GLuint index, GLclampd n, GLclampd f)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *node;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   node = alloc_instruction(ctx, OPCODE_DEPTH_INDEXED, 3);
   if (node) {
      node[1].ui = index;
      /* Mesa stores these as floats internally so we deliberately convert
       * them to a float here.
       */
      node[2].f = n;
      node[3].f = f;
   }
   if (ctx->ExecuteFlag) {
      CALL_DepthRangeIndexed(ctx->Exec, (index, n, f));
   }
}

static void GLAPIENTRY
save_DepthRangeArrayv(GLuint first, GLsizei count, const GLclampd *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DEPTH_ARRAY_V, 2 + POINTER_DWORDS);
   if (n) {
      n[1].ui = first;
      n[2].si = count;
      save_pointer(&n[3], memdup(v, count * 2 * sizeof(GLclampd)));
   }
   if (ctx->ExecuteFlag) {
      CALL_DepthRangeArrayv(ctx->Exec, (first, count, v));
   }
}

static void GLAPIENTRY
save_WindowPos4fMESA(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_WINDOW_POS, 4);
   if (n) {
      n[1].f = x;
      n[2].f = y;
      n[3].f = z;
      n[4].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_WindowPos4fMESA(ctx->Exec, (x, y, z, w));
   }
}

static void GLAPIENTRY
save_WindowPos2dMESA(GLdouble x, GLdouble y)
{
   save_WindowPos4fMESA((GLfloat) x, (GLfloat) y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos2fMESA(GLfloat x, GLfloat y)
{
   save_WindowPos4fMESA(x, y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos2iMESA(GLint x, GLint y)
{
   save_WindowPos4fMESA((GLfloat) x, (GLfloat) y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos2sMESA(GLshort x, GLshort y)
{
   save_WindowPos4fMESA(x, y, 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos3dMESA(GLdouble x, GLdouble y, GLdouble z)
{
   save_WindowPos4fMESA((GLfloat) x, (GLfloat) y, (GLfloat) z, 1.0F);
}

static void GLAPIENTRY
save_WindowPos3fMESA(GLfloat x, GLfloat y, GLfloat z)
{
   save_WindowPos4fMESA(x, y, z, 1.0F);
}

static void GLAPIENTRY
save_WindowPos3iMESA(GLint x, GLint y, GLint z)
{
   save_WindowPos4fMESA((GLfloat) x, (GLfloat) y, (GLfloat) z, 1.0F);
}

static void GLAPIENTRY
save_WindowPos3sMESA(GLshort x, GLshort y, GLshort z)
{
   save_WindowPos4fMESA(x, y, z, 1.0F);
}

static void GLAPIENTRY
save_WindowPos4dMESA(GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
   save_WindowPos4fMESA((GLfloat) x, (GLfloat) y, (GLfloat) z, (GLfloat) w);
}

static void GLAPIENTRY
save_WindowPos4iMESA(GLint x, GLint y, GLint z, GLint w)
{
   save_WindowPos4fMESA((GLfloat) x, (GLfloat) y, (GLfloat) z, (GLfloat) w);
}

static void GLAPIENTRY
save_WindowPos4sMESA(GLshort x, GLshort y, GLshort z, GLshort w)
{
   save_WindowPos4fMESA(x, y, z, w);
}

static void GLAPIENTRY
save_WindowPos2dvMESA(const GLdouble * v)
{
   save_WindowPos4fMESA((GLfloat) v[0], (GLfloat) v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos2fvMESA(const GLfloat * v)
{
   save_WindowPos4fMESA(v[0], v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos2ivMESA(const GLint * v)
{
   save_WindowPos4fMESA((GLfloat) v[0], (GLfloat) v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos2svMESA(const GLshort * v)
{
   save_WindowPos4fMESA(v[0], v[1], 0.0F, 1.0F);
}

static void GLAPIENTRY
save_WindowPos3dvMESA(const GLdouble * v)
{
   save_WindowPos4fMESA((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2], 1.0F);
}

static void GLAPIENTRY
save_WindowPos3fvMESA(const GLfloat * v)
{
   save_WindowPos4fMESA(v[0], v[1], v[2], 1.0F);
}

static void GLAPIENTRY
save_WindowPos3ivMESA(const GLint * v)
{
   save_WindowPos4fMESA((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2], 1.0F);
}

static void GLAPIENTRY
save_WindowPos3svMESA(const GLshort * v)
{
   save_WindowPos4fMESA(v[0], v[1], v[2], 1.0F);
}

static void GLAPIENTRY
save_WindowPos4dvMESA(const GLdouble * v)
{
   save_WindowPos4fMESA((GLfloat) v[0], (GLfloat) v[1],
                        (GLfloat) v[2], (GLfloat) v[3]);
}

static void GLAPIENTRY
save_WindowPos4fvMESA(const GLfloat * v)
{
   save_WindowPos4fMESA(v[0], v[1], v[2], v[3]);
}

static void GLAPIENTRY
save_WindowPos4ivMESA(const GLint * v)
{
   save_WindowPos4fMESA((GLfloat) v[0], (GLfloat) v[1],
                        (GLfloat) v[2], (GLfloat) v[3]);
}

static void GLAPIENTRY
save_WindowPos4svMESA(const GLshort * v)
{
   save_WindowPos4fMESA(v[0], v[1], v[2], v[3]);
}



/* GL_ARB_multitexture */
static void GLAPIENTRY
save_ActiveTextureARB(GLenum target)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ACTIVE_TEXTURE, 1);
   if (n) {
      n[1].e = target;
   }
   if (ctx->ExecuteFlag) {
      CALL_ActiveTexture(ctx->Exec, (target));
   }
}


/* GL_ARB_transpose_matrix */

static void GLAPIENTRY
save_LoadTransposeMatrixdARB(const GLdouble m[16])
{
   GLfloat tm[16];
   _math_transposefd(tm, m);
   save_LoadMatrixf(tm);
}


static void GLAPIENTRY
save_LoadTransposeMatrixfARB(const GLfloat m[16])
{
   GLfloat tm[16];
   _math_transposef(tm, m);
   save_LoadMatrixf(tm);
}


static void GLAPIENTRY
save_MultTransposeMatrixdARB(const GLdouble m[16])
{
   GLfloat tm[16];
   _math_transposefd(tm, m);
   save_MultMatrixf(tm);
}


static void GLAPIENTRY
save_MultTransposeMatrixfARB(const GLfloat m[16])
{
   GLfloat tm[16];
   _math_transposef(tm, m);
   save_MultMatrixf(tm);
}

static GLvoid *copy_data(const GLvoid *data, GLsizei size, const char *func)
{
   GET_CURRENT_CONTEXT(ctx);
   GLvoid *image;

   if (!data)
      return NULL;

   image = malloc(size);
   if (!image) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "%s", func);
      return NULL;
   }
   memcpy(image, data, size);

   return image;
}


/* GL_ARB_texture_compression */
static void GLAPIENTRY
save_CompressedTexImage1DARB(GLenum target, GLint level,
                             GLenum internalFormat, GLsizei width,
                             GLint border, GLsizei imageSize,
                             const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_1D) {
      /* don't compile, execute immediately */
      CALL_CompressedTexImage1D(ctx->Exec, (target, level, internalFormat,
                                               width, border, imageSize,
                                               data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEX_IMAGE_1D,
                            6 + POINTER_DWORDS);
      if (n) {
         n[1].e = target;
         n[2].i = level;
         n[3].e = internalFormat;
         n[4].i = (GLint) width;
         n[5].i = border;
         n[6].i = imageSize;
         save_pointer(&n[7],
                      copy_data(data, imageSize, "glCompressedTexImage1DARB"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedTexImage1D(ctx->Exec,
                                      (target, level, internalFormat, width,
                                       border, imageSize, data));
      }
   }
}


static void GLAPIENTRY
save_CompressedTexImage2DARB(GLenum target, GLint level,
                             GLenum internalFormat, GLsizei width,
                             GLsizei height, GLint border, GLsizei imageSize,
                             const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_2D) {
      /* don't compile, execute immediately */
      CALL_CompressedTexImage2D(ctx->Exec, (target, level, internalFormat,
                                               width, height, border,
                                               imageSize, data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEX_IMAGE_2D,
                            7 + POINTER_DWORDS);
      if (n) {
         n[1].e = target;
         n[2].i = level;
         n[3].e = internalFormat;
         n[4].i = (GLint) width;
         n[5].i = (GLint) height;
         n[6].i = border;
         n[7].i = imageSize;
         save_pointer(&n[8],
                      copy_data(data, imageSize, "glCompressedTexImage2DARB"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedTexImage2D(ctx->Exec,
                                      (target, level, internalFormat, width,
                                       height, border, imageSize, data));
      }
   }
}


static void GLAPIENTRY
save_CompressedTexImage3DARB(GLenum target, GLint level,
                             GLenum internalFormat, GLsizei width,
                             GLsizei height, GLsizei depth, GLint border,
                             GLsizei imageSize, const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_3D) {
      /* don't compile, execute immediately */
      CALL_CompressedTexImage3D(ctx->Exec, (target, level, internalFormat,
                                               width, height, depth, border,
                                               imageSize, data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEX_IMAGE_3D,
                            8 + POINTER_DWORDS);
      if (n) {
         n[1].e = target;
         n[2].i = level;
         n[3].e = internalFormat;
         n[4].i = (GLint) width;
         n[5].i = (GLint) height;
         n[6].i = (GLint) depth;
         n[7].i = border;
         n[8].i = imageSize;
         save_pointer(&n[9],
                      copy_data(data, imageSize, "glCompressedTexImage3DARB"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedTexImage3D(ctx->Exec,
                                      (target, level, internalFormat, width,
                                       height, depth, border, imageSize,
                                       data));
      }
   }
}


static void GLAPIENTRY
save_CompressedTexSubImage1DARB(GLenum target, GLint level, GLint xoffset,
                                GLsizei width, GLenum format,
                                GLsizei imageSize, const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEX_SUB_IMAGE_1D,
                         6 + POINTER_DWORDS);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = (GLint) width;
      n[5].e = format;
      n[6].i = imageSize;
      save_pointer(&n[7],
                   copy_data(data, imageSize, "glCompressedTexSubImage1DARB"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedTexSubImage1D(ctx->Exec, (target, level, xoffset,
                                                  width, format, imageSize,
                                                  data));
   }
}


static void GLAPIENTRY
save_CompressedTexSubImage2DARB(GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLsizei width, GLsizei height,
                                GLenum format, GLsizei imageSize,
                                const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEX_SUB_IMAGE_2D,
                         8 + POINTER_DWORDS);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = yoffset;
      n[5].i = (GLint) width;
      n[6].i = (GLint) height;
      n[7].e = format;
      n[8].i = imageSize;
      save_pointer(&n[9],
                   copy_data(data, imageSize, "glCompressedTexSubImage2DARB"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedTexSubImage2D(ctx->Exec,
                                      (target, level, xoffset, yoffset, width,
                                       height, format, imageSize, data));
   }
}


static void GLAPIENTRY
save_CompressedTexSubImage3DARB(GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLint zoffset, GLsizei width,
                                GLsizei height, GLsizei depth, GLenum format,
                                GLsizei imageSize, const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEX_SUB_IMAGE_3D,
                         10 + POINTER_DWORDS);
   if (n) {
      n[1].e = target;
      n[2].i = level;
      n[3].i = xoffset;
      n[4].i = yoffset;
      n[5].i = zoffset;
      n[6].i = (GLint) width;
      n[7].i = (GLint) height;
      n[8].i = (GLint) depth;
      n[9].e = format;
      n[10].i = imageSize;
      save_pointer(&n[11],
                   copy_data(data, imageSize, "glCompressedTexSubImage3DARB"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedTexSubImage3D(ctx->Exec,
                                      (target, level, xoffset, yoffset,
                                       zoffset, width, height, depth, format,
                                       imageSize, data));
   }
}


/* GL_ARB_multisample */
static void GLAPIENTRY
save_SampleCoverageARB(GLclampf value, GLboolean invert)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SAMPLE_COVERAGE, 2);
   if (n) {
      n[1].f = value;
      n[2].b = invert;
   }
   if (ctx->ExecuteFlag) {
      CALL_SampleCoverage(ctx->Exec, (value, invert));
   }
}


/*
 * GL_ARB_vertex_program
 */
static void GLAPIENTRY
save_BindProgramARB(GLenum target, GLuint id)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BIND_PROGRAM_ARB, 2);
   if (n) {
      n[1].e = target;
      n[2].ui = id;
   }
   if (ctx->ExecuteFlag) {
      CALL_BindProgramARB(ctx->Exec, (target, id));
   }
}

static void GLAPIENTRY
save_ProgramEnvParameter4fARB(GLenum target, GLuint index,
                              GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_ENV_PARAMETER_ARB, 6);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
      n[3].f = x;
      n[4].f = y;
      n[5].f = z;
      n[6].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramEnvParameter4fARB(ctx->Exec, (target, index, x, y, z, w));
   }
}


static void GLAPIENTRY
save_ProgramEnvParameter4fvARB(GLenum target, GLuint index,
                               const GLfloat *params)
{
   save_ProgramEnvParameter4fARB(target, index, params[0], params[1],
                                 params[2], params[3]);
}


static void GLAPIENTRY
save_ProgramEnvParameters4fvEXT(GLenum target, GLuint index, GLsizei count,
                                const GLfloat * params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   if (count > 0) {
      GLint i;
      const GLfloat * p = params;

      for (i = 0 ; i < count ; i++) {
         n = alloc_instruction(ctx, OPCODE_PROGRAM_ENV_PARAMETER_ARB, 6);
         if (n) {
            n[1].e = target;
            n[2].ui = index;
            n[3].f = p[0];
            n[4].f = p[1];
            n[5].f = p[2];
            n[6].f = p[3];
            p += 4;
         }
      }
   }

   if (ctx->ExecuteFlag) {
      CALL_ProgramEnvParameters4fvEXT(ctx->Exec, (target, index, count, params));
   }
}


static void GLAPIENTRY
save_ProgramEnvParameter4dARB(GLenum target, GLuint index,
                              GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
   save_ProgramEnvParameter4fARB(target, index,
                                 (GLfloat) x,
                                 (GLfloat) y, (GLfloat) z, (GLfloat) w);
}


static void GLAPIENTRY
save_ProgramEnvParameter4dvARB(GLenum target, GLuint index,
                               const GLdouble *params)
{
   save_ProgramEnvParameter4fARB(target, index,
                                 (GLfloat) params[0],
                                 (GLfloat) params[1],
                                 (GLfloat) params[2], (GLfloat) params[3]);
}


static void GLAPIENTRY
save_ProgramLocalParameter4fARB(GLenum target, GLuint index,
                                GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_LOCAL_PARAMETER_ARB, 6);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
      n[3].f = x;
      n[4].f = y;
      n[5].f = z;
      n[6].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramLocalParameter4fARB(ctx->Exec, (target, index, x, y, z, w));
   }
}


static void GLAPIENTRY
save_ProgramLocalParameter4fvARB(GLenum target, GLuint index,
                                 const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_LOCAL_PARAMETER_ARB, 6);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
      n[3].f = params[0];
      n[4].f = params[1];
      n[5].f = params[2];
      n[6].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramLocalParameter4fvARB(ctx->Exec, (target, index, params));
   }
}


static void GLAPIENTRY
save_ProgramLocalParameters4fvEXT(GLenum target, GLuint index, GLsizei count,
                                  const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   if (count > 0) {
      GLint i;
      const GLfloat * p = params;

      for (i = 0 ; i < count ; i++) {
         n = alloc_instruction(ctx, OPCODE_PROGRAM_LOCAL_PARAMETER_ARB, 6);
         if (n) {
            n[1].e = target;
            n[2].ui = index;
            n[3].f = p[0];
            n[4].f = p[1];
            n[5].f = p[2];
            n[6].f = p[3];
            p += 4;
         }
      }
   }

   if (ctx->ExecuteFlag) {
      CALL_ProgramLocalParameters4fvEXT(ctx->Exec, (target, index, count, params));
   }
}


static void GLAPIENTRY
save_ProgramLocalParameter4dARB(GLenum target, GLuint index,
                                GLdouble x, GLdouble y,
                                GLdouble z, GLdouble w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_LOCAL_PARAMETER_ARB, 6);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
      n[3].f = (GLfloat) x;
      n[4].f = (GLfloat) y;
      n[5].f = (GLfloat) z;
      n[6].f = (GLfloat) w;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramLocalParameter4dARB(ctx->Exec, (target, index, x, y, z, w));
   }
}


static void GLAPIENTRY
save_ProgramLocalParameter4dvARB(GLenum target, GLuint index,
                                 const GLdouble *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_LOCAL_PARAMETER_ARB, 6);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
      n[3].f = (GLfloat) params[0];
      n[4].f = (GLfloat) params[1];
      n[5].f = (GLfloat) params[2];
      n[6].f = (GLfloat) params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramLocalParameter4dvARB(ctx->Exec, (target, index, params));
   }
}


/* GL_EXT_stencil_two_side */
static void GLAPIENTRY
save_ActiveStencilFaceEXT(GLenum face)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_ACTIVE_STENCIL_FACE_EXT, 1);
   if (n) {
      n[1].e = face;
   }
   if (ctx->ExecuteFlag) {
      CALL_ActiveStencilFaceEXT(ctx->Exec, (face));
   }
}


/* GL_EXT_depth_bounds_test */
static void GLAPIENTRY
save_DepthBoundsEXT(GLclampd zmin, GLclampd zmax)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DEPTH_BOUNDS_EXT, 2);
   if (n) {
      n[1].f = (GLfloat) zmin;
      n[2].f = (GLfloat) zmax;
   }
   if (ctx->ExecuteFlag) {
      CALL_DepthBoundsEXT(ctx->Exec, (zmin, zmax));
   }
}



static void GLAPIENTRY
save_ProgramStringARB(GLenum target, GLenum format, GLsizei len,
                      const GLvoid * string)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_PROGRAM_STRING_ARB, 3 + POINTER_DWORDS);
   if (n) {
      GLubyte *programCopy = malloc(len);
      if (!programCopy) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "glProgramStringARB");
         return;
      }
      memcpy(programCopy, string, len);
      n[1].e = target;
      n[2].e = format;
      n[3].i = len;
      save_pointer(&n[4], programCopy);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramStringARB(ctx->Exec, (target, format, len, string));
   }
}


static void GLAPIENTRY
save_BeginQueryARB(GLenum target, GLuint id)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BEGIN_QUERY_ARB, 2);
   if (n) {
      n[1].e = target;
      n[2].ui = id;
   }
   if (ctx->ExecuteFlag) {
      CALL_BeginQuery(ctx->Exec, (target, id));
   }
}

static void GLAPIENTRY
save_EndQueryARB(GLenum target)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_END_QUERY_ARB, 1);
   if (n) {
      n[1].e = target;
   }
   if (ctx->ExecuteFlag) {
      CALL_EndQuery(ctx->Exec, (target));
   }
}

static void GLAPIENTRY
save_QueryCounter(GLuint id, GLenum target)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_QUERY_COUNTER, 2);
   if (n) {
      n[1].ui = id;
      n[2].e = target;
   }
   if (ctx->ExecuteFlag) {
      CALL_QueryCounter(ctx->Exec, (id, target));
   }
}

static void GLAPIENTRY
save_BeginQueryIndexed(GLenum target, GLuint index, GLuint id)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BEGIN_QUERY_INDEXED, 3);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
      n[3].ui = id;
   }
   if (ctx->ExecuteFlag) {
      CALL_BeginQueryIndexed(ctx->Exec, (target, index, id));
   }
}

static void GLAPIENTRY
save_EndQueryIndexed(GLenum target, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_END_QUERY_INDEXED, 2);
   if (n) {
      n[1].e = target;
      n[2].ui = index;
   }
   if (ctx->ExecuteFlag) {
      CALL_EndQueryIndexed(ctx->Exec, (target, index));
   }
}


static void GLAPIENTRY
save_DrawBuffersARB(GLsizei count, const GLenum * buffers)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DRAW_BUFFERS_ARB, 1 + MAX_DRAW_BUFFERS);
   if (n) {
      GLint i;
      n[1].i = count;
      if (count > MAX_DRAW_BUFFERS)
         count = MAX_DRAW_BUFFERS;
      for (i = 0; i < count; i++) {
         n[2 + i].e = buffers[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawBuffers(ctx->Exec, (count, buffers));
   }
}

static void GLAPIENTRY
save_BindFragmentShaderATI(GLuint id)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   n = alloc_instruction(ctx, OPCODE_BIND_FRAGMENT_SHADER_ATI, 1);
   if (n) {
      n[1].ui = id;
   }
   if (ctx->ExecuteFlag) {
      CALL_BindFragmentShaderATI(ctx->Exec, (id));
   }
}

static void GLAPIENTRY
save_SetFragmentShaderConstantATI(GLuint dst, const GLfloat *value)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   n = alloc_instruction(ctx, OPCODE_SET_FRAGMENT_SHADER_CONSTANTS_ATI, 5);
   if (n) {
      n[1].ui = dst;
      n[2].f = value[0];
      n[3].f = value[1];
      n[4].f = value[2];
      n[5].f = value[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_SetFragmentShaderConstantATI(ctx->Exec, (dst, value));
   }
}

static void GLAPIENTRY
save_EvalCoord1f(GLfloat x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);
   n = alloc_instruction(ctx, OPCODE_EVAL_C1, 1);
   if (n) {
      n[1].f = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_EvalCoord1f(ctx->Exec, (x));
   }
}

static void GLAPIENTRY
save_EvalCoord1fv(const GLfloat * v)
{
   save_EvalCoord1f(v[0]);
}

static void GLAPIENTRY
save_EvalCoord2f(GLfloat x, GLfloat y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);
   n = alloc_instruction(ctx, OPCODE_EVAL_C2, 2);
   if (n) {
      n[1].f = x;
      n[2].f = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_EvalCoord2f(ctx->Exec, (x, y));
   }
}

static void GLAPIENTRY
save_EvalCoord2fv(const GLfloat * v)
{
   save_EvalCoord2f(v[0], v[1]);
}


static void GLAPIENTRY
save_EvalPoint1(GLint x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);
   n = alloc_instruction(ctx, OPCODE_EVAL_P1, 1);
   if (n) {
      n[1].i = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_EvalPoint1(ctx->Exec, (x));
   }
}

static void GLAPIENTRY
save_EvalPoint2(GLint x, GLint y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);
   n = alloc_instruction(ctx, OPCODE_EVAL_P2, 2);
   if (n) {
      n[1].i = x;
      n[2].i = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_EvalPoint2(ctx->Exec, (x, y));
   }
}


/**
 * Compare 'count' elements of vectors 'a' and 'b'.
 * \return GL_TRUE if equal, GL_FALSE if different.
 */
static inline GLboolean
compare_vec(const GLfloat *a, const GLfloat *b, GLuint count)
{
   return memcmp( a, b, count * sizeof(GLfloat) ) == 0;
}


/**
 * This glMaterial function is used for glMaterial calls that are outside
 * a glBegin/End pair.  For glMaterial inside glBegin/End, see the VBO code.
 */
static void GLAPIENTRY
save_Materialfv(GLenum face, GLenum pname, const GLfloat * param)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   int args, i;
   GLuint bitmask;

   switch (face) {
   case GL_BACK:
   case GL_FRONT:
   case GL_FRONT_AND_BACK:
      break;
   default:
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glMaterial(face)");
      return;
   }

   switch (pname) {
   case GL_EMISSION:
   case GL_AMBIENT:
   case GL_DIFFUSE:
   case GL_SPECULAR:
   case GL_AMBIENT_AND_DIFFUSE:
      args = 4;
      break;
   case GL_SHININESS:
      args = 1;
      break;
   case GL_COLOR_INDEXES:
      args = 3;
      break;
   default:
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glMaterial(pname)");
      return;
   }

   if (ctx->ExecuteFlag) {
      CALL_Materialfv(ctx->Exec, (face, pname, param));
   }

   bitmask = _mesa_material_bitmask(ctx, face, pname, ~0, NULL);

   /* Try to eliminate redundant statechanges.  Because it is legal to
    * call glMaterial even inside begin/end calls, don't need to worry
    * about ctx->Driver.CurrentSavePrimitive here.
    */
   for (i = 0; i < MAT_ATTRIB_MAX; i++) {
      if (bitmask & (1 << i)) {
         if (ctx->ListState.ActiveMaterialSize[i] == args &&
             compare_vec(ctx->ListState.CurrentMaterial[i], param, args)) {
            /* no change in material value */
            bitmask &= ~(1 << i);
         }
         else {
            ctx->ListState.ActiveMaterialSize[i] = args;
            COPY_SZ_4V(ctx->ListState.CurrentMaterial[i], args, param);
         }
      }
   }

   /* If this call has no effect, return early */
   if (bitmask == 0)
      return;

   SAVE_FLUSH_VERTICES(ctx);

   n = alloc_instruction(ctx, OPCODE_MATERIAL, 6);
   if (n) {
      n[1].e = face;
      n[2].e = pname;
      for (i = 0; i < args; i++)
         n[3 + i].f = param[i];
   }
}

static void GLAPIENTRY
save_Begin(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!_mesa_is_valid_prim_mode(ctx, mode)) {
      /* compile this error into the display list */
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glBegin(mode)");
   }
   else if (_mesa_inside_dlist_begin_end(ctx)) {
      /* compile this error into the display list */
      _mesa_compile_error(ctx, GL_INVALID_OPERATION, "recursive glBegin");
   }
   else {
      ctx->Driver.CurrentSavePrimitive = mode;

      vbo_save_NotifyBegin(ctx, mode, false);
   }
}

static void GLAPIENTRY
save_End(void)
{
   GET_CURRENT_CONTEXT(ctx);
   SAVE_FLUSH_VERTICES(ctx);
   (void) alloc_instruction(ctx, OPCODE_END, 0);
   ctx->Driver.CurrentSavePrimitive = PRIM_OUTSIDE_BEGIN_END;
   if (ctx->ExecuteFlag) {
      CALL_End(ctx->Exec, ());
   }
}

static void GLAPIENTRY
save_PrimitiveRestartNV(void)
{
   /* Note: this is used when outside a glBegin/End pair in a display list */
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_PRIMITIVE_RESTART_NV, 0);
   if (ctx->ExecuteFlag) {
      CALL_PrimitiveRestartNV(ctx->Exec, ());
   }
}


static void GLAPIENTRY
save_BlitFramebufferEXT(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                        GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                        GLbitfield mask, GLenum filter)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BLIT_FRAMEBUFFER, 10);
   if (n) {
      n[1].i = srcX0;
      n[2].i = srcY0;
      n[3].i = srcX1;
      n[4].i = srcY1;
      n[5].i = dstX0;
      n[6].i = dstY0;
      n[7].i = dstX1;
      n[8].i = dstY1;
      n[9].i = mask;
      n[10].e = filter;
   }
   if (ctx->ExecuteFlag) {
      CALL_BlitFramebuffer(ctx->Exec, (srcX0, srcY0, srcX1, srcY1,
                                          dstX0, dstY0, dstX1, dstY1,
                                          mask, filter));
   }
}


/** GL_EXT_provoking_vertex */
static void GLAPIENTRY
save_ProvokingVertexEXT(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROVOKING_VERTEX, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      /*CALL_ProvokingVertex(ctx->Exec, (mode));*/
      _mesa_ProvokingVertex(mode);
   }
}


/** GL_EXT_transform_feedback */
static void GLAPIENTRY
save_BeginTransformFeedback(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BEGIN_TRANSFORM_FEEDBACK, 1);
   if (n) {
      n[1].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_BeginTransformFeedback(ctx->Exec, (mode));
   }
}


/** GL_EXT_transform_feedback */
static void GLAPIENTRY
save_EndTransformFeedback(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_END_TRANSFORM_FEEDBACK, 0);
   if (ctx->ExecuteFlag) {
      CALL_EndTransformFeedback(ctx->Exec, ());
   }
}

static void GLAPIENTRY
save_BindTransformFeedback(GLenum target, GLuint name)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BIND_TRANSFORM_FEEDBACK, 2);
   if (n) {
      n[1].e = target;
      n[2].ui = name;
   }
   if (ctx->ExecuteFlag) {
      CALL_BindTransformFeedback(ctx->Exec, (target, name));
   }
}

static void GLAPIENTRY
save_PauseTransformFeedback(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_PAUSE_TRANSFORM_FEEDBACK, 0);
   if (ctx->ExecuteFlag) {
      CALL_PauseTransformFeedback(ctx->Exec, ());
   }
}

static void GLAPIENTRY
save_ResumeTransformFeedback(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   (void) alloc_instruction(ctx, OPCODE_RESUME_TRANSFORM_FEEDBACK, 0);
   if (ctx->ExecuteFlag) {
      CALL_ResumeTransformFeedback(ctx->Exec, ());
   }
}

static void GLAPIENTRY
save_DrawTransformFeedback(GLenum mode, GLuint name)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DRAW_TRANSFORM_FEEDBACK, 2);
   if (n) {
      n[1].e = mode;
      n[2].ui = name;
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawTransformFeedback(ctx->Exec, (mode, name));
   }
}

static void GLAPIENTRY
save_DrawTransformFeedbackStream(GLenum mode, GLuint name, GLuint stream)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DRAW_TRANSFORM_FEEDBACK_STREAM, 3);
   if (n) {
      n[1].e = mode;
      n[2].ui = name;
      n[3].ui = stream;
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawTransformFeedbackStream(ctx->Exec, (mode, name, stream));
   }
}

static void GLAPIENTRY
save_DrawTransformFeedbackInstanced(GLenum mode, GLuint name,
                                    GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DRAW_TRANSFORM_FEEDBACK_INSTANCED, 3);
   if (n) {
      n[1].e = mode;
      n[2].ui = name;
      n[3].si = primcount;
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawTransformFeedbackInstanced(ctx->Exec, (mode, name, primcount));
   }
}

static void GLAPIENTRY
save_DrawTransformFeedbackStreamInstanced(GLenum mode, GLuint name,
                                          GLuint stream, GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DRAW_TRANSFORM_FEEDBACK_STREAM_INSTANCED, 4);
   if (n) {
      n[1].e = mode;
      n[2].ui = name;
      n[3].ui = stream;
      n[4].si = primcount;
   }
   if (ctx->ExecuteFlag) {
      CALL_DrawTransformFeedbackStreamInstanced(ctx->Exec, (mode, name, stream,
                                                            primcount));
   }
}

static void GLAPIENTRY
save_DispatchCompute(GLuint num_groups_x, GLuint num_groups_y,
                     GLuint num_groups_z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_DISPATCH_COMPUTE, 3);
   if (n) {
      n[1].ui = num_groups_x;
      n[2].ui = num_groups_y;
      n[3].ui = num_groups_z;
   }
   if (ctx->ExecuteFlag) {
      CALL_DispatchCompute(ctx->Exec, (num_groups_x, num_groups_y,
                                       num_groups_z));
   }
}

static void GLAPIENTRY
save_DispatchComputeIndirect(GLintptr indirect)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_error(ctx, GL_INVALID_OPERATION,
               "glDispatchComputeIndirect() during display list compile");
}

static void ALWAYS_INLINE
save_Attr32bit(struct gl_context *ctx, unsigned attr, unsigned size,
               GLenum type, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);
   unsigned base_op;
   unsigned index = attr;

   /* We don't care about GL_INT vs GL_UNSIGNED_INT. The idea is to get W=1
    * right for 3 or lower number of components, so only distinguish between
    * FLOAT and INT.
    */
   if (type == GL_FLOAT) {
      if (attr >= VERT_ATTRIB_GENERIC0) {
         base_op = OPCODE_ATTR_1F_ARB;
         attr -= VERT_ATTRIB_GENERIC0;
      } else {
         base_op = OPCODE_ATTR_1F_NV;
      }
   } else {
      base_op = OPCODE_ATTR_1I;
      attr -= VERT_ATTRIB_GENERIC0;
   }

   n = alloc_instruction(ctx, base_op + size - 1, 1 + size);
   if (n) {
      n[1].ui = attr;
      n[2].ui = x;
      if (size >= 2) n[3].ui = y;
      if (size >= 3) n[4].ui = z;
      if (size >= 4) n[5].ui = w;
   }

   ctx->ListState.ActiveAttribSize[index] = size;
   ASSIGN_4V(ctx->ListState.CurrentAttrib[index], x, y, z, w);

   if (ctx->ExecuteFlag) {
      if (type == GL_FLOAT) {
         if (base_op == OPCODE_ATTR_1F_NV) {
            if (size == 4)
               CALL_VertexAttrib4fNV(ctx->Exec, (attr, uif(x), uif(y), uif(z), uif(w)));
            else if (size == 3)
               CALL_VertexAttrib3fNV(ctx->Exec, (attr, uif(x), uif(y), uif(z)));
            else if (size == 2)
               CALL_VertexAttrib2fNV(ctx->Exec, (attr, uif(x), uif(y)));
            else
               CALL_VertexAttrib1fNV(ctx->Exec, (attr, uif(x)));
         } else {
            if (size == 4)
               CALL_VertexAttrib4fARB(ctx->Exec, (attr, uif(x), uif(y), uif(z), uif(w)));
            else if (size == 3)
               CALL_VertexAttrib3fARB(ctx->Exec, (attr, uif(x), uif(y), uif(z)));
            else if (size == 2)
               CALL_VertexAttrib2fARB(ctx->Exec, (attr, uif(x), uif(y)));
            else
               CALL_VertexAttrib1fARB(ctx->Exec, (attr, uif(x)));
         }
      } else {
         if (size == 4)
            CALL_VertexAttribI4iEXT(ctx->Exec, (attr, x, y, z, w));
         else if (size == 3)
            CALL_VertexAttribI3iEXT(ctx->Exec, (attr, x, y, z));
         else if (size == 2)
            CALL_VertexAttribI2iEXT(ctx->Exec, (attr, x, y));
         else
            CALL_VertexAttribI1iEXT(ctx->Exec, (attr, x));
      }
   }
}

static void ALWAYS_INLINE
save_Attr64bit(struct gl_context *ctx, unsigned attr, unsigned size,
               GLenum type, uint64_t x, uint64_t y, uint64_t z, uint64_t w)
{
   Node *n;
   SAVE_FLUSH_VERTICES(ctx);
   unsigned base_op;
   unsigned index = attr;

   if (type == GL_DOUBLE) {
      base_op = OPCODE_ATTR_1D;
   } else {
      base_op = OPCODE_ATTR_1UI64;
      assert(size == 1);
   }

   attr -= VERT_ATTRIB_GENERIC0;
   n = alloc_instruction(ctx, base_op + size - 1, 1 + size * 2);
   if (n) {
      n[1].ui = attr;
      ASSIGN_UINT64_TO_NODES(n, 2, x);
      if (size >= 2) ASSIGN_UINT64_TO_NODES(n, 4, y);
      if (size >= 3) ASSIGN_UINT64_TO_NODES(n, 6, z);
      if (size >= 4) ASSIGN_UINT64_TO_NODES(n, 8, w);
   }

   ctx->ListState.ActiveAttribSize[index] = size;
   memcpy(ctx->ListState.CurrentAttrib[index], &n[2], size * sizeof(uint64_t));

   if (ctx->ExecuteFlag) {
      uint64_t v[] = {x, y, z, w};
      if (type == GL_DOUBLE) {
         if (size == 4)
            CALL_VertexAttribL4dv(ctx->Exec, (attr, (GLdouble*)v));
         else if (size == 3)
            CALL_VertexAttribL3dv(ctx->Exec, (attr, (GLdouble*)v));
         else if (size == 2)
            CALL_VertexAttribL2dv(ctx->Exec, (attr, (GLdouble*)v));
         else
            CALL_VertexAttribL1d(ctx->Exec, (attr, UINT64_AS_DOUBLE(x)));
      } else {
         CALL_VertexAttribL1ui64ARB(ctx->Exec, (attr, x));
      }
   }
}

/**
 * If index=0, does glVertexAttrib*() alias glVertex() to emit a vertex?
 * It depends on a few things, including whether we're inside or outside
 * of glBegin/glEnd.
 */
static inline bool
is_vertex_position(const struct gl_context *ctx, GLuint index)
{
   return (index == 0 &&
           _mesa_attr_zero_aliases_vertex(ctx) &&
           _mesa_inside_dlist_begin_end(ctx));
}

/**
 * This macro is used to implement all the glVertex, glColor, glTexCoord,
 * glVertexAttrib, etc functions.
 * \param A  VBO_ATTRIB_x attribute index
 * \param N  attribute size (1..4)
 * \param T  type (GL_FLOAT, GL_DOUBLE, GL_INT, GL_UNSIGNED_INT)
 * \param C  cast type (uint32_t or uint64_t)
 * \param V0, V1, v2, V3  attribute value
 */
#define ATTR_UNION(A, N, T, C, V0, V1, V2, V3)                          \
do {                                                                    \
   if (sizeof(C) == 4) {                                                \
      save_Attr32bit(ctx, A, N, T, V0, V1, V2, V3);                     \
   } else {                                                             \
      save_Attr64bit(ctx, A, N, T, V0, V1, V2, V3);                     \
   }                                                                    \
} while (0)

#undef ERROR
#define ERROR(err) _mesa_error(ctx, err, __func__)
#define TAG(x) save_##x

#define VBO_ATTRIB_POS           VERT_ATTRIB_POS
#define VBO_ATTRIB_NORMAL        VERT_ATTRIB_NORMAL
#define VBO_ATTRIB_COLOR0        VERT_ATTRIB_COLOR0
#define VBO_ATTRIB_COLOR1        VERT_ATTRIB_COLOR1
#define VBO_ATTRIB_FOG           VERT_ATTRIB_FOG
#define VBO_ATTRIB_COLOR_INDEX   VERT_ATTRIB_COLOR_INDEX
#define VBO_ATTRIB_EDGEFLAG      VERT_ATTRIB_EDGEFLAG
#define VBO_ATTRIB_TEX0          VERT_ATTRIB_TEX0
#define VBO_ATTRIB_GENERIC0      VERT_ATTRIB_GENERIC0
#define VBO_ATTRIB_MAX           VERT_ATTRIB_MAX

#include "vbo/vbo_attrib_tmp.h"

static void GLAPIENTRY
save_UseProgram(GLuint program)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_USE_PROGRAM, 1);
   if (n) {
      n[1].ui = program;
   }
   if (ctx->ExecuteFlag) {
      CALL_UseProgram(ctx->Exec, (program));
   }
}


static void GLAPIENTRY
save_Uniform1fARB(GLint location, GLfloat x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1F, 2);
   if (n) {
      n[1].i = location;
      n[2].f = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1f(ctx->Exec, (location, x));
   }
}


static void GLAPIENTRY
save_Uniform2fARB(GLint location, GLfloat x, GLfloat y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2F, 3);
   if (n) {
      n[1].i = location;
      n[2].f = x;
      n[3].f = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2f(ctx->Exec, (location, x, y));
   }
}


static void GLAPIENTRY
save_Uniform3fARB(GLint location, GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3F, 4);
   if (n) {
      n[1].i = location;
      n[2].f = x;
      n[3].f = y;
      n[4].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3f(ctx->Exec, (location, x, y, z));
   }
}


static void GLAPIENTRY
save_Uniform4fARB(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4F, 5);
   if (n) {
      n[1].i = location;
      n[2].f = x;
      n[3].f = y;
      n[4].f = z;
      n[5].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4f(ctx->Exec, (location, x, y, z, w));
   }
}


static void GLAPIENTRY
save_Uniform1fvARB(GLint location, GLsizei count, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1FV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 1 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1fv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform2fvARB(GLint location, GLsizei count, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2FV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2fv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform3fvARB(GLint location, GLsizei count, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3FV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3fv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform4fvARB(GLint location, GLsizei count, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4FV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4fv(ctx->Exec, (location, count, v));
   }
}


static void GLAPIENTRY
save_Uniform1d(GLint location, GLdouble x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1D, 3);
   if (n) {
      n[1].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 2, x);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1d(ctx->Exec, (location, x));
   }
}


static void GLAPIENTRY
save_Uniform2d(GLint location, GLdouble x, GLdouble y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2D, 5);
   if (n) {
      n[1].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 2, x);
      ASSIGN_DOUBLE_TO_NODES(n, 4, y);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2d(ctx->Exec, (location, x, y));
   }
}


static void GLAPIENTRY
save_Uniform3d(GLint location, GLdouble x, GLdouble y, GLdouble z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3D, 7);
   if (n) {
      n[1].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 2, x);
      ASSIGN_DOUBLE_TO_NODES(n, 4, y);
      ASSIGN_DOUBLE_TO_NODES(n, 6, z);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3d(ctx->Exec, (location, x, y, z));
   }
}


static void GLAPIENTRY
save_Uniform4d(GLint location, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4D, 9);
   if (n) {
      n[1].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 2, x);
      ASSIGN_DOUBLE_TO_NODES(n, 4, y);
      ASSIGN_DOUBLE_TO_NODES(n, 6, z);
      ASSIGN_DOUBLE_TO_NODES(n, 8, w);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4d(ctx->Exec, (location, x, y, z, w));
   }
}


static void GLAPIENTRY
save_Uniform1dv(GLint location, GLsizei count, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1DV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 1 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1dv(ctx->Exec, (location, count, v));
   }
}


static void GLAPIENTRY
save_Uniform2dv(GLint location, GLsizei count, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2DV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2dv(ctx->Exec, (location, count, v));
   }
}


static void GLAPIENTRY
save_Uniform3dv(GLint location, GLsizei count, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3DV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3dv(ctx->Exec, (location, count, v));
   }
}


static void GLAPIENTRY
save_Uniform4dv(GLint location, GLsizei count, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4DV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4dv(ctx->Exec, (location, count, v));
   }
}


static void GLAPIENTRY
save_Uniform1iARB(GLint location, GLint x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1I, 2);
   if (n) {
      n[1].i = location;
      n[2].i = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1i(ctx->Exec, (location, x));
   }
}

static void GLAPIENTRY
save_Uniform2iARB(GLint location, GLint x, GLint y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2I, 3);
   if (n) {
      n[1].i = location;
      n[2].i = x;
      n[3].i = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2i(ctx->Exec, (location, x, y));
   }
}

static void GLAPIENTRY
save_Uniform3iARB(GLint location, GLint x, GLint y, GLint z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3I, 4);
   if (n) {
      n[1].i = location;
      n[2].i = x;
      n[3].i = y;
      n[4].i = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3i(ctx->Exec, (location, x, y, z));
   }
}

static void GLAPIENTRY
save_Uniform4iARB(GLint location, GLint x, GLint y, GLint z, GLint w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4I, 5);
   if (n) {
      n[1].i = location;
      n[2].i = x;
      n[3].i = y;
      n[4].i = z;
      n[5].i = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4i(ctx->Exec, (location, x, y, z, w));
   }
}



static void GLAPIENTRY
save_Uniform1ivARB(GLint location, GLsizei count, const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1IV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 1 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1iv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform2ivARB(GLint location, GLsizei count, const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2IV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 2 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2iv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform3ivARB(GLint location, GLsizei count, const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3IV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 3 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3iv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform4ivARB(GLint location, GLsizei count, const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4IV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4iv(ctx->Exec, (location, count, v));
   }
}



static void GLAPIENTRY
save_Uniform1ui(GLint location, GLuint x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1UI, 2);
   if (n) {
      n[1].i = location;
      n[2].i = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1ui(ctx->Exec, (location, x));
   }
}

static void GLAPIENTRY
save_Uniform2ui(GLint location, GLuint x, GLuint y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2UI, 3);
   if (n) {
      n[1].i = location;
      n[2].i = x;
      n[3].i = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2ui(ctx->Exec, (location, x, y));
   }
}

static void GLAPIENTRY
save_Uniform3ui(GLint location, GLuint x, GLuint y, GLuint z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3UI, 4);
   if (n) {
      n[1].i = location;
      n[2].i = x;
      n[3].i = y;
      n[4].i = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3ui(ctx->Exec, (location, x, y, z));
   }
}

static void GLAPIENTRY
save_Uniform4ui(GLint location, GLuint x, GLuint y, GLuint z, GLuint w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4UI, 5);
   if (n) {
      n[1].i = location;
      n[2].i = x;
      n[3].i = y;
      n[4].i = z;
      n[5].i = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4ui(ctx->Exec, (location, x, y, z, w));
   }
}



static void GLAPIENTRY
save_Uniform1uiv(GLint location, GLsizei count, const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1UIV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 1 * sizeof(*v)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1uiv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform2uiv(GLint location, GLsizei count, const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2UIV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 2 * sizeof(*v)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2uiv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform3uiv(GLint location, GLsizei count, const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3UIV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 3 * sizeof(*v)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3uiv(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform4uiv(GLint location, GLsizei count, const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4UIV, 2 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      save_pointer(&n[3], memdup(v, count * 4 * sizeof(*v)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4uiv(ctx->Exec, (location, count, v));
   }
}



static void GLAPIENTRY
save_UniformMatrix2fvARB(GLint location, GLsizei count, GLboolean transpose,
                         const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX22, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 2 * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix2fv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix3fvARB(GLint location, GLsizei count, GLboolean transpose,
                         const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX33, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 3 * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix3fv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix4fvARB(GLint location, GLsizei count, GLboolean transpose,
                         const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX44, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 4 * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix4fv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX23, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 2 * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix2x3fv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX32, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 3 * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix3x2fv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX24, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 2 * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix2x4fv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX42, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 4 * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix4x2fv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX34, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 3 * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix3x4fv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX43, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 4 * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix4x3fv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix2dv(GLint location, GLsizei count, GLboolean transpose,
                      const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX22D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 2 * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix2dv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix3dv(GLint location, GLsizei count, GLboolean transpose,
                      const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX33D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 3 * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix3dv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix4dv(GLint location, GLsizei count, GLboolean transpose,
                      const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX44D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 4 * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix4dv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix2x3dv(GLint location, GLsizei count, GLboolean transpose,
                        const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX23D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 2 * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix2x3dv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix3x2dv(GLint location, GLsizei count, GLboolean transpose,
                        const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX32D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 3 * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix3x2dv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix2x4dv(GLint location, GLsizei count, GLboolean transpose,
                        const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX24D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 2 * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix2x4dv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_UniformMatrix4x2dv(GLint location, GLsizei count, GLboolean transpose,
                        const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX42D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 4 * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix4x2dv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix3x4dv(GLint location, GLsizei count, GLboolean transpose,
                        const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX34D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 3 * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix3x4dv(ctx->Exec, (location, count, transpose, m));
   }
}


static void GLAPIENTRY
save_UniformMatrix4x3dv(GLint location, GLsizei count, GLboolean transpose,
                        const GLdouble *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_MATRIX43D, 3 + POINTER_DWORDS);
   if (n) {
      n[1].i = location;
      n[2].i = count;
      n[3].b = transpose;
      save_pointer(&n[4], memdup(m, count * 4 * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformMatrix4x3dv(ctx->Exec, (location, count, transpose, m));
   }
}

static void GLAPIENTRY
save_Uniform1i64ARB(GLint location, GLint64 x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1I64, 3);
   if (n) {
      n[1].i = location;
      ASSIGN_INT64_TO_NODES(n, 2, x);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1i64ARB(ctx->Exec, (location, x));
   }
}

static void GLAPIENTRY
save_Uniform2i64ARB(GLint location, GLint64 x, GLint64 y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2I64, 5);
   if (n) {
      n[1].i = location;
      ASSIGN_INT64_TO_NODES(n, 2, x);
      ASSIGN_INT64_TO_NODES(n, 4, y);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2i64ARB(ctx->Exec, (location, x, y));
   }
}

static void GLAPIENTRY
save_Uniform3i64ARB(GLint location, GLint64 x, GLint64 y, GLint64 z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3I64, 7);
   if (n) {
      n[1].i = location;
      ASSIGN_INT64_TO_NODES(n, 2, x);
      ASSIGN_INT64_TO_NODES(n, 4, y);
      ASSIGN_INT64_TO_NODES(n, 6, z);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3i64ARB(ctx->Exec, (location, x, y, z));
   }
}

static void GLAPIENTRY
save_Uniform4i64ARB(GLint location, GLint64 x, GLint64 y, GLint64 z, GLint64 w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4I64, 9);
   if (n) {
      n[1].i = location;
      ASSIGN_INT64_TO_NODES(n, 2, x);
      ASSIGN_INT64_TO_NODES(n, 4, y);
      ASSIGN_INT64_TO_NODES(n, 6, z);
      ASSIGN_INT64_TO_NODES(n, 8, w);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4i64ARB(ctx->Exec, (location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_Uniform1i64vARB(GLint location, GLsizei count, const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1I64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 1 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1i64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform2i64vARB(GLint location, GLsizei count, const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2I64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 2 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2i64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform3i64vARB(GLint location, GLsizei count, const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3I64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 3 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3i64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform4i64vARB(GLint location, GLsizei count, const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4I64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4i64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform1ui64ARB(GLint location, GLuint64 x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1UI64, 3);
   if (n) {
      n[1].i = location;
      ASSIGN_UINT64_TO_NODES(n, 2, x);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1ui64ARB(ctx->Exec, (location, x));
   }
}

static void GLAPIENTRY
save_Uniform2ui64ARB(GLint location, GLuint64 x, GLuint64 y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2UI64, 5);
   if (n) {
      n[1].i = location;
      ASSIGN_UINT64_TO_NODES(n, 2, x);
      ASSIGN_UINT64_TO_NODES(n, 4, y);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2ui64ARB(ctx->Exec, (location, x, y));
   }
}

static void GLAPIENTRY
save_Uniform3ui64ARB(GLint location, GLuint64 x, GLuint64 y, GLuint64 z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3UI64, 7);
   if (n) {
      n[1].i = location;
      ASSIGN_UINT64_TO_NODES(n, 2, x);
      ASSIGN_UINT64_TO_NODES(n, 4, y);
      ASSIGN_UINT64_TO_NODES(n, 6, z);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3ui64ARB(ctx->Exec, (location, x, y, z));
   }
}

static void GLAPIENTRY
save_Uniform4ui64ARB(GLint location, GLuint64 x, GLuint64 y, GLuint64 z, GLuint64 w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4UI64, 9);
   if (n) {
      n[1].i = location;
      ASSIGN_UINT64_TO_NODES(n, 2, x);
      ASSIGN_UINT64_TO_NODES(n, 4, y);
      ASSIGN_UINT64_TO_NODES(n, 6, z);
      ASSIGN_UINT64_TO_NODES(n, 8, w);
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4ui64ARB(ctx->Exec, (location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_Uniform1ui64vARB(GLint location, GLsizei count, const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_1UI64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 1 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform1ui64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform2ui64vARB(GLint location, GLsizei count, const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_2UI64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 2 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform2ui64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform3ui64vARB(GLint location, GLsizei count, const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_3UI64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 3 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform3ui64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_Uniform4ui64vARB(GLint location, GLsizei count, const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_4UI64V, 2 + POINTER_DWORDS);
   if (n) {
     n[1].i = location;
     n[2].i = count;
     save_pointer(&n[3], memdup(v, count * 4 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_Uniform4ui64vARB(ctx->Exec, (location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform1i64ARB(GLuint program, GLint location, GLint64 x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1I64, 4);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_INT64_TO_NODES(n, 3, x);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1i64ARB(ctx->Exec, (program, location, x));
   }
}

static void GLAPIENTRY
save_ProgramUniform2i64ARB(GLuint program, GLint location, GLint64 x,
                           GLint64 y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2I64, 6);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_INT64_TO_NODES(n, 3, x);
      ASSIGN_INT64_TO_NODES(n, 5, y);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2i64ARB(ctx->Exec, (program, location, x, y));
   }
}

static void GLAPIENTRY
save_ProgramUniform3i64ARB(GLuint program, GLint location, GLint64 x,
                           GLint64 y, GLint64 z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3I64, 8);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_INT64_TO_NODES(n, 3, x);
      ASSIGN_INT64_TO_NODES(n, 5, y);
      ASSIGN_INT64_TO_NODES(n, 7, z);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3i64ARB(ctx->Exec, (program, location, x, y, z));
   }
}

static void GLAPIENTRY
save_ProgramUniform4i64ARB(GLuint program, GLint location, GLint64 x,
                           GLint64 y, GLint64 z, GLint64 w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4I64, 10);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_INT64_TO_NODES(n, 3, x);
      ASSIGN_INT64_TO_NODES(n, 5, y);
      ASSIGN_INT64_TO_NODES(n, 7, z);
      ASSIGN_INT64_TO_NODES(n, 9, w);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4i64ARB(ctx->Exec, (program, location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_ProgramUniform1i64vARB(GLuint program, GLint location, GLsizei count,
                            const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1I64V, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1i64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform2i64vARB(GLuint program, GLint location, GLsizei count,
                            const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2I64V, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2i64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform3i64vARB(GLuint program, GLint location, GLsizei count,
                            const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3I64V, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3i64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform4i64vARB(GLuint program, GLint location, GLsizei count,
                            const GLint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4I64V, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4i64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform1ui64ARB(GLuint program, GLint location, GLuint64 x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1UI64, 4);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_UINT64_TO_NODES(n, 3, x);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1ui64ARB(ctx->Exec, (program, location, x));
   }
}

static void GLAPIENTRY
save_ProgramUniform2ui64ARB(GLuint program, GLint location, GLuint64 x,
                            GLuint64 y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2UI64, 6);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_UINT64_TO_NODES(n, 3, x);
      ASSIGN_UINT64_TO_NODES(n, 5, y);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2ui64ARB(ctx->Exec, (program, location, x, y));
   }
}

static void GLAPIENTRY
save_ProgramUniform3ui64ARB(GLuint program, GLint location, GLuint64 x,
                            GLuint64 y, GLuint64 z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3UI64, 8);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_UINT64_TO_NODES(n, 3, x);
      ASSIGN_UINT64_TO_NODES(n, 5, y);
      ASSIGN_UINT64_TO_NODES(n, 7, z);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3ui64ARB(ctx->Exec, (program, location, x, y, z));
   }
}

static void GLAPIENTRY
save_ProgramUniform4ui64ARB(GLuint program, GLint location, GLuint64 x,
                            GLuint64 y, GLuint64 z, GLuint64 w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4UI64, 10);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_UINT64_TO_NODES(n, 3, x);
      ASSIGN_UINT64_TO_NODES(n, 5, y);
      ASSIGN_UINT64_TO_NODES(n, 7, z);
      ASSIGN_UINT64_TO_NODES(n, 9, w);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4i64ARB(ctx->Exec, (program, location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_ProgramUniform1ui64vARB(GLuint program, GLint location, GLsizei count,
                             const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1UI64V,
                         3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1ui64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform2ui64vARB(GLuint program, GLint location, GLsizei count,
                            const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2UI64V,
                         3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2ui64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform3ui64vARB(GLuint program, GLint location, GLsizei count,
                             const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3UI64V,
                         3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3ui64vARB(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform4ui64vARB(GLuint program, GLint location, GLsizei count,
                             const GLuint64 *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4UI64V,
                         3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLuint64)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4ui64vARB(ctx->Exec, (program, location, count, v));
   }
}


static void GLAPIENTRY
save_UseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_USE_PROGRAM_STAGES, 3);
   if (n) {
      n[1].ui = pipeline;
      n[2].ui = stages;
      n[3].ui = program;
   }
   if (ctx->ExecuteFlag) {
      CALL_UseProgramStages(ctx->Exec, (pipeline, stages, program));
   }
}

static void GLAPIENTRY
save_ProgramUniform1f(GLuint program, GLint location, GLfloat x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1F, 3);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].f = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1f(ctx->Exec, (program, location, x));
   }
}

static void GLAPIENTRY
save_ProgramUniform2f(GLuint program, GLint location, GLfloat x, GLfloat y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2F, 4);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].f = x;
      n[4].f = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2f(ctx->Exec, (program, location, x, y));
   }
}

static void GLAPIENTRY
save_ProgramUniform3f(GLuint program, GLint location,
                      GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3F, 5);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].f = x;
      n[4].f = y;
      n[5].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3f(ctx->Exec, (program, location, x, y, z));
   }
}

static void GLAPIENTRY
save_ProgramUniform4f(GLuint program, GLint location,
                      GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4F, 6);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].f = x;
      n[4].f = y;
      n[5].f = z;
      n[6].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4f(ctx->Exec, (program, location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_ProgramUniform1fv(GLuint program, GLint location, GLsizei count,
                       const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1FV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1fv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform2fv(GLuint program, GLint location, GLsizei count,
                       const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2FV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2fv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform3fv(GLuint program, GLint location, GLsizei count,
                       const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3FV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3fv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform4fv(GLuint program, GLint location, GLsizei count,
                       const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4FV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4fv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform1d(GLuint program, GLint location, GLdouble x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1D, 4);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 3, x);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1d(ctx->Exec, (program, location, x));
   }
}

static void GLAPIENTRY
save_ProgramUniform2d(GLuint program, GLint location, GLdouble x, GLdouble y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2D, 6);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 3, x);
      ASSIGN_DOUBLE_TO_NODES(n, 5, y);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2d(ctx->Exec, (program, location, x, y));
   }
}

static void GLAPIENTRY
save_ProgramUniform3d(GLuint program, GLint location,
                      GLdouble x, GLdouble y, GLdouble z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3D, 8);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 3, x);
      ASSIGN_DOUBLE_TO_NODES(n, 5, y);
      ASSIGN_DOUBLE_TO_NODES(n, 7, z);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3d(ctx->Exec, (program, location, x, y, z));
   }
}

static void GLAPIENTRY
save_ProgramUniform4d(GLuint program, GLint location,
                      GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4D, 10);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      ASSIGN_DOUBLE_TO_NODES(n, 3, x);
      ASSIGN_DOUBLE_TO_NODES(n, 5, y);
      ASSIGN_DOUBLE_TO_NODES(n, 7, z);
      ASSIGN_DOUBLE_TO_NODES(n, 9, w);
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4d(ctx->Exec, (program, location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_ProgramUniform1dv(GLuint program, GLint location, GLsizei count,
                       const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1DV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1dv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform2dv(GLuint program, GLint location, GLsizei count,
                       const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2DV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2dv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform3dv(GLuint program, GLint location, GLsizei count,
                       const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3DV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3dv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform4dv(GLuint program, GLint location, GLsizei count,
                       const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4DV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4dv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform1i(GLuint program, GLint location, GLint x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1I, 3);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1i(ctx->Exec, (program, location, x));
   }
}

static void GLAPIENTRY
save_ProgramUniform2i(GLuint program, GLint location, GLint x, GLint y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2I, 4);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = x;
      n[4].i = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2i(ctx->Exec, (program, location, x, y));
   }
}

static void GLAPIENTRY
save_ProgramUniform3i(GLuint program, GLint location,
                      GLint x, GLint y, GLint z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3I, 5);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = x;
      n[4].i = y;
      n[5].i = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3i(ctx->Exec, (program, location, x, y, z));
   }
}

static void GLAPIENTRY
save_ProgramUniform4i(GLuint program, GLint location,
                      GLint x, GLint y, GLint z, GLint w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4I, 6);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = x;
      n[4].i = y;
      n[5].i = z;
      n[6].i = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4i(ctx->Exec, (program, location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_ProgramUniform1iv(GLuint program, GLint location, GLsizei count,
                       const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1IV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1iv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform2iv(GLuint program, GLint location, GLsizei count,
                       const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2IV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 2 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2iv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform3iv(GLuint program, GLint location, GLsizei count,
                       const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3IV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 3 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3iv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform4iv(GLuint program, GLint location, GLsizei count,
                       const GLint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4IV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 4 * sizeof(GLint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4iv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform1ui(GLuint program, GLint location, GLuint x)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1UI, 3);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].ui = x;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1ui(ctx->Exec, (program, location, x));
   }
}

static void GLAPIENTRY
save_ProgramUniform2ui(GLuint program, GLint location, GLuint x, GLuint y)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2UI, 4);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].ui = x;
      n[4].ui = y;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2ui(ctx->Exec, (program, location, x, y));
   }
}

static void GLAPIENTRY
save_ProgramUniform3ui(GLuint program, GLint location,
                       GLuint x, GLuint y, GLuint z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3UI, 5);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].ui = x;
      n[4].ui = y;
      n[5].ui = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3ui(ctx->Exec, (program, location, x, y, z));
   }
}

static void GLAPIENTRY
save_ProgramUniform4ui(GLuint program, GLint location,
                       GLuint x, GLuint y, GLuint z, GLuint w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4UI, 6);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].ui = x;
      n[4].ui = y;
      n[5].ui = z;
      n[6].ui = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4ui(ctx->Exec, (program, location, x, y, z, w));
   }
}

static void GLAPIENTRY
save_ProgramUniform1uiv(GLuint program, GLint location, GLsizei count,
                        const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_1UIV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 1 * sizeof(GLuint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform1uiv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform2uiv(GLuint program, GLint location, GLsizei count,
                        const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_2UIV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 2 * sizeof(GLuint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform2uiv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform3uiv(GLuint program, GLint location, GLsizei count,
                        const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_3UIV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 3 * sizeof(GLuint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform3uiv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniform4uiv(GLuint program, GLint location, GLsizei count,
                        const GLuint *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_4UIV, 3 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      save_pointer(&n[4], memdup(v, count * 4 * sizeof(GLuint)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniform4uiv(ctx->Exec, (program, location, count, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count,
                             GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX22F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 2 * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix2fv(ctx->Exec,
                                   (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix2x3fv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX23F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 2 * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix2x3fv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix2x4fv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX24F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 2 * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix2x4fv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix3x2fv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX32F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 3 * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix3x2fv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count,
                             GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX33F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 3 * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix3fv(ctx->Exec,
                                   (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix3x4fv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX34F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 3 * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix3x4fv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix4x2fv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX42F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 4 * 2 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix4x2fv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix4x3fv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX43F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 4 * 3 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix4x3fv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count,
                             GLboolean transpose, const GLfloat *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX44F,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 4 * 4 * sizeof(GLfloat)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix4fv(ctx->Exec,
                                   (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix2dv(GLuint program, GLint location, GLsizei count,
                             GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX22D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 2 * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix2dv(ctx->Exec,
                                   (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix2x3dv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX23D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 2 * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix2x3dv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix2x4dv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX24D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 2 * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix2x4dv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix3x2dv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX32D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 3 * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix3x2dv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix3dv(GLuint program, GLint location, GLsizei count,
                             GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX33D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 3 * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix3dv(ctx->Exec,
                                   (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix3x4dv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX34D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 3 * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix3x4dv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix4x2dv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX42D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 4 * 2 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix4x2dv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix4x3dv(GLuint program, GLint location, GLsizei count,
                               GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX43D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 4 * 3 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix4x3dv(ctx->Exec,
                                     (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ProgramUniformMatrix4dv(GLuint program, GLint location, GLsizei count,
                             GLboolean transpose, const GLdouble *v)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_PROGRAM_UNIFORM_MATRIX44D,
                         4 + POINTER_DWORDS);
   if (n) {
      n[1].ui = program;
      n[2].i = location;
      n[3].i = count;
      n[4].b = transpose;
      save_pointer(&n[5], memdup(v, count * 4 * 4 * sizeof(GLdouble)));
   }
   if (ctx->ExecuteFlag) {
      CALL_ProgramUniformMatrix4dv(ctx->Exec,
                                   (program, location, count, transpose, v));
   }
}

static void GLAPIENTRY
save_ClipControl(GLenum origin, GLenum depth)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLIP_CONTROL, 2);
   if (n) {
      n[1].e = origin;
      n[2].e = depth;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClipControl(ctx->Exec, (origin, depth));
   }
}

static void GLAPIENTRY
save_ClampColorARB(GLenum target, GLenum clamp)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLAMP_COLOR, 2);
   if (n) {
      n[1].e = target;
      n[2].e = clamp;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClampColor(ctx->Exec, (target, clamp));
   }
}

/** GL_EXT_texture_integer */
static void GLAPIENTRY
save_ClearColorIi(GLint red, GLint green, GLint blue, GLint alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEARCOLOR_I, 4);
   if (n) {
      n[1].i = red;
      n[2].i = green;
      n[3].i = blue;
      n[4].i = alpha;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearColorIiEXT(ctx->Exec, (red, green, blue, alpha));
   }
}

/** GL_EXT_texture_integer */
static void GLAPIENTRY
save_ClearColorIui(GLuint red, GLuint green, GLuint blue, GLuint alpha)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CLEARCOLOR_UI, 4);
   if (n) {
      n[1].ui = red;
      n[2].ui = green;
      n[3].ui = blue;
      n[4].ui = alpha;
   }
   if (ctx->ExecuteFlag) {
      CALL_ClearColorIuiEXT(ctx->Exec, (red, green, blue, alpha));
   }
}

/** GL_EXT_texture_integer */
static void GLAPIENTRY
save_TexParameterIiv(GLenum target, GLenum pname, const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXPARAMETER_I, 6);
   if (n) {
      n[1].e = target;
      n[2].e = pname;
      n[3].i = params[0];
      n[4].i = params[1];
      n[5].i = params[2];
      n[6].i = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TexParameterIiv(ctx->Exec, (target, pname, params));
   }
}

/** GL_EXT_texture_integer */
static void GLAPIENTRY
save_TexParameterIuiv(GLenum target, GLenum pname, const GLuint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXPARAMETER_UI, 6);
   if (n) {
      n[1].e = target;
      n[2].e = pname;
      n[3].ui = params[0];
      n[4].ui = params[1];
      n[5].ui = params[2];
      n[6].ui = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TexParameterIuiv(ctx->Exec, (target, pname, params));
   }
}

/* GL_ARB_instanced_arrays */
static void GLAPIENTRY
save_VertexAttribDivisor(GLuint index, GLuint divisor)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_VERTEX_ATTRIB_DIVISOR, 2);
   if (n) {
      n[1].ui = index;
      n[2].ui = divisor;
   }
   if (ctx->ExecuteFlag) {
      CALL_VertexAttribDivisor(ctx->Exec, (index, divisor));
   }
}


/* GL_NV_texture_barrier */
static void GLAPIENTRY
save_TextureBarrierNV(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   alloc_instruction(ctx, OPCODE_TEXTURE_BARRIER_NV, 0);
   if (ctx->ExecuteFlag) {
      CALL_TextureBarrierNV(ctx->Exec, ());
   }
}


/* GL_ARB_sampler_objects */
static void GLAPIENTRY
save_BindSampler(GLuint unit, GLuint sampler)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BIND_SAMPLER, 2);
   if (n) {
      n[1].ui = unit;
      n[2].ui = sampler;
   }
   if (ctx->ExecuteFlag) {
      CALL_BindSampler(ctx->Exec, (unit, sampler));
   }
}

static void GLAPIENTRY
save_SamplerParameteriv(GLuint sampler, GLenum pname, const GLint *params)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SAMPLER_PARAMETERIV, 6);
   if (n) {
      n[1].ui = sampler;
      n[2].e = pname;
      n[3].i = params[0];
      if (pname == GL_TEXTURE_BORDER_COLOR) {
         n[4].i = params[1];
         n[5].i = params[2];
         n[6].i = params[3];
      }
      else {
         n[4].i = n[5].i = n[6].i = 0;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_SamplerParameteriv(ctx->Exec, (sampler, pname, params));
   }
}

static void GLAPIENTRY
save_SamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
   GLint parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0;
   save_SamplerParameteriv(sampler, pname, parray);
}

static void GLAPIENTRY
save_SamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *params)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SAMPLER_PARAMETERFV, 6);
   if (n) {
      n[1].ui = sampler;
      n[2].e = pname;
      n[3].f = params[0];
      if (pname == GL_TEXTURE_BORDER_COLOR) {
         n[4].f = params[1];
         n[5].f = params[2];
         n[6].f = params[3];
      }
      else {
         n[4].f = n[5].f = n[6].f = 0.0F;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_SamplerParameterfv(ctx->Exec, (sampler, pname, params));
   }
}

static void GLAPIENTRY
save_SamplerParameterf(GLuint sampler, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_SamplerParameterfv(sampler, pname, parray);
}

static void GLAPIENTRY
save_SamplerParameterIiv(GLuint sampler, GLenum pname, const GLint *params)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SAMPLER_PARAMETERIIV, 6);
   if (n) {
      n[1].ui = sampler;
      n[2].e = pname;
      n[3].i = params[0];
      if (pname == GL_TEXTURE_BORDER_COLOR) {
         n[4].i = params[1];
         n[5].i = params[2];
         n[6].i = params[3];
      }
      else {
         n[4].i = n[5].i = n[6].i = 0;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_SamplerParameterIiv(ctx->Exec, (sampler, pname, params));
   }
}

static void GLAPIENTRY
save_SamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint *params)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SAMPLER_PARAMETERUIV, 6);
   if (n) {
      n[1].ui = sampler;
      n[2].e = pname;
      n[3].ui = params[0];
      if (pname == GL_TEXTURE_BORDER_COLOR) {
         n[4].ui = params[1];
         n[5].ui = params[2];
         n[6].ui = params[3];
      }
      else {
         n[4].ui = n[5].ui = n[6].ui = 0;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_SamplerParameterIuiv(ctx->Exec, (sampler, pname, params));
   }
}

static void GLAPIENTRY
save_WaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_WAIT_SYNC, 4);
   if (n) {
      union uint64_pair p;
      p.uint64 = timeout;
      n[1].bf = flags;
      n[2].ui = p.uint32[0];
      n[3].ui = p.uint32[1];
      save_pointer(&n[4], sync);
   }
   if (ctx->ExecuteFlag) {
      CALL_WaitSync(ctx->Exec, (sync, flags, timeout));
   }
}


/** GL_NV_conditional_render */
static void GLAPIENTRY
save_BeginConditionalRender(GLuint queryId, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BEGIN_CONDITIONAL_RENDER, 2);
   if (n) {
      n[1].i = queryId;
      n[2].e = mode;
   }
   if (ctx->ExecuteFlag) {
      CALL_BeginConditionalRender(ctx->Exec, (queryId, mode));
   }
}

static void GLAPIENTRY
save_EndConditionalRender(void)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   alloc_instruction(ctx, OPCODE_END_CONDITIONAL_RENDER, 0);
   if (ctx->ExecuteFlag) {
      CALL_EndConditionalRender(ctx->Exec, ());
   }
}

static void GLAPIENTRY
save_UniformBlockBinding(GLuint prog, GLuint index, GLuint binding)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_BLOCK_BINDING, 3);
   if (n) {
      n[1].ui = prog;
      n[2].ui = index;
      n[3].ui = binding;
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformBlockBinding(ctx->Exec, (prog, index, binding));
   }
}

static void GLAPIENTRY
save_UniformSubroutinesuiv(GLenum shadertype, GLsizei count,
                           const GLuint *indices)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_UNIFORM_SUBROUTINES, 2 + POINTER_DWORDS);
   if (n) {
      GLint *indices_copy = NULL;

      if (count > 0)
         indices_copy = memdup(indices, sizeof(GLuint) * 4 * count);
      n[1].e = shadertype;
      n[2].si = count;
      save_pointer(&n[3], indices_copy);
   }
   if (ctx->ExecuteFlag) {
      CALL_UniformSubroutinesuiv(ctx->Exec, (shadertype, count, indices));
   }
}

/** GL_EXT_window_rectangles */
static void GLAPIENTRY
save_WindowRectanglesEXT(GLenum mode, GLsizei count, const GLint *box)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_WINDOW_RECTANGLES, 2 + POINTER_DWORDS);
   if (n) {
      GLint *box_copy = NULL;

      if (count > 0)
         box_copy = memdup(box, sizeof(GLint) * 4 * count);
      n[1].e = mode;
      n[2].si = count;
      save_pointer(&n[3], box_copy);
   }
   if (ctx->ExecuteFlag) {
      CALL_WindowRectanglesEXT(ctx->Exec, (mode, count, box));
   }
}


/** GL_NV_conservative_raster */
static void GLAPIENTRY
save_SubpixelPrecisionBiasNV(GLuint xbits, GLuint ybits)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_SUBPIXEL_PRECISION_BIAS, 2);
   if (n) {
      n[1].ui = xbits;
      n[2].ui = ybits;
   }
   if (ctx->ExecuteFlag) {
      CALL_SubpixelPrecisionBiasNV(ctx->Exec, (xbits, ybits));
   }
}

/** GL_NV_conservative_raster_dilate */
static void GLAPIENTRY
save_ConservativeRasterParameterfNV(GLenum pname, GLfloat param)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CONSERVATIVE_RASTER_PARAMETER_F, 2);
   if (n) {
      n[1].e = pname;
      n[2].f = param;
   }
   if (ctx->ExecuteFlag) {
      CALL_ConservativeRasterParameterfNV(ctx->Exec, (pname, param));
   }
}

/** GL_NV_conservative_raster_pre_snap_triangles */
static void GLAPIENTRY
save_ConservativeRasterParameteriNV(GLenum pname, GLint param)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_CONSERVATIVE_RASTER_PARAMETER_I, 2);
   if (n) {
      n[1].e = pname;
      n[2].i = param;
   }
   if (ctx->ExecuteFlag) {
      CALL_ConservativeRasterParameteriNV(ctx->Exec, (pname, param));
   }
}

/** GL_EXT_direct_state_access */

static void GLAPIENTRY
save_MatrixLoadfEXT(GLenum matrixMode, const GLfloat *m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_LOAD, 17);
   if (n) {
      n[1].e = matrixMode;
      for (unsigned i = 0; i < 16; i++) {
         n[2 + i].f = m[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixLoadfEXT(ctx->Exec, (matrixMode, m));
   }
}

static void GLAPIENTRY
save_MatrixLoaddEXT(GLenum matrixMode, const GLdouble *m)
{
   GLfloat f[16];
   for (unsigned i = 0; i < 16; i++) {
      f[i] = (GLfloat) m[i];
   }
   save_MatrixLoadfEXT(matrixMode, f);
}

static void GLAPIENTRY
save_MatrixMultfEXT(GLenum matrixMode, const GLfloat * m)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_MULT, 17);
   if (n) {
      n[1].e = matrixMode;
      for (unsigned i = 0; i < 16; i++) {
         n[2 + i].f = m[i];
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixMultfEXT(ctx->Exec, (matrixMode, m));
   }
}

static void GLAPIENTRY
save_MatrixMultdEXT(GLenum matrixMode, const GLdouble * m)
{
   GLfloat f[16];
   for (unsigned i = 0; i < 16; i++) {
      f[i] = (GLfloat) m[i];
   }
   save_MatrixMultfEXT(matrixMode, f);
}

static void GLAPIENTRY
save_MatrixRotatefEXT(GLenum matrixMode, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_ROTATE, 5);
   if (n) {
      n[1].e = matrixMode;
      n[2].f = angle;
      n[3].f = x;
      n[4].f = y;
      n[5].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixRotatefEXT(ctx->Exec, (matrixMode, angle, x, y, z));
   }
}

static void GLAPIENTRY
save_MatrixRotatedEXT(GLenum matrixMode, GLdouble angle, GLdouble x, GLdouble y, GLdouble z)
{
   save_MatrixRotatefEXT(matrixMode, (GLfloat) angle, (GLfloat) x, (GLfloat) y, (GLfloat) z);
}

static void GLAPIENTRY
save_MatrixScalefEXT(GLenum matrixMode, GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_SCALE, 4);
   if (n) {
      n[1].e = matrixMode;
      n[2].f = x;
      n[3].f = y;
      n[4].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixScalefEXT(ctx->Exec, (matrixMode, x, y, z));
   }
}

static void GLAPIENTRY
save_MatrixScaledEXT(GLenum matrixMode, GLdouble x, GLdouble y, GLdouble z)
{
   save_MatrixScalefEXT(matrixMode, (GLfloat) x, (GLfloat) y, (GLfloat) z);
}

static void GLAPIENTRY
save_MatrixTranslatefEXT(GLenum matrixMode, GLfloat x, GLfloat y, GLfloat z)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_TRANSLATE, 4);
   if (n) {
      n[1].e = matrixMode;
      n[2].f = x;
      n[3].f = y;
      n[4].f = z;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixTranslatefEXT(ctx->Exec, (matrixMode, x, y, z));
   }
}

static void GLAPIENTRY
save_MatrixTranslatedEXT(GLenum matrixMode, GLdouble x, GLdouble y, GLdouble z)
{
   save_MatrixTranslatefEXT(matrixMode, (GLfloat) x, (GLfloat) y, (GLfloat) z);
}

static void GLAPIENTRY
save_MatrixLoadIdentityEXT(GLenum matrixMode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_LOAD_IDENTITY, 1);
   if (n) {
      n[1].e = matrixMode;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixLoadIdentityEXT(ctx->Exec, (matrixMode));
   }
}

static void GLAPIENTRY
save_MatrixOrthoEXT(GLenum matrixMode, GLdouble left, GLdouble right,
                    GLdouble bottom, GLdouble top, GLdouble nearval, GLdouble farval)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_ORTHO, 7);
   if (n) {
      n[1].e = matrixMode;
      n[2].f = (GLfloat) left;
      n[3].f = (GLfloat) right;
      n[4].f = (GLfloat) bottom;
      n[5].f = (GLfloat) top;
      n[6].f = (GLfloat) nearval;
      n[7].f = (GLfloat) farval;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixOrthoEXT(ctx->Exec, (matrixMode, left, right, bottom, top, nearval, farval));
   }
}


static void GLAPIENTRY
save_MatrixFrustumEXT(GLenum matrixMode, GLdouble left, GLdouble right,
                      GLdouble bottom, GLdouble top, GLdouble nearval, GLdouble farval)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_FRUSTUM, 7);
   if (n) {
      n[1].e = matrixMode;
      n[2].f = (GLfloat) left;
      n[3].f = (GLfloat) right;
      n[4].f = (GLfloat) bottom;
      n[5].f = (GLfloat) top;
      n[6].f = (GLfloat) nearval;
      n[7].f = (GLfloat) farval;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixFrustumEXT(ctx->Exec, (matrixMode, left, right, bottom, top, nearval, farval));
   }
}

static void GLAPIENTRY
save_MatrixPushEXT(GLenum matrixMode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node* n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_PUSH, 1);
   if (n) {
      n[1].e = matrixMode;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixPushEXT(ctx->Exec, (matrixMode));
   }
}

static void GLAPIENTRY
save_MatrixPopEXT(GLenum matrixMode)
{
   GET_CURRENT_CONTEXT(ctx);
   Node* n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MATRIX_POP, 1);
   if (n) {
      n[1].e = matrixMode;
   }
   if (ctx->ExecuteFlag) {
      CALL_MatrixPopEXT(ctx->Exec, (matrixMode));
   }
}

static void GLAPIENTRY
save_MatrixLoadTransposefEXT(GLenum matrixMode, const GLfloat m[16])
{
   GLfloat tm[16];
   _math_transposef(tm, m);
   save_MatrixLoadfEXT(matrixMode, tm);
}

static void GLAPIENTRY
save_MatrixLoadTransposedEXT(GLenum matrixMode, const GLdouble m[16])
{
   GLfloat tm[16];
   _math_transposefd(tm, m);
   save_MatrixLoadfEXT(matrixMode, tm);
}

static void GLAPIENTRY
save_MatrixMultTransposefEXT(GLenum matrixMode, const GLfloat m[16])
{
   GLfloat tm[16];
   _math_transposef(tm, m);
   save_MatrixMultfEXT(matrixMode, tm);
}

static void GLAPIENTRY
save_MatrixMultTransposedEXT(GLenum matrixMode, const GLdouble m[16])
{
   GLfloat tm[16];
   _math_transposefd(tm, m);
   save_MatrixMultfEXT(matrixMode, tm);
}

static void GLAPIENTRY
save_TextureParameterfvEXT(GLuint texture, GLenum target, GLenum pname,
                           const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXTUREPARAMETER_F, 7);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].e = pname;
      n[4].f = params[0];
      n[5].f = params[1];
      n[6].f = params[2];
      n[7].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureParameterfvEXT(ctx->Exec, (texture, target, pname, params));
   }
}


static void GLAPIENTRY
save_TextureParameterfEXT(GLuint texture, GLenum target, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_TextureParameterfvEXT(texture, target, pname, parray);
}

static void GLAPIENTRY
save_TextureParameterivEXT(GLuint texture, GLenum target, GLenum pname, const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXTUREPARAMETER_I, 7);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].e = pname;
      n[4].i = params[0];
      n[5].i = params[1];
      n[6].i = params[2];
      n[7].i = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureParameterivEXT(ctx->Exec, (texture, target, pname, params));
   }
}

static void GLAPIENTRY
save_TextureParameteriEXT(GLuint texture, GLenum target, GLenum pname, GLint param)
{
   GLint fparam[4];
   fparam[0] = param;
   fparam[1] = fparam[2] = fparam[3] = 0;
   save_TextureParameterivEXT(texture, target, pname, fparam);
}

static void GLAPIENTRY
save_TextureParameterIivEXT(GLuint texture, GLenum target, GLenum pname, const GLint* params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXTUREPARAMETER_II, 7);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].e = pname;
      n[4].i = params[0];
      n[5].i = params[1];
      n[6].i = params[2];
      n[7].i = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureParameterIivEXT(ctx->Exec, (texture, target, pname, params));
   }
}

static void GLAPIENTRY
save_TextureParameterIuivEXT(GLuint texture, GLenum target, GLenum pname, const GLuint* params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_TEXTUREPARAMETER_IUI, 7);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].e = pname;
      n[4].ui = params[0];
      n[5].ui = params[1];
      n[6].ui = params[2];
      n[7].ui = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureParameterIuivEXT(ctx->Exec, (texture, target, pname, params));
   }
}


static void GLAPIENTRY
save_TextureImage1DEXT(GLuint texture, GLenum target,
                       GLint level, GLint components,
                       GLsizei width, GLint border,
                       GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_1D) {
      /* don't compile, execute immediately */
      CALL_TextureImage1DEXT(ctx->Exec, (texture, target, level, components, width,
                                         border, format, type, pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_TEXTURE_IMAGE1D, 8 + POINTER_DWORDS);
      if (n) {
         n[1].ui = texture;
         n[2].e = target;
         n[3].i = level;
         n[4].i = components;
         n[5].i = (GLint) width;
         n[6].i = border;
         n[7].e = format;
         n[8].e = type;
         save_pointer(&n[9],
                      unpack_image(ctx, 1, width, 1, 1, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_TextureImage1DEXT(ctx->Exec, (texture, target, level, components, width,
                                            border, format, type, pixels));
      }
   }
}


static void GLAPIENTRY
save_TextureImage2DEXT(GLuint texture, GLenum target,
                       GLint level, GLint components,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_2D) {
      /* don't compile, execute immediately */
      CALL_TextureImage2DEXT(ctx->Exec, (texture, target, level, components, width,
                                         height, border, format, type, pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_TEXTURE_IMAGE2D, 9 + POINTER_DWORDS);
      if (n) {
         n[1].ui = texture;
         n[2].e = target;
         n[3].i = level;
         n[4].i = components;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = border;
         n[8].e = format;
         n[9].e = type;
         save_pointer(&n[10],
                      unpack_image(ctx, 2, width, height, 1, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_TextureImage2DEXT(ctx->Exec, (texture, target, level, components, width,
                                            height, border, format, type, pixels));
      }
   }
}


static void GLAPIENTRY
save_TextureImage3DEXT(GLuint texture, GLenum target,
                       GLint level, GLint internalFormat,
                       GLsizei width, GLsizei height, GLsizei depth,
                       GLint border,
                       GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_3D) {
      /* don't compile, execute immediately */
      CALL_TextureImage3DEXT(ctx->Exec, (texture, target, level, internalFormat, width,
                                         height, depth, border, format, type,
                                         pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_TEXTURE_IMAGE3D, 10 + POINTER_DWORDS);
      if (n) {
         n[1].ui = texture;
         n[2].e = target;
         n[3].i = level;
         n[4].i = (GLint) internalFormat;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = (GLint) depth;
         n[8].i = border;
         n[9].e = format;
         n[10].e = type;
         save_pointer(&n[11],
                      unpack_image(ctx, 3, width, height, depth, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_TextureImage3DEXT(ctx->Exec, (texture, target, level, internalFormat,
                                            width, height, depth, border, format,
                                            type, pixels));
      }
   }
}


static void GLAPIENTRY
save_TextureSubImage1DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset,
                   GLsizei width, GLenum format, GLenum type,
                   const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_TEXTURE_SUB_IMAGE1D, 7 + POINTER_DWORDS);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = (GLint) width;
      n[6].e = format;
      n[7].e = type;
      save_pointer(&n[8],
                   unpack_image(ctx, 1, width, 1, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureSubImage1DEXT(ctx->Exec, (texture, target, level, xoffset, width,
                                            format, type, pixels));
   }
}


static void GLAPIENTRY
save_TextureSubImage2DEXT(GLuint texture, GLenum target, GLint level,
                          GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_TEXTURE_SUB_IMAGE2D, 9 + POINTER_DWORDS);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = (GLint) width;
      n[7].i = (GLint) height;
      n[8].e = format;
      n[9].e = type;
      save_pointer(&n[10],
                   unpack_image(ctx, 2, width, height, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureSubImage2DEXT(ctx->Exec, (texture, target, level, xoffset, yoffset,
                                            width, height, format, type, pixels));
   }
}


static void GLAPIENTRY
save_TextureSubImage3DEXT(GLuint texture, GLenum target, GLint level,
                          GLint xoffset, GLint yoffset, GLint zoffset,
                          GLsizei width, GLsizei height, GLsizei depth,
                          GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_TEXTURE_SUB_IMAGE3D, 11 + POINTER_DWORDS);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = zoffset;
      n[7].i = (GLint) width;
      n[8].i = (GLint) height;
      n[9].i = (GLint) depth;
      n[10].e = format;
      n[11].e = type;
      save_pointer(&n[12],
                   unpack_image(ctx, 3, width, height, depth, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_TextureSubImage3DEXT(ctx->Exec, (texture, target, level,
                                            xoffset, yoffset, zoffset,
                                            width, height, depth, format, type,
                                            pixels));
   }
}

static void GLAPIENTRY
save_CopyTextureImage1DEXT(GLuint texture, GLenum target, GLint level,
                           GLenum internalformat, GLint x, GLint y,
                           GLsizei width, GLint border)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEXTURE_IMAGE1D, 8);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].e = internalformat;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
      n[8].i = border;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTextureImage1DEXT(ctx->Exec, (texture, target, level,
                                             internalformat, x, y,
                                             width, border));
   }
}

static void GLAPIENTRY
save_CopyTextureImage2DEXT(GLuint texture, GLenum target, GLint level,
                           GLenum internalformat,
                           GLint x, GLint y, GLsizei width,
                           GLsizei height, GLint border)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEXTURE_IMAGE2D, 9);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].e = internalformat;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
      n[8].i = height;
      n[9].i = border;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTextureImage2DEXT(ctx->Exec, (texture, target, level,
                                             internalformat, x, y,
                                             width, height, border));
   }
}

static void GLAPIENTRY
save_CopyTextureSubImage1DEXT(GLuint texture, GLenum target, GLint level,
                              GLint xoffset, GLint x, GLint y, GLsizei width)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEXTURE_SUB_IMAGE1D, 7);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTextureSubImage1DEXT(ctx->Exec,
                             (texture, target, level, xoffset, x, y, width));
   }
}

static void GLAPIENTRY
save_CopyTextureSubImage2DEXT(GLuint texture, GLenum target, GLint level,
                              GLint xoffset, GLint yoffset,
                              GLint x, GLint y, GLsizei width, GLint height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEXTURE_SUB_IMAGE2D, 9);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = x;
      n[7].i = y;
      n[8].i = width;
      n[9].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTextureSubImage2DEXT(ctx->Exec, (texture, target, level,
                                                xoffset, yoffset,
                                                x, y, width, height));
   }
}


static void GLAPIENTRY
save_CopyTextureSubImage3DEXT(GLuint texture, GLenum target, GLint level,
                              GLint xoffset, GLint yoffset, GLint zoffset,
                              GLint x, GLint y, GLsizei width, GLint height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_TEXTURE_SUB_IMAGE3D, 10);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = zoffset;
      n[7].i = x;
      n[8].i = y;
      n[9].i = width;
      n[10].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyTextureSubImage3DEXT(ctx->Exec, (texture, target, level,
                                                xoffset, yoffset, zoffset,
                                                x, y, width, height));
   }
}


static void GLAPIENTRY
save_BindMultiTextureEXT(GLenum texunit, GLenum target, GLuint texture)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_BIND_MULTITEXTURE, 3);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].ui = texture;
   }
   if (ctx->ExecuteFlag) {
      CALL_BindMultiTextureEXT(ctx->Exec, (texunit, target, texture));
   }
}


static void GLAPIENTRY
save_MultiTexParameterfvEXT(GLenum texunit, GLenum target, GLenum pname,
                           const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MULTITEXPARAMETER_F, 7);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].e = pname;
      n[4].f = params[0];
      n[5].f = params[1];
      n[6].f = params[2];
      n[7].f = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexParameterfvEXT(ctx->Exec, (texunit, target, pname, params));
   }
}


static void GLAPIENTRY
save_MultiTexParameterfEXT(GLenum texunit, GLenum target, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_MultiTexParameterfvEXT(texunit, target, pname, parray);
}

static void GLAPIENTRY
save_MultiTexParameterivEXT(GLenum texunit, GLenum target, GLenum pname, const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MULTITEXPARAMETER_I, 7);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].e = pname;
      n[4].i = params[0];
      n[5].i = params[1];
      n[6].i = params[2];
      n[7].i = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexParameterivEXT(ctx->Exec, (texunit, target, pname, params));
   }
}

static void GLAPIENTRY
save_MultiTexParameterIivEXT(GLenum texunit, GLenum target, GLenum pname, const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MULTITEXPARAMETER_II, 7);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].e = pname;
      n[4].i = params[0];
      n[5].i = params[1];
      n[6].i = params[2];
      n[7].i = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexParameterIivEXT(ctx->Exec, (texunit, target, pname, params));
   }
}

static void GLAPIENTRY
save_MultiTexParameterIuivEXT(GLenum texunit, GLenum target, GLenum pname, const GLuint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MULTITEXPARAMETER_IUI, 7);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].e = pname;
      n[4].ui = params[0];
      n[5].ui = params[1];
      n[6].ui = params[2];
      n[7].ui = params[3];
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexParameterIuivEXT(ctx->Exec, (texunit, target, pname, params));
   }
}

static void GLAPIENTRY
save_MultiTexParameteriEXT(GLenum texunit, GLenum target, GLenum pname, GLint param)
{
   GLint fparam[4];
   fparam[0] = param;
   fparam[1] = fparam[2] = fparam[3] = 0;
   save_MultiTexParameterivEXT(texunit, target, pname, fparam);
}


static void GLAPIENTRY
save_MultiTexImage1DEXT(GLenum texunit, GLenum target,
                        GLint level, GLint components,
                        GLsizei width, GLint border,
                        GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_1D) {
      /* don't compile, execute immediately */
      CALL_MultiTexImage1DEXT(ctx->Exec, (texunit, target, level, components, width,
                                         border, format, type, pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_MULTITEX_IMAGE1D, 8 + POINTER_DWORDS);
      if (n) {
         n[1].e = texunit;
         n[2].e = target;
         n[3].i = level;
         n[4].i = components;
         n[5].i = (GLint) width;
         n[6].i = border;
         n[7].e = format;
         n[8].e = type;
         save_pointer(&n[9],
                      unpack_image(ctx, 1, width, 1, 1, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_MultiTexImage1DEXT(ctx->Exec, (texunit, target, level, components, width,
                                            border, format, type, pixels));
      }
   }
}


static void GLAPIENTRY
save_MultiTexImage2DEXT(GLenum texunit, GLenum target,
                       GLint level, GLint components,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_2D) {
      /* don't compile, execute immediately */
      CALL_MultiTexImage2DEXT(ctx->Exec, (texunit, target, level, components, width,
                                         height, border, format, type, pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_MULTITEX_IMAGE2D, 9 + POINTER_DWORDS);
      if (n) {
         n[1].e = texunit;
         n[2].e = target;
         n[3].i = level;
         n[4].i = components;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = border;
         n[8].e = format;
         n[9].e = type;
         save_pointer(&n[10],
                      unpack_image(ctx, 2, width, height, 1, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_MultiTexImage2DEXT(ctx->Exec, (texunit, target, level, components, width,
                                            height, border, format, type, pixels));
      }
   }
}


static void GLAPIENTRY
save_MultiTexImage3DEXT(GLenum texunit, GLenum target,
                       GLint level, GLint internalFormat,
                       GLsizei width, GLsizei height, GLsizei depth,
                       GLint border,
                       GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_3D) {
      /* don't compile, execute immediately */
      CALL_MultiTexImage3DEXT(ctx->Exec, (texunit, target, level, internalFormat, width,
                                         height, depth, border, format, type,
                                         pixels));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
      n = alloc_instruction(ctx, OPCODE_MULTITEX_IMAGE3D, 10 + POINTER_DWORDS);
      if (n) {
         n[1].e = texunit;
         n[2].e = target;
         n[3].i = level;
         n[4].i = (GLint) internalFormat;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = (GLint) depth;
         n[8].i = border;
         n[9].e = format;
         n[10].e = type;
         save_pointer(&n[11],
                      unpack_image(ctx, 3, width, height, depth, format, type,
                                   pixels, &ctx->Unpack));
      }
      if (ctx->ExecuteFlag) {
         CALL_MultiTexImage3DEXT(ctx->Exec, (texunit, target, level, internalFormat,
                                            width, height, depth, border, format,
                                            type, pixels));
      }
   }
}


static void GLAPIENTRY
save_MultiTexSubImage1DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset,
                   GLsizei width, GLenum format, GLenum type,
                   const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_MULTITEX_SUB_IMAGE1D, 7 + POINTER_DWORDS);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = (GLint) width;
      n[6].e = format;
      n[7].e = type;
      save_pointer(&n[8],
                   unpack_image(ctx, 1, width, 1, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexSubImage1DEXT(ctx->Exec, (texunit, target, level, xoffset, width,
                                            format, type, pixels));
   }
}


static void GLAPIENTRY
save_MultiTexSubImage2DEXT(GLenum texunit, GLenum target, GLint level,
                          GLint xoffset, GLint yoffset,
                          GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_MULTITEX_SUB_IMAGE2D, 9 + POINTER_DWORDS);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = (GLint) width;
      n[7].i = (GLint) height;
      n[8].e = format;
      n[9].e = type;
      save_pointer(&n[10],
                   unpack_image(ctx, 2, width, height, 1, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexSubImage2DEXT(ctx->Exec, (texunit, target, level, xoffset, yoffset,
                                            width, height, format, type, pixels));
   }
}


static void GLAPIENTRY
save_MultiTexSubImage3DEXT(GLenum texunit, GLenum target, GLint level,
                          GLint xoffset, GLint yoffset, GLint zoffset,
                          GLsizei width, GLsizei height, GLsizei depth,
                          GLenum format, GLenum type, const GLvoid * pixels)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_MULTITEX_SUB_IMAGE3D, 11 + POINTER_DWORDS);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = zoffset;
      n[7].i = (GLint) width;
      n[8].i = (GLint) height;
      n[9].i = (GLint) depth;
      n[10].e = format;
      n[11].e = type;
      save_pointer(&n[12],
                   unpack_image(ctx, 3, width, height, depth, format, type,
                                pixels, &ctx->Unpack));
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexSubImage3DEXT(ctx->Exec, (texunit, target, level,
                                            xoffset, yoffset, zoffset,
                                            width, height, depth, format, type,
                                            pixels));
   }
}


static void GLAPIENTRY
save_CopyMultiTexImage1DEXT(GLenum texunit, GLenum target, GLint level,
                           GLenum internalformat, GLint x, GLint y,
                           GLsizei width, GLint border)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_MULTITEX_IMAGE1D, 8);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].e = internalformat;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
      n[8].i = border;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyMultiTexImage1DEXT(ctx->Exec, (texunit, target, level,
                                             internalformat, x, y,
                                             width, border));
   }
}


static void GLAPIENTRY
save_CopyMultiTexImage2DEXT(GLenum texunit, GLenum target, GLint level,
                           GLenum internalformat,
                           GLint x, GLint y, GLsizei width,
                           GLsizei height, GLint border)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_MULTITEX_IMAGE2D, 9);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].e = internalformat;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
      n[8].i = height;
      n[9].i = border;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyMultiTexImage2DEXT(ctx->Exec, (texunit, target, level,
                                             internalformat, x, y,
                                             width, height, border));
   }
}


static void GLAPIENTRY
save_CopyMultiTexSubImage1DEXT(GLenum texunit, GLenum target, GLint level,
                              GLint xoffset, GLint x, GLint y, GLsizei width)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_MULTITEX_SUB_IMAGE1D, 7);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = x;
      n[6].i = y;
      n[7].i = width;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyMultiTexSubImage1DEXT(ctx->Exec,
                             (texunit, target, level, xoffset, x, y, width));
   }
}


static void GLAPIENTRY
save_CopyMultiTexSubImage2DEXT(GLenum texunit, GLenum target, GLint level,
                              GLint xoffset, GLint yoffset,
                              GLint x, GLint y, GLsizei width, GLint height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_MULTITEX_SUB_IMAGE2D, 9);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = x;
      n[7].i = y;
      n[8].i = width;
      n[9].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyMultiTexSubImage2DEXT(ctx->Exec, (texunit, target, level,
                                                xoffset, yoffset,
                                                x, y, width, height));
   }
}


static void GLAPIENTRY
save_CopyMultiTexSubImage3DEXT(GLenum texunit, GLenum target, GLint level,
                              GLint xoffset, GLint yoffset, GLint zoffset,
                              GLint x, GLint y, GLsizei width, GLint height)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_COPY_MULTITEX_SUB_IMAGE3D, 10);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = zoffset;
      n[7].i = x;
      n[8].i = y;
      n[9].i = width;
      n[10].i = height;
   }
   if (ctx->ExecuteFlag) {
      CALL_CopyMultiTexSubImage3DEXT(ctx->Exec, (texunit, target, level,
                                                xoffset, yoffset, zoffset,
                                                x, y, width, height));
   }
}


static void GLAPIENTRY
save_MultiTexEnvfvEXT(GLenum texunit, GLenum target, GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_MULTITEXENV, 7);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].e = pname;
      if (pname == GL_TEXTURE_ENV_COLOR) {
         n[4].f = params[0];
         n[5].f = params[1];
         n[6].f = params[2];
         n[7].f = params[3];
      }
      else {
         n[4].f = params[0];
         n[5].f = n[6].f = n[7].f = 0.0F;
      }
   }
   if (ctx->ExecuteFlag) {
      CALL_MultiTexEnvfvEXT(ctx->Exec, (texunit, target, pname, params));
   }
}


static void GLAPIENTRY
save_MultiTexEnvfEXT(GLenum texunit, GLenum target, GLenum pname, GLfloat param)
{
   GLfloat parray[4];
   parray[0] = (GLfloat) param;
   parray[1] = parray[2] = parray[3] = 0.0F;
   save_MultiTexEnvfvEXT(texunit, target, pname, parray);
}


static void GLAPIENTRY
save_MultiTexEnviEXT(GLenum texunit, GLenum target, GLenum pname, GLint param)
{
   GLfloat p[4];
   p[0] = (GLfloat) param;
   p[1] = p[2] = p[3] = 0.0F;
   save_MultiTexEnvfvEXT(texunit, target, pname, p);
}


static void GLAPIENTRY
save_MultiTexEnvivEXT(GLenum texunit, GLenum target, GLenum pname, const GLint * param)
{
   GLfloat p[4];
   if (pname == GL_TEXTURE_ENV_COLOR) {
      p[0] = INT_TO_FLOAT(param[0]);
      p[1] = INT_TO_FLOAT(param[1]);
      p[2] = INT_TO_FLOAT(param[2]);
      p[3] = INT_TO_FLOAT(param[3]);
   }
   else {
      p[0] = (GLfloat) param[0];
      p[1] = p[2] = p[3] = 0.0F;
   }
   save_MultiTexEnvfvEXT(texunit, target, pname, p);
}


static void GLAPIENTRY
save_CompressedTextureImage1DEXT(GLuint texture, GLenum target, GLint level,
                                 GLenum internalFormat, GLsizei width,
                                 GLint border, GLsizei imageSize,
                                 const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_1D) {
      /* don't compile, execute immediately */
      CALL_CompressedTextureImage1DEXT(ctx->Exec, (texture, target, level,
                                                   internalFormat, width,
                                                   border, imageSize,
                                                   data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEXTURE_IMAGE_1D,
                            7 + POINTER_DWORDS);
      if (n) {
         n[1].ui = texture;
         n[2].e = target;
         n[3].i = level;
         n[4].e = internalFormat;
         n[5].i = (GLint) width;
         n[6].i = border;
         n[7].i = imageSize;
         save_pointer(&n[8],
                      copy_data(data, imageSize, "glCompressedTextureImage1DEXT"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedTextureImage1DEXT(ctx->Exec,
                                          (texture, target, level, internalFormat,
                                           width, border, imageSize, data));
      }
   }
}


static void GLAPIENTRY
save_CompressedTextureImage2DEXT(GLuint texture, GLenum target, GLint level,
                                 GLenum internalFormat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize,
                                 const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_2D) {
      /* don't compile, execute immediately */
      CALL_CompressedTextureImage2DEXT(ctx->Exec, (texture, target, level,
                                                   internalFormat, width, height,
                                                   border, imageSize, data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEXTURE_IMAGE_2D,
                            8 + POINTER_DWORDS);
      if (n) {
         n[1].ui = texture;
         n[2].e = target;
         n[3].i = level;
         n[4].e = internalFormat;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = border;
         n[8].i = imageSize;
         save_pointer(&n[9],
                      copy_data(data, imageSize, "glCompressedTextureImage2DEXT"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedTextureImage2DEXT(ctx->Exec,
                                          (texture, target, level, internalFormat,
                                           width, height, border, imageSize, data));
      }
   }
}


static void GLAPIENTRY
save_CompressedTextureImage3DEXT(GLuint texture, GLenum target, GLint level,
                                 GLenum internalFormat, GLsizei width,
                                 GLsizei height, GLsizei depth, GLint border,
                                 GLsizei imageSize, const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_3D) {
      /* don't compile, execute immediately */
      CALL_CompressedTextureImage3DEXT(ctx->Exec, (texture, target, level,
                                                   internalFormat, width,
                                                   height, depth, border,
                                                   imageSize, data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEXTURE_IMAGE_3D,
                            9 + POINTER_DWORDS);
      if (n) {
         n[1].ui = texture;
         n[2].e = target;
         n[3].i = level;
         n[4].e = internalFormat;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = (GLint) depth;
         n[8].i = border;
         n[9].i = imageSize;
         save_pointer(&n[10],
                      copy_data(data, imageSize, "glCompressedTextureImage3DEXT"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedTextureImage3DEXT(ctx->Exec,
                                          (texture, target, level, internalFormat,
                                           width, height, depth, border, imageSize,
                                           data));
      }
   }
}


static void GLAPIENTRY
save_CompressedTextureSubImage1DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset,
                                    GLsizei width, GLenum format,
                                    GLsizei imageSize, const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_1D,
                         7 + POINTER_DWORDS);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = (GLint) width;
      n[6].e = format;
      n[7].i = imageSize;
      save_pointer(&n[8],
                   copy_data(data, imageSize, "glCompressedTextureSubImage1DEXT"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedTextureSubImage1DEXT(ctx->Exec, (texture, target, level, xoffset,
                                                      width, format, imageSize, data));
   }
}


static void GLAPIENTRY
save_CompressedTextureSubImage2DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset,
                                    GLint yoffset, GLsizei width, GLsizei height,
                                    GLenum format, GLsizei imageSize,
                                    const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_2D,
                         9 + POINTER_DWORDS);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = (GLint) width;
      n[7].i = (GLint) height;
      n[8].e = format;
      n[9].i = imageSize;
      save_pointer(&n[10],
                   copy_data(data, imageSize, "glCompressedTextureSubImage2DEXT"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedTextureSubImage2DEXT(ctx->Exec,
                                          (texture, target, level, xoffset, yoffset,
                                           width, height, format, imageSize, data));
   }
}


static void GLAPIENTRY
save_CompressedTextureSubImage3DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset,
                                    GLint yoffset, GLint zoffset, GLsizei width,
                                    GLsizei height, GLsizei depth, GLenum format,
                                    GLsizei imageSize, const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_3D,
                         11 + POINTER_DWORDS);
   if (n) {
      n[1].ui = texture;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = zoffset;
      n[7].i = (GLint) width;
      n[8].i = (GLint) height;
      n[9].i = (GLint) depth;
      n[10].e = format;
      n[11].i = imageSize;
      save_pointer(&n[12],
                   copy_data(data, imageSize, "glCompressedTextureSubImage3DEXT"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedTextureSubImage3DEXT(ctx->Exec,
                                          (texture, target, level, xoffset, yoffset,
                                           zoffset, width, height, depth, format,
                                           imageSize, data));
   }
}


static void GLAPIENTRY
save_CompressedMultiTexImage1DEXT(GLenum texunit, GLenum target, GLint level,
                                  GLenum internalFormat, GLsizei width,
                                  GLint border, GLsizei imageSize,
                                  const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_1D) {
      /* don't compile, execute immediately */
      CALL_CompressedMultiTexImage1DEXT(ctx->Exec, (texunit, target, level,
                                                   internalFormat, width,
                                                   border, imageSize,
                                                   data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_MULTITEX_IMAGE_1D,
                            7 + POINTER_DWORDS);
      if (n) {
         n[1].e = texunit;
         n[2].e = target;
         n[3].i = level;
         n[4].e = internalFormat;
         n[5].i = (GLint) width;
         n[6].i = border;
         n[7].i = imageSize;
         save_pointer(&n[8],
                      copy_data(data, imageSize, "glCompressedMultiTexImage1DEXT"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedMultiTexImage1DEXT(ctx->Exec,
                                           (texunit, target, level, internalFormat,
                                            width, border, imageSize, data));
      }
   }
}


static void GLAPIENTRY
save_CompressedMultiTexImage2DEXT(GLenum texunit, GLenum target, GLint level,
                                  GLenum internalFormat, GLsizei width,
                                  GLsizei height, GLint border, GLsizei imageSize,
                                  const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_2D) {
      /* don't compile, execute immediately */
      CALL_CompressedMultiTexImage2DEXT(ctx->Exec, (texunit, target, level,
                                                   internalFormat, width, height,
                                                   border, imageSize, data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_MULTITEX_IMAGE_2D,
                            8 + POINTER_DWORDS);
      if (n) {
         n[1].e = texunit;
         n[2].e = target;
         n[3].i = level;
         n[4].e = internalFormat;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = border;
         n[8].i = imageSize;
         save_pointer(&n[9],
                      copy_data(data, imageSize, "glCompressedMultiTexImage2DEXT"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedMultiTexImage2DEXT(ctx->Exec,
                                           (texunit, target, level, internalFormat,
                                            width, height, border, imageSize, data));
      }
   }
}


static void GLAPIENTRY
save_CompressedMultiTexImage3DEXT(GLenum texunit, GLenum target, GLint level,
                                  GLenum internalFormat, GLsizei width,
                                  GLsizei height, GLsizei depth, GLint border,
                                  GLsizei imageSize, const GLvoid * data)
{
   GET_CURRENT_CONTEXT(ctx);
   if (target == GL_PROXY_TEXTURE_3D) {
      /* don't compile, execute immediately */
      CALL_CompressedMultiTexImage3DEXT(ctx->Exec, (texunit, target, level,
                                                   internalFormat, width,
                                                   height, depth, border,
                                                   imageSize, data));
   }
   else {
      Node *n;
      ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

      n = alloc_instruction(ctx, OPCODE_COMPRESSED_MULTITEX_IMAGE_3D,
                            9 + POINTER_DWORDS);
      if (n) {
         n[1].e = texunit;
         n[2].e = target;
         n[3].i = level;
         n[4].e = internalFormat;
         n[5].i = (GLint) width;
         n[6].i = (GLint) height;
         n[7].i = (GLint) depth;
         n[8].i = border;
         n[9].i = imageSize;
         save_pointer(&n[10],
                      copy_data(data, imageSize, "glCompressedMultiTexImage3DEXT"));
      }
      if (ctx->ExecuteFlag) {
         CALL_CompressedMultiTexImage3DEXT(ctx->Exec,
                                           (texunit, target, level, internalFormat,
                                            width, height, depth, border, imageSize,
                                            data));
      }
   }
}


static void GLAPIENTRY
save_CompressedMultiTexSubImage1DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset,
                                     GLsizei width, GLenum format,
                                     GLsizei imageSize, const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_1D,
                         7 + POINTER_DWORDS);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = (GLint) width;
      n[6].e = format;
      n[7].i = imageSize;
      save_pointer(&n[8],
                   copy_data(data, imageSize, "glCompressedMultiTexSubImage1DEXT"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedMultiTexSubImage1DEXT(ctx->Exec, (texunit, target, level, xoffset,
                                                       width, format, imageSize, data));
   }
}


static void GLAPIENTRY
save_CompressedMultiTexSubImage2DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset,
                                     GLint yoffset, GLsizei width, GLsizei height,
                                     GLenum format, GLsizei imageSize,
                                     const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_2D,
                         9 + POINTER_DWORDS);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = (GLint) width;
      n[7].i = (GLint) height;
      n[8].e = format;
      n[9].i = imageSize;
      save_pointer(&n[10],
                   copy_data(data, imageSize, "glCompressedMultiTexSubImage2DEXT"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedMultiTexSubImage2DEXT(ctx->Exec,
                                           (texunit, target, level, xoffset, yoffset,
                                            width, height, format, imageSize, data));
   }
}


static void GLAPIENTRY
save_CompressedMultiTexSubImage3DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset,
                                     GLint yoffset, GLint zoffset, GLsizei width,
                                     GLsizei height, GLsizei depth, GLenum format,
                                     GLsizei imageSize, const GLvoid * data)
{
   Node *n;
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_3D,
                         11 + POINTER_DWORDS);
   if (n) {
      n[1].e = texunit;
      n[2].e = target;
      n[3].i = level;
      n[4].i = xoffset;
      n[5].i = yoffset;
      n[6].i = zoffset;
      n[7].i = (GLint) width;
      n[8].i = (GLint) height;
      n[9].i = (GLint) depth;
      n[10].e = format;
      n[11].i = imageSize;
      save_pointer(&n[12],
                   copy_data(data, imageSize, "glCompressedMultiTexSubImage3DEXT"));
   }
   if (ctx->ExecuteFlag) {
      CALL_CompressedMultiTexSubImage3DEXT(ctx->Exec,
                                           (texunit, target, level, xoffset, yoffset,
                                            zoffset, width, height, depth, format,
                                            imageSize, data));
   }
}


static void GLAPIENTRY
save_NamedProgramStringEXT(GLuint program, GLenum target, GLenum format, GLsizei len,
                           const GLvoid * string)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;

   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);

   n = alloc_instruction(ctx, OPCODE_NAMED_PROGRAM_STRING, 4 + POINTER_DWORDS);
   if (n) {
      GLubyte *programCopy = malloc(len);
      if (!programCopy) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "glNamedProgramStringEXT");
         return;
      }
      memcpy(programCopy, string, len);
      n[1].ui = program;
      n[2].e = target;
      n[3].e = format;
      n[4].i = len;
      save_pointer(&n[5], programCopy);
   }
   if (ctx->ExecuteFlag) {
      CALL_NamedProgramStringEXT(ctx->Exec, (program, target, format, len, string));
   }
}


static void GLAPIENTRY
save_NamedProgramLocalParameter4fEXT(GLuint program, GLenum target, GLuint index,
                                     GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
   GET_CURRENT_CONTEXT(ctx);
   Node *n;
   ASSERT_OUTSIDE_SAVE_BEGIN_END_AND_FLUSH(ctx);
   n = alloc_instruction(ctx, OPCODE_NAMED_PROGRAM_LOCAL_PARAMETER, 7);
   if (n) {
      n[1].ui = program;
      n[2].e = target;
      n[3].ui = index;
      n[4].f = x;
      n[5].f = y;
      n[6].f = z;
      n[7].f = w;
   }
   if (ctx->ExecuteFlag) {
      CALL_NamedProgramLocalParameter4fEXT(ctx->Exec, (program, target, index, x, y, z, w));
   }
}


static void GLAPIENTRY
save_NamedProgramLocalParameter4fvEXT(GLuint program, GLenum target, GLuint index,
                                      const GLfloat *params)
{
   save_NamedProgramLocalParameter4fEXT(program, target, index, params[0],
                                        params[1], params[2], params[3]);
}


static void GLAPIENTRY
save_NamedProgramLocalParameter4dEXT(GLuint program, GLenum target, GLuint index,
                                    GLdouble x, GLdouble y,
                                    GLdouble z, GLdouble w)
{
      save_NamedProgramLocalParameter4fEXT(program, target, index, (GLfloat) x,
                                           (GLfloat) y, (GLfloat) z, (GLfloat) w);
}


static void GLAPIENTRY
save_NamedProgramLocalParameter4dvEXT(GLuint program, GLenum target, GLuint index,
                                      const GLdouble *params)
{
   save_NamedProgramLocalParameter4fEXT(program, target, index, (GLfloat) params[0],
                                        (GLfloat) params[1], (GLfloat) params[2],
                                        (GLfloat) params[3]);
}


/**
 * Save an error-generating command into display list.
 *
 * KW: Will appear in the list before the vertex buffer containing the
 * command that provoked the error.  I don't see this as a problem.
 */
static void
save_error(struct gl_context *ctx, GLenum error, const char *s)
{
   Node *n;
   n = alloc_instruction(ctx, OPCODE_ERROR, 1 + POINTER_DWORDS);
   if (n) {
      n[1].e = error;
      save_pointer(&n[2], (void *) s);
      /* note: the data/string here doesn't have to be freed in
       * _mesa_delete_list() since the string is never dynamically
       * allocated.
       */
   }
}


/**
 * Compile an error into current display list.
 */
void
_mesa_compile_error(struct gl_context *ctx, GLenum error, const char *s)
{
   if (ctx->CompileFlag)
      save_error(ctx, error, s);
   if (ctx->ExecuteFlag)
      _mesa_error(ctx, error, "%s", s);
}


/**
 * Test if ID names a display list.
 */
bool
_mesa_get_list(struct gl_context *ctx, GLuint list,
               struct gl_display_list **dlist)
{
   struct gl_display_list * dl =
      list > 0 ? _mesa_lookup_list(ctx, list) : NULL;

   if (dlist)
      *dlist = dl;

   return dl != NULL;
}



/**********************************************************************/
/*                     Display list execution                         */
/**********************************************************************/


/*
 * Execute a display list.  Note that the ListBase offset must have already
 * been added before calling this function.  I.e. the list argument is
 * the absolute list number, not relative to ListBase.
 * \param list - display list number
 */
static void
execute_list(struct gl_context *ctx, GLuint list)
{
   struct gl_display_list *dlist;
   Node *n;

   if (list == 0 || !_mesa_get_list(ctx, list, &dlist))
      return;

   if (ctx->ListState.CallDepth == MAX_LIST_NESTING) {
      /* raise an error? */
      return;
   }

   ctx->ListState.CallDepth++;

   vbo_save_BeginCallList(ctx, dlist);

   n = dlist->Head;

   while (1) {
      const OpCode opcode = n[0].opcode;

      switch (opcode) {
         case OPCODE_ERROR:
            _mesa_error(ctx, n[1].e, "%s", (const char *) get_pointer(&n[2]));
            break;
         case OPCODE_ACCUM:
            CALL_Accum(ctx->Exec, (n[1].e, n[2].f));
            break;
         case OPCODE_ALPHA_FUNC:
            CALL_AlphaFunc(ctx->Exec, (n[1].e, n[2].f));
            break;
         case OPCODE_BIND_TEXTURE:
            CALL_BindTexture(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_BITMAP:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_Bitmap(ctx->Exec, ((GLsizei) n[1].i, (GLsizei) n[2].i,
                                       n[3].f, n[4].f, n[5].f, n[6].f,
                                       get_pointer(&n[7])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_BLEND_COLOR:
            CALL_BlendColor(ctx->Exec, (n[1].f, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_BLEND_EQUATION:
            CALL_BlendEquation(ctx->Exec, (n[1].e));
            break;
         case OPCODE_BLEND_EQUATION_SEPARATE:
            CALL_BlendEquationSeparate(ctx->Exec, (n[1].e, n[2].e));
            break;
         case OPCODE_BLEND_FUNC_SEPARATE:
            CALL_BlendFuncSeparate(ctx->Exec,
                                      (n[1].e, n[2].e, n[3].e, n[4].e));
            break;

         case OPCODE_BLEND_FUNC_I:
            /* GL_ARB_draw_buffers_blend */
            CALL_BlendFunciARB(ctx->Exec, (n[1].ui, n[2].e, n[3].e));
            break;
         case OPCODE_BLEND_FUNC_SEPARATE_I:
            /* GL_ARB_draw_buffers_blend */
            CALL_BlendFuncSeparateiARB(ctx->Exec, (n[1].ui, n[2].e, n[3].e,
                                                   n[4].e, n[5].e));
            break;
         case OPCODE_BLEND_EQUATION_I:
            /* GL_ARB_draw_buffers_blend */
            CALL_BlendEquationiARB(ctx->Exec, (n[1].ui, n[2].e));
            break;
         case OPCODE_BLEND_EQUATION_SEPARATE_I:
            /* GL_ARB_draw_buffers_blend */
            CALL_BlendEquationSeparateiARB(ctx->Exec,
                                           (n[1].ui, n[2].e, n[3].e));
            break;

         case OPCODE_CALL_LIST:
            /* Generated by glCallList(), don't add ListBase */
            if (ctx->ListState.CallDepth < MAX_LIST_NESTING) {
               execute_list(ctx, n[1].ui);
            }
            break;
         case OPCODE_CALL_LISTS:
            if (ctx->ListState.CallDepth < MAX_LIST_NESTING) {
               CALL_CallLists(ctx->Exec, (n[1].i, n[2].e, get_pointer(&n[3])));
            }
            break;
         case OPCODE_CLEAR:
            CALL_Clear(ctx->Exec, (n[1].bf));
            break;
         case OPCODE_CLEAR_BUFFER_IV:
            {
               GLint value[4];
               value[0] = n[3].i;
               value[1] = n[4].i;
               value[2] = n[5].i;
               value[3] = n[6].i;
               CALL_ClearBufferiv(ctx->Exec, (n[1].e, n[2].i, value));
            }
            break;
         case OPCODE_CLEAR_BUFFER_UIV:
            {
               GLuint value[4];
               value[0] = n[3].ui;
               value[1] = n[4].ui;
               value[2] = n[5].ui;
               value[3] = n[6].ui;
               CALL_ClearBufferuiv(ctx->Exec, (n[1].e, n[2].i, value));
            }
            break;
         case OPCODE_CLEAR_BUFFER_FV:
            {
               GLfloat value[4];
               value[0] = n[3].f;
               value[1] = n[4].f;
               value[2] = n[5].f;
               value[3] = n[6].f;
               CALL_ClearBufferfv(ctx->Exec, (n[1].e, n[2].i, value));
            }
            break;
         case OPCODE_CLEAR_BUFFER_FI:
            CALL_ClearBufferfi(ctx->Exec, (n[1].e, n[2].i, n[3].f, n[4].i));
            break;
         case OPCODE_CLEAR_COLOR:
            CALL_ClearColor(ctx->Exec, (n[1].f, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_CLEAR_ACCUM:
            CALL_ClearAccum(ctx->Exec, (n[1].f, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_CLEAR_DEPTH:
            CALL_ClearDepth(ctx->Exec, ((GLclampd) n[1].f));
            break;
         case OPCODE_CLEAR_INDEX:
            CALL_ClearIndex(ctx->Exec, ((GLfloat) n[1].ui));
            break;
         case OPCODE_CLEAR_STENCIL:
            CALL_ClearStencil(ctx->Exec, (n[1].i));
            break;
         case OPCODE_CLIP_PLANE:
            {
               GLdouble eq[4];
               eq[0] = n[2].f;
               eq[1] = n[3].f;
               eq[2] = n[4].f;
               eq[3] = n[5].f;
               CALL_ClipPlane(ctx->Exec, (n[1].e, eq));
            }
            break;
         case OPCODE_COLOR_MASK:
            CALL_ColorMask(ctx->Exec, (n[1].b, n[2].b, n[3].b, n[4].b));
            break;
         case OPCODE_COLOR_MASK_INDEXED:
            CALL_ColorMaski(ctx->Exec, (n[1].ui, n[2].b, n[3].b,
                                                 n[4].b, n[5].b));
            break;
         case OPCODE_COLOR_MATERIAL:
            CALL_ColorMaterial(ctx->Exec, (n[1].e, n[2].e));
            break;
         case OPCODE_COPY_PIXELS:
            CALL_CopyPixels(ctx->Exec, (n[1].i, n[2].i,
                                        (GLsizei) n[3].i, (GLsizei) n[4].i,
                                        n[5].e));
            break;
         case OPCODE_COPY_TEX_IMAGE1D:
            CALL_CopyTexImage1D(ctx->Exec, (n[1].e, n[2].i, n[3].e, n[4].i,
                                            n[5].i, n[6].i, n[7].i));
            break;
         case OPCODE_COPY_TEX_IMAGE2D:
            CALL_CopyTexImage2D(ctx->Exec, (n[1].e, n[2].i, n[3].e, n[4].i,
                                            n[5].i, n[6].i, n[7].i, n[8].i));
            break;
         case OPCODE_COPY_TEX_SUB_IMAGE1D:
            CALL_CopyTexSubImage1D(ctx->Exec, (n[1].e, n[2].i, n[3].i,
                                               n[4].i, n[5].i, n[6].i));
            break;
         case OPCODE_COPY_TEX_SUB_IMAGE2D:
            CALL_CopyTexSubImage2D(ctx->Exec, (n[1].e, n[2].i, n[3].i,
                                               n[4].i, n[5].i, n[6].i, n[7].i,
                                               n[8].i));
            break;
         case OPCODE_COPY_TEX_SUB_IMAGE3D:
            CALL_CopyTexSubImage3D(ctx->Exec, (n[1].e, n[2].i, n[3].i,
                                               n[4].i, n[5].i, n[6].i, n[7].i,
                                               n[8].i, n[9].i));
            break;
         case OPCODE_CULL_FACE:
            CALL_CullFace(ctx->Exec, (n[1].e));
            break;
         case OPCODE_DEPTH_FUNC:
            CALL_DepthFunc(ctx->Exec, (n[1].e));
            break;
         case OPCODE_DEPTH_MASK:
            CALL_DepthMask(ctx->Exec, (n[1].b));
            break;
         case OPCODE_DEPTH_RANGE:
            CALL_DepthRange(ctx->Exec,
                            ((GLclampd) n[1].f, (GLclampd) n[2].f));
            break;
         case OPCODE_DISABLE:
            CALL_Disable(ctx->Exec, (n[1].e));
            break;
         case OPCODE_DISABLE_INDEXED:
            CALL_Disablei(ctx->Exec, (n[1].ui, n[2].e));
            break;
         case OPCODE_DRAW_BUFFER:
            CALL_DrawBuffer(ctx->Exec, (n[1].e));
            break;
         case OPCODE_DRAW_PIXELS:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_DrawPixels(ctx->Exec, (n[1].i, n[2].i, n[3].e, n[4].e,
                                           get_pointer(&n[5])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_ENABLE:
            CALL_Enable(ctx->Exec, (n[1].e));
            break;
         case OPCODE_ENABLE_INDEXED:
            CALL_Enablei(ctx->Exec, (n[1].ui, n[2].e));
            break;
         case OPCODE_EVALMESH1:
            CALL_EvalMesh1(ctx->Exec, (n[1].e, n[2].i, n[3].i));
            break;
         case OPCODE_EVALMESH2:
            CALL_EvalMesh2(ctx->Exec,
                           (n[1].e, n[2].i, n[3].i, n[4].i, n[5].i));
            break;
         case OPCODE_FOG:
            {
               GLfloat p[4];
               p[0] = n[2].f;
               p[1] = n[3].f;
               p[2] = n[4].f;
               p[3] = n[5].f;
               CALL_Fogfv(ctx->Exec, (n[1].e, p));
            }
            break;
         case OPCODE_FRONT_FACE:
            CALL_FrontFace(ctx->Exec, (n[1].e));
            break;
         case OPCODE_FRUSTUM:
            CALL_Frustum(ctx->Exec,
                         (n[1].f, n[2].f, n[3].f, n[4].f, n[5].f, n[6].f));
            break;
         case OPCODE_HINT:
            CALL_Hint(ctx->Exec, (n[1].e, n[2].e));
            break;
         case OPCODE_INDEX_MASK:
            CALL_IndexMask(ctx->Exec, (n[1].ui));
            break;
         case OPCODE_INIT_NAMES:
            CALL_InitNames(ctx->Exec, ());
            break;
         case OPCODE_LIGHT:
            {
               GLfloat p[4];
               p[0] = n[3].f;
               p[1] = n[4].f;
               p[2] = n[5].f;
               p[3] = n[6].f;
               CALL_Lightfv(ctx->Exec, (n[1].e, n[2].e, p));
            }
            break;
         case OPCODE_LIGHT_MODEL:
            {
               GLfloat p[4];
               p[0] = n[2].f;
               p[1] = n[3].f;
               p[2] = n[4].f;
               p[3] = n[5].f;
               CALL_LightModelfv(ctx->Exec, (n[1].e, p));
            }
            break;
         case OPCODE_LINE_STIPPLE:
            CALL_LineStipple(ctx->Exec, (n[1].i, n[2].us));
            break;
         case OPCODE_LINE_WIDTH:
            CALL_LineWidth(ctx->Exec, (n[1].f));
            break;
         case OPCODE_LIST_BASE:
            CALL_ListBase(ctx->Exec, (n[1].ui));
            break;
         case OPCODE_LOAD_IDENTITY:
            CALL_LoadIdentity(ctx->Exec, ());
            break;
         case OPCODE_LOAD_MATRIX:
            STATIC_ASSERT(sizeof(Node) == sizeof(GLfloat));
            CALL_LoadMatrixf(ctx->Exec, (&n[1].f));
            break;
         case OPCODE_LOAD_NAME:
            CALL_LoadName(ctx->Exec, (n[1].ui));
            break;
         case OPCODE_LOGIC_OP:
            CALL_LogicOp(ctx->Exec, (n[1].e));
            break;
         case OPCODE_MAP1:
            {
               GLenum target = n[1].e;
               GLint ustride = _mesa_evaluator_components(target);
               GLint uorder = n[5].i;
               GLfloat u1 = n[2].f;
               GLfloat u2 = n[3].f;
               CALL_Map1f(ctx->Exec, (target, u1, u2, ustride, uorder,
                                      (GLfloat *) get_pointer(&n[6])));
            }
            break;
         case OPCODE_MAP2:
            {
               GLenum target = n[1].e;
               GLfloat u1 = n[2].f;
               GLfloat u2 = n[3].f;
               GLfloat v1 = n[4].f;
               GLfloat v2 = n[5].f;
               GLint ustride = n[6].i;
               GLint vstride = n[7].i;
               GLint uorder = n[8].i;
               GLint vorder = n[9].i;
               CALL_Map2f(ctx->Exec, (target, u1, u2, ustride, uorder,
                                      v1, v2, vstride, vorder,
                                      (GLfloat *) get_pointer(&n[10])));
            }
            break;
         case OPCODE_MAPGRID1:
            CALL_MapGrid1f(ctx->Exec, (n[1].i, n[2].f, n[3].f));
            break;
         case OPCODE_MAPGRID2:
            CALL_MapGrid2f(ctx->Exec,
                           (n[1].i, n[2].f, n[3].f, n[4].i, n[5].f, n[6].f));
            break;
         case OPCODE_MATRIX_MODE:
            CALL_MatrixMode(ctx->Exec, (n[1].e));
            break;
         case OPCODE_MULT_MATRIX:
            CALL_MultMatrixf(ctx->Exec, (&n[1].f));
            break;
         case OPCODE_ORTHO:
            CALL_Ortho(ctx->Exec,
                       (n[1].f, n[2].f, n[3].f, n[4].f, n[5].f, n[6].f));
            break;
         case OPCODE_PASSTHROUGH:
            CALL_PassThrough(ctx->Exec, (n[1].f));
            break;
         case OPCODE_PATCH_PARAMETER_I:
            CALL_PatchParameteri(ctx->Exec, (n[1].e, n[2].i));
            break;
         case OPCODE_PATCH_PARAMETER_FV_INNER:
            {
               GLfloat params[2];
               params[0] = n[2].f;
               params[1] = n[3].f;
               CALL_PatchParameterfv(ctx->Exec, (n[1].e, params));
            }
            break;
         case OPCODE_PATCH_PARAMETER_FV_OUTER:
            {
               GLfloat params[4];
               params[0] = n[2].f;
               params[1] = n[3].f;
               params[2] = n[4].f;
               params[3] = n[5].f;
               CALL_PatchParameterfv(ctx->Exec, (n[1].e, params));
            }
            break;
         case OPCODE_PIXEL_MAP:
            CALL_PixelMapfv(ctx->Exec,
                            (n[1].e, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_PIXEL_TRANSFER:
            CALL_PixelTransferf(ctx->Exec, (n[1].e, n[2].f));
            break;
         case OPCODE_PIXEL_ZOOM:
            CALL_PixelZoom(ctx->Exec, (n[1].f, n[2].f));
            break;
         case OPCODE_POINT_SIZE:
            CALL_PointSize(ctx->Exec, (n[1].f));
            break;
         case OPCODE_POINT_PARAMETERS:
            {
               GLfloat params[3];
               params[0] = n[2].f;
               params[1] = n[3].f;
               params[2] = n[4].f;
               CALL_PointParameterfv(ctx->Exec, (n[1].e, params));
            }
            break;
         case OPCODE_POLYGON_MODE:
            CALL_PolygonMode(ctx->Exec, (n[1].e, n[2].e));
            break;
         case OPCODE_POLYGON_STIPPLE:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_PolygonStipple(ctx->Exec, (get_pointer(&n[1])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_POLYGON_OFFSET:
            CALL_PolygonOffset(ctx->Exec, (n[1].f, n[2].f));
            break;
         case OPCODE_POLYGON_OFFSET_CLAMP:
            CALL_PolygonOffsetClampEXT(ctx->Exec, (n[1].f, n[2].f, n[3].f));
            break;
         case OPCODE_POP_ATTRIB:
            CALL_PopAttrib(ctx->Exec, ());
            break;
         case OPCODE_POP_MATRIX:
            CALL_PopMatrix(ctx->Exec, ());
            break;
         case OPCODE_POP_NAME:
            CALL_PopName(ctx->Exec, ());
            break;
         case OPCODE_PRIORITIZE_TEXTURE:
            CALL_PrioritizeTextures(ctx->Exec, (1, &n[1].ui, &n[2].f));
            break;
         case OPCODE_PUSH_ATTRIB:
            CALL_PushAttrib(ctx->Exec, (n[1].bf));
            break;
         case OPCODE_PUSH_MATRIX:
            CALL_PushMatrix(ctx->Exec, ());
            break;
         case OPCODE_PUSH_NAME:
            CALL_PushName(ctx->Exec, (n[1].ui));
            break;
         case OPCODE_RASTER_POS:
            CALL_RasterPos4f(ctx->Exec, (n[1].f, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_READ_BUFFER:
            CALL_ReadBuffer(ctx->Exec, (n[1].e));
            break;
         case OPCODE_ROTATE:
            CALL_Rotatef(ctx->Exec, (n[1].f, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_SCALE:
            CALL_Scalef(ctx->Exec, (n[1].f, n[2].f, n[3].f));
            break;
         case OPCODE_SCISSOR:
            CALL_Scissor(ctx->Exec, (n[1].i, n[2].i, n[3].i, n[4].i));
            break;
         case OPCODE_SHADE_MODEL:
            CALL_ShadeModel(ctx->Exec, (n[1].e));
            break;
         case OPCODE_PROVOKING_VERTEX:
            CALL_ProvokingVertex(ctx->Exec, (n[1].e));
            break;
         case OPCODE_STENCIL_FUNC:
            CALL_StencilFunc(ctx->Exec, (n[1].e, n[2].i, n[3].ui));
            break;
         case OPCODE_STENCIL_MASK:
            CALL_StencilMask(ctx->Exec, (n[1].ui));
            break;
         case OPCODE_STENCIL_OP:
            CALL_StencilOp(ctx->Exec, (n[1].e, n[2].e, n[3].e));
            break;
         case OPCODE_STENCIL_FUNC_SEPARATE:
            CALL_StencilFuncSeparate(ctx->Exec,
                                     (n[1].e, n[2].e, n[3].i, n[4].ui));
            break;
         case OPCODE_STENCIL_MASK_SEPARATE:
            CALL_StencilMaskSeparate(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_STENCIL_OP_SEPARATE:
            CALL_StencilOpSeparate(ctx->Exec,
                                   (n[1].e, n[2].e, n[3].e, n[4].e));
            break;
         case OPCODE_TEXENV:
            {
               GLfloat params[4];
               params[0] = n[3].f;
               params[1] = n[4].f;
               params[2] = n[5].f;
               params[3] = n[6].f;
               CALL_TexEnvfv(ctx->Exec, (n[1].e, n[2].e, params));
            }
            break;
         case OPCODE_TEXGEN:
            {
               GLfloat params[4];
               params[0] = n[3].f;
               params[1] = n[4].f;
               params[2] = n[5].f;
               params[3] = n[6].f;
               CALL_TexGenfv(ctx->Exec, (n[1].e, n[2].e, params));
            }
            break;
         case OPCODE_TEXPARAMETER:
            {
               GLfloat params[4];
               params[0] = n[3].f;
               params[1] = n[4].f;
               params[2] = n[5].f;
               params[3] = n[6].f;
               CALL_TexParameterfv(ctx->Exec, (n[1].e, n[2].e, params));
            }
            break;
         case OPCODE_TEX_IMAGE1D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TexImage1D(ctx->Exec, (n[1].e,      /* target */
                                           n[2].i,      /* level */
                                           n[3].i,      /* components */
                                           n[4].i,      /* width */
                                           n[5].e,      /* border */
                                           n[6].e,      /* format */
                                           n[7].e,      /* type */
                                           get_pointer(&n[8])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEX_IMAGE2D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TexImage2D(ctx->Exec, (n[1].e,      /* target */
                                           n[2].i,      /* level */
                                           n[3].i,      /* components */
                                           n[4].i,      /* width */
                                           n[5].i,      /* height */
                                           n[6].e,      /* border */
                                           n[7].e,      /* format */
                                           n[8].e,      /* type */
                                           get_pointer(&n[9])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEX_IMAGE3D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TexImage3D(ctx->Exec, (n[1].e,      /* target */
                                           n[2].i,      /* level */
                                           n[3].i,      /* components */
                                           n[4].i,      /* width */
                                           n[5].i,      /* height */
                                           n[6].i,      /* depth  */
                                           n[7].e,      /* border */
                                           n[8].e,      /* format */
                                           n[9].e,      /* type */
                                           get_pointer(&n[10])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEX_SUB_IMAGE1D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TexSubImage1D(ctx->Exec, (n[1].e, n[2].i, n[3].i,
                                              n[4].i, n[5].e,
                                              n[6].e, get_pointer(&n[7])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEX_SUB_IMAGE2D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TexSubImage2D(ctx->Exec, (n[1].e, n[2].i, n[3].i,
                                              n[4].i, n[5].e,
                                              n[6].i, n[7].e, n[8].e,
                                              get_pointer(&n[9])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEX_SUB_IMAGE3D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TexSubImage3D(ctx->Exec, (n[1].e, n[2].i, n[3].i,
                                              n[4].i, n[5].i, n[6].i, n[7].i,
                                              n[8].i, n[9].e, n[10].e,
                                              get_pointer(&n[11])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TRANSLATE:
            CALL_Translatef(ctx->Exec, (n[1].f, n[2].f, n[3].f));
            break;
         case OPCODE_VIEWPORT:
            CALL_Viewport(ctx->Exec, (n[1].i, n[2].i,
                                      (GLsizei) n[3].i, (GLsizei) n[4].i));
            break;
         case OPCODE_WINDOW_POS:
            CALL_WindowPos4fMESA(ctx->Exec, (n[1].f, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_VIEWPORT_ARRAY_V:
            CALL_ViewportArrayv(ctx->Exec, (n[1].ui, n[2].si,
                                            get_pointer(&n[3])));
            break;
         case OPCODE_VIEWPORT_INDEXED_F:
            CALL_ViewportIndexedf(ctx->Exec, (n[1].ui, n[2].f, n[3].f, n[4].f,
                                              n[5].f));
            break;
         case OPCODE_VIEWPORT_INDEXED_FV: {
            GLfloat v[4];
            v[0] = n[2].f;
            v[1] = n[3].f;
            v[2] = n[4].f;
            v[3] = n[5].f;
            CALL_ViewportIndexedfv(ctx->Exec, (n[1].ui, v));
            break;
         }
         case OPCODE_SCISSOR_ARRAY_V:
            CALL_ScissorArrayv(ctx->Exec, (n[1].ui, n[2].si,
                                           get_pointer(&n[3])));
            break;
         case OPCODE_SCISSOR_INDEXED:
            CALL_ScissorIndexed(ctx->Exec, (n[1].ui, n[2].i, n[3].i, n[4].si,
                                            n[5].si));
            break;
         case OPCODE_SCISSOR_INDEXED_V: {
            GLint v[4];
            v[0] = n[2].i;
            v[1] = n[3].i;
            v[2] = n[4].si;
            v[3] = n[5].si;
            CALL_ScissorIndexedv(ctx->Exec, (n[1].ui, v));
            break;
         }
         case OPCODE_DEPTH_ARRAY_V:
            CALL_DepthRangeArrayv(ctx->Exec, (n[1].ui, n[2].si,
                                              get_pointer(&n[3])));
            break;
         case OPCODE_DEPTH_INDEXED:
            CALL_DepthRangeIndexed(ctx->Exec, (n[1].ui, n[2].f, n[3].f));
            break;
         case OPCODE_ACTIVE_TEXTURE:   /* GL_ARB_multitexture */
            CALL_ActiveTexture(ctx->Exec, (n[1].e));
            break;
         case OPCODE_COMPRESSED_TEX_IMAGE_1D:  /* GL_ARB_texture_compression */
            CALL_CompressedTexImage1D(ctx->Exec, (n[1].e, n[2].i, n[3].e,
                                                  n[4].i, n[5].i, n[6].i,
                                                  get_pointer(&n[7])));
            break;
         case OPCODE_COMPRESSED_TEX_IMAGE_2D:  /* GL_ARB_texture_compression */
            CALL_CompressedTexImage2D(ctx->Exec, (n[1].e, n[2].i, n[3].e,
                                                  n[4].i, n[5].i, n[6].i,
                                                  n[7].i, get_pointer(&n[8])));
            break;
         case OPCODE_COMPRESSED_TEX_IMAGE_3D:  /* GL_ARB_texture_compression */
            CALL_CompressedTexImage3D(ctx->Exec, (n[1].e, n[2].i, n[3].e,
                                                  n[4].i, n[5].i, n[6].i,
                                                  n[7].i, n[8].i,
                                                  get_pointer(&n[9])));
            break;
         case OPCODE_COMPRESSED_TEX_SUB_IMAGE_1D:      /* GL_ARB_texture_compress */
            CALL_CompressedTexSubImage1D(ctx->Exec,
                                            (n[1].e, n[2].i, n[3].i, n[4].i,
                                             n[5].e, n[6].i,
                                             get_pointer(&n[7])));
            break;
         case OPCODE_COMPRESSED_TEX_SUB_IMAGE_2D:      /* GL_ARB_texture_compress */
            CALL_CompressedTexSubImage2D(ctx->Exec,
                                            (n[1].e, n[2].i, n[3].i, n[4].i,
                                             n[5].i, n[6].i, n[7].e, n[8].i,
                                             get_pointer(&n[9])));
            break;
         case OPCODE_COMPRESSED_TEX_SUB_IMAGE_3D:      /* GL_ARB_texture_compress */
            CALL_CompressedTexSubImage3D(ctx->Exec,
                                            (n[1].e, n[2].i, n[3].i, n[4].i,
                                             n[5].i, n[6].i, n[7].i, n[8].i,
                                             n[9].e, n[10].i,
                                             get_pointer(&n[11])));
            break;
         case OPCODE_SAMPLE_COVERAGE:  /* GL_ARB_multisample */
            CALL_SampleCoverage(ctx->Exec, (n[1].f, n[2].b));
            break;
         case OPCODE_WINDOW_POS_ARB:   /* GL_ARB_window_pos */
            CALL_WindowPos3f(ctx->Exec, (n[1].f, n[2].f, n[3].f));
            break;
         case OPCODE_BIND_PROGRAM_ARB:  /* GL_ARB_vertex_program */
            CALL_BindProgramARB(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_PROGRAM_LOCAL_PARAMETER_ARB:
            CALL_ProgramLocalParameter4fARB(ctx->Exec,
                                            (n[1].e, n[2].ui, n[3].f, n[4].f,
                                             n[5].f, n[6].f));
            break;
         case OPCODE_ACTIVE_STENCIL_FACE_EXT:
            CALL_ActiveStencilFaceEXT(ctx->Exec, (n[1].e));
            break;
         case OPCODE_DEPTH_BOUNDS_EXT:
            CALL_DepthBoundsEXT(ctx->Exec, (n[1].f, n[2].f));
            break;
         case OPCODE_PROGRAM_STRING_ARB:
            CALL_ProgramStringARB(ctx->Exec,
                                  (n[1].e, n[2].e, n[3].i,
                                   get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_ENV_PARAMETER_ARB:
            CALL_ProgramEnvParameter4fARB(ctx->Exec, (n[1].e, n[2].ui, n[3].f,
                                                      n[4].f, n[5].f,
                                                      n[6].f));
            break;
         case OPCODE_BEGIN_QUERY_ARB:
            CALL_BeginQuery(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_END_QUERY_ARB:
            CALL_EndQuery(ctx->Exec, (n[1].e));
            break;
         case OPCODE_QUERY_COUNTER:
            CALL_QueryCounter(ctx->Exec, (n[1].ui, n[2].e));
            break;
         case OPCODE_BEGIN_QUERY_INDEXED:
            CALL_BeginQueryIndexed(ctx->Exec, (n[1].e, n[2].ui, n[3].ui));
            break;
         case OPCODE_END_QUERY_INDEXED:
            CALL_EndQueryIndexed(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_DRAW_BUFFERS_ARB:
            {
               GLenum buffers[MAX_DRAW_BUFFERS];
               GLint i, count = MIN2(n[1].i, MAX_DRAW_BUFFERS);
               for (i = 0; i < count; i++)
                  buffers[i] = n[2 + i].e;
               CALL_DrawBuffers(ctx->Exec, (n[1].i, buffers));
            }
            break;
         case OPCODE_BLIT_FRAMEBUFFER:
            CALL_BlitFramebuffer(ctx->Exec, (n[1].i, n[2].i, n[3].i, n[4].i,
                                                n[5].i, n[6].i, n[7].i, n[8].i,
                                                n[9].i, n[10].e));
            break;
         case OPCODE_PRIMITIVE_RESTART_NV:
            CALL_PrimitiveRestartNV(ctx->Exec, ());
            break;

         case OPCODE_USE_PROGRAM:
            CALL_UseProgram(ctx->Exec, (n[1].ui));
            break;
         case OPCODE_UNIFORM_1F:
            CALL_Uniform1f(ctx->Exec, (n[1].i, n[2].f));
            break;
         case OPCODE_UNIFORM_2F:
            CALL_Uniform2f(ctx->Exec, (n[1].i, n[2].f, n[3].f));
            break;
         case OPCODE_UNIFORM_3F:
            CALL_Uniform3f(ctx->Exec, (n[1].i, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_UNIFORM_4F:
            CALL_Uniform4f(ctx->Exec,
                              (n[1].i, n[2].f, n[3].f, n[4].f, n[5].f));
            break;
         case OPCODE_UNIFORM_1FV:
            CALL_Uniform1fv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_2FV:
            CALL_Uniform2fv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_3FV:
            CALL_Uniform3fv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_4FV:
            CALL_Uniform4fv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_1D: {
            union float64_pair x;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;

            CALL_Uniform1d(ctx->Exec, (n[1].i, x.d));
            break;
         }
         case OPCODE_UNIFORM_2D: {
            union float64_pair x;
            union float64_pair y;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;
            y.uint32[0] = n[4].ui;
            y.uint32[1] = n[5].ui;

            CALL_Uniform2d(ctx->Exec, (n[1].i, x.d, y.d));
            break;
         }
         case OPCODE_UNIFORM_3D: {
            union float64_pair x;
            union float64_pair y;
            union float64_pair z;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;
            y.uint32[0] = n[4].ui;
            y.uint32[1] = n[5].ui;
            z.uint32[0] = n[6].ui;
            z.uint32[1] = n[7].ui;

            CALL_Uniform3d(ctx->Exec, (n[1].i, x.d, y.d, z.d));
            break;
         }
         case OPCODE_UNIFORM_4D: {
            union float64_pair x;
            union float64_pair y;
            union float64_pair z;
            union float64_pair w;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;
            y.uint32[0] = n[4].ui;
            y.uint32[1] = n[5].ui;
            z.uint32[0] = n[6].ui;
            z.uint32[1] = n[7].ui;
            w.uint32[0] = n[8].ui;
            w.uint32[1] = n[9].ui;

            CALL_Uniform4d(ctx->Exec, (n[1].i, x.d, y.d, z.d, w.d));
            break;
         }
         case OPCODE_UNIFORM_1DV:
            CALL_Uniform1dv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_2DV:
            CALL_Uniform2dv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_3DV:
            CALL_Uniform3dv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_4DV:
            CALL_Uniform4dv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_1I:
            CALL_Uniform1i(ctx->Exec, (n[1].i, n[2].i));
            break;
         case OPCODE_UNIFORM_2I:
            CALL_Uniform2i(ctx->Exec, (n[1].i, n[2].i, n[3].i));
            break;
         case OPCODE_UNIFORM_3I:
            CALL_Uniform3i(ctx->Exec, (n[1].i, n[2].i, n[3].i, n[4].i));
            break;
         case OPCODE_UNIFORM_4I:
            CALL_Uniform4i(ctx->Exec,
                              (n[1].i, n[2].i, n[3].i, n[4].i, n[5].i));
            break;
         case OPCODE_UNIFORM_1IV:
            CALL_Uniform1iv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_2IV:
            CALL_Uniform2iv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_3IV:
            CALL_Uniform3iv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_4IV:
            CALL_Uniform4iv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_1UI:
            CALL_Uniform1ui(ctx->Exec, (n[1].i, n[2].i));
            break;
         case OPCODE_UNIFORM_2UI:
            CALL_Uniform2ui(ctx->Exec, (n[1].i, n[2].i, n[3].i));
            break;
         case OPCODE_UNIFORM_3UI:
            CALL_Uniform3ui(ctx->Exec, (n[1].i, n[2].i, n[3].i, n[4].i));
            break;
         case OPCODE_UNIFORM_4UI:
            CALL_Uniform4ui(ctx->Exec,
                            (n[1].i, n[2].i, n[3].i, n[4].i, n[5].i));
            break;
         case OPCODE_UNIFORM_1UIV:
            CALL_Uniform1uiv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_2UIV:
            CALL_Uniform2uiv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_3UIV:
            CALL_Uniform3uiv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_4UIV:
            CALL_Uniform4uiv(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_MATRIX22:
            CALL_UniformMatrix2fv(ctx->Exec,
                                  (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX33:
            CALL_UniformMatrix3fv(ctx->Exec,
                                  (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX44:
            CALL_UniformMatrix4fv(ctx->Exec,
                                  (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX23:
            CALL_UniformMatrix2x3fv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX32:
            CALL_UniformMatrix3x2fv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX24:
            CALL_UniformMatrix2x4fv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX42:
            CALL_UniformMatrix4x2fv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX34:
            CALL_UniformMatrix3x4fv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX43:
            CALL_UniformMatrix4x3fv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX22D:
            CALL_UniformMatrix2dv(ctx->Exec,
                                  (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX33D:
            CALL_UniformMatrix3dv(ctx->Exec,
                                  (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX44D:
            CALL_UniformMatrix4dv(ctx->Exec,
                                  (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX23D:
            CALL_UniformMatrix2x3dv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX32D:
            CALL_UniformMatrix3x2dv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX24D:
            CALL_UniformMatrix2x4dv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX42D:
            CALL_UniformMatrix4x2dv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX34D:
            CALL_UniformMatrix3x4dv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;
         case OPCODE_UNIFORM_MATRIX43D:
            CALL_UniformMatrix4x3dv(ctx->Exec,
                                    (n[1].i, n[2].i, n[3].b, get_pointer(&n[4])));
            break;

         case OPCODE_UNIFORM_1I64: {
            union int64_pair x;

            x.int32[0] = n[2].i;
            x.int32[1] = n[3].i;

            CALL_Uniform1i64ARB(ctx->Exec, (n[1].i, x.int64));
            break;
         }
         case OPCODE_UNIFORM_2I64: {
            union int64_pair x;
            union int64_pair y;

            x.int32[0] = n[2].i;
            x.int32[1] = n[3].i;
            y.int32[0] = n[4].i;
            y.int32[1] = n[5].i;

            CALL_Uniform2i64ARB(ctx->Exec, (n[1].i, x.int64, y.int64));
            break;
         }
         case OPCODE_UNIFORM_3I64: {
            union int64_pair x;
            union int64_pair y;
            union int64_pair z;

            x.int32[0] = n[2].i;
            x.int32[1] = n[3].i;
            y.int32[0] = n[4].i;
            y.int32[1] = n[5].i;
            z.int32[0] = n[6].i;
            z.int32[1] = n[7].i;


            CALL_Uniform3i64ARB(ctx->Exec, (n[1].i, x.int64, y.int64, z.int64));
            break;
         }
         case OPCODE_UNIFORM_4I64: {
            union int64_pair x;
            union int64_pair y;
            union int64_pair z;
            union int64_pair w;

            x.int32[0] = n[2].i;
            x.int32[1] = n[3].i;
            y.int32[0] = n[4].i;
            y.int32[1] = n[5].i;
            z.int32[0] = n[6].i;
            z.int32[1] = n[7].i;
            w.int32[0] = n[8].i;
            w.int32[1] = n[9].i;

            CALL_Uniform4i64ARB(ctx->Exec, (n[1].i, x.int64, y.int64, z.int64, w.int64));
            break;
         }
         case OPCODE_UNIFORM_1I64V:
            CALL_Uniform1i64vARB(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_2I64V:
            CALL_Uniform2i64vARB(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_3I64V:
            CALL_Uniform3i64vARB(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_4I64V:
            CALL_Uniform4i64vARB(ctx->Exec, (n[1].i, n[2].i, get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_1UI64: {
            union uint64_pair x;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;

            CALL_Uniform1ui64ARB(ctx->Exec, (n[1].i, x.uint64));
            break;
         }
         case OPCODE_UNIFORM_2UI64: {
            union uint64_pair x;
            union uint64_pair y;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;
            y.uint32[0] = n[4].ui;
            y.uint32[1] = n[5].ui;

            CALL_Uniform2ui64ARB(ctx->Exec, (n[1].i, x.uint64, y.uint64));
            break;
         }
         case OPCODE_UNIFORM_3UI64: {
            union uint64_pair x;
            union uint64_pair y;
            union uint64_pair z;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;
            y.uint32[0] = n[4].ui;
            y.uint32[1] = n[5].ui;
            z.uint32[0] = n[6].ui;
            z.uint32[1] = n[7].ui;


            CALL_Uniform3ui64ARB(ctx->Exec, (n[1].i, x.uint64, y.uint64,
                                 z.uint64));
            break;
         }
         case OPCODE_UNIFORM_4UI64: {
            union uint64_pair x;
            union uint64_pair y;
            union uint64_pair z;
            union uint64_pair w;

            x.uint32[0] = n[2].ui;
            x.uint32[1] = n[3].ui;
            y.uint32[0] = n[4].ui;
            y.uint32[1] = n[5].ui;
            z.uint32[0] = n[6].ui;
            z.uint32[1] = n[7].ui;
            w.uint32[0] = n[8].ui;
            w.uint32[1] = n[9].ui;

            CALL_Uniform4ui64ARB(ctx->Exec, (n[1].i, x.uint64, y.uint64,
                                 z.uint64, w.uint64));
            break;
         }
         case OPCODE_UNIFORM_1UI64V:
            CALL_Uniform1ui64vARB(ctx->Exec, (n[1].i, n[2].i,
                                  get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_2UI64V:
            CALL_Uniform2ui64vARB(ctx->Exec, (n[1].i, n[2].i,
                                  get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_3UI64V:
            CALL_Uniform3ui64vARB(ctx->Exec, (n[1].i, n[2].i,
                                  get_pointer(&n[3])));
            break;
         case OPCODE_UNIFORM_4UI64V:
            CALL_Uniform4ui64vARB(ctx->Exec, (n[1].i, n[2].i,
                                  get_pointer(&n[3])));
            break;

         case OPCODE_PROGRAM_UNIFORM_1I64: {
            union int64_pair x;

            x.int32[0] = n[3].i;
            x.int32[1] = n[4].i;

            CALL_ProgramUniform1i64ARB(ctx->Exec, (n[1].ui, n[2].i, x.int64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_2I64: {
            union int64_pair x;
            union int64_pair y;

            x.int32[0] = n[3].i;
            x.int32[1] = n[4].i;
            y.int32[0] = n[5].i;
            y.int32[1] = n[6].i;

            CALL_ProgramUniform2i64ARB(ctx->Exec, (n[1].ui, n[2].i, x.int64,
                                       y.int64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_3I64: {
            union int64_pair x;
            union int64_pair y;
            union int64_pair z;

            x.int32[0] = n[3].i;
            x.int32[1] = n[4].i;
            y.int32[0] = n[5].i;
            y.int32[1] = n[6].i;
            z.int32[0] = n[7].i;
            z.int32[1] = n[8].i;

            CALL_ProgramUniform3i64ARB(ctx->Exec, (n[1].ui, n[2].i, x.int64,
                                       y.int64, z.int64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_4I64: {
            union int64_pair x;
            union int64_pair y;
            union int64_pair z;
            union int64_pair w;

            x.int32[0] = n[3].i;
            x.int32[1] = n[4].i;
            y.int32[0] = n[5].i;
            y.int32[1] = n[6].i;
            z.int32[0] = n[7].i;
            z.int32[1] = n[8].i;
            w.int32[0] = n[9].i;
            w.int32[1] = n[10].i;

            CALL_ProgramUniform4i64ARB(ctx->Exec, (n[1].ui, n[2].i, x.int64,
                                       y.int64, z.int64, w.int64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_1I64V:
            CALL_ProgramUniform1i64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                        get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_2I64V:
            CALL_ProgramUniform2i64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                        get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_3I64V:
            CALL_ProgramUniform3i64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                        get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_4I64V:
            CALL_ProgramUniform4i64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                        get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_1UI64: {
            union uint64_pair x;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;

            CALL_ProgramUniform1i64ARB(ctx->Exec, (n[1].ui, n[2].i, x.uint64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_2UI64: {
            union uint64_pair x;
            union uint64_pair y;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;
            y.uint32[0] = n[5].ui;
            y.uint32[1] = n[6].ui;

            CALL_ProgramUniform2ui64ARB(ctx->Exec, (n[1].ui, n[2].i, x.uint64,
                                        y.uint64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_3UI64: {
            union uint64_pair x;
            union uint64_pair y;
            union uint64_pair z;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;
            y.uint32[0] = n[5].ui;
            y.uint32[1] = n[6].ui;
            z.uint32[0] = n[7].ui;
            z.uint32[1] = n[8].ui;

            CALL_ProgramUniform3ui64ARB(ctx->Exec, (n[1].ui, n[2].i, x.uint64,
                                        y.uint64, z.uint64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_4UI64: {
            union uint64_pair x;
            union uint64_pair y;
            union uint64_pair z;
            union uint64_pair w;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;
            y.uint32[0] = n[5].ui;
            y.uint32[1] = n[6].ui;
            z.uint32[0] = n[7].ui;
            z.uint32[1] = n[8].ui;
            w.uint32[0] = n[9].ui;
            w.uint32[1] = n[10].ui;

            CALL_ProgramUniform4ui64ARB(ctx->Exec, (n[1].ui, n[2].i, x.uint64,
                                        y.uint64, z.uint64, w.uint64));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_1UI64V:
            CALL_ProgramUniform1ui64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                         get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_2UI64V:
            CALL_ProgramUniform2ui64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                         get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_3UI64V:
            CALL_ProgramUniform3ui64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                         get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_4UI64V:
            CALL_ProgramUniform4ui64vARB(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                         get_pointer(&n[4])));
            break;

         case OPCODE_USE_PROGRAM_STAGES:
            CALL_UseProgramStages(ctx->Exec, (n[1].ui, n[2].ui, n[3].ui));
            break;
         case OPCODE_PROGRAM_UNIFORM_1F:
            CALL_ProgramUniform1f(ctx->Exec, (n[1].ui, n[2].i, n[3].f));
            break;
         case OPCODE_PROGRAM_UNIFORM_2F:
            CALL_ProgramUniform2f(ctx->Exec, (n[1].ui, n[2].i, n[3].f, n[4].f));
            break;
         case OPCODE_PROGRAM_UNIFORM_3F:
            CALL_ProgramUniform3f(ctx->Exec, (n[1].ui, n[2].i,
                                              n[3].f, n[4].f, n[5].f));
            break;
         case OPCODE_PROGRAM_UNIFORM_4F:
            CALL_ProgramUniform4f(ctx->Exec, (n[1].ui, n[2].i,
                                              n[3].f, n[4].f, n[5].f, n[6].f));
            break;
         case OPCODE_PROGRAM_UNIFORM_1FV:
            CALL_ProgramUniform1fv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_2FV:
            CALL_ProgramUniform2fv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_3FV:
            CALL_ProgramUniform3fv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_4FV:
            CALL_ProgramUniform4fv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_1D: {
            union float64_pair x;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;

            CALL_ProgramUniform1d(ctx->Exec, (n[1].ui, n[2].i, x.d));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_2D: {
            union float64_pair x;
            union float64_pair y;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;
            y.uint32[0] = n[5].ui;
            y.uint32[1] = n[6].ui;

            CALL_ProgramUniform2d(ctx->Exec, (n[1].ui, n[2].i, x.d, y.d));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_3D: {
            union float64_pair x;
            union float64_pair y;
            union float64_pair z;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;
            y.uint32[0] = n[5].ui;
            y.uint32[1] = n[6].ui;
            z.uint32[0] = n[7].ui;
            z.uint32[1] = n[8].ui;

            CALL_ProgramUniform3d(ctx->Exec, (n[1].ui, n[2].i,
                                              x.d, y.d, z.d));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_4D: {
            union float64_pair x;
            union float64_pair y;
            union float64_pair z;
            union float64_pair w;

            x.uint32[0] = n[3].ui;
            x.uint32[1] = n[4].ui;
            y.uint32[0] = n[5].ui;
            y.uint32[1] = n[6].ui;
            z.uint32[0] = n[7].ui;
            z.uint32[1] = n[8].ui;
            w.uint32[0] = n[9].ui;
            w.uint32[1] = n[10].ui;

            CALL_ProgramUniform4d(ctx->Exec, (n[1].ui, n[2].i,
                                              x.d, y.d, z.d, w.d));
            break;
         }
         case OPCODE_PROGRAM_UNIFORM_1DV:
            CALL_ProgramUniform1dv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_2DV:
            CALL_ProgramUniform2dv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_3DV:
            CALL_ProgramUniform3dv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_4DV:
            CALL_ProgramUniform4dv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_1I:
            CALL_ProgramUniform1i(ctx->Exec, (n[1].ui, n[2].i, n[3].i));
            break;
         case OPCODE_PROGRAM_UNIFORM_2I:
            CALL_ProgramUniform2i(ctx->Exec, (n[1].ui, n[2].i, n[3].i, n[4].i));
            break;
         case OPCODE_PROGRAM_UNIFORM_3I:
            CALL_ProgramUniform3i(ctx->Exec, (n[1].ui, n[2].i,
                                              n[3].i, n[4].i, n[5].i));
            break;
         case OPCODE_PROGRAM_UNIFORM_4I:
            CALL_ProgramUniform4i(ctx->Exec, (n[1].ui, n[2].i,
                                              n[3].i, n[4].i, n[5].i, n[6].i));
            break;
         case OPCODE_PROGRAM_UNIFORM_1IV:
            CALL_ProgramUniform1iv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_2IV:
            CALL_ProgramUniform2iv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_3IV:
            CALL_ProgramUniform3iv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_4IV:
            CALL_ProgramUniform4iv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                               get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_1UI:
            CALL_ProgramUniform1ui(ctx->Exec, (n[1].ui, n[2].i, n[3].ui));
            break;
         case OPCODE_PROGRAM_UNIFORM_2UI:
            CALL_ProgramUniform2ui(ctx->Exec, (n[1].ui, n[2].i,
                                               n[3].ui, n[4].ui));
            break;
         case OPCODE_PROGRAM_UNIFORM_3UI:
            CALL_ProgramUniform3ui(ctx->Exec, (n[1].ui, n[2].i,
                                               n[3].ui, n[4].ui, n[5].ui));
            break;
         case OPCODE_PROGRAM_UNIFORM_4UI:
            CALL_ProgramUniform4ui(ctx->Exec, (n[1].ui, n[2].i,
                                               n[3].ui,
                                               n[4].ui, n[5].ui, n[6].ui));
            break;
         case OPCODE_PROGRAM_UNIFORM_1UIV:
            CALL_ProgramUniform1uiv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                                get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_2UIV:
            CALL_ProgramUniform2uiv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                                get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_3UIV:
            CALL_ProgramUniform3uiv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                                get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_4UIV:
            CALL_ProgramUniform4uiv(ctx->Exec, (n[1].ui, n[2].i, n[3].i,
                                                get_pointer(&n[4])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX22F:
            CALL_ProgramUniformMatrix2fv(ctx->Exec,
                                         (n[1].ui, n[2].i, n[3].i, n[4].b,
                                          get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX23F:
            CALL_ProgramUniformMatrix2x3fv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX24F:
            CALL_ProgramUniformMatrix2x4fv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX32F:
            CALL_ProgramUniformMatrix3x2fv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX33F:
            CALL_ProgramUniformMatrix3fv(ctx->Exec,
                                         (n[1].ui, n[2].i, n[3].i, n[4].b,
                                          get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX34F:
            CALL_ProgramUniformMatrix3x4fv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX42F:
            CALL_ProgramUniformMatrix4x2fv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX43F:
            CALL_ProgramUniformMatrix4x3fv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX44F:
            CALL_ProgramUniformMatrix4fv(ctx->Exec,
                                         (n[1].ui, n[2].i, n[3].i, n[4].b,
                                          get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX22D:
            CALL_ProgramUniformMatrix2dv(ctx->Exec,
                                         (n[1].ui, n[2].i, n[3].i, n[4].b,
                                          get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX23D:
            CALL_ProgramUniformMatrix2x3dv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX24D:
            CALL_ProgramUniformMatrix2x4dv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX32D:
            CALL_ProgramUniformMatrix3x2dv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX33D:
            CALL_ProgramUniformMatrix3dv(ctx->Exec,
                                         (n[1].ui, n[2].i, n[3].i, n[4].b,
                                          get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX34D:
            CALL_ProgramUniformMatrix3x4dv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX42D:
            CALL_ProgramUniformMatrix4x2dv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX43D:
            CALL_ProgramUniformMatrix4x3dv(ctx->Exec,
                                           (n[1].ui, n[2].i, n[3].i, n[4].b,
                                            get_pointer(&n[5])));
            break;
         case OPCODE_PROGRAM_UNIFORM_MATRIX44D:
            CALL_ProgramUniformMatrix4dv(ctx->Exec,
                                         (n[1].ui, n[2].i, n[3].i, n[4].b,
                                          get_pointer(&n[5])));
            break;

         case OPCODE_CLIP_CONTROL:
            CALL_ClipControl(ctx->Exec, (n[1].e, n[2].e));
            break;

         case OPCODE_CLAMP_COLOR:
            CALL_ClampColor(ctx->Exec, (n[1].e, n[2].e));
            break;

         case OPCODE_BIND_FRAGMENT_SHADER_ATI:
            CALL_BindFragmentShaderATI(ctx->Exec, (n[1].i));
            break;
         case OPCODE_SET_FRAGMENT_SHADER_CONSTANTS_ATI:
            CALL_SetFragmentShaderConstantATI(ctx->Exec, (n[1].ui, &n[2].f));
            break;
         case OPCODE_ATTR_1F_NV:
            CALL_VertexAttrib1fNV(ctx->Exec, (n[1].e, n[2].f));
            break;
         case OPCODE_ATTR_2F_NV:
            CALL_VertexAttrib2fvNV(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_ATTR_3F_NV:
            CALL_VertexAttrib3fvNV(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_ATTR_4F_NV:
            CALL_VertexAttrib4fvNV(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_ATTR_1F_ARB:
            CALL_VertexAttrib1fARB(ctx->Exec, (n[1].e, n[2].f));
            break;
         case OPCODE_ATTR_2F_ARB:
            CALL_VertexAttrib2fvARB(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_ATTR_3F_ARB:
            CALL_VertexAttrib3fvARB(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_ATTR_4F_ARB:
            CALL_VertexAttrib4fvARB(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_ATTR_1I:
            CALL_VertexAttribI1iEXT(ctx->Exec, (n[1].e, n[2].i));
            break;
         case OPCODE_ATTR_2I:
            CALL_VertexAttribI2ivEXT(ctx->Exec, (n[1].e, &n[2].i));
            break;
         case OPCODE_ATTR_3I:
            CALL_VertexAttribI3ivEXT(ctx->Exec, (n[1].e, &n[2].i));
            break;
         case OPCODE_ATTR_4I:
            CALL_VertexAttribI4ivEXT(ctx->Exec, (n[1].e, &n[2].i));
            break;
         case OPCODE_ATTR_1D: {
            GLdouble *d = (GLdouble *) &n[2];
            CALL_VertexAttribL1d(ctx->Exec, (n[1].ui, *d));
            break;
         }
         case OPCODE_ATTR_2D: {
            GLdouble *d = (GLdouble *) &n[2];
            CALL_VertexAttribL2dv(ctx->Exec, (n[1].ui, d));
            break;
         }
         case OPCODE_ATTR_3D: {
            GLdouble *d = (GLdouble *) &n[2];
            CALL_VertexAttribL3dv(ctx->Exec, (n[1].ui, d));
            break;
         }
         case OPCODE_ATTR_4D: {
            GLdouble *d = (GLdouble *) &n[2];
            CALL_VertexAttribL4dv(ctx->Exec, (n[1].ui, d));
            break;
         }
         case OPCODE_ATTR_1UI64: {
            uint64_t *ui64 = (uint64_t *) &n[2];
            CALL_VertexAttribL1ui64ARB(ctx->Exec, (n[1].ui, *ui64));
            break;
         }
         case OPCODE_MATERIAL:
            CALL_Materialfv(ctx->Exec, (n[1].e, n[2].e, &n[3].f));
            break;
         case OPCODE_BEGIN:
            CALL_Begin(ctx->Exec, (n[1].e));
            break;
         case OPCODE_END:
            CALL_End(ctx->Exec, ());
            break;
         case OPCODE_EVAL_C1:
            CALL_EvalCoord1f(ctx->Exec, (n[1].f));
            break;
         case OPCODE_EVAL_C2:
            CALL_EvalCoord2f(ctx->Exec, (n[1].f, n[2].f));
            break;
         case OPCODE_EVAL_P1:
            CALL_EvalPoint1(ctx->Exec, (n[1].i));
            break;
         case OPCODE_EVAL_P2:
            CALL_EvalPoint2(ctx->Exec, (n[1].i, n[2].i));
            break;

         /* GL_EXT_texture_integer */
         case OPCODE_CLEARCOLOR_I:
            CALL_ClearColorIiEXT(ctx->Exec, (n[1].i, n[2].i, n[3].i, n[4].i));
            break;
         case OPCODE_CLEARCOLOR_UI:
            CALL_ClearColorIuiEXT(ctx->Exec,
                                  (n[1].ui, n[2].ui, n[3].ui, n[4].ui));
            break;
         case OPCODE_TEXPARAMETER_I:
            {
               GLint params[4];
               params[0] = n[3].i;
               params[1] = n[4].i;
               params[2] = n[5].i;
               params[3] = n[6].i;
               CALL_TexParameterIiv(ctx->Exec, (n[1].e, n[2].e, params));
            }
            break;
         case OPCODE_TEXPARAMETER_UI:
            {
               GLuint params[4];
               params[0] = n[3].ui;
               params[1] = n[4].ui;
               params[2] = n[5].ui;
               params[3] = n[6].ui;
               CALL_TexParameterIuiv(ctx->Exec, (n[1].e, n[2].e, params));
            }
            break;

         case OPCODE_VERTEX_ATTRIB_DIVISOR:
            /* GL_ARB_instanced_arrays */
            CALL_VertexAttribDivisor(ctx->Exec, (n[1].ui, n[2].ui));
            break;

         case OPCODE_TEXTURE_BARRIER_NV:
            CALL_TextureBarrierNV(ctx->Exec, ());
            break;

         /* GL_EXT/ARB_transform_feedback */
         case OPCODE_BEGIN_TRANSFORM_FEEDBACK:
            CALL_BeginTransformFeedback(ctx->Exec, (n[1].e));
            break;
         case OPCODE_END_TRANSFORM_FEEDBACK:
            CALL_EndTransformFeedback(ctx->Exec, ());
            break;
         case OPCODE_BIND_TRANSFORM_FEEDBACK:
            CALL_BindTransformFeedback(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_PAUSE_TRANSFORM_FEEDBACK:
            CALL_PauseTransformFeedback(ctx->Exec, ());
            break;
         case OPCODE_RESUME_TRANSFORM_FEEDBACK:
            CALL_ResumeTransformFeedback(ctx->Exec, ());
            break;
         case OPCODE_DRAW_TRANSFORM_FEEDBACK:
            CALL_DrawTransformFeedback(ctx->Exec, (n[1].e, n[2].ui));
            break;
         case OPCODE_DRAW_TRANSFORM_FEEDBACK_STREAM:
            CALL_DrawTransformFeedbackStream(ctx->Exec,
                                             (n[1].e, n[2].ui, n[3].ui));
            break;
         case OPCODE_DRAW_TRANSFORM_FEEDBACK_INSTANCED:
            CALL_DrawTransformFeedbackInstanced(ctx->Exec,
                                                (n[1].e, n[2].ui, n[3].si));
            break;
         case OPCODE_DRAW_TRANSFORM_FEEDBACK_STREAM_INSTANCED:
            CALL_DrawTransformFeedbackStreamInstanced(ctx->Exec,
                                       (n[1].e, n[2].ui, n[3].ui, n[4].si));
            break;


         case OPCODE_BIND_SAMPLER:
            CALL_BindSampler(ctx->Exec, (n[1].ui, n[2].ui));
            break;
         case OPCODE_SAMPLER_PARAMETERIV:
            {
               GLint params[4];
               params[0] = n[3].i;
               params[1] = n[4].i;
               params[2] = n[5].i;
               params[3] = n[6].i;
               CALL_SamplerParameteriv(ctx->Exec, (n[1].ui, n[2].e, params));
            }
            break;
         case OPCODE_SAMPLER_PARAMETERFV:
            {
               GLfloat params[4];
               params[0] = n[3].f;
               params[1] = n[4].f;
               params[2] = n[5].f;
               params[3] = n[6].f;
               CALL_SamplerParameterfv(ctx->Exec, (n[1].ui, n[2].e, params));
            }
            break;
         case OPCODE_SAMPLER_PARAMETERIIV:
            {
               GLint params[4];
               params[0] = n[3].i;
               params[1] = n[4].i;
               params[2] = n[5].i;
               params[3] = n[6].i;
               CALL_SamplerParameterIiv(ctx->Exec, (n[1].ui, n[2].e, params));
            }
            break;
         case OPCODE_SAMPLER_PARAMETERUIV:
            {
               GLuint params[4];
               params[0] = n[3].ui;
               params[1] = n[4].ui;
               params[2] = n[5].ui;
               params[3] = n[6].ui;
               CALL_SamplerParameterIuiv(ctx->Exec, (n[1].ui, n[2].e, params));
            }
            break;

         /* ARB_compute_shader */
         case OPCODE_DISPATCH_COMPUTE:
            CALL_DispatchCompute(ctx->Exec, (n[1].ui, n[2].ui, n[3].ui));
            break;

         /* GL_ARB_sync */
         case OPCODE_WAIT_SYNC:
            {
               union uint64_pair p;
               p.uint32[0] = n[2].ui;
               p.uint32[1] = n[3].ui;
               CALL_WaitSync(ctx->Exec,
                             (get_pointer(&n[4]), n[1].bf, p.uint64));
            }
            break;

         /* GL_NV_conditional_render */
         case OPCODE_BEGIN_CONDITIONAL_RENDER:
            CALL_BeginConditionalRender(ctx->Exec, (n[1].i, n[2].e));
            break;
         case OPCODE_END_CONDITIONAL_RENDER:
            CALL_EndConditionalRender(ctx->Exec, ());
            break;

         case OPCODE_UNIFORM_BLOCK_BINDING:
            CALL_UniformBlockBinding(ctx->Exec, (n[1].ui, n[2].ui, n[3].ui));
            break;

         case OPCODE_UNIFORM_SUBROUTINES:
            CALL_UniformSubroutinesuiv(ctx->Exec, (n[1].e, n[2].si,
                                                   get_pointer(&n[3])));
            break;

         /* GL_EXT_window_rectangles */
         case OPCODE_WINDOW_RECTANGLES:
            CALL_WindowRectanglesEXT(
                  ctx->Exec, (n[1].e, n[2].si, get_pointer(&n[3])));
            break;

         /* GL_NV_conservative_raster */
         case OPCODE_SUBPIXEL_PRECISION_BIAS:
            CALL_SubpixelPrecisionBiasNV(ctx->Exec, (n[1].ui, n[2].ui));
            break;

         /* GL_NV_conservative_raster_dilate */
         case OPCODE_CONSERVATIVE_RASTER_PARAMETER_F:
            CALL_ConservativeRasterParameterfNV(ctx->Exec, (n[1].e, n[2].f));
            break;

         /* GL_NV_conservative_raster_pre_snap_triangles */
         case OPCODE_CONSERVATIVE_RASTER_PARAMETER_I:
            CALL_ConservativeRasterParameteriNV(ctx->Exec, (n[1].e, n[2].i));
            break;

         /* GL_EXT_direct_state_access */
         case OPCODE_MATRIX_LOAD:
            CALL_MatrixLoadfEXT(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_MATRIX_MULT:
            CALL_MatrixMultfEXT(ctx->Exec, (n[1].e, &n[2].f));
            break;
         case OPCODE_MATRIX_ROTATE:
            CALL_MatrixRotatefEXT(ctx->Exec, (n[1].e, n[2].f, n[3].f, n[4].f, n[5].f));
            break;
         case OPCODE_MATRIX_SCALE:
            CALL_MatrixScalefEXT(ctx->Exec, (n[1].e, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_MATRIX_TRANSLATE:
            CALL_MatrixTranslatefEXT(ctx->Exec, (n[1].e, n[2].f, n[3].f, n[4].f));
            break;
         case OPCODE_MATRIX_LOAD_IDENTITY:
            CALL_MatrixLoadIdentityEXT(ctx->Exec, (n[1].e));
            break;
         case OPCODE_MATRIX_ORTHO:
            CALL_MatrixOrthoEXT(ctx->Exec, (n[1].e,
                                            n[2].f, n[3].f, n[4].f,
                                            n[5].f, n[6].f, n[7].f));
            break;
         case OPCODE_MATRIX_FRUSTUM:
            CALL_MatrixFrustumEXT(ctx->Exec, (n[1].e,
                                              n[2].f, n[3].f, n[4].f,
                                              n[5].f, n[6].f, n[7].f));
            break;
         case OPCODE_MATRIX_PUSH:
            CALL_MatrixPushEXT(ctx->Exec, (n[1].e));
            break;
         case OPCODE_MATRIX_POP:
            CALL_MatrixPopEXT(ctx->Exec, (n[1].e));
            break;
         case OPCODE_TEXTUREPARAMETER_F:
            {
               GLfloat params[4];
               params[0] = n[4].f;
               params[1] = n[5].f;
               params[2] = n[6].f;
               params[3] = n[7].f;
               CALL_TextureParameterfvEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_TEXTUREPARAMETER_I:
            {
               GLint params[4];
               params[0] = n[4].i;
               params[1] = n[5].i;
               params[2] = n[6].i;
               params[3] = n[7].i;
               CALL_TextureParameterivEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_TEXTUREPARAMETER_II:
            {
               GLint params[4];
               params[0] = n[4].i;
               params[1] = n[5].i;
               params[2] = n[6].i;
               params[3] = n[7].i;
               CALL_TextureParameterIivEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_TEXTUREPARAMETER_IUI:
            {
               GLuint params[4];
               params[0] = n[4].ui;
               params[1] = n[5].ui;
               params[2] = n[6].ui;
               params[3] = n[7].ui;
               CALL_TextureParameterIuivEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_TEXTURE_IMAGE1D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TextureImage1DEXT(ctx->Exec, (n[1].ui, /* texture */
                                                  n[2].e,  /* target */
                                                  n[3].i,  /* level */
                                                  n[4].i,  /* components */
                                                  n[5].i,  /* width */
                                                  n[6].e,  /* border */
                                                  n[7].e,  /* format */
                                                  n[8].e,  /* type */
                                                  get_pointer(&n[9])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEXTURE_IMAGE2D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TextureImage2DEXT(ctx->Exec, (n[1].ui, /* texture */
                                                  n[2].e,  /* target */
                                                  n[3].i,  /* level */
                                                  n[4].i,  /* components */
                                                  n[5].i,  /* width */
                                                  n[6].i,  /* height */
                                                  n[7].e,  /* border */
                                                  n[8].e,  /* format */
                                                  n[9].e,  /* type */
                                                  get_pointer(&n[10])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEXTURE_IMAGE3D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TextureImage3DEXT(ctx->Exec, (n[1].ui, /* texture */
                                                  n[2].e,  /* target */
                                                  n[3].i,  /* level */
                                                  n[4].i,  /* components */
                                                  n[5].i,  /* width */
                                                  n[6].i,  /* height */
                                                  n[7].i,  /* depth  */
                                                  n[8].e,  /* border */
                                                  n[9].e,  /* format */
                                                  n[10].e, /* type */
                                                  get_pointer(&n[11])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEXTURE_SUB_IMAGE1D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TextureSubImage1DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                     n[4].i, n[5].i, n[6].e,
                                                     n[7].e, get_pointer(&n[8])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_TEXTURE_SUB_IMAGE2D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TextureSubImage2DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                     n[4].i, n[5].i, n[6].e,
                                                     n[7].i, n[8].e, n[9].e,
                                                     get_pointer(&n[10])));
               ctx->Unpack = save;
            }
            break;
         case OPCODE_TEXTURE_SUB_IMAGE3D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_TextureSubImage3DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                     n[4].i, n[5].i, n[6].i,
                                                     n[7].i, n[8].i, n[9].i,
                                                     n[10].e, n[11].e,
                                                     get_pointer(&n[12])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_COPY_TEXTURE_IMAGE1D:
            CALL_CopyTextureImage1DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                   n[4].e, n[5].i, n[6].i,
                                                   n[7].i, n[8].i));
            break;
         case OPCODE_COPY_TEXTURE_IMAGE2D:
            CALL_CopyTextureImage2DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                   n[4].e, n[5].i, n[6].i,
                                                   n[7].i, n[8].i, n[9].i));
            break;
         case OPCODE_COPY_TEXTURE_SUB_IMAGE1D:
            CALL_CopyTextureSubImage1DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                      n[4].i, n[5].i, n[6].i,
                                                      n[7].i));
            break;
         case OPCODE_COPY_TEXTURE_SUB_IMAGE2D:
            CALL_CopyTextureSubImage2DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                      n[4].i, n[5].i, n[6].i,
                                                      n[7].i, n[8].i, n[9].i));
            break;
         case OPCODE_COPY_TEXTURE_SUB_IMAGE3D:
            CALL_CopyTextureSubImage3DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                      n[4].i, n[5].i, n[6].i,
                                                      n[7].i, n[8].i, n[9].i,
                                                      n[10].i));
            break;
         case OPCODE_BIND_MULTITEXTURE:
            CALL_BindMultiTextureEXT(ctx->Exec, (n[1].e, n[2].e, n[3].ui));
            break;
         case OPCODE_MULTITEXPARAMETER_F:
            {
               GLfloat params[4];
               params[0] = n[4].f;
               params[1] = n[5].f;
               params[2] = n[6].f;
               params[3] = n[7].f;
               CALL_MultiTexParameterfvEXT(ctx->Exec, (n[1].e, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_MULTITEXPARAMETER_I:
            {
               GLint params[4];
               params[0] = n[4].i;
               params[1] = n[5].i;
               params[2] = n[6].i;
               params[3] = n[7].i;
               CALL_MultiTexParameterivEXT(ctx->Exec, (n[1].e, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_MULTITEXPARAMETER_II:
            {
               GLint params[4];
               params[0] = n[4].i;
               params[1] = n[5].i;
               params[2] = n[6].i;
               params[3] = n[7].i;
               CALL_MultiTexParameterIivEXT(ctx->Exec, (n[1].e, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_MULTITEXPARAMETER_IUI:
            {
               GLuint params[4];
               params[0] = n[4].ui;
               params[1] = n[5].ui;
               params[2] = n[6].ui;
               params[3] = n[7].ui;
               CALL_MultiTexParameterIuivEXT(ctx->Exec, (n[1].e, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_MULTITEX_IMAGE1D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_MultiTexImage1DEXT(ctx->Exec, (n[1].e, /* texture */
                                                  n[2].e,  /* target */
                                                  n[3].i,  /* level */
                                                  n[4].i,  /* components */
                                                  n[5].i,  /* width */
                                                  n[6].e,  /* border */
                                                  n[7].e,  /* format */
                                                  n[8].e,  /* type */
                                                  get_pointer(&n[9])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_MULTITEX_IMAGE2D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_MultiTexImage2DEXT(ctx->Exec, (n[1].e, /* texture */
                                                  n[2].e,  /* target */
                                                  n[3].i,  /* level */
                                                  n[4].i,  /* components */
                                                  n[5].i,  /* width */
                                                  n[6].i,  /* height */
                                                  n[7].e,  /* border */
                                                  n[8].e,  /* format */
                                                  n[9].e,  /* type */
                                                  get_pointer(&n[10])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_MULTITEX_IMAGE3D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_MultiTexImage3DEXT(ctx->Exec, (n[1].e, /* texture */
                                                  n[2].e,  /* target */
                                                  n[3].i,  /* level */
                                                  n[4].i,  /* components */
                                                  n[5].i,  /* width */
                                                  n[6].i,  /* height */
                                                  n[7].i,  /* depth  */
                                                  n[8].e,  /* border */
                                                  n[9].e,  /* format */
                                                  n[10].e, /* type */
                                                  get_pointer(&n[11])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_MULTITEX_SUB_IMAGE1D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_MultiTexSubImage1DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                     n[4].i, n[5].i, n[6].e,
                                                     n[7].e, get_pointer(&n[8])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_MULTITEX_SUB_IMAGE2D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_MultiTexSubImage2DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                     n[4].i, n[5].i, n[6].e,
                                                     n[7].i, n[8].e, n[9].e,
                                                     get_pointer(&n[10])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_MULTITEX_SUB_IMAGE3D:
            {
               const struct gl_pixelstore_attrib save = ctx->Unpack;
               ctx->Unpack = ctx->DefaultPacking;
               CALL_MultiTexSubImage3DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                     n[4].i, n[5].i, n[6].i,
                                                     n[7].i, n[8].i, n[9].i,
                                                     n[10].e, n[11].e,
                                                     get_pointer(&n[12])));
               ctx->Unpack = save;      /* restore */
            }
            break;
         case OPCODE_COPY_MULTITEX_IMAGE1D:
            CALL_CopyMultiTexImage1DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                   n[4].e, n[5].i, n[6].i,
                                                   n[7].i, n[8].i));
            break;
         case OPCODE_COPY_MULTITEX_IMAGE2D:
            CALL_CopyMultiTexImage2DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                   n[4].e, n[5].i, n[6].i,
                                                   n[7].i, n[8].i, n[9].i));
            break;
         case OPCODE_COPY_MULTITEX_SUB_IMAGE1D:
            CALL_CopyMultiTexSubImage1DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                      n[4].i, n[5].i, n[6].i,
                                                      n[7].i));
            break;
         case OPCODE_COPY_MULTITEX_SUB_IMAGE2D:
            CALL_CopyMultiTexSubImage2DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                      n[4].i, n[5].i, n[6].i,
                                                      n[7].i, n[8].i, n[9].i));
            break;
         case OPCODE_COPY_MULTITEX_SUB_IMAGE3D:
            CALL_CopyMultiTexSubImage3DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                      n[4].i, n[5].i, n[6].i,
                                                      n[7].i, n[8].i, n[9].i,
                                                      n[10].i));
            break;
         case OPCODE_MULTITEXENV:
            {
               GLfloat params[4];
               params[0] = n[4].f;
               params[1] = n[5].f;
               params[2] = n[6].f;
               params[3] = n[7].f;
               CALL_MultiTexEnvfvEXT(ctx->Exec, (n[1].e, n[2].e, n[3].e, params));
            }
            break;
         case OPCODE_COMPRESSED_TEXTURE_IMAGE_1D:
            CALL_CompressedTextureImage1DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                         n[4].e, n[5].i, n[6].i,
                                                         n[7].i, get_pointer(&n[8])));
            break;
         case OPCODE_COMPRESSED_TEXTURE_IMAGE_2D:
            CALL_CompressedTextureImage2DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                         n[4].e, n[5].i, n[6].i,
                                                         n[7].i, n[8].i,
                                                         get_pointer(&n[9])));
            break;
         case OPCODE_COMPRESSED_TEXTURE_IMAGE_3D:
            CALL_CompressedTextureImage3DEXT(ctx->Exec, (n[1].ui, n[2].e, n[3].i,
                                                         n[4].e, n[5].i, n[6].i,
                                                         n[7].i, n[8].i, n[9].i,
                                                         get_pointer(&n[10])));
            break;
         case OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_1D:
            CALL_CompressedTextureSubImage1DEXT(ctx->Exec,
                                                (n[1].ui, n[2].e, n[3].i, n[4].i,
                                                 n[5].i, n[6].e, n[7].i,
                                                 get_pointer(&n[8])));
            break;
         case OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_2D:
            CALL_CompressedTextureSubImage2DEXT(ctx->Exec,
                                                (n[1].ui, n[2].e, n[3].i, n[4].i,
                                                 n[5].i, n[6].i, n[7].i, n[8].e,
                                                 n[9].i, get_pointer(&n[10])));
            break;
         case OPCODE_COMPRESSED_TEXTURE_SUB_IMAGE_3D:
            CALL_CompressedTextureSubImage3DEXT(ctx->Exec,
                                                (n[1].ui, n[2].e, n[3].i, n[4].i,
                                                 n[5].i, n[6].i, n[7].i, n[8].i,
                                                 n[9].i, n[10].e, n[11].i,
                                                 get_pointer(&n[12])));
            break;
         case OPCODE_COMPRESSED_MULTITEX_IMAGE_1D:
            CALL_CompressedMultiTexImage1DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                         n[4].e, n[5].i, n[6].i,
                                                         n[7].i, get_pointer(&n[8])));
            break;
         case OPCODE_COMPRESSED_MULTITEX_IMAGE_2D:
            CALL_CompressedMultiTexImage2DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                         n[4].e, n[5].i, n[6].i,
                                                         n[7].i, n[8].i,
                                                         get_pointer(&n[9])));
            break;
         case OPCODE_COMPRESSED_MULTITEX_IMAGE_3D:
            CALL_CompressedMultiTexImage3DEXT(ctx->Exec, (n[1].e, n[2].e, n[3].i,
                                                         n[4].e, n[5].i, n[6].i,
                                                         n[7].i, n[8].i, n[9].i,
                                                         get_pointer(&n[10])));
            break;
         case OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_1D:
            CALL_CompressedMultiTexSubImage1DEXT(ctx->Exec,
                                                (n[1].e, n[2].e, n[3].i, n[4].i,
                                                 n[5].i, n[6].e, n[7].i,
                                                 get_pointer(&n[8])));
            break;
         case OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_2D:
            CALL_CompressedMultiTexSubImage2DEXT(ctx->Exec,
                                                (n[1].e, n[2].e, n[3].i, n[4].i,
                                                 n[5].i, n[6].i, n[7].i, n[8].e,
                                                 n[9].i, get_pointer(&n[10])));
            break;
         case OPCODE_COMPRESSED_MULTITEX_SUB_IMAGE_3D:
            CALL_CompressedMultiTexSubImage3DEXT(ctx->Exec,
                                                (n[1].e, n[2].e, n[3].i, n[4].i,
                                                 n[5].i, n[6].i, n[7].i, n[8].i,
                                                 n[9].i, n[10].e, n[11].i,
                                                 get_pointer(&n[12])));
            break;
         case OPCODE_NAMED_PROGRAM_STRING:
            CALL_NamedProgramStringEXT(ctx->Exec,
                                  (n[1].ui, n[2].e, n[3].e, n[4].i,
                                   get_pointer(&n[5])));
            break;
         case OPCODE_NAMED_PROGRAM_LOCAL_PARAMETER:
            CALL_NamedProgramLocalParameter4fEXT(ctx->Exec,
                                            (n[1].ui, n[2].e, n[3].ui, n[4].f,
                                             n[5].f, n[6].f, n[7].f));
            break;

         case OPCODE_VERTEX_LIST:
            vbo_save_playback_vertex_list(ctx, &n[1]);
            break;

         case OPCODE_CONTINUE:
            n = (Node *) get_pointer(&n[1]);
            continue;
         case OPCODE_NOP:
            /* no-op */
            break;
         default:
            {
               char msg[1000];
               snprintf(msg, sizeof(msg), "Error in execute_list: opcode=%d",
                             (int) opcode);
               _mesa_problem(ctx, "%s", msg);
            }
            FALLTHROUGH;
         case OPCODE_END_OF_LIST:
            vbo_save_EndCallList(ctx);
            ctx->ListState.CallDepth--;
            return;
      }

      /* increment n to point to next compiled command */
      assert(InstSize[opcode] > 0);
      n += InstSize[opcode];
   }
}



/**********************************************************************/
/*                           GL functions                             */
/**********************************************************************/

/**
 * Test if a display list number is valid.
 */
GLboolean GLAPIENTRY
_mesa_IsList(GLuint list)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_VERTICES(ctx, 0, 0);      /* must be called before assert */
   ASSERT_OUTSIDE_BEGIN_END_WITH_RETVAL(ctx, GL_FALSE);
   return _mesa_get_list(ctx, list, NULL);
}


/**
 * Delete a sequence of consecutive display lists.
 */
void GLAPIENTRY
_mesa_DeleteLists(GLuint list, GLsizei range)
{
   GET_CURRENT_CONTEXT(ctx);
   GLuint i;
   FLUSH_VERTICES(ctx, 0, 0);      /* must be called before assert */
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   if (range < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glDeleteLists");
      return;
   }

   if (range > 1) {
      /* We may be deleting a set of bitmap lists.  See if there's a
       * bitmap atlas to free.
       */
      struct gl_bitmap_atlas *atlas = lookup_bitmap_atlas(ctx, list);
      if (atlas) {
         _mesa_delete_bitmap_atlas(ctx, atlas);
         _mesa_HashRemove(ctx->Shared->BitmapAtlas, list);
      }
   }

   for (i = list; i < list + range; i++) {
      destroy_list(ctx, i);
   }
}


/**
 * Return a display list number, n, such that lists n through n+range-1
 * are free.
 */
GLuint GLAPIENTRY
_mesa_GenLists(GLsizei range)
{
   GET_CURRENT_CONTEXT(ctx);
   GLuint base;
   FLUSH_VERTICES(ctx, 0, 0);      /* must be called before assert */
   ASSERT_OUTSIDE_BEGIN_END_WITH_RETVAL(ctx, 0);

   if (range < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGenLists");
      return 0;
   }
   if (range == 0) {
      return 0;
   }

   /*
    * Make this an atomic operation
    */
   _mesa_HashLockMutex(ctx->Shared->DisplayList);

   base = _mesa_HashFindFreeKeyBlock(ctx->Shared->DisplayList, range);
   if (base) {
      /* reserve the list IDs by with empty/dummy lists */
      GLint i;
      for (i = 0; i < range; i++) {
         _mesa_HashInsertLocked(ctx->Shared->DisplayList, base + i,
                                make_list(base + i, 1), true);
      }
   }

   if (USE_BITMAP_ATLAS &&
       range > 16 &&
       ctx->Driver.DrawAtlasBitmaps) {
      /* "range > 16" is a rough heuristic to guess when glGenLists might be
       * used to allocate display lists for glXUseXFont or wglUseFontBitmaps.
       * Create the empty atlas now.
       */
      struct gl_bitmap_atlas *atlas = lookup_bitmap_atlas(ctx, base);
      if (!atlas) {
         atlas = alloc_bitmap_atlas(ctx, base, true);
      }
      if (atlas) {
         /* Atlas _should_ be new/empty now, but clobbering is OK */
         assert(atlas->numBitmaps == 0);
         atlas->numBitmaps = range;
      }
   }

   _mesa_HashUnlockMutex(ctx->Shared->DisplayList);

   return base;
}


/**
 * Begin a new display list.
 */
void GLAPIENTRY
_mesa_NewList(GLuint name, GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);

   FLUSH_CURRENT(ctx, 0);       /* must be called before assert */
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glNewList %u %s\n", name,
                  _mesa_enum_to_string(mode));

   if (name == 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glNewList");
      return;
   }

   if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
      _mesa_error(ctx, GL_INVALID_ENUM, "glNewList");
      return;
   }

   if (ctx->ListState.CurrentList) {
      /* already compiling a display list */
      _mesa_error(ctx, GL_INVALID_OPERATION, "glNewList");
      return;
   }

   ctx->CompileFlag = GL_TRUE;
   ctx->ExecuteFlag = (mode == GL_COMPILE_AND_EXECUTE);

   /* Reset accumulated list state */
   invalidate_saved_current_state( ctx );

   /* Allocate new display list */
   ctx->ListState.CurrentList = make_list(name, BLOCK_SIZE);
   ctx->ListState.CurrentBlock = ctx->ListState.CurrentList->Head;
   ctx->ListState.CurrentPos = 0;

   vbo_save_NewList(ctx, name, mode);

   ctx->CurrentServerDispatch = ctx->Save;
   _glapi_set_dispatch(ctx->CurrentServerDispatch);
   if (ctx->MarshalExec == NULL) {
      ctx->CurrentClientDispatch = ctx->CurrentServerDispatch;
   }
}


/**
 * End definition of current display list.
 */
void GLAPIENTRY
_mesa_EndList(void)
{
   GET_CURRENT_CONTEXT(ctx);
   SAVE_FLUSH_VERTICES(ctx);
   FLUSH_VERTICES(ctx, 0, 0);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glEndList\n");

   if (ctx->ExecuteFlag && _mesa_inside_dlist_begin_end(ctx)) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glEndList() called inside glBegin/End");
   }

   /* Check that a list is under construction */
   if (!ctx->ListState.CurrentList) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glEndList");
      return;
   }

   /* Call before emitting END_OF_LIST, in case the driver wants to
    * emit opcodes itself.
    */
   vbo_save_EndList(ctx);

   (void) alloc_instruction(ctx, OPCODE_END_OF_LIST, 0);

   trim_list(ctx);

   /* Destroy old list, if any */
   destroy_list(ctx, ctx->ListState.CurrentList->Name);

   /* Install the new list */
   _mesa_HashInsert(ctx->Shared->DisplayList,
                    ctx->ListState.CurrentList->Name,
                    ctx->ListState.CurrentList, true);


   if (MESA_VERBOSE & VERBOSE_DISPLAY_LIST)
      mesa_print_display_list(ctx->ListState.CurrentList->Name);

   ctx->ListState.CurrentList = NULL;
   ctx->ListState.CurrentBlock = NULL;
   ctx->ListState.CurrentPos = 0;
   ctx->ExecuteFlag = GL_TRUE;
   ctx->CompileFlag = GL_FALSE;

   ctx->CurrentServerDispatch = ctx->Exec;
   _glapi_set_dispatch(ctx->CurrentServerDispatch);
   if (ctx->MarshalExec == NULL) {
      ctx->CurrentClientDispatch = ctx->CurrentServerDispatch;
   }
}


void GLAPIENTRY
_mesa_CallList(GLuint list)
{
   GLboolean save_compile_flag;
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_CURRENT(ctx, 0);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glCallList %d\n", list);

   if (list == 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glCallList(list==0)");
      return;
   }

   if (0)
      mesa_print_display_list( list );

   /* Save the CompileFlag status, turn it off, execute the display list,
    * and restore the CompileFlag. This is needed for GL_COMPILE_AND_EXECUTE
    * because the call is already recorded and we just need to execute it.
    */
   save_compile_flag = ctx->CompileFlag;
   if (save_compile_flag) {
      ctx->CompileFlag = GL_FALSE;
   }

   execute_list(ctx, list);
   ctx->CompileFlag = save_compile_flag;

   /* also restore API function pointers to point to "save" versions */
   if (save_compile_flag) {
      ctx->CurrentServerDispatch = ctx->Save;
       _glapi_set_dispatch(ctx->CurrentServerDispatch);
      if (ctx->MarshalExec == NULL) {
         ctx->CurrentClientDispatch = ctx->CurrentServerDispatch;
      }
   }
}


/**
 * Try to execute a glCallLists() command where the display lists contain
 * glBitmap commands with a texture atlas.
 * \return true for success, false otherwise
 */
static bool
render_bitmap_atlas(struct gl_context *ctx, GLsizei n, GLenum type,
                    const void *lists)
{
   struct gl_bitmap_atlas *atlas;
   int i;

   if (!USE_BITMAP_ATLAS ||
       !ctx->Current.RasterPosValid ||
       ctx->List.ListBase == 0 ||
       type != GL_UNSIGNED_BYTE ||
       !ctx->Driver.DrawAtlasBitmaps) {
      /* unsupported */
      return false;
   }

   atlas = lookup_bitmap_atlas(ctx, ctx->List.ListBase);

   if (!atlas) {
      /* Even if glGenLists wasn't called, we can still try to create
       * the atlas now.
       */
      atlas = alloc_bitmap_atlas(ctx, ctx->List.ListBase, false);
   }

   if (atlas && !atlas->complete && !atlas->incomplete) {
      /* Try to build the bitmap atlas now.
       * If the atlas was created in glGenLists, we'll have recorded the
       * number of lists (bitmaps).  Otherwise, take a guess at 256.
       */
      if (atlas->numBitmaps == 0)
         atlas->numBitmaps = 256;
      build_bitmap_atlas(ctx, atlas, ctx->List.ListBase);
   }

   if (!atlas || !atlas->complete) {
      return false;
   }

   /* check that all display list IDs are in the atlas */
   for (i = 0; i < n; i++) {
      const GLubyte *ids = (const GLubyte *) lists;

      if (ids[i] >= atlas->numBitmaps) {
         return false;
      }
   }

   ctx->Driver.DrawAtlasBitmaps(ctx, atlas, n, (const GLubyte *) lists);

   return true;
}


/**
 * Execute glCallLists:  call multiple display lists.
 */
void GLAPIENTRY
_mesa_CallLists(GLsizei n, GLenum type, const GLvoid * lists)
{
   GET_CURRENT_CONTEXT(ctx);
   GLboolean save_compile_flag;

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glCallLists %d\n", n);

   if (type < GL_BYTE || type > GL_4_BYTES) {
      _mesa_error(ctx, GL_INVALID_ENUM, "glCallLists(type)");
      return;
   }

   if (n < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glCallLists(n < 0)");
      return;
   } else if (n == 0 || lists == NULL) {
      /* nothing to do */
      return;
   }

   if (render_bitmap_atlas(ctx, n, type, lists)) {
      return;
   }

   /* Save the CompileFlag status, turn it off, execute the display lists,
    * and restore the CompileFlag. This is needed for GL_COMPILE_AND_EXECUTE
    * because the call is already recorded and we just need to execute it.
    */
   save_compile_flag = ctx->CompileFlag;
   ctx->CompileFlag = GL_FALSE;

   GLbyte *bptr;
   GLubyte *ubptr;
   GLshort *sptr;
   GLushort *usptr;
   GLint *iptr;
   GLuint *uiptr;
   GLfloat *fptr;

   GLuint base = ctx->List.ListBase;

   /* A loop inside a switch is faster than a switch inside a loop. */
   switch (type) {
   case GL_BYTE:
      bptr = (GLbyte *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)bptr[i]);
      break;
   case GL_UNSIGNED_BYTE:
      ubptr = (GLubyte *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)ubptr[i]);
      break;
   case GL_SHORT:
      sptr = (GLshort *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)sptr[i]);
      break;
   case GL_UNSIGNED_SHORT:
      usptr = (GLushort *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)usptr[i]);
      break;
   case GL_INT:
      iptr = (GLint *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)iptr[i]);
      break;
   case GL_UNSIGNED_INT:
      uiptr = (GLuint *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)uiptr[i]);
      break;
   case GL_FLOAT:
      fptr = (GLfloat *) lists;
      for (unsigned i = 0; i < n; i++)
         execute_list(ctx, base + (int)fptr[i]);
      break;
   case GL_2_BYTES:
      ubptr = (GLubyte *) lists;
      for (unsigned i = 0; i < n; i++) {
         execute_list(ctx, base +
                      (int)ubptr[2 * i] * 256 +
                      (int)ubptr[2 * i + 1]);
      }
      break;
   case GL_3_BYTES:
      ubptr = (GLubyte *) lists;
      for (unsigned i = 0; i < n; i++) {
         execute_list(ctx, base +
                      (int)ubptr[3 * i] * 65536 +
                      (int)ubptr[3 * i + 1] * 256 +
                      (int)ubptr[3 * i + 2]);
      }
      break;
   case GL_4_BYTES:
      ubptr = (GLubyte *) lists;
      for (unsigned i = 0; i < n; i++) {
         execute_list(ctx, base +
                      (int)ubptr[4 * i] * 16777216 +
                      (int)ubptr[4 * i + 1] * 65536 +
                      (int)ubptr[4 * i + 2] * 256 +
                      (int)ubptr[4 * i + 3]);
      }
      break;
   }

   ctx->CompileFlag = save_compile_flag;

   /* also restore API function pointers to point to "save" versions */
   if (save_compile_flag) {
      ctx->CurrentServerDispatch = ctx->Save;
      _glapi_set_dispatch(ctx->CurrentServerDispatch);
      if (ctx->MarshalExec == NULL) {
         ctx->CurrentClientDispatch = ctx->CurrentServerDispatch;
      }
   }
}


/**
 * Set the offset added to list numbers in glCallLists.
 */
void GLAPIENTRY
_mesa_ListBase(GLuint base)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_VERTICES(ctx, 0, GL_LIST_BIT);   /* must be called before assert */
   ASSERT_OUTSIDE_BEGIN_END(ctx);
   ctx->List.ListBase = base;
}

/**
 * Setup the given dispatch table to point to Mesa's display list
 * building functions.
 *
 * This does not include any of the tnl functions - they are
 * initialized from _mesa_init_api_defaults and from the active vtxfmt
 * struct.
 */
void
_mesa_initialize_save_table(const struct gl_context *ctx)
{
   struct _glapi_table *table = ctx->Save;
   int numEntries = MAX2(_gloffset_COUNT, _glapi_get_dispatch_table_size());

   /* Initially populate the dispatch table with the contents of the
    * normal-execution dispatch table.  This lets us skip populating functions
    * that should be called directly instead of compiled into display lists.
    */
   memcpy(table, ctx->Exec, numEntries * sizeof(_glapi_proc));

   /* VBO functions */
   vbo_initialize_save_dispatch(ctx, table);

   /* GL 1.0 */
   SET_Accum(table, save_Accum);
   SET_AlphaFunc(table, save_AlphaFunc);
   SET_Bitmap(table, save_Bitmap);
   SET_BlendFunc(table, save_BlendFunc);
   SET_CallList(table, save_CallList);
   SET_CallLists(table, save_CallLists);
   SET_Clear(table, save_Clear);
   SET_ClearAccum(table, save_ClearAccum);
   SET_ClearColor(table, save_ClearColor);
   SET_ClearDepth(table, save_ClearDepth);
   SET_ClearIndex(table, save_ClearIndex);
   SET_ClearStencil(table, save_ClearStencil);
   SET_ClipPlane(table, save_ClipPlane);
   SET_ColorMask(table, save_ColorMask);
   SET_ColorMaski(table, save_ColorMaskIndexed);
   SET_ColorMaterial(table, save_ColorMaterial);
   SET_CopyPixels(table, save_CopyPixels);
   SET_CullFace(table, save_CullFace);
   SET_DepthFunc(table, save_DepthFunc);
   SET_DepthMask(table, save_DepthMask);
   SET_DepthRange(table, save_DepthRange);
   SET_Disable(table, save_Disable);
   SET_Disablei(table, save_DisableIndexed);
   SET_DrawBuffer(table, save_DrawBuffer);
   SET_DrawPixels(table, save_DrawPixels);
   SET_Enable(table, save_Enable);
   SET_Enablei(table, save_EnableIndexed);
   SET_EvalMesh1(table, save_EvalMesh1);
   SET_EvalMesh2(table, save_EvalMesh2);
   SET_Fogf(table, save_Fogf);
   SET_Fogfv(table, save_Fogfv);
   SET_Fogi(table, save_Fogi);
   SET_Fogiv(table, save_Fogiv);
   SET_FrontFace(table, save_FrontFace);
   SET_Frustum(table, save_Frustum);
   SET_Hint(table, save_Hint);
   SET_IndexMask(table, save_IndexMask);
   SET_InitNames(table, save_InitNames);
   SET_LightModelf(table, save_LightModelf);
   SET_LightModelfv(table, save_LightModelfv);
   SET_LightModeli(table, save_LightModeli);
   SET_LightModeliv(table, save_LightModeliv);
   SET_Lightf(table, save_Lightf);
   SET_Lightfv(table, save_Lightfv);
   SET_Lighti(table, save_Lighti);
   SET_Lightiv(table, save_Lightiv);
   SET_LineStipple(table, save_LineStipple);
   SET_LineWidth(table, save_LineWidth);
   SET_ListBase(table, save_ListBase);
   SET_LoadIdentity(table, save_LoadIdentity);
   SET_LoadMatrixd(table, save_LoadMatrixd);
   SET_LoadMatrixf(table, save_LoadMatrixf);
   SET_LoadName(table, save_LoadName);
   SET_LogicOp(table, save_LogicOp);
   SET_Map1d(table, save_Map1d);
   SET_Map1f(table, save_Map1f);
   SET_Map2d(table, save_Map2d);
   SET_Map2f(table, save_Map2f);
   SET_MapGrid1d(table, save_MapGrid1d);
   SET_MapGrid1f(table, save_MapGrid1f);
   SET_MapGrid2d(table, save_MapGrid2d);
   SET_MapGrid2f(table, save_MapGrid2f);
   SET_MatrixMode(table, save_MatrixMode);
   SET_MultMatrixd(table, save_MultMatrixd);
   SET_MultMatrixf(table, save_MultMatrixf);
   SET_NewList(table, save_NewList);
   SET_Ortho(table, save_Ortho);
   SET_PassThrough(table, save_PassThrough);
   SET_PixelMapfv(table, save_PixelMapfv);
   SET_PixelMapuiv(table, save_PixelMapuiv);
   SET_PixelMapusv(table, save_PixelMapusv);
   SET_PixelTransferf(table, save_PixelTransferf);
   SET_PixelTransferi(table, save_PixelTransferi);
   SET_PixelZoom(table, save_PixelZoom);
   SET_PointSize(table, save_PointSize);
   SET_PolygonMode(table, save_PolygonMode);
   SET_PolygonOffset(table, save_PolygonOffset);
   SET_PolygonStipple(table, save_PolygonStipple);
   SET_PopAttrib(table, save_PopAttrib);
   SET_PopMatrix(table, save_PopMatrix);
   SET_PopName(table, save_PopName);
   SET_PushAttrib(table, save_PushAttrib);
   SET_PushMatrix(table, save_PushMatrix);
   SET_PushName(table, save_PushName);
   SET_RasterPos2d(table, save_RasterPos2d);
   SET_RasterPos2dv(table, save_RasterPos2dv);
   SET_RasterPos2f(table, save_RasterPos2f);
   SET_RasterPos2fv(table, save_RasterPos2fv);
   SET_RasterPos2i(table, save_RasterPos2i);
   SET_RasterPos2iv(table, save_RasterPos2iv);
   SET_RasterPos2s(table, save_RasterPos2s);
   SET_RasterPos2sv(table, save_RasterPos2sv);
   SET_RasterPos3d(table, save_RasterPos3d);
   SET_RasterPos3dv(table, save_RasterPos3dv);
   SET_RasterPos3f(table, save_RasterPos3f);
   SET_RasterPos3fv(table, save_RasterPos3fv);
   SET_RasterPos3i(table, save_RasterPos3i);
   SET_RasterPos3iv(table, save_RasterPos3iv);
   SET_RasterPos3s(table, save_RasterPos3s);
   SET_RasterPos3sv(table, save_RasterPos3sv);
   SET_RasterPos4d(table, save_RasterPos4d);
   SET_RasterPos4dv(table, save_RasterPos4dv);
   SET_RasterPos4f(table, save_RasterPos4f);
   SET_RasterPos4fv(table, save_RasterPos4fv);
   SET_RasterPos4i(table, save_RasterPos4i);
   SET_RasterPos4iv(table, save_RasterPos4iv);
   SET_RasterPos4s(table, save_RasterPos4s);
   SET_RasterPos4sv(table, save_RasterPos4sv);
   SET_ReadBuffer(table, save_ReadBuffer);
   SET_Rotated(table, save_Rotated);
   SET_Rotatef(table, save_Rotatef);
   SET_Scaled(table, save_Scaled);
   SET_Scalef(table, save_Scalef);
   SET_Scissor(table, save_Scissor);
   SET_ShadeModel(table, save_ShadeModel);
   SET_StencilFunc(table, save_StencilFunc);
   SET_StencilMask(table, save_StencilMask);
   SET_StencilOp(table, save_StencilOp);
   SET_TexEnvf(table, save_TexEnvf);
   SET_TexEnvfv(table, save_TexEnvfv);
   SET_TexEnvi(table, save_TexEnvi);
   SET_TexEnviv(table, save_TexEnviv);
   SET_TexGend(table, save_TexGend);
   SET_TexGendv(table, save_TexGendv);
   SET_TexGenf(table, save_TexGenf);
   SET_TexGenfv(table, save_TexGenfv);
   SET_TexGeni(table, save_TexGeni);
   SET_TexGeniv(table, save_TexGeniv);
   SET_TexImage1D(table, save_TexImage1D);
   SET_TexImage2D(table, save_TexImage2D);
   SET_TexParameterf(table, save_TexParameterf);
   SET_TexParameterfv(table, save_TexParameterfv);
   SET_TexParameteri(table, save_TexParameteri);
   SET_TexParameteriv(table, save_TexParameteriv);
   SET_Translated(table, save_Translated);
   SET_Translatef(table, save_Translatef);
   SET_Viewport(table, save_Viewport);

   /* GL 1.1 */
   SET_BindTexture(table, save_BindTexture);
   SET_CopyTexImage1D(table, save_CopyTexImage1D);
   SET_CopyTexImage2D(table, save_CopyTexImage2D);
   SET_CopyTexSubImage1D(table, save_CopyTexSubImage1D);
   SET_CopyTexSubImage2D(table, save_CopyTexSubImage2D);
   SET_PrioritizeTextures(table, save_PrioritizeTextures);
   SET_TexSubImage1D(table, save_TexSubImage1D);
   SET_TexSubImage2D(table, save_TexSubImage2D);

   /* GL 1.2 */
   SET_CopyTexSubImage3D(table, save_CopyTexSubImage3D);
   SET_TexImage3D(table, save_TexImage3D);
   SET_TexSubImage3D(table, save_TexSubImage3D);

   /* GL 2.0 */
   SET_StencilFuncSeparate(table, save_StencilFuncSeparate);
   SET_StencilMaskSeparate(table, save_StencilMaskSeparate);
   SET_StencilOpSeparate(table, save_StencilOpSeparate);

   /* ATI_separate_stencil */
   SET_StencilFuncSeparateATI(table, save_StencilFuncSeparateATI);

   /* GL_ARB_imaging */
   /* Not all are supported */
   SET_BlendColor(table, save_BlendColor);
   SET_BlendEquation(table, save_BlendEquation);

   /* 2. GL_EXT_blend_color */
#if 0
   SET_BlendColorEXT(table, save_BlendColorEXT);
#endif

   /* 6. GL_EXT_texture3d */
#if 0
   SET_CopyTexSubImage3DEXT(table, save_CopyTexSubImage3D);
   SET_TexImage3DEXT(table, save_TexImage3DEXT);
   SET_TexSubImage3DEXT(table, save_TexSubImage3D);
#endif

   /* 37. GL_EXT_blend_minmax */
#if 0
   SET_BlendEquationEXT(table, save_BlendEquationEXT);
#endif

   /* 54. GL_EXT_point_parameters */
   SET_PointParameterf(table, save_PointParameterfEXT);
   SET_PointParameterfv(table, save_PointParameterfvEXT);

   /* 91. GL_ARB_tessellation_shader */
   SET_PatchParameteri(table, save_PatchParameteri);
   SET_PatchParameterfv(table, save_PatchParameterfv);

   /* 100. ARB_viewport_array */
   SET_ViewportArrayv(table, save_ViewportArrayv);
   SET_ViewportIndexedf(table, save_ViewportIndexedf);
   SET_ViewportIndexedfv(table, save_ViewportIndexedfv);
   SET_ScissorArrayv(table, save_ScissorArrayv);
   SET_ScissorIndexed(table, save_ScissorIndexed);
   SET_ScissorIndexedv(table, save_ScissorIndexedv);
   SET_DepthRangeArrayv(table, save_DepthRangeArrayv);
   SET_DepthRangeIndexed(table, save_DepthRangeIndexed);

   /* 122. ARB_compute_shader */
   SET_DispatchCompute(table, save_DispatchCompute);
   SET_DispatchComputeIndirect(table, save_DispatchComputeIndirect);

   /* 173. GL_EXT_blend_func_separate */
   SET_BlendFuncSeparate(table, save_BlendFuncSeparateEXT);

   /* 197. GL_MESA_window_pos */
   SET_WindowPos2d(table, save_WindowPos2dMESA);
   SET_WindowPos2dv(table, save_WindowPos2dvMESA);
   SET_WindowPos2f(table, save_WindowPos2fMESA);
   SET_WindowPos2fv(table, save_WindowPos2fvMESA);
   SET_WindowPos2i(table, save_WindowPos2iMESA);
   SET_WindowPos2iv(table, save_WindowPos2ivMESA);
   SET_WindowPos2s(table, save_WindowPos2sMESA);
   SET_WindowPos2sv(table, save_WindowPos2svMESA);
   SET_WindowPos3d(table, save_WindowPos3dMESA);
   SET_WindowPos3dv(table, save_WindowPos3dvMESA);
   SET_WindowPos3f(table, save_WindowPos3fMESA);
   SET_WindowPos3fv(table, save_WindowPos3fvMESA);
   SET_WindowPos3i(table, save_WindowPos3iMESA);
   SET_WindowPos3iv(table, save_WindowPos3ivMESA);
   SET_WindowPos3s(table, save_WindowPos3sMESA);
   SET_WindowPos3sv(table, save_WindowPos3svMESA);
   SET_WindowPos4dMESA(table, save_WindowPos4dMESA);
   SET_WindowPos4dvMESA(table, save_WindowPos4dvMESA);
   SET_WindowPos4fMESA(table, save_WindowPos4fMESA);
   SET_WindowPos4fvMESA(table, save_WindowPos4fvMESA);
   SET_WindowPos4iMESA(table, save_WindowPos4iMESA);
   SET_WindowPos4ivMESA(table, save_WindowPos4ivMESA);
   SET_WindowPos4sMESA(table, save_WindowPos4sMESA);
   SET_WindowPos4svMESA(table, save_WindowPos4svMESA);

   /* 245. GL_ATI_fragment_shader */
   SET_BindFragmentShaderATI(table, save_BindFragmentShaderATI);
   SET_SetFragmentShaderConstantATI(table, save_SetFragmentShaderConstantATI);

   /* 262. GL_ARB_point_sprite */
   SET_PointParameteri(table, save_PointParameteri);
   SET_PointParameteriv(table, save_PointParameteriv);

   /* 268. GL_EXT_stencil_two_side */
   SET_ActiveStencilFaceEXT(table, save_ActiveStencilFaceEXT);

   /* ???. GL_EXT_depth_bounds_test */
   SET_DepthBoundsEXT(table, save_DepthBoundsEXT);

   /* ARB 1. GL_ARB_multitexture */
   SET_ActiveTexture(table, save_ActiveTextureARB);

   /* ARB 3. GL_ARB_transpose_matrix */
   SET_LoadTransposeMatrixd(table, save_LoadTransposeMatrixdARB);
   SET_LoadTransposeMatrixf(table, save_LoadTransposeMatrixfARB);
   SET_MultTransposeMatrixd(table, save_MultTransposeMatrixdARB);
   SET_MultTransposeMatrixf(table, save_MultTransposeMatrixfARB);

   /* ARB 5. GL_ARB_multisample */
   SET_SampleCoverage(table, save_SampleCoverageARB);

   /* ARB 12. GL_ARB_texture_compression */
   SET_CompressedTexImage3D(table, save_CompressedTexImage3DARB);
   SET_CompressedTexImage2D(table, save_CompressedTexImage2DARB);
   SET_CompressedTexImage1D(table, save_CompressedTexImage1DARB);
   SET_CompressedTexSubImage3D(table, save_CompressedTexSubImage3DARB);
   SET_CompressedTexSubImage2D(table, save_CompressedTexSubImage2DARB);
   SET_CompressedTexSubImage1D(table, save_CompressedTexSubImage1DARB);

   /* ARB 14. GL_ARB_point_parameters */
   /* aliased with EXT_point_parameters functions */

   /* ARB 25. GL_ARB_window_pos */
   /* aliased with MESA_window_pos functions */

   /* ARB 26. GL_ARB_vertex_program */
   /* ARB 27. GL_ARB_fragment_program */
   /* glVertexAttrib* functions alias the NV ones, handled elsewhere */
   SET_ProgramStringARB(table, save_ProgramStringARB);
   SET_BindProgramARB(table, save_BindProgramARB);
   SET_ProgramEnvParameter4dARB(table, save_ProgramEnvParameter4dARB);
   SET_ProgramEnvParameter4dvARB(table, save_ProgramEnvParameter4dvARB);
   SET_ProgramEnvParameter4fARB(table, save_ProgramEnvParameter4fARB);
   SET_ProgramEnvParameter4fvARB(table, save_ProgramEnvParameter4fvARB);
   SET_ProgramLocalParameter4dARB(table, save_ProgramLocalParameter4dARB);
   SET_ProgramLocalParameter4dvARB(table, save_ProgramLocalParameter4dvARB);
   SET_ProgramLocalParameter4fARB(table, save_ProgramLocalParameter4fARB);
   SET_ProgramLocalParameter4fvARB(table, save_ProgramLocalParameter4fvARB);

   SET_BeginQuery(table, save_BeginQueryARB);
   SET_EndQuery(table, save_EndQueryARB);
   SET_QueryCounter(table, save_QueryCounter);

   SET_DrawBuffers(table, save_DrawBuffersARB);

   SET_BlitFramebuffer(table, save_BlitFramebufferEXT);

   SET_UseProgram(table, save_UseProgram);
   SET_Uniform1f(table, save_Uniform1fARB);
   SET_Uniform2f(table, save_Uniform2fARB);
   SET_Uniform3f(table, save_Uniform3fARB);
   SET_Uniform4f(table, save_Uniform4fARB);
   SET_Uniform1fv(table, save_Uniform1fvARB);
   SET_Uniform2fv(table, save_Uniform2fvARB);
   SET_Uniform3fv(table, save_Uniform3fvARB);
   SET_Uniform4fv(table, save_Uniform4fvARB);
   SET_Uniform1i(table, save_Uniform1iARB);
   SET_Uniform2i(table, save_Uniform2iARB);
   SET_Uniform3i(table, save_Uniform3iARB);
   SET_Uniform4i(table, save_Uniform4iARB);
   SET_Uniform1iv(table, save_Uniform1ivARB);
   SET_Uniform2iv(table, save_Uniform2ivARB);
   SET_Uniform3iv(table, save_Uniform3ivARB);
   SET_Uniform4iv(table, save_Uniform4ivARB);
   SET_UniformMatrix2fv(table, save_UniformMatrix2fvARB);
   SET_UniformMatrix3fv(table, save_UniformMatrix3fvARB);
   SET_UniformMatrix4fv(table, save_UniformMatrix4fvARB);
   SET_UniformMatrix2x3fv(table, save_UniformMatrix2x3fv);
   SET_UniformMatrix3x2fv(table, save_UniformMatrix3x2fv);
   SET_UniformMatrix2x4fv(table, save_UniformMatrix2x4fv);
   SET_UniformMatrix4x2fv(table, save_UniformMatrix4x2fv);
   SET_UniformMatrix3x4fv(table, save_UniformMatrix3x4fv);
   SET_UniformMatrix4x3fv(table, save_UniformMatrix4x3fv);

   /* 299. GL_EXT_blend_equation_separate */
   SET_BlendEquationSeparate(table, save_BlendEquationSeparateEXT);

   /* GL_EXT_gpu_program_parameters */
   SET_ProgramEnvParameters4fvEXT(table, save_ProgramEnvParameters4fvEXT);
   SET_ProgramLocalParameters4fvEXT(table, save_ProgramLocalParameters4fvEXT);

   /* 364. GL_EXT_provoking_vertex */
   SET_ProvokingVertex(table, save_ProvokingVertexEXT);

   /* GL_EXT_texture_integer */
   SET_ClearColorIiEXT(table, save_ClearColorIi);
   SET_ClearColorIuiEXT(table, save_ClearColorIui);
   SET_TexParameterIiv(table, save_TexParameterIiv);
   SET_TexParameterIuiv(table, save_TexParameterIuiv);

   /* GL_ARB_clip_control */
   SET_ClipControl(table, save_ClipControl);

   /* GL_ARB_color_buffer_float */
   SET_ClampColor(table, save_ClampColorARB);

   /* GL 3.0 */
   SET_ClearBufferiv(table, save_ClearBufferiv);
   SET_ClearBufferuiv(table, save_ClearBufferuiv);
   SET_ClearBufferfv(table, save_ClearBufferfv);
   SET_ClearBufferfi(table, save_ClearBufferfi);
   SET_Uniform1ui(table, save_Uniform1ui);
   SET_Uniform2ui(table, save_Uniform2ui);
   SET_Uniform3ui(table, save_Uniform3ui);
   SET_Uniform4ui(table, save_Uniform4ui);
   SET_Uniform1uiv(table, save_Uniform1uiv);
   SET_Uniform2uiv(table, save_Uniform2uiv);
   SET_Uniform3uiv(table, save_Uniform3uiv);
   SET_Uniform4uiv(table, save_Uniform4uiv);

   /* GL_ARB_gpu_shader_fp64 */
   SET_Uniform1d(table, save_Uniform1d);
   SET_Uniform2d(table, save_Uniform2d);
   SET_Uniform3d(table, save_Uniform3d);
   SET_Uniform4d(table, save_Uniform4d);
   SET_Uniform1dv(table, save_Uniform1dv);
   SET_Uniform2dv(table, save_Uniform2dv);
   SET_Uniform3dv(table, save_Uniform3dv);
   SET_Uniform4dv(table, save_Uniform4dv);
   SET_UniformMatrix2dv(table, save_UniformMatrix2dv);
   SET_UniformMatrix3dv(table, save_UniformMatrix3dv);
   SET_UniformMatrix4dv(table, save_UniformMatrix4dv);
   SET_UniformMatrix2x3dv(table, save_UniformMatrix2x3dv);
   SET_UniformMatrix3x2dv(table, save_UniformMatrix3x2dv);
   SET_UniformMatrix2x4dv(table, save_UniformMatrix2x4dv);
   SET_UniformMatrix4x2dv(table, save_UniformMatrix4x2dv);
   SET_UniformMatrix3x4dv(table, save_UniformMatrix3x4dv);
   SET_UniformMatrix4x3dv(table, save_UniformMatrix4x3dv);

   /* GL_ARB_gpu_shader_int64 */
   SET_Uniform1i64ARB(table, save_Uniform1i64ARB);
   SET_Uniform2i64ARB(table, save_Uniform2i64ARB);
   SET_Uniform3i64ARB(table, save_Uniform3i64ARB);
   SET_Uniform4i64ARB(table, save_Uniform4i64ARB);
   SET_Uniform1i64vARB(table, save_Uniform1i64vARB);
   SET_Uniform2i64vARB(table, save_Uniform2i64vARB);
   SET_Uniform3i64vARB(table, save_Uniform3i64vARB);
   SET_Uniform4i64vARB(table, save_Uniform4i64vARB);
   SET_Uniform1ui64ARB(table, save_Uniform1ui64ARB);
   SET_Uniform2ui64ARB(table, save_Uniform2ui64ARB);
   SET_Uniform3ui64ARB(table, save_Uniform3ui64ARB);
   SET_Uniform4ui64ARB(table, save_Uniform4ui64ARB);
   SET_Uniform1ui64vARB(table, save_Uniform1ui64vARB);
   SET_Uniform2ui64vARB(table, save_Uniform2ui64vARB);
   SET_Uniform3ui64vARB(table, save_Uniform3ui64vARB);
   SET_Uniform4ui64vARB(table, save_Uniform4ui64vARB);

   SET_ProgramUniform1i64ARB(table, save_ProgramUniform1i64ARB);
   SET_ProgramUniform2i64ARB(table, save_ProgramUniform2i64ARB);
   SET_ProgramUniform3i64ARB(table, save_ProgramUniform3i64ARB);
   SET_ProgramUniform4i64ARB(table, save_ProgramUniform4i64ARB);
   SET_ProgramUniform1i64vARB(table, save_ProgramUniform1i64vARB);
   SET_ProgramUniform2i64vARB(table, save_ProgramUniform2i64vARB);
   SET_ProgramUniform3i64vARB(table, save_ProgramUniform3i64vARB);
   SET_ProgramUniform4i64vARB(table, save_ProgramUniform4i64vARB);
   SET_ProgramUniform1ui64ARB(table, save_ProgramUniform1ui64ARB);
   SET_ProgramUniform2ui64ARB(table, save_ProgramUniform2ui64ARB);
   SET_ProgramUniform3ui64ARB(table, save_ProgramUniform3ui64ARB);
   SET_ProgramUniform4ui64ARB(table, save_ProgramUniform4ui64ARB);
   SET_ProgramUniform1ui64vARB(table, save_ProgramUniform1ui64vARB);
   SET_ProgramUniform2ui64vARB(table, save_ProgramUniform2ui64vARB);
   SET_ProgramUniform3ui64vARB(table, save_ProgramUniform3ui64vARB);
   SET_ProgramUniform4ui64vARB(table, save_ProgramUniform4ui64vARB);

   /* These are: */
   SET_BeginTransformFeedback(table, save_BeginTransformFeedback);
   SET_EndTransformFeedback(table, save_EndTransformFeedback);
   SET_BindTransformFeedback(table, save_BindTransformFeedback);
   SET_PauseTransformFeedback(table, save_PauseTransformFeedback);
   SET_ResumeTransformFeedback(table, save_ResumeTransformFeedback);
   SET_DrawTransformFeedback(table, save_DrawTransformFeedback);
   SET_DrawTransformFeedbackStream(table, save_DrawTransformFeedbackStream);
   SET_DrawTransformFeedbackInstanced(table,
                                      save_DrawTransformFeedbackInstanced);
   SET_DrawTransformFeedbackStreamInstanced(table,
                                save_DrawTransformFeedbackStreamInstanced);
   SET_BeginQueryIndexed(table, save_BeginQueryIndexed);
   SET_EndQueryIndexed(table, save_EndQueryIndexed);

   /* GL_ARB_instanced_arrays */
   SET_VertexAttribDivisor(table, save_VertexAttribDivisor);

   /* GL_NV_texture_barrier */
   SET_TextureBarrierNV(table, save_TextureBarrierNV);

   SET_BindSampler(table, save_BindSampler);
   SET_SamplerParameteri(table, save_SamplerParameteri);
   SET_SamplerParameterf(table, save_SamplerParameterf);
   SET_SamplerParameteriv(table, save_SamplerParameteriv);
   SET_SamplerParameterfv(table, save_SamplerParameterfv);
   SET_SamplerParameterIiv(table, save_SamplerParameterIiv);
   SET_SamplerParameterIuiv(table, save_SamplerParameterIuiv);

   /* GL_ARB_draw_buffer_blend */
   SET_BlendFunciARB(table, save_BlendFunci);
   SET_BlendFuncSeparateiARB(table, save_BlendFuncSeparatei);
   SET_BlendEquationiARB(table, save_BlendEquationi);
   SET_BlendEquationSeparateiARB(table, save_BlendEquationSeparatei);

   /* GL_NV_conditional_render */
   SET_BeginConditionalRender(table, save_BeginConditionalRender);
   SET_EndConditionalRender(table, save_EndConditionalRender);

   /* GL_ARB_sync */
   SET_WaitSync(table, save_WaitSync);

   /* GL_ARB_uniform_buffer_object */
   SET_UniformBlockBinding(table, save_UniformBlockBinding);

   /* GL_ARB_shader_subroutines */
   SET_UniformSubroutinesuiv(table, save_UniformSubroutinesuiv);

   /* GL_ARB_draw_instanced */
   SET_DrawArraysInstancedARB(table, save_DrawArraysInstancedARB);
   SET_DrawElementsInstancedARB(table, save_DrawElementsInstancedARB);

   /* GL_ARB_draw_elements_base_vertex */
   SET_DrawElementsInstancedBaseVertex(table, save_DrawElementsInstancedBaseVertexARB);

   /* GL_ARB_base_instance */
   SET_DrawArraysInstancedBaseInstance(table, save_DrawArraysInstancedBaseInstance);
   SET_DrawElementsInstancedBaseInstance(table, save_DrawElementsInstancedBaseInstance);
   SET_DrawElementsInstancedBaseVertexBaseInstance(table, save_DrawElementsInstancedBaseVertexBaseInstance);

   /* GL_ARB_draw_indirect / GL_ARB_multi_draw_indirect */
   SET_DrawArraysIndirect(table, save_DrawArraysIndirect);
   SET_DrawElementsIndirect(table, save_DrawElementsIndirect);
   SET_MultiDrawArraysIndirect(table, save_MultiDrawArraysIndirect);
   SET_MultiDrawElementsIndirect(table, save_MultiDrawElementsIndirect);

   /* OpenGL 4.2 / GL_ARB_separate_shader_objects */
   SET_UseProgramStages(table, save_UseProgramStages);
   SET_ProgramUniform1f(table, save_ProgramUniform1f);
   SET_ProgramUniform2f(table, save_ProgramUniform2f);
   SET_ProgramUniform3f(table, save_ProgramUniform3f);
   SET_ProgramUniform4f(table, save_ProgramUniform4f);
   SET_ProgramUniform1fv(table, save_ProgramUniform1fv);
   SET_ProgramUniform2fv(table, save_ProgramUniform2fv);
   SET_ProgramUniform3fv(table, save_ProgramUniform3fv);
   SET_ProgramUniform4fv(table, save_ProgramUniform4fv);
   SET_ProgramUniform1d(table, save_ProgramUniform1d);
   SET_ProgramUniform2d(table, save_ProgramUniform2d);
   SET_ProgramUniform3d(table, save_ProgramUniform3d);
   SET_ProgramUniform4d(table, save_ProgramUniform4d);
   SET_ProgramUniform1dv(table, save_ProgramUniform1dv);
   SET_ProgramUniform2dv(table, save_ProgramUniform2dv);
   SET_ProgramUniform3dv(table, save_ProgramUniform3dv);
   SET_ProgramUniform4dv(table, save_ProgramUniform4dv);
   SET_ProgramUniform1i(table, save_ProgramUniform1i);
   SET_ProgramUniform2i(table, save_ProgramUniform2i);
   SET_ProgramUniform3i(table, save_ProgramUniform3i);
   SET_ProgramUniform4i(table, save_ProgramUniform4i);
   SET_ProgramUniform1iv(table, save_ProgramUniform1iv);
   SET_ProgramUniform2iv(table, save_ProgramUniform2iv);
   SET_ProgramUniform3iv(table, save_ProgramUniform3iv);
   SET_ProgramUniform4iv(table, save_ProgramUniform4iv);
   SET_ProgramUniform1ui(table, save_ProgramUniform1ui);
   SET_ProgramUniform2ui(table, save_ProgramUniform2ui);
   SET_ProgramUniform3ui(table, save_ProgramUniform3ui);
   SET_ProgramUniform4ui(table, save_ProgramUniform4ui);
   SET_ProgramUniform1uiv(table, save_ProgramUniform1uiv);
   SET_ProgramUniform2uiv(table, save_ProgramUniform2uiv);
   SET_ProgramUniform3uiv(table, save_ProgramUniform3uiv);
   SET_ProgramUniform4uiv(table, save_ProgramUniform4uiv);
   SET_ProgramUniformMatrix2fv(table, save_ProgramUniformMatrix2fv);
   SET_ProgramUniformMatrix3fv(table, save_ProgramUniformMatrix3fv);
   SET_ProgramUniformMatrix4fv(table, save_ProgramUniformMatrix4fv);
   SET_ProgramUniformMatrix2x3fv(table, save_ProgramUniformMatrix2x3fv);
   SET_ProgramUniformMatrix3x2fv(table, save_ProgramUniformMatrix3x2fv);
   SET_ProgramUniformMatrix2x4fv(table, save_ProgramUniformMatrix2x4fv);
   SET_ProgramUniformMatrix4x2fv(table, save_ProgramUniformMatrix4x2fv);
   SET_ProgramUniformMatrix3x4fv(table, save_ProgramUniformMatrix3x4fv);
   SET_ProgramUniformMatrix4x3fv(table, save_ProgramUniformMatrix4x3fv);
   SET_ProgramUniformMatrix2dv(table, save_ProgramUniformMatrix2dv);
   SET_ProgramUniformMatrix3dv(table, save_ProgramUniformMatrix3dv);
   SET_ProgramUniformMatrix4dv(table, save_ProgramUniformMatrix4dv);
   SET_ProgramUniformMatrix2x3dv(table, save_ProgramUniformMatrix2x3dv);
   SET_ProgramUniformMatrix3x2dv(table, save_ProgramUniformMatrix3x2dv);
   SET_ProgramUniformMatrix2x4dv(table, save_ProgramUniformMatrix2x4dv);
   SET_ProgramUniformMatrix4x2dv(table, save_ProgramUniformMatrix4x2dv);
   SET_ProgramUniformMatrix3x4dv(table, save_ProgramUniformMatrix3x4dv);
   SET_ProgramUniformMatrix4x3dv(table, save_ProgramUniformMatrix4x3dv);

   /* GL_{ARB,EXT}_polygon_offset_clamp */
   SET_PolygonOffsetClampEXT(table, save_PolygonOffsetClampEXT);

   /* GL_EXT_window_rectangles */
   SET_WindowRectanglesEXT(table, save_WindowRectanglesEXT);

   /* GL_NV_conservative_raster */
   SET_SubpixelPrecisionBiasNV(table, save_SubpixelPrecisionBiasNV);

   /* GL_NV_conservative_raster_dilate */
   SET_ConservativeRasterParameterfNV(table, save_ConservativeRasterParameterfNV);

   /* GL_NV_conservative_raster_pre_snap_triangles */
   SET_ConservativeRasterParameteriNV(table, save_ConservativeRasterParameteriNV);

   /* GL_EXT_direct_state_access */
   SET_MatrixLoadfEXT(table, save_MatrixLoadfEXT);
   SET_MatrixLoaddEXT(table, save_MatrixLoaddEXT);
   SET_MatrixMultfEXT(table, save_MatrixMultfEXT);
   SET_MatrixMultdEXT(table, save_MatrixMultdEXT);
   SET_MatrixRotatefEXT(table, save_MatrixRotatefEXT);
   SET_MatrixRotatedEXT(table, save_MatrixRotatedEXT);
   SET_MatrixScalefEXT(table, save_MatrixScalefEXT);
   SET_MatrixScaledEXT(table, save_MatrixScaledEXT);
   SET_MatrixTranslatefEXT(table, save_MatrixTranslatefEXT);
   SET_MatrixTranslatedEXT(table, save_MatrixTranslatedEXT);
   SET_MatrixLoadIdentityEXT(table, save_MatrixLoadIdentityEXT);
   SET_MatrixOrthoEXT(table, save_MatrixOrthoEXT);
   SET_MatrixFrustumEXT(table, save_MatrixFrustumEXT);
   SET_MatrixPushEXT(table, save_MatrixPushEXT);
   SET_MatrixPopEXT(table, save_MatrixPopEXT);
   SET_MatrixLoadTransposefEXT(table, save_MatrixLoadTransposefEXT);
   SET_MatrixLoadTransposedEXT(table, save_MatrixLoadTransposedEXT);
   SET_MatrixMultTransposefEXT(table, save_MatrixMultTransposefEXT);
   SET_MatrixMultTransposedEXT(table, save_MatrixMultTransposedEXT);
   SET_TextureParameteriEXT(table, save_TextureParameteriEXT);
   SET_TextureParameterivEXT(table, save_TextureParameterivEXT);
   SET_TextureParameterfEXT(table, save_TextureParameterfEXT);
   SET_TextureParameterfvEXT(table, save_TextureParameterfvEXT);
   SET_TextureParameterIivEXT(table, save_TextureParameterIivEXT);
   SET_TextureParameterIuivEXT(table, save_TextureParameterIuivEXT);
   SET_TextureImage1DEXT(table, save_TextureImage1DEXT);
   SET_TextureImage2DEXT(table, save_TextureImage2DEXT);
   SET_TextureImage3DEXT(table, save_TextureImage3DEXT);
   SET_TextureSubImage1DEXT(table, save_TextureSubImage1DEXT);
   SET_TextureSubImage2DEXT(table, save_TextureSubImage2DEXT);
   SET_TextureSubImage3DEXT(table, save_TextureSubImage3DEXT);
   SET_CopyTextureImage1DEXT(table, save_CopyTextureImage1DEXT);
   SET_CopyTextureImage2DEXT(table, save_CopyTextureImage2DEXT);
   SET_CopyTextureSubImage1DEXT(table, save_CopyTextureSubImage1DEXT);
   SET_CopyTextureSubImage2DEXT(table, save_CopyTextureSubImage2DEXT);
   SET_CopyTextureSubImage3DEXT(table, save_CopyTextureSubImage3DEXT);
   SET_BindMultiTextureEXT(table, save_BindMultiTextureEXT);
   SET_MultiTexParameteriEXT(table, save_MultiTexParameteriEXT);
   SET_MultiTexParameterivEXT(table, save_MultiTexParameterivEXT);
   SET_MultiTexParameterIivEXT(table, save_MultiTexParameterIivEXT);
   SET_MultiTexParameterIuivEXT(table, save_MultiTexParameterIuivEXT);
   SET_MultiTexParameterfEXT(table, save_MultiTexParameterfEXT);
   SET_MultiTexParameterfvEXT(table, save_MultiTexParameterfvEXT);
   SET_MultiTexImage1DEXT(table, save_MultiTexImage1DEXT);
   SET_MultiTexImage2DEXT(table, save_MultiTexImage2DEXT);
   SET_MultiTexImage3DEXT(table, save_MultiTexImage3DEXT);
   SET_MultiTexSubImage1DEXT(table, save_MultiTexSubImage1DEXT);
   SET_MultiTexSubImage2DEXT(table, save_MultiTexSubImage2DEXT);
   SET_MultiTexSubImage3DEXT(table, save_MultiTexSubImage3DEXT);
   SET_CopyMultiTexImage1DEXT(table, save_CopyMultiTexImage1DEXT);
   SET_CopyMultiTexImage2DEXT(table, save_CopyMultiTexImage2DEXT);
   SET_CopyMultiTexSubImage1DEXT(table, save_CopyMultiTexSubImage1DEXT);
   SET_CopyMultiTexSubImage2DEXT(table, save_CopyMultiTexSubImage2DEXT);
   SET_CopyMultiTexSubImage3DEXT(table, save_CopyMultiTexSubImage3DEXT);
   SET_MultiTexEnvfEXT(table, save_MultiTexEnvfEXT);
   SET_MultiTexEnvfvEXT(table, save_MultiTexEnvfvEXT);
   SET_MultiTexEnviEXT(table, save_MultiTexEnviEXT);
   SET_MultiTexEnvivEXT(table, save_MultiTexEnvivEXT);
   SET_CompressedTextureImage1DEXT(table, save_CompressedTextureImage1DEXT);
   SET_CompressedTextureImage2DEXT(table, save_CompressedTextureImage2DEXT);
   SET_CompressedTextureImage3DEXT(table, save_CompressedTextureImage3DEXT);
   SET_CompressedTextureSubImage1DEXT(table, save_CompressedTextureSubImage1DEXT);
   SET_CompressedTextureSubImage2DEXT(table, save_CompressedTextureSubImage2DEXT);
   SET_CompressedTextureSubImage3DEXT(table, save_CompressedTextureSubImage3DEXT);
   SET_CompressedMultiTexImage1DEXT(table, save_CompressedMultiTexImage1DEXT);
   SET_CompressedMultiTexImage2DEXT(table, save_CompressedMultiTexImage2DEXT);
   SET_CompressedMultiTexImage3DEXT(table, save_CompressedMultiTexImage3DEXT);
   SET_CompressedMultiTexSubImage1DEXT(table, save_CompressedMultiTexSubImage1DEXT);
   SET_CompressedMultiTexSubImage2DEXT(table, save_CompressedMultiTexSubImage2DEXT);
   SET_CompressedMultiTexSubImage3DEXT(table, save_CompressedMultiTexSubImage3DEXT);
   SET_NamedProgramStringEXT(table, save_NamedProgramStringEXT);
   SET_NamedProgramLocalParameter4dEXT(table, save_NamedProgramLocalParameter4dEXT);
   SET_NamedProgramLocalParameter4dvEXT(table, save_NamedProgramLocalParameter4dvEXT);
   SET_NamedProgramLocalParameter4fEXT(table, save_NamedProgramLocalParameter4fEXT);
   SET_NamedProgramLocalParameter4fvEXT(table, save_NamedProgramLocalParameter4fvEXT);
}



static const char *
enum_string(GLenum k)
{
   return _mesa_enum_to_string(k);
}


/**
 * Print the commands in a display list.  For debugging only.
 * TODO: many commands aren't handled yet.
 * \param fname  filename to write display list to.  If null, use stdout.
 */
static void GLAPIENTRY
print_list(struct gl_context *ctx, GLuint list, const char *fname)
{
   struct gl_display_list *dlist;
   Node *n;
   FILE *f = stdout;

   if (fname) {
      f = fopen(fname, "w");
      if (!f)
         return;
   }

   if (!_mesa_get_list(ctx, list, &dlist)) {
      fprintf(f, "%u is not a display list ID\n", list);
      fflush(f);
      if (fname)
         fclose(f);
      return;
   }

   n = dlist->Head;

   fprintf(f, "START-LIST %u, address %p\n", list, (void *) n);

   while (1) {
      const OpCode opcode = n[0].opcode;

      switch (opcode) {
         case OPCODE_ACCUM:
            fprintf(f, "Accum %s %g\n", enum_string(n[1].e), n[2].f);
            break;
         case OPCODE_ACTIVE_TEXTURE:
            fprintf(f, "ActiveTexture(%s)\n", enum_string(n[1].e));
            break;
         case OPCODE_BITMAP:
            fprintf(f, "Bitmap %d %d %g %g %g %g %p\n", n[1].i, n[2].i,
                   n[3].f, n[4].f, n[5].f, n[6].f,
                   get_pointer(&n[7]));
            break;
         case OPCODE_BLEND_COLOR:
            fprintf(f, "BlendColor %f, %f, %f, %f\n",
                    n[1].f, n[2].f, n[3].f, n[4].f);
            break;
         case OPCODE_BLEND_EQUATION:
            fprintf(f, "BlendEquation %s\n",
                    enum_string(n[1].e));
            break;
         case OPCODE_BLEND_EQUATION_SEPARATE:
            fprintf(f, "BlendEquationSeparate %s, %s\n",
                    enum_string(n[1].e),
                    enum_string(n[2].e));
            break;
         case OPCODE_BLEND_FUNC_SEPARATE:
            fprintf(f, "BlendFuncSeparate %s, %s, %s, %s\n",
                    enum_string(n[1].e),
                    enum_string(n[2].e),
                    enum_string(n[3].e),
                    enum_string(n[4].e));
            break;
         case OPCODE_BLEND_EQUATION_I:
            fprintf(f, "BlendEquationi %u, %s\n",
                    n[1].ui, enum_string(n[2].e));
            break;
         case OPCODE_BLEND_EQUATION_SEPARATE_I:
            fprintf(f, "BlendEquationSeparatei %u, %s, %s\n",
                    n[1].ui, enum_string(n[2].e), enum_string(n[3].e));
            break;
         case OPCODE_BLEND_FUNC_I:
            fprintf(f, "BlendFunci %u, %s, %s\n",
                    n[1].ui, enum_string(n[2].e), enum_string(n[3].e));
            break;
         case OPCODE_BLEND_FUNC_SEPARATE_I:
            fprintf(f, "BlendFuncSeparatei %u, %s, %s, %s, %s\n",
                    n[1].ui,
                    enum_string(n[2].e),
                    enum_string(n[3].e),
                    enum_string(n[4].e),
                    enum_string(n[5].e));
            break;
         case OPCODE_CALL_LIST:
            fprintf(f, "CallList %d\n", (int) n[1].ui);
            break;
         case OPCODE_CALL_LISTS:
            fprintf(f, "CallLists %d, %s\n", n[1].i, enum_string(n[1].e));
            break;
         case OPCODE_DISABLE:
            fprintf(f, "Disable %s\n", enum_string(n[1].e));
            break;
         case OPCODE_ENABLE:
            fprintf(f, "Enable %s\n", enum_string(n[1].e));
            break;
         case OPCODE_FRUSTUM:
            fprintf(f, "Frustum %g %g %g %g %g %g\n",
                         n[1].f, n[2].f, n[3].f, n[4].f, n[5].f, n[6].f);
            break;
         case OPCODE_LINE_STIPPLE:
            fprintf(f, "LineStipple %d %x\n", n[1].i, (int) n[2].us);
            break;
         case OPCODE_LINE_WIDTH:
            fprintf(f, "LineWidth %f\n", n[1].f);
            break;
         case OPCODE_LOAD_IDENTITY:
            fprintf(f, "LoadIdentity\n");
            break;
         case OPCODE_LOAD_MATRIX:
            fprintf(f, "LoadMatrix\n");
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[1].f, n[5].f, n[9].f, n[13].f);
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[2].f, n[6].f, n[10].f, n[14].f);
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[3].f, n[7].f, n[11].f, n[15].f);
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[4].f, n[8].f, n[12].f, n[16].f);
            break;
         case OPCODE_MULT_MATRIX:
            fprintf(f, "MultMatrix (or Rotate)\n");
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[1].f, n[5].f, n[9].f, n[13].f);
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[2].f, n[6].f, n[10].f, n[14].f);
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[3].f, n[7].f, n[11].f, n[15].f);
            fprintf(f, "  %8f %8f %8f %8f\n",
                         n[4].f, n[8].f, n[12].f, n[16].f);
            break;
         case OPCODE_ORTHO:
            fprintf(f, "Ortho %g %g %g %g %g %g\n",
                         n[1].f, n[2].f, n[3].f, n[4].f, n[5].f, n[6].f);
            break;
         case OPCODE_POINT_SIZE:
            fprintf(f, "PointSize %f\n", n[1].f);
            break;
         case OPCODE_POP_ATTRIB:
            fprintf(f, "PopAttrib\n");
            break;
         case OPCODE_POP_MATRIX:
            fprintf(f, "PopMatrix\n");
            break;
         case OPCODE_POP_NAME:
            fprintf(f, "PopName\n");
            break;
         case OPCODE_PUSH_ATTRIB:
            fprintf(f, "PushAttrib %x\n", n[1].bf);
            break;
         case OPCODE_PUSH_MATRIX:
            fprintf(f, "PushMatrix\n");
            break;
         case OPCODE_PUSH_NAME:
            fprintf(f, "PushName %d\n", (int) n[1].ui);
            break;
         case OPCODE_RASTER_POS:
            fprintf(f, "RasterPos %g %g %g %g\n",
                         n[1].f, n[2].f, n[3].f, n[4].f);
            break;
         case OPCODE_ROTATE:
            fprintf(f, "Rotate %g %g %g %g\n",
                         n[1].f, n[2].f, n[3].f, n[4].f);
            break;
         case OPCODE_SCALE:
            fprintf(f, "Scale %g %g %g\n", n[1].f, n[2].f, n[3].f);
            break;
         case OPCODE_TRANSLATE:
            fprintf(f, "Translate %g %g %g\n", n[1].f, n[2].f, n[3].f);
            break;
         case OPCODE_BIND_TEXTURE:
            fprintf(f, "BindTexture %s %d\n",
                         _mesa_enum_to_string(n[1].ui), n[2].ui);
            break;
         case OPCODE_SHADE_MODEL:
            fprintf(f, "ShadeModel %s\n", _mesa_enum_to_string(n[1].ui));
            break;
         case OPCODE_MAP1:
            fprintf(f, "Map1 %s %.3f %.3f %d %d\n",
                         _mesa_enum_to_string(n[1].ui),
                         n[2].f, n[3].f, n[4].i, n[5].i);
            break;
         case OPCODE_MAP2:
            fprintf(f, "Map2 %s %.3f %.3f %.3f %.3f %d %d %d %d\n",
                         _mesa_enum_to_string(n[1].ui),
                         n[2].f, n[3].f, n[4].f, n[5].f,
                         n[6].i, n[7].i, n[8].i, n[9].i);
            break;
         case OPCODE_MAPGRID1:
            fprintf(f, "MapGrid1 %d %.3f %.3f\n", n[1].i, n[2].f, n[3].f);
            break;
         case OPCODE_MAPGRID2:
            fprintf(f, "MapGrid2 %d %.3f %.3f, %d %.3f %.3f\n",
                         n[1].i, n[2].f, n[3].f, n[4].i, n[5].f, n[6].f);
            break;
         case OPCODE_EVALMESH1:
            fprintf(f, "EvalMesh1 %d %d\n", n[1].i, n[2].i);
            break;
         case OPCODE_EVALMESH2:
            fprintf(f, "EvalMesh2 %d %d %d %d\n",
                         n[1].i, n[2].i, n[3].i, n[4].i);
            break;

         case OPCODE_ATTR_1F_NV:
            fprintf(f, "ATTR_1F_NV attr %d: %f\n", n[1].i, n[2].f);
            break;
         case OPCODE_ATTR_2F_NV:
            fprintf(f, "ATTR_2F_NV attr %d: %f %f\n",
                         n[1].i, n[2].f, n[3].f);
            break;
         case OPCODE_ATTR_3F_NV:
            fprintf(f, "ATTR_3F_NV attr %d: %f %f %f\n",
                         n[1].i, n[2].f, n[3].f, n[4].f);
            break;
         case OPCODE_ATTR_4F_NV:
            fprintf(f, "ATTR_4F_NV attr %d: %f %f %f %f\n",
                         n[1].i, n[2].f, n[3].f, n[4].f, n[5].f);
            break;
         case OPCODE_ATTR_1F_ARB:
            fprintf(f, "ATTR_1F_ARB attr %d: %f\n", n[1].i, n[2].f);
            break;
         case OPCODE_ATTR_2F_ARB:
            fprintf(f, "ATTR_2F_ARB attr %d: %f %f\n",
                         n[1].i, n[2].f, n[3].f);
            break;
         case OPCODE_ATTR_3F_ARB:
            fprintf(f, "ATTR_3F_ARB attr %d: %f %f %f\n",
                         n[1].i, n[2].f, n[3].f, n[4].f);
            break;
         case OPCODE_ATTR_4F_ARB:
            fprintf(f, "ATTR_4F_ARB attr %d: %f %f %f %f\n",
                         n[1].i, n[2].f, n[3].f, n[4].f, n[5].f);
            break;

         case OPCODE_MATERIAL:
            fprintf(f, "MATERIAL %x %x: %f %f %f %f\n",
                         n[1].i, n[2].i, n[3].f, n[4].f, n[5].f, n[6].f);
            break;
         case OPCODE_BEGIN:
            fprintf(f, "BEGIN %x\n", n[1].i);
            break;
         case OPCODE_END:
            fprintf(f, "END\n");
            break;
         case OPCODE_EVAL_C1:
            fprintf(f, "EVAL_C1 %f\n", n[1].f);
            break;
         case OPCODE_EVAL_C2:
            fprintf(f, "EVAL_C2 %f %f\n", n[1].f, n[2].f);
            break;
         case OPCODE_EVAL_P1:
            fprintf(f, "EVAL_P1 %d\n", n[1].i);
            break;
         case OPCODE_EVAL_P2:
            fprintf(f, "EVAL_P2 %d %d\n", n[1].i, n[2].i);
            break;

         case OPCODE_PROVOKING_VERTEX:
            fprintf(f, "ProvokingVertex %s\n",
                         _mesa_enum_to_string(n[1].ui));
            break;

            /*
             * meta opcodes/commands
             */
         case OPCODE_ERROR:
            fprintf(f, "Error: %s %s\n", enum_string(n[1].e),
                   (const char *) get_pointer(&n[2]));
            break;
         case OPCODE_CONTINUE:
            fprintf(f, "DISPLAY-LIST-CONTINUE\n");
            n = (Node *) get_pointer(&n[1]);
            continue;
         case OPCODE_NOP:
            fprintf(f, "NOP\n");
            break;
         case OPCODE_VERTEX_LIST:
            vbo_print_vertex_list(ctx, (struct vbo_save_vertex_list *) &n[1], f);
            break;
         default:
            if (opcode < 0 || opcode > OPCODE_END_OF_LIST) {
               printf
                  ("ERROR IN DISPLAY LIST: opcode = %d, address = %p\n",
                   opcode, (void *) n);
            } else {
               fprintf(f, "command %d, %u operands\n", opcode,
                            InstSize[opcode]);
               break;
            }
            FALLTHROUGH;
         case OPCODE_END_OF_LIST:
            fprintf(f, "END-LIST %u\n", list);
            fflush(f);
            if (fname)
               fclose(f);
            return;
      }

      /* increment n to point to next compiled command */
      assert(InstSize[opcode] > 0);
      n += InstSize[opcode];
   }
}


void
_mesa_glthread_execute_list(struct gl_context *ctx, GLuint list)
{
   struct gl_display_list *dlist;

   if (list == 0 ||
       ctx->GLThread.ListCallDepth == MAX_LIST_NESTING ||
       !_mesa_get_list(ctx, list, &dlist))
      return;

   ctx->GLThread.ListCallDepth++;

   Node *n = dlist->Head;

   while (1) {
      const OpCode opcode = n[0].opcode;

      switch (opcode) {
         case OPCODE_CALL_LIST:
            /* Generated by glCallList(), don't add ListBase */
            if (ctx->GLThread.ListCallDepth < MAX_LIST_NESTING)
               _mesa_glthread_execute_list(ctx, n[1].ui);
            break;
         case OPCODE_CALL_LISTS:
            if (ctx->GLThread.ListCallDepth < MAX_LIST_NESTING)
               _mesa_glthread_CallLists(ctx, n[1].i, n[2].e, get_pointer(&n[3]));
            break;
         case OPCODE_DISABLE:
            _mesa_glthread_Disable(ctx, n[1].e);
            break;
         case OPCODE_ENABLE:
            _mesa_glthread_Enable(ctx, n[1].e);
            break;
         case OPCODE_LIST_BASE:
            _mesa_glthread_ListBase(ctx, n[1].ui);
            break;
         case OPCODE_MATRIX_MODE:
            _mesa_glthread_MatrixMode(ctx, n[1].e);
            break;
         case OPCODE_POP_ATTRIB:
            _mesa_glthread_PopAttrib(ctx);
            break;
         case OPCODE_POP_MATRIX:
            _mesa_glthread_PopMatrix(ctx);
            break;
         case OPCODE_PUSH_ATTRIB:
            _mesa_glthread_PushAttrib(ctx, n[1].bf);
            break;
         case OPCODE_PUSH_MATRIX:
            _mesa_glthread_PushMatrix(ctx);
            break;
         case OPCODE_ACTIVE_TEXTURE:   /* GL_ARB_multitexture */
            _mesa_glthread_ActiveTexture(ctx, n[1].e);
            break;
         case OPCODE_MATRIX_PUSH:
            _mesa_glthread_MatrixPushEXT(ctx, n[1].e);
            break;
         case OPCODE_MATRIX_POP:
            _mesa_glthread_MatrixPopEXT(ctx, n[1].e);
            break;
         case OPCODE_CONTINUE:
            n = (Node *)get_pointer(&n[1]);
            continue;
         case OPCODE_END_OF_LIST:
            ctx->GLThread.ListCallDepth--;
            return;
         default:
            /* ignore */
            break;
      }

      /* increment n to point to next compiled command */
      assert(InstSize[opcode] > 0);
      n += InstSize[opcode];
   }
}


/**
 * Clients may call this function to help debug display list problems.
 * This function is _ONLY_FOR_DEBUGGING_PURPOSES_.  It may be removed,
 * changed, or break in the future without notice.
 */
void
mesa_print_display_list(GLuint list)
{
   GET_CURRENT_CONTEXT(ctx);
   print_list(ctx, list, NULL);
}


/**********************************************************************/
/*****                      Initialization                        *****/
/**********************************************************************/

void
_mesa_install_dlist_vtxfmt(struct _glapi_table *disp,
                           const GLvertexformat *vfmt)
{
   SET_CallList(disp, vfmt->CallList);
   SET_CallLists(disp, vfmt->CallLists);
}


/**
 * Initialize display list state for given context.
 */
void
_mesa_init_display_list(struct gl_context *ctx)
{
   static GLboolean tableInitialized = GL_FALSE;
   GLvertexformat *vfmt = &ctx->ListState.ListVtxfmt;

   /* zero-out the instruction size table, just once */
   if (!tableInitialized) {
      memset(InstSize, 0, sizeof(InstSize));
      tableInitialized = GL_TRUE;
   }

   /* Display list */
   ctx->ListState.CallDepth = 0;
   ctx->ExecuteFlag = GL_TRUE;
   ctx->CompileFlag = GL_FALSE;
   ctx->ListState.CurrentBlock = NULL;
   ctx->ListState.CurrentPos = 0;

   /* Display List group */
   ctx->List.ListBase = 0;

   InstSize[OPCODE_NOP] = 1;
   InstSize[OPCODE_VERTEX_LIST] = 1 + align(sizeof(struct vbo_save_vertex_list), sizeof(Node)) / sizeof(Node);

#define NAME_AE(x) _ae_##x
#define NAME_CALLLIST(x) save_##x
#define NAME(x) save_##x
#define NAME_ES(x) save_##x##ARB

#include "vbo/vbo_init_tmp.h"
}
