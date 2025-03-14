/*
 * Copyright (c) 2013 Rob Clark <robdclark@gmail.com>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <util/u_debug.h>
#include <util/log.h>

#include "isa/isa.h"

#include "disasm.h"
#include "instr-a3xx.h"

static enum debug_t debug;

static const char *levels[] = {
		"",
		"\t",
		"\t\t",
		"\t\t\t",
		"\t\t\t\t",
		"\t\t\t\t\t",
		"\t\t\t\t\t\t",
		"\t\t\t\t\t\t\t",
		"\t\t\t\t\t\t\t\t",
		"\t\t\t\t\t\t\t\t\t",
		"x",
		"x",
		"x",
		"x",
		"x",
		"x",
};

struct disasm_ctx {
	FILE *out;
	struct isa_decode_options *options;
	unsigned level;
	unsigned extra_cycles;

	/**
	 * nop_count/has_end used to detect the real end of shader.  Since
	 * in some cases there can be a epilogue following an `end` we look
	 * for a sequence of `nop`s following the `end`
	 */
	int nop_count;      /* number of nop's since non-nop instruction: */
	bool has_end;       /* have we seen end instruction */

	int cur_n;          /* current instr # */
	int cur_opc_cat;    /* current opc_cat */

	int sfu_delay;

	/**
	 * State accumulated decoding fields of the current instruction,
	 * handled after decoding is complete (ie. at start of next instr)
	 */
	struct {
		bool ss;
		uint8_t nop;
		uint8_t repeat;
	} last;

	/**
	 * State accumulated decoding fields of src or dst register
	 */
	struct {
		bool half;
		bool r;
		enum {
			FILE_GPR = 1,
			FILE_CONST = 2,
		} file;
		unsigned num;
	} reg;

	struct shader_stats *stats;
};

static void print_stats(struct disasm_ctx *ctx)
{
	if (ctx->options->gpu_id >= 600) {
		/* handle MERGEREGS case.. this isn't *entirely* accurate, as
		 * you can have shader stages not using merged register file,
		 * but it is good enough for a guestimate:
		 */
		unsigned n = (ctx->stats->halfreg + 1) / 2;

		ctx->stats->halfreg = 0;
		ctx->stats->fullreg = MAX2(ctx->stats->fullreg, n);
	}

	unsigned instructions = ctx->cur_n + ctx->extra_cycles + 1;

	fprintf(ctx->out, "%sStats:\n", levels[ctx->level]);
	fprintf(ctx->out, "%s- shaderdb: %u instr, %u nops, %u non-nops, %u mov, %u cov\n",
			levels[ctx->level],
			instructions,
			ctx->stats->nops,
			instructions - ctx->stats->nops,
			ctx->stats->mov_count,
			ctx->stats->cov_count);

	fprintf(ctx->out, "%s- shaderdb: %u last-baryf, %d half, %d full, %u constlen\n",
			levels[ctx->level],
			ctx->stats->last_baryf,
			DIV_ROUND_UP(ctx->stats->halfreg, 4),
			DIV_ROUND_UP(ctx->stats->fullreg, 4),
			DIV_ROUND_UP(ctx->stats->constlen, 4));

	fprintf(ctx->out, "%s- shaderdb: %u cat0, %u cat1, %u cat2, %u cat3, %u cat4, %u cat5, %u cat6, %u cat7\n",
			levels[ctx->level],
			ctx->stats->instrs_per_cat[0],
			ctx->stats->instrs_per_cat[1],
			ctx->stats->instrs_per_cat[2],
			ctx->stats->instrs_per_cat[3],
			ctx->stats->instrs_per_cat[4],
			ctx->stats->instrs_per_cat[5],
			ctx->stats->instrs_per_cat[6],
			ctx->stats->instrs_per_cat[7]);

	fprintf(ctx->out, "%s- shaderdb: %u sstall, %u (ss), %u (sy)\n",
			levels[ctx->level],
			ctx->stats->sstall,
			ctx->stats->ss,
			ctx->stats->sy);
}

/* size of largest OPC field of all the instruction categories: */
#define NOPC_BITS 6

static const struct opc_info {
	const char *name;
} opcs[1 << (3+NOPC_BITS)] = {
#define OPC(cat, opc, name) [(opc)] = { #name }
	/* category 0: */
	OPC(0, OPC_NOP,          nop),
	OPC(0, OPC_B,            b),
	OPC(0, OPC_JUMP,         jump),
	OPC(0, OPC_CALL,         call),
	OPC(0, OPC_RET,          ret),
	OPC(0, OPC_KILL,         kill),
	OPC(0, OPC_DEMOTE,       demote),
	OPC(0, OPC_END,          end),
	OPC(0, OPC_EMIT,         emit),
	OPC(0, OPC_CUT,          cut),
	OPC(0, OPC_CHMASK,       chmask),
	OPC(0, OPC_CHSH,         chsh),
	OPC(0, OPC_FLOW_REV,     flow_rev),
	OPC(0, OPC_PREDT,        predt),
	OPC(0, OPC_PREDF,        predf),
	OPC(0, OPC_PREDE,        prede),
	OPC(0, OPC_BKT,          bkt),
	OPC(0, OPC_STKS,         stks),
	OPC(0, OPC_STKR,         stkr),
	OPC(0, OPC_XSET,         xset),
	OPC(0, OPC_XCLR,         xclr),
	OPC(0, OPC_GETONE,       getone),
	OPC(0, OPC_DBG,          dbg),
	OPC(0, OPC_SHPS,         shps),
	OPC(0, OPC_SHPE,         shpe),

	/* category 1: */
	OPC(1, OPC_MOV,          ),
	OPC(1, OPC_MOVMSK,       movmsk),

	/* category 2: */
	OPC(2, OPC_ADD_F,        add.f),
	OPC(2, OPC_MIN_F,        min.f),
	OPC(2, OPC_MAX_F,        max.f),
	OPC(2, OPC_MUL_F,        mul.f),
	OPC(2, OPC_SIGN_F,       sign.f),
	OPC(2, OPC_CMPS_F,       cmps.f),
	OPC(2, OPC_ABSNEG_F,     absneg.f),
	OPC(2, OPC_CMPV_F,       cmpv.f),
	OPC(2, OPC_FLOOR_F,      floor.f),
	OPC(2, OPC_CEIL_F,       ceil.f),
	OPC(2, OPC_RNDNE_F,      rndne.f),
	OPC(2, OPC_RNDAZ_F,      rndaz.f),
	OPC(2, OPC_TRUNC_F,      trunc.f),
	OPC(2, OPC_ADD_U,        add.u),
	OPC(2, OPC_ADD_S,        add.s),
	OPC(2, OPC_SUB_U,        sub.u),
	OPC(2, OPC_SUB_S,        sub.s),
	OPC(2, OPC_CMPS_U,       cmps.u),
	OPC(2, OPC_CMPS_S,       cmps.s),
	OPC(2, OPC_MIN_U,        min.u),
	OPC(2, OPC_MIN_S,        min.s),
	OPC(2, OPC_MAX_U,        max.u),
	OPC(2, OPC_MAX_S,        max.s),
	OPC(2, OPC_ABSNEG_S,     absneg.s),
	OPC(2, OPC_AND_B,        and.b),
	OPC(2, OPC_OR_B,         or.b),
	OPC(2, OPC_NOT_B,        not.b),
	OPC(2, OPC_XOR_B,        xor.b),
	OPC(2, OPC_CMPV_U,       cmpv.u),
	OPC(2, OPC_CMPV_S,       cmpv.s),
	OPC(2, OPC_MUL_U24,      mul.u24),
	OPC(2, OPC_MUL_S24,      mul.s24),
	OPC(2, OPC_MULL_U,       mull.u),
	OPC(2, OPC_BFREV_B,      bfrev.b),
	OPC(2, OPC_CLZ_S,        clz.s),
	OPC(2, OPC_CLZ_B,        clz.b),
	OPC(2, OPC_SHL_B,        shl.b),
	OPC(2, OPC_SHR_B,        shr.b),
	OPC(2, OPC_ASHR_B,       ashr.b),
	OPC(2, OPC_BARY_F,       bary.f),
	OPC(2, OPC_MGEN_B,       mgen.b),
	OPC(2, OPC_GETBIT_B,     getbit.b),
	OPC(2, OPC_SETRM,        setrm),
	OPC(2, OPC_CBITS_B,      cbits.b),
	OPC(2, OPC_SHB,          shb),
	OPC(2, OPC_MSAD,         msad),

	/* category 3: */
	OPC(3, OPC_MAD_U16,      mad.u16),
	OPC(3, OPC_MADSH_U16,    madsh.u16),
	OPC(3, OPC_MAD_S16,      mad.s16),
	OPC(3, OPC_MADSH_M16,    madsh.m16),
	OPC(3, OPC_MAD_U24,      mad.u24),
	OPC(3, OPC_MAD_S24,      mad.s24),
	OPC(3, OPC_MAD_F16,      mad.f16),
	OPC(3, OPC_MAD_F32,      mad.f32),
	OPC(3, OPC_SEL_B16,      sel.b16),
	OPC(3, OPC_SEL_B32,      sel.b32),
	OPC(3, OPC_SEL_S16,      sel.s16),
	OPC(3, OPC_SEL_S32,      sel.s32),
	OPC(3, OPC_SEL_F16,      sel.f16),
	OPC(3, OPC_SEL_F32,      sel.f32),
	OPC(3, OPC_SAD_S16,      sad.s16),
	OPC(3, OPC_SAD_S32,      sad.s32),

	/* category 4: */
	OPC(4, OPC_RCP,          rcp),
	OPC(4, OPC_RSQ,          rsq),
	OPC(4, OPC_LOG2,         log2),
	OPC(4, OPC_EXP2,         exp2),
	OPC(4, OPC_SIN,          sin),
	OPC(4, OPC_COS,          cos),
	OPC(4, OPC_SQRT,         sqrt),
	OPC(4, OPC_HRSQ,         hrsq),
	OPC(4, OPC_HLOG2,        hlog2),
	OPC(4, OPC_HEXP2,        hexp2),

	/* category 5: */
	OPC(5, OPC_ISAM,         isam),
	OPC(5, OPC_ISAML,        isaml),
	OPC(5, OPC_ISAMM,        isamm),
	OPC(5, OPC_SAM,          sam),
	OPC(5, OPC_SAMB,         samb),
	OPC(5, OPC_SAML,         saml),
	OPC(5, OPC_SAMGQ,        samgq),
	OPC(5, OPC_GETLOD,       getlod),
	OPC(5, OPC_CONV,         conv),
	OPC(5, OPC_CONVM,        convm),
	OPC(5, OPC_GETSIZE,      getsize),
	OPC(5, OPC_GETBUF,       getbuf),
	OPC(5, OPC_GETPOS,       getpos),
	OPC(5, OPC_GETINFO,      getinfo),
	OPC(5, OPC_DSX,          dsx),
	OPC(5, OPC_DSY,          dsy),
	OPC(5, OPC_GATHER4R,     gather4r),
	OPC(5, OPC_GATHER4G,     gather4g),
	OPC(5, OPC_GATHER4B,     gather4b),
	OPC(5, OPC_GATHER4A,     gather4a),
	OPC(5, OPC_SAMGP0,       samgp0),
	OPC(5, OPC_SAMGP1,       samgp1),
	OPC(5, OPC_SAMGP2,       samgp2),
	OPC(5, OPC_SAMGP3,       samgp3),
	OPC(5, OPC_DSXPP_1,      dsxpp.1),
	OPC(5, OPC_DSYPP_1,      dsypp.1),
	OPC(5, OPC_RGETPOS,      rgetpos),
	OPC(5, OPC_RGETINFO,     rgetinfo),
	/* macros are needed here for ir3_print */
	OPC(5, OPC_DSXPP_MACRO,  dsxpp.macro),
	OPC(5, OPC_DSYPP_MACRO,  dsypp.macro),


	/* category 6: */
	OPC(6, OPC_LDG,          ldg),
	OPC(6, OPC_LDG_A,        ldg.a),
	OPC(6, OPC_LDL,          ldl),
	OPC(6, OPC_LDP,          ldp),
	OPC(6, OPC_STG,          stg),
	OPC(6, OPC_STG_A,        stg.a),
	OPC(6, OPC_STL,          stl),
	OPC(6, OPC_STP,          stp),
	OPC(6, OPC_LDIB,         ldib),
	OPC(6, OPC_G2L,          g2l),
	OPC(6, OPC_L2G,          l2g),
	OPC(6, OPC_PREFETCH,     prefetch),
	OPC(6, OPC_LDLW,         ldlw),
	OPC(6, OPC_STLW,         stlw),
	OPC(6, OPC_RESFMT,       resfmt),
	OPC(6, OPC_RESINFO,      resinfo),
	OPC(6, OPC_ATOMIC_ADD,     atomic.add),
	OPC(6, OPC_ATOMIC_SUB,     atomic.sub),
	OPC(6, OPC_ATOMIC_XCHG,    atomic.xchg),
	OPC(6, OPC_ATOMIC_INC,     atomic.inc),
	OPC(6, OPC_ATOMIC_DEC,     atomic.dec),
	OPC(6, OPC_ATOMIC_CMPXCHG, atomic.cmpxchg),
	OPC(6, OPC_ATOMIC_MIN,     atomic.min),
	OPC(6, OPC_ATOMIC_MAX,     atomic.max),
	OPC(6, OPC_ATOMIC_AND,     atomic.and),
	OPC(6, OPC_ATOMIC_OR,      atomic.or),
	OPC(6, OPC_ATOMIC_XOR,     atomic.xor),
	OPC(6, OPC_LDGB,         ldgb),
	OPC(6, OPC_STGB,         stgb),
	OPC(6, OPC_STIB,         stib),
	OPC(6, OPC_LDC,          ldc),
	OPC(6, OPC_LDLV,         ldlv),
	OPC(6, OPC_PIPR,         pipr),
	OPC(6, OPC_PIPC,         pipc),
	OPC(6, OPC_EMIT2,        emit),
	OPC(6, OPC_ENDLS,        endls),
	OPC(6, OPC_GETSPID,      getspid),
	OPC(6, OPC_GETWID,       getwid),

	OPC(7, OPC_BAR,          bar),
	OPC(7, OPC_FENCE,        fence),

#undef OPC
};

#define GETINFO(instr) (&(opcs[((instr)->opc_cat << NOPC_BITS) | instr_opc(instr, ctx->gpu_id)]))

const char *disasm_a3xx_instr_name(opc_t opc)
{
	if (opc_cat(opc) == -1) return "??meta??";
	return opcs[opc].name;
}


static void
disasm_field_cb(void *d, const char *field_name, struct isa_decode_value *val)
{
	struct disasm_ctx *ctx = d;

	if (!strcmp(field_name, "NAME")) {
		if (!strcmp("nop", val->str)) {
			if (ctx->has_end) {
				ctx->nop_count++;
				if (ctx->nop_count > 3) {
					ctx->options->stop = true;
				}
			}
			ctx->stats->nops += 1 + ctx->last.repeat;
		} else {
			ctx->nop_count = 0;
		}

		if (!strcmp("end", val->str)) {
			ctx->has_end = true;
			ctx->nop_count = 0;
		} else if (!strcmp("chsh", val->str)) {
			ctx->options->stop = true;
		} else if (!strcmp("bary.f", val->str)) {
			ctx->stats->last_baryf = ctx->cur_n;
		}
	} else if (!strcmp(field_name, "REPEAT")) {
		ctx->extra_cycles += val->num;
		ctx->stats->instrs_per_cat[ctx->cur_opc_cat] += val->num;
		ctx->last.repeat = val->num;
	} else if (!strcmp(field_name, "NOP")) {
		ctx->extra_cycles += val->num;
		ctx->stats->instrs_per_cat[0] += val->num;
		ctx->stats->nops += val->num;
		ctx->last.nop = val->num;
	} else if (!strcmp(field_name, "SY")) {
		ctx->stats->sy += val->num;
	} else if (!strcmp(field_name, "SS")) {
		ctx->stats->ss += val->num;
		ctx->last.ss = !!val->num;
	} else if (!strcmp(field_name, "CONST")) {
		ctx->reg.num = val->num;
		ctx->reg.file = FILE_CONST;
	} else if (!strcmp(field_name, "GPR")) {
		/* don't count GPR regs r48.x (shared) or higher: */
		if (val->num < 48) {
			ctx->reg.num = val->num;
			ctx->reg.file = FILE_GPR;
		}
	} else if (!strcmp(field_name, "SRC_R") ||
			!strcmp(field_name, "SRC1_R") ||
			!strcmp(field_name, "SRC2_R") ||
			!strcmp(field_name, "SRC3_R")) {
		ctx->reg.r = val->num;
	} else if (!strcmp(field_name, "DST")) {
		/* Dest register is always repeated
		 *
		 * Note that this doesn't really properly handle instructions
		 * that write multiple components.. the old disasm didn't handle
		 * that case either.
		 */
		ctx->reg.r = true;
	} else if (strstr(field_name, "HALF")) {
		ctx->reg.half = val->num;
	} else if (!strcmp(field_name, "SWIZ")) {
		unsigned num = (ctx->reg.num << 2) | val->num;
		if (ctx->reg.r)
			num += ctx->last.repeat;

		if (ctx->reg.file == FILE_CONST) {
			ctx->stats->constlen = MAX2(ctx->stats->constlen, num);
		} else if (ctx->reg.file == FILE_GPR) {
			if (ctx->reg.half) {
				ctx->stats->halfreg = MAX2(ctx->stats->halfreg, num);
			} else {
				ctx->stats->fullreg = MAX2(ctx->stats->fullreg, num);
			}
		}

		memset(&ctx->reg, 0, sizeof(ctx->reg));
	}
}

/**
 * Handle stat updates dealt with at the end of instruction decoding,
 * ie. before beginning of next instruction
 */
static void
disasm_handle_last(struct disasm_ctx *ctx)
{
	if (ctx->last.ss) {
		ctx->stats->sstall += ctx->sfu_delay;
		ctx->sfu_delay = 0;
	}

	if (ctx->cur_opc_cat == 4) {
		ctx->sfu_delay = 10;
	} else {
		int n = MIN2(ctx->sfu_delay, 1 + ctx->last.repeat + ctx->last.nop);
		ctx->sfu_delay -= n;
	}

	memset(&ctx->last, 0, sizeof(ctx->last));
}

static void
disasm_instr_cb(void *d, unsigned n, uint64_t instr)
{
	struct disasm_ctx *ctx = d;
	uint32_t *dwords = (uint32_t *)&instr;
	unsigned opc_cat = instr >> 61;

	/* There are some cases where we can get instr_cb called multiple
	 * times per instruction (like when we need an extra line for branch
	 * target labels), don't update stats in these cases:
	 */
	if (n != ctx->cur_n) {
		if (n > 0) {
			disasm_handle_last(ctx);
		}
		ctx->stats->instrs_per_cat[opc_cat]++;
		ctx->cur_n = n;

		/* mov vs cov stats are a bit harder to fish out of the field
		 * names, because current ir3-cat1.xml doesn't use {NAME} for
		 * this distinction.  So for now just handle this case with
		 * some hand-coded parsing:
		 */
		if (opc_cat == 1) {
			unsigned opc      = (instr >> 57) & 0x3;
			unsigned src_type = (instr >> 50) & 0x7;
			unsigned dst_type = (instr >> 46) & 0x7;

			if (opc == 0) {
				if (src_type == dst_type) {
					ctx->stats->mov_count++;
				} else {
					ctx->stats->cov_count++;
				}
			}
		}
	}

	ctx->cur_opc_cat = opc_cat;

	if (debug & PRINT_RAW) {
		fprintf(ctx->out, "%s:%d:%04d:%04d[%08xx_%08xx] ", levels[ctx->level],
			opc_cat, n, ctx->extra_cycles + n, dwords[1], dwords[0]);
	}
}

int disasm_a3xx_stat(uint32_t *dwords, int sizedwords, int level, FILE *out,
		unsigned gpu_id, struct shader_stats *stats)
{
	struct isa_decode_options decode_options = {
		.gpu_id = gpu_id,
		.show_errors = true,
		.max_errors = 5,
		.branch_labels = true,
		.field_cb = disasm_field_cb,
		.instr_cb = disasm_instr_cb,
	};
	struct disasm_ctx ctx = {
		.out = out,
		.level = level,
		.options = &decode_options,
		.stats = stats,
		.cur_n = -1,
	};

	memset(stats, 0, sizeof(*stats));

	decode_options.cbdata = &ctx;

	isa_decode(dwords, sizedwords * 4, out, &decode_options);

	disasm_handle_last(&ctx);

	if (debug & PRINT_STATS)
		print_stats(&ctx);

	return 0;
}

void disasm_a3xx_set_debug(enum debug_t d)
{
	debug = d;
}

#include <setjmp.h>

static bool jmp_env_valid;
static jmp_buf jmp_env;

void
ir3_assert_handler(const char *expr, const char *file, int line,
		const char *func)
{
	mesa_loge("%s:%u: %s: Assertion `%s' failed.", file, line, func, expr);
	if (jmp_env_valid)
		longjmp(jmp_env, 1);
	abort();
}

#define TRY(x) do { \
		assert(!jmp_env_valid); \
		if (setjmp(jmp_env) == 0) { \
			jmp_env_valid = true; \
			x; \
		} \
		jmp_env_valid = false; \
	} while (0)


int disasm_a3xx(uint32_t *dwords, int sizedwords, int level, FILE *out, unsigned gpu_id)
{
	struct shader_stats stats;
	return disasm_a3xx_stat(dwords, sizedwords, level, out, gpu_id, &stats);
}

int try_disasm_a3xx(uint32_t *dwords, int sizedwords, int level, FILE *out, unsigned gpu_id)
{
	struct shader_stats stats;
	int ret = -1;
	TRY(ret = disasm_a3xx_stat(dwords, sizedwords, level, out, gpu_id, &stats));
	return ret;
}
