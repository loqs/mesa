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

#ifndef INSTR_A3XX_H_
#define INSTR_A3XX_H_

#define PACKED __attribute__((__packed__))

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

void ir3_assert_handler(const char *expr, const char *file, int line,
		const char *func) __attribute__((weak)) __attribute__ ((__noreturn__));

/* A wrapper for assert() that allows overriding handling of a failed
 * assert.  This is needed for tools like crashdec which can want to
 * attempt to disassemble memory that might not actually be valid
 * instructions.
 */
#define ir3_assert(expr) do { \
		if (!(expr)) { \
			if (ir3_assert_handler) { \
				ir3_assert_handler(#expr, __FILE__, __LINE__, __func__); \
			} \
			assert(expr); \
		} \
	} while (0)
/* size of largest OPC field of all the instruction categories: */
#define NOPC_BITS 6

#define _OPC(cat, opc)   (((cat) << NOPC_BITS) | opc)

typedef enum {
	/* category 0: */
	OPC_NOP             = _OPC(0, 0),
	OPC_B               = _OPC(0, 1),
	OPC_JUMP            = _OPC(0, 2),
	OPC_CALL            = _OPC(0, 3),
	OPC_RET             = _OPC(0, 4),
	OPC_KILL            = _OPC(0, 5),
	OPC_END             = _OPC(0, 6),
	OPC_EMIT            = _OPC(0, 7),
	OPC_CUT             = _OPC(0, 8),
	OPC_CHMASK          = _OPC(0, 9),
	OPC_CHSH            = _OPC(0, 10),
	OPC_FLOW_REV        = _OPC(0, 11),

	OPC_BKT             = _OPC(0, 16),
	OPC_STKS            = _OPC(0, 17),
	OPC_STKR            = _OPC(0, 18),
	OPC_XSET            = _OPC(0, 19),
	OPC_XCLR            = _OPC(0, 20),
	OPC_GETONE          = _OPC(0, 21),
	OPC_DBG             = _OPC(0, 22),
	OPC_SHPS            = _OPC(0, 23),   /* shader prologue start */
	OPC_SHPE            = _OPC(0, 24),   /* shader prologue end */

	OPC_PREDT           = _OPC(0, 29),   /* predicated true */
	OPC_PREDF           = _OPC(0, 30),   /* predicated false */
	OPC_PREDE           = _OPC(0, 31),   /* predicated end */

	/* Logical opcodes for different branch instruction variations: */
	OPC_BR              = _OPC(0, 40),
	OPC_BRAO            = _OPC(0, 41),
	OPC_BRAA            = _OPC(0, 42),
	OPC_BRAC            = _OPC(0, 43),
	OPC_BANY            = _OPC(0, 44),
	OPC_BALL            = _OPC(0, 45),
	OPC_BRAX            = _OPC(0, 46),

	/* Logical opcode to distinguish kill and demote */
	OPC_DEMOTE          = _OPC(0, 47),

	/* category 1: */
	OPC_MOV             = _OPC(1, 0),
	OPC_MOVP            = _OPC(1, 1),
	/* swz, gat, sct */
	OPC_MOVMSK          = _OPC(1, 3),

	/* Virtual opcodes for instructions differentiated via a "sub-opcode" that
	 * replaces the repeat field:
	 */
	OPC_SWZ            = _OPC(1, 4),
	OPC_GAT            = _OPC(1, 5),
	OPC_SCT            = _OPC(1, 6),

	/* Logical opcodes for different variants of mov: */
	OPC_MOV_IMMED       = _OPC(1, 40),
	OPC_MOV_CONST       = _OPC(1, 41),
	OPC_MOV_GPR         = _OPC(1, 42),
	OPC_MOV_RELGPR      = _OPC(1, 43),
	OPC_MOV_RELCONST    = _OPC(1, 44),

	/* category 2: */
	OPC_ADD_F           = _OPC(2, 0),
	OPC_MIN_F           = _OPC(2, 1),
	OPC_MAX_F           = _OPC(2, 2),
	OPC_MUL_F           = _OPC(2, 3),
	OPC_SIGN_F          = _OPC(2, 4),
	OPC_CMPS_F          = _OPC(2, 5),
	OPC_ABSNEG_F        = _OPC(2, 6),
	OPC_CMPV_F          = _OPC(2, 7),
	/* 8 - invalid */
	OPC_FLOOR_F         = _OPC(2, 9),
	OPC_CEIL_F          = _OPC(2, 10),
	OPC_RNDNE_F         = _OPC(2, 11),
	OPC_RNDAZ_F         = _OPC(2, 12),
	OPC_TRUNC_F         = _OPC(2, 13),
	/* 14-15 - invalid */
	OPC_ADD_U           = _OPC(2, 16),
	OPC_ADD_S           = _OPC(2, 17),
	OPC_SUB_U           = _OPC(2, 18),
	OPC_SUB_S           = _OPC(2, 19),
	OPC_CMPS_U          = _OPC(2, 20),
	OPC_CMPS_S          = _OPC(2, 21),
	OPC_MIN_U           = _OPC(2, 22),
	OPC_MIN_S           = _OPC(2, 23),
	OPC_MAX_U           = _OPC(2, 24),
	OPC_MAX_S           = _OPC(2, 25),
	OPC_ABSNEG_S        = _OPC(2, 26),
	/* 27 - invalid */
	OPC_AND_B           = _OPC(2, 28),
	OPC_OR_B            = _OPC(2, 29),
	OPC_NOT_B           = _OPC(2, 30),
	OPC_XOR_B           = _OPC(2, 31),
	/* 32 - invalid */
	OPC_CMPV_U          = _OPC(2, 33),
	OPC_CMPV_S          = _OPC(2, 34),
	/* 35-47 - invalid */
	OPC_MUL_U24         = _OPC(2, 48), /* 24b mul into 32b result */
	OPC_MUL_S24         = _OPC(2, 49), /* 24b mul into 32b result with sign extension */
	OPC_MULL_U          = _OPC(2, 50),
	OPC_BFREV_B         = _OPC(2, 51),
	OPC_CLZ_S           = _OPC(2, 52),
	OPC_CLZ_B           = _OPC(2, 53),
	OPC_SHL_B           = _OPC(2, 54),
	OPC_SHR_B           = _OPC(2, 55),
	OPC_ASHR_B          = _OPC(2, 56),
	OPC_BARY_F          = _OPC(2, 57),
	OPC_MGEN_B          = _OPC(2, 58),
	OPC_GETBIT_B        = _OPC(2, 59),
	OPC_SETRM           = _OPC(2, 60),
	OPC_CBITS_B         = _OPC(2, 61),
	OPC_SHB             = _OPC(2, 62),
	OPC_MSAD            = _OPC(2, 63),

	/* category 3: */
	OPC_MAD_U16         = _OPC(3, 0),
	OPC_MADSH_U16       = _OPC(3, 1),
	OPC_MAD_S16         = _OPC(3, 2),
	OPC_MADSH_M16       = _OPC(3, 3),   /* should this be .s16? */
	OPC_MAD_U24         = _OPC(3, 4),
	OPC_MAD_S24         = _OPC(3, 5),
	OPC_MAD_F16         = _OPC(3, 6),
	OPC_MAD_F32         = _OPC(3, 7),
	OPC_SEL_B16         = _OPC(3, 8),
	OPC_SEL_B32         = _OPC(3, 9),
	OPC_SEL_S16         = _OPC(3, 10),
	OPC_SEL_S32         = _OPC(3, 11),
	OPC_SEL_F16         = _OPC(3, 12),
	OPC_SEL_F32         = _OPC(3, 13),
	OPC_SAD_S16         = _OPC(3, 14),
	OPC_SAD_S32         = _OPC(3, 15),

	/* category 4: */
	OPC_RCP             = _OPC(4, 0),
	OPC_RSQ             = _OPC(4, 1),
	OPC_LOG2            = _OPC(4, 2),
	OPC_EXP2            = _OPC(4, 3),
	OPC_SIN             = _OPC(4, 4),
	OPC_COS             = _OPC(4, 5),
	OPC_SQRT            = _OPC(4, 6),
	/* NOTE that these are 8+opc from their highp equivs, so it's possible
	 * that the high order bit in the opc field has been repurposed for
	 * half-precision use?  But note that other ops (rcp/lsin/cos/sqrt)
	 * still use the same opc as highp
	 */
	OPC_HRSQ            = _OPC(4, 9),
	OPC_HLOG2           = _OPC(4, 10),
	OPC_HEXP2           = _OPC(4, 11),

	/* category 5: */
	OPC_ISAM            = _OPC(5, 0),
	OPC_ISAML           = _OPC(5, 1),
	OPC_ISAMM           = _OPC(5, 2),
	OPC_SAM             = _OPC(5, 3),
	OPC_SAMB            = _OPC(5, 4),
	OPC_SAML            = _OPC(5, 5),
	OPC_SAMGQ           = _OPC(5, 6),
	OPC_GETLOD          = _OPC(5, 7),
	OPC_CONV            = _OPC(5, 8),
	OPC_CONVM           = _OPC(5, 9),
	OPC_GETSIZE         = _OPC(5, 10),
	OPC_GETBUF          = _OPC(5, 11),
	OPC_GETPOS          = _OPC(5, 12),
	OPC_GETINFO         = _OPC(5, 13),
	OPC_DSX             = _OPC(5, 14),
	OPC_DSY             = _OPC(5, 15),
	OPC_GATHER4R        = _OPC(5, 16),
	OPC_GATHER4G        = _OPC(5, 17),
	OPC_GATHER4B        = _OPC(5, 18),
	OPC_GATHER4A        = _OPC(5, 19),
	OPC_SAMGP0          = _OPC(5, 20),
	OPC_SAMGP1          = _OPC(5, 21),
	OPC_SAMGP2          = _OPC(5, 22),
	OPC_SAMGP3          = _OPC(5, 23),
	OPC_DSXPP_1         = _OPC(5, 24),
	OPC_DSYPP_1         = _OPC(5, 25),
	OPC_RGETPOS         = _OPC(5, 26),
	OPC_RGETINFO        = _OPC(5, 27),
	/* cat5 meta instructions, placed above the cat5 opc field's size */
	OPC_DSXPP_MACRO     = _OPC(5, 32),
	OPC_DSYPP_MACRO     = _OPC(5, 33),

	/* category 6: */
	OPC_LDG             = _OPC(6, 0),        /* load-global */
	OPC_LDL             = _OPC(6, 1),
	OPC_LDP             = _OPC(6, 2),
	OPC_STG             = _OPC(6, 3),        /* store-global */
	OPC_STL             = _OPC(6, 4),
	OPC_STP             = _OPC(6, 5),
	OPC_LDIB            = _OPC(6, 6),
	OPC_G2L             = _OPC(6, 7),
	OPC_L2G             = _OPC(6, 8),
	OPC_PREFETCH        = _OPC(6, 9),
	OPC_LDLW            = _OPC(6, 10),
	OPC_STLW            = _OPC(6, 11),
	OPC_RESFMT          = _OPC(6, 14),
	OPC_RESINFO         = _OPC(6, 15),
	OPC_ATOMIC_ADD      = _OPC(6, 16),
	OPC_ATOMIC_SUB      = _OPC(6, 17),
	OPC_ATOMIC_XCHG     = _OPC(6, 18),
	OPC_ATOMIC_INC      = _OPC(6, 19),
	OPC_ATOMIC_DEC      = _OPC(6, 20),
	OPC_ATOMIC_CMPXCHG  = _OPC(6, 21),
	OPC_ATOMIC_MIN      = _OPC(6, 22),
	OPC_ATOMIC_MAX      = _OPC(6, 23),
	OPC_ATOMIC_AND      = _OPC(6, 24),
	OPC_ATOMIC_OR       = _OPC(6, 25),
	OPC_ATOMIC_XOR      = _OPC(6, 26),
	OPC_LDGB            = _OPC(6, 27),
	OPC_STGB            = _OPC(6, 28),
	OPC_STIB            = _OPC(6, 29),
	OPC_LDC             = _OPC(6, 30),
	OPC_LDLV            = _OPC(6, 31),
	OPC_PIPR            = _OPC(6, 32), /* ??? */
	OPC_PIPC            = _OPC(6, 33), /* ??? */
	OPC_EMIT2           = _OPC(6, 34), /* ??? */
	OPC_ENDLS           = _OPC(6, 35), /* ??? */
	OPC_GETSPID         = _OPC(6, 36), /* SP ID */
	OPC_GETWID          = _OPC(6, 37), /* wavefront ID */

	/* Logical opcodes for things that differ in a6xx+ */
	OPC_STC             = _OPC(6, 40),
	OPC_RESINFO_B       = _OPC(6, 41),
	OPC_LDIB_B          = _OPC(6, 42),
	OPC_STIB_B          = _OPC(6, 43),

	/* Logical opcodes for different atomic instruction variations: */
	OPC_ATOMIC_B_ADD      = _OPC(6, 44),
	OPC_ATOMIC_B_SUB      = _OPC(6, 45),
	OPC_ATOMIC_B_XCHG     = _OPC(6, 46),
	OPC_ATOMIC_B_INC      = _OPC(6, 47),
	OPC_ATOMIC_B_DEC      = _OPC(6, 48),
	OPC_ATOMIC_B_CMPXCHG  = _OPC(6, 49),
	OPC_ATOMIC_B_MIN      = _OPC(6, 50),
	OPC_ATOMIC_B_MAX      = _OPC(6, 51),
	OPC_ATOMIC_B_AND      = _OPC(6, 52),
	OPC_ATOMIC_B_OR       = _OPC(6, 53),
	OPC_ATOMIC_B_XOR      = _OPC(6, 54),

	OPC_LDG_A           = _OPC(6, 55),
	OPC_STG_A           = _OPC(6, 56),

	/* category 7: */
	OPC_BAR             = _OPC(7, 0),
	OPC_FENCE           = _OPC(7, 1),

	/* meta instructions (category -1): */
	/* placeholder instr to mark shader inputs: */
	OPC_META_INPUT      = _OPC(-1, 0),
	/* The "collect" and "split" instructions are used for keeping
	 * track of instructions that write to multiple dst registers
	 * (split) like texture sample instructions, or read multiple
	 * consecutive scalar registers (collect) (bary.f, texture samp)
	 *
	 * A "split" extracts a scalar component from a vecN, and a
	 * "collect" gathers multiple scalar components into a vecN
	 */
	OPC_META_SPLIT      = _OPC(-1, 2),
	OPC_META_COLLECT    = _OPC(-1, 3),

	/* placeholder for texture fetches that run before FS invocation
	 * starts:
	 */
	OPC_META_TEX_PREFETCH = _OPC(-1, 4),

	/* Parallel copies have multiple destinations, and copy each destination
	 * to its corresponding source. This happens "in parallel," meaning that
	 * it happens as-if every source is read first and then every destination
	 * is stored. These are produced in RA when register shuffling is
	 * required, and then lowered away immediately afterwards.
	 */
	OPC_META_PARALLEL_COPY = _OPC(-1, 5),
	OPC_META_PHI = _OPC(-1, 6),
} opc_t;

#define opc_cat(opc) ((int)((opc) >> NOPC_BITS))
#define opc_op(opc)  ((unsigned)((opc) & ((1 << NOPC_BITS) - 1)))

const char *disasm_a3xx_instr_name(opc_t opc);

typedef enum {
	TYPE_F16 = 0,
	TYPE_F32 = 1,
	TYPE_U16 = 2,
	TYPE_U32 = 3,
	TYPE_S16 = 4,
	TYPE_S32 = 5,
	TYPE_U8  = 6,
	TYPE_S8  = 7,  // XXX I assume?
} type_t;

static inline uint32_t type_size(type_t type)
{
	switch (type) {
	case TYPE_F32:
	case TYPE_U32:
	case TYPE_S32:
		return 32;
	case TYPE_F16:
	case TYPE_U16:
	case TYPE_S16:
		return 16;
	case TYPE_U8:
	case TYPE_S8:
		return 8;
	default:
		ir3_assert(0); /* invalid type */
		return 0;
	}
}

static inline int type_float(type_t type)
{
	return (type == TYPE_F32) || (type == TYPE_F16);
}

static inline int type_uint(type_t type)
{
	return (type == TYPE_U32) || (type == TYPE_U16) || (type == TYPE_U8);
}

static inline int type_sint(type_t type)
{
	return (type == TYPE_S32) || (type == TYPE_S16) || (type == TYPE_S8);
}

typedef enum {
	ROUND_ZERO = 0,
	ROUND_EVEN = 1,
	ROUND_POS_INF = 2,
	ROUND_NEG_INF = 3,
} round_t;

typedef union PACKED {
	/* normal gpr or const src register: */
	struct PACKED {
		uint32_t comp  : 2;
		uint32_t num   : 10;
	};
	/* for immediate val: */
	int32_t  iim_val   : 11;
	/* to make compiler happy: */
	uint32_t dummy32;
	uint32_t dummy10   : 10;
	int32_t  idummy10  : 10;
	uint32_t dummy11   : 11;
	uint32_t dummy12   : 12;
	uint32_t dummy13   : 13;
	uint32_t dummy8    : 8;
	int32_t  idummy13  : 13;
	int32_t  idummy8   : 8;
} reg_t;

/* comp:
 *   0 - x
 *   1 - y
 *   2 - z
 *   3 - w
 */
static inline uint32_t regid(int num, int comp)
{
	return (num << 2) | (comp & 0x3);
}

#define INVALID_REG      regid(63, 0)
#define VALIDREG(r)      ((r) != INVALID_REG)
#define CONDREG(r, val)  COND(VALIDREG(r), (val))

/* special registers: */
#define REG_A0 61       /* address register */
#define REG_P0 62       /* predicate register */

static inline int reg_special(reg_t reg)
{
	return (reg.num == REG_A0) || (reg.num == REG_P0);
}

typedef enum {
	BRANCH_PLAIN = 0,   /* br */
	BRANCH_OR    = 1,   /* brao */
	BRANCH_AND   = 2,   /* braa */
	BRANCH_CONST = 3,   /* brac */
	BRANCH_ANY   = 4,   /* bany */
	BRANCH_ALL   = 5,   /* ball */
	BRANCH_X     = 6,   /* brax ??? */
} brtype_t;

typedef struct PACKED {
	/* dword0: */
	union PACKED {
		struct PACKED {
			int16_t  immed    : 16;
			uint32_t dummy1   : 16;
		} a3xx;
		struct PACKED {
			int32_t  immed    : 20;
			uint32_t dummy1   : 12;
		} a4xx;
		struct PACKED {
			int32_t immed     : 32;
		} a5xx;
	};

	/* dword1: */
	uint32_t idx      : 5;  /* brac.N index */
	uint32_t brtype   : 3;  /* branch type, see brtype_t */
	uint32_t repeat   : 3;
	uint32_t dummy3   : 1;
	uint32_t ss       : 1;
	uint32_t inv2     : 1;
	uint32_t comp2    : 2;
	uint32_t eq       : 1;
	uint32_t opc_hi   : 1;  /* at least one bit */
	uint32_t dummy4   : 2;
	uint32_t inv1     : 1;
	uint32_t comp1    : 2;  /* component for first src */
	uint32_t opc      : 4;
	uint32_t jmp_tgt  : 1;
	uint32_t sync     : 1;
	uint32_t opc_cat  : 3;
} instr_cat0_t;

typedef struct PACKED {
	/* dword0: */
	union PACKED {
		/* for normal src register: */
		struct PACKED {
			uint32_t src : 11;
			/* at least low bit of pad must be zero or it will
			 * look like a address relative src
			 */
			uint32_t pad : 21;
		};
		/* for address relative: */
		struct PACKED {
			int32_t  off : 10;
			uint32_t src_rel_c : 1;
			uint32_t src_rel : 1;
			uint32_t unknown : 20;
		};
		/* for immediate: */
		int32_t  iim_val;
		uint32_t uim_val;
		float    fim_val;
	};

	/* dword1: */
	uint32_t dst        : 8;
	uint32_t repeat     : 3;
	uint32_t src_r      : 1;
	uint32_t ss         : 1;
	uint32_t ul         : 1;
	uint32_t dst_type   : 3;
	uint32_t dst_rel    : 1;
	uint32_t src_type   : 3;
	uint32_t src_c      : 1;
	uint32_t src_im     : 1;
	uint32_t even       : 1;
	uint32_t pos_inf    : 1;
	uint32_t opc        : 2;
	uint32_t jmp_tgt    : 1;
	uint32_t sync       : 1;
	uint32_t opc_cat    : 3;
} instr_cat1_t;

typedef struct PACKED {
	/* dword0: */
	union PACKED {
		struct PACKED {
			uint32_t src1         : 11;
			uint32_t must_be_zero1: 2;
			uint32_t src1_im      : 1;   /* immediate */
			uint32_t src1_neg     : 1;   /* negate */
			uint32_t src1_abs     : 1;   /* absolute value */
		};
		struct PACKED {
			uint32_t src1         : 10;
			uint32_t src1_c       : 1;   /* relative-const */
			uint32_t src1_rel     : 1;   /* relative address */
			uint32_t must_be_zero : 1;
			uint32_t dummy        : 3;
		} rel1;
		struct PACKED {
			uint32_t src1         : 12;
			uint32_t src1_c       : 1;   /* const */
			int32_t dummy        : 3;
		} c1;
	};

	union PACKED {
		struct PACKED {
			uint32_t src2         : 11;
			uint32_t must_be_zero2: 2;
			uint32_t src2_im      : 1;   /* immediate */
			uint32_t src2_neg     : 1;   /* negate */
			uint32_t src2_abs     : 1;   /* absolute value */
		};
		struct PACKED {
			uint32_t src2         : 10;
			uint32_t src2_c       : 1;   /* relative-const */
			uint32_t src2_rel     : 1;   /* relative address */
			uint32_t must_be_zero : 1;
			uint32_t dummy        : 3;
		} rel2;
		struct PACKED {
			uint32_t src2         : 12;
			uint32_t src2_c       : 1;   /* const */
			uint32_t dummy        : 3;
		} c2;
	};

	/* dword1: */
	uint32_t dst      : 8;
	uint32_t repeat   : 2;
	uint32_t sat      : 1;
	uint32_t src1_r   : 1;   /* doubles as nop0 if repeat==0 */
	uint32_t ss       : 1;
	uint32_t ul       : 1;   /* dunno */
	uint32_t dst_half : 1;   /* or widen/narrow.. ie. dst hrN <-> rN */
	uint32_t ei       : 1;
	uint32_t cond     : 3;
	uint32_t src2_r   : 1;   /* doubles as nop1 if repeat==0 */
	uint32_t full     : 1;   /* not half */
	uint32_t opc      : 6;
	uint32_t jmp_tgt  : 1;
	uint32_t sync     : 1;
	uint32_t opc_cat  : 3;
} instr_cat2_t;

typedef struct PACKED {
	/* dword0: */
	union PACKED {
		struct PACKED {
			uint32_t src1         : 11;
			uint32_t must_be_zero1: 2;
			uint32_t src2_c       : 1;
			uint32_t src1_neg     : 1;
			uint32_t src2_r       : 1;  /* doubles as nop1 if repeat==0 */
		};
		struct PACKED {
			uint32_t src1         : 10;
			uint32_t src1_c       : 1;
			uint32_t src1_rel     : 1;
			uint32_t must_be_zero : 1;
			uint32_t dummy        : 3;
		} rel1;
		struct PACKED {
			uint32_t src1         : 12;
			uint32_t src1_c       : 1;
			uint32_t dummy        : 3;
		} c1;
	};

	union PACKED {
		struct PACKED {
			uint32_t src3         : 11;
			uint32_t must_be_zero2: 2;
			uint32_t src3_r       : 1;
			uint32_t src2_neg     : 1;
			uint32_t src3_neg     : 1;
		};
		struct PACKED {
			uint32_t src3         : 10;
			uint32_t src3_c       : 1;
			uint32_t src3_rel     : 1;
			uint32_t must_be_zero : 1;
			uint32_t dummy        : 3;
		} rel2;
		struct PACKED {
			uint32_t src3         : 12;
			uint32_t src3_c       : 1;
			uint32_t dummy        : 3;
		} c2;
	};

	/* dword1: */
	uint32_t dst      : 8;
	uint32_t repeat   : 2;
	uint32_t sat      : 1;
	uint32_t src1_r   : 1;   /* doubles as nop0 if repeat==0 */
	uint32_t ss       : 1;
	uint32_t ul       : 1;
	uint32_t dst_half : 1;   /* or widen/narrow.. ie. dst hrN <-> rN */
	uint32_t src2     : 8;
	uint32_t opc      : 4;
	uint32_t jmp_tgt  : 1;
	uint32_t sync     : 1;
	uint32_t opc_cat  : 3;
} instr_cat3_t;

static inline bool instr_cat3_full(instr_cat3_t *cat3)
{
	switch (_OPC(3, cat3->opc)) {
	case OPC_MAD_F16:
	case OPC_MAD_U16:
	case OPC_MAD_S16:
	case OPC_SEL_B16:
	case OPC_SEL_S16:
	case OPC_SEL_F16:
	case OPC_SAD_S16:
	case OPC_SAD_S32:  // really??
		return false;
	default:
		return true;
	}
}

typedef struct PACKED {
	/* dword0: */
	union PACKED {
		struct PACKED {
			uint32_t src          : 11;
			uint32_t must_be_zero1: 2;
			uint32_t src_im       : 1;   /* immediate */
			uint32_t src_neg      : 1;   /* negate */
			uint32_t src_abs      : 1;   /* absolute value */
		};
		struct PACKED {
			uint32_t src          : 10;
			uint32_t src_c        : 1;   /* relative-const */
			uint32_t src_rel      : 1;   /* relative address */
			uint32_t must_be_zero : 1;
			uint32_t dummy        : 3;
		} rel;
		struct PACKED {
			uint32_t src          : 12;
			uint32_t src_c        : 1;   /* const */
			uint32_t dummy        : 3;
		} c;
	};
	uint32_t dummy1   : 16;  /* seem to be ignored */

	/* dword1: */
	uint32_t dst      : 8;
	uint32_t repeat   : 2;
	uint32_t sat      : 1;
	uint32_t src_r    : 1;
	uint32_t ss       : 1;
	uint32_t ul       : 1;
	uint32_t dst_half : 1;   /* or widen/narrow.. ie. dst hrN <-> rN */
	uint32_t dummy2   : 5;   /* seem to be ignored */
	uint32_t full     : 1;   /* not half */
	uint32_t opc      : 6;
	uint32_t jmp_tgt  : 1;
	uint32_t sync     : 1;
	uint32_t opc_cat  : 3;
} instr_cat4_t;

/* With is_bindless_s2en = 1, this determines whether bindless is enabled and
 * if so, how to get the (base, index) pair for both sampler and texture.
 * There is a single base embedded in the instruction, which is always used
 * for the texture.
 */
typedef enum {
	/* Use traditional GL binding model, get texture and sampler index
	 * from src3 which is not presumed to be uniform. This is
	 * backwards-compatible with earlier generations, where this field was
	 * always 0 and nonuniform-indexed sampling always worked.
	 */
	CAT5_NONUNIFORM = 0,

	/* The sampler base comes from the low 3 bits of a1.x, and the sampler
	 * and texture index come from src3 which is presumed to be uniform.
	 */
	CAT5_BINDLESS_A1_UNIFORM = 1,

	/* The texture and sampler share the same base, and the sampler and
	 * texture index come from src3 which is *not* presumed to be uniform.
	 */
	CAT5_BINDLESS_NONUNIFORM = 2,

	/* The sampler base comes from the low 3 bits of a1.x, and the sampler
	 * and texture index come from src3 which is *not* presumed to be
	 * uniform.
	 */
	CAT5_BINDLESS_A1_NONUNIFORM = 3,

	/* Use traditional GL binding model, get texture and sampler index
	 * from src3 which is presumed to be uniform.
	 */
	CAT5_UNIFORM = 4,

	/* The texture and sampler share the same base, and the sampler and
	 * texture index come from src3 which is presumed to be uniform.
	 */
	CAT5_BINDLESS_UNIFORM = 5,

	/* The texture and sampler share the same base, get sampler index from low
	 * 4 bits of src3 and texture index from high 4 bits.
	 */
	CAT5_BINDLESS_IMM = 6,

	/* The sampler base comes from the low 3 bits of a1.x, and the texture
	 * index comes from the next 8 bits of a1.x. The sampler index is an
	 * immediate in src3.
	 */
	CAT5_BINDLESS_A1_IMM = 7,
} cat5_desc_mode_t;

typedef struct PACKED {
	/* dword0: */
	union PACKED {
		/* normal case: */
		struct PACKED {
			uint32_t full     : 1;   /* not half */
			uint32_t src1     : 8;
			uint32_t src2     : 8;
			uint32_t dummy1   : 4;   /* seem to be ignored */
			uint32_t samp     : 4;
			uint32_t tex      : 7;
		} norm;
		/* s2en case: */
		struct PACKED {
			uint32_t full         : 1;   /* not half */
			uint32_t src1         : 8;
			uint32_t src2         : 8;
			uint32_t dummy1       : 2;
			uint32_t base_hi      : 2;
			uint32_t src3         : 8;
			uint32_t desc_mode    : 3;
		} s2en_bindless;
		/* same in either case: */
		// XXX I think, confirm this
		struct PACKED {
			uint32_t full     : 1;   /* not half */
			uint32_t src1     : 8;
			uint32_t src2     : 8;
			uint32_t pad      : 15;
		};
	};

	/* dword1: */
	uint32_t dst              : 8;
	uint32_t wrmask           : 4;   /* write-mask */
	uint32_t type             : 3;
	uint32_t base_lo          : 1;   /* used with bindless */
	uint32_t is_3d            : 1;

	uint32_t is_a             : 1;
	uint32_t is_s             : 1;
	uint32_t is_s2en_bindless : 1;
	uint32_t is_o             : 1;
	uint32_t is_p             : 1;

	uint32_t opc              : 5;
	uint32_t jmp_tgt          : 1;
	uint32_t sync             : 1;
	uint32_t opc_cat          : 3;
} instr_cat5_t;

/* dword0 encoding for src_off: [src1 + off], src3: */
typedef struct PACKED {
	/* dword0: */
	uint32_t mustbe1  : 1;
	int32_t  off      : 13;   /* src2 */
	uint32_t src1     : 8;
	uint32_t src1_im  : 1;
	uint32_t src3_im  : 1;
	uint32_t src3     : 8;

	/* dword1: */
	uint32_t dword1;
} instr_cat6a_t;

/* dword0 encoding for !src_off: [src1], src2 */
typedef struct PACKED {
	/* dword0: */
	uint32_t mustbe0  : 1;
	uint32_t src1     : 8;
	uint32_t pad      : 5;
	uint32_t ignore0  : 8;
	uint32_t src1_im  : 1;
	uint32_t src2_im  : 1;
	uint32_t src2     : 8;

	/* dword1: */
	uint32_t dword1;
} instr_cat6b_t;

/* dword1 encoding for dst_off: */
typedef struct PACKED {
	/* dword0: */
	uint32_t dw0_pad1 : 9;
	int32_t off_high : 5;
	uint32_t dw0_pad2 : 18;

	uint32_t off      : 8;
	uint32_t mustbe1  : 1;
	uint32_t dst      : 8;
	uint32_t pad1     : 15;
} instr_cat6c_t;

/* dword1 encoding for !dst_off: */
typedef struct PACKED {
	/* dword0: */
	uint32_t dword0;

	uint32_t dst      : 8;
	uint32_t mustbe0  : 1;
	uint32_t idx      : 8;
	uint32_t pad0     : 15;
} instr_cat6d_t;

/* ldgb and atomics..
 *
 * ldgb:      pad0=0, pad3=1
 * atomic .g: pad0=1, pad3=1
 *        .l: pad0=1, pad3=0
 */
typedef struct PACKED {
	/* dword0: */
	uint32_t pad0     : 1;
	uint32_t src3     : 8;
	uint32_t d        : 2;
	uint32_t typed    : 1;
	uint32_t type_size : 2;
	uint32_t src1     : 8;
	uint32_t src1_im  : 1;
	uint32_t src2_im  : 1;
	uint32_t src2     : 8;

	/* dword1: */
	uint32_t dst      : 8;
	uint32_t mustbe0  : 1;
	uint32_t src_ssbo : 8;
	uint32_t pad2     : 3;  // type
	uint32_t g        : 1;
	uint32_t src_ssbo_im : 1;
	uint32_t pad4     : 10; // opc/jmp_tgt/sync/opc_cat
} instr_cat6ldgb_t;

/* stgb, pad0=0, pad3=2
 */
typedef struct PACKED {
	/* dword0: */
	uint32_t mustbe1  : 1;  // ???
	uint32_t src1     : 8;
	uint32_t d        : 2;
	uint32_t typed    : 1;
	uint32_t type_size : 2;
	uint32_t pad0     : 9;
	uint32_t src2_im  : 1;
	uint32_t src2     : 8;

	/* dword1: */
	uint32_t src3     : 8;
	uint32_t src3_im  : 1;
	uint32_t dst_ssbo : 8;
	uint32_t pad2     : 3;  // type
	uint32_t pad3     : 2;
	uint32_t pad4     : 10; // opc/jmp_tgt/sync/opc_cat
} instr_cat6stgb_t;

typedef union PACKED {
	instr_cat6a_t a;
	instr_cat6b_t b;
	instr_cat6c_t c;
	instr_cat6d_t d;
	instr_cat6ldgb_t ldgb;
	instr_cat6stgb_t stgb;
	struct PACKED {
		/* dword0: */
		uint32_t src_off  : 1;
		uint32_t pad1     : 31;

		/* dword1: */
		uint32_t pad2     : 8;
		uint32_t dst_off  : 1;
		uint32_t pad3     : 8;
		uint32_t type     : 3;
		uint32_t g        : 1;  /* or in some cases it means dst immed */
		uint32_t pad4     : 1;
		uint32_t opc      : 5;
		uint32_t jmp_tgt  : 1;
		uint32_t sync     : 1;
		uint32_t opc_cat  : 3;
	};
} instr_cat6_t;

/* Similar to cat5_desc_mode_t, describes how the descriptor is loaded.
 */
typedef enum {
	/* Use old GL binding model with an immediate index. */
	CAT6_IMM = 0,

	CAT6_UNIFORM = 1,

	CAT6_NONUNIFORM = 2,

	/* Use the bindless model, with an immediate index.
	 */
	CAT6_BINDLESS_IMM = 4,

	/* Use the bindless model, with a uniform register index.
	 */
	CAT6_BINDLESS_UNIFORM = 5,

	/* Use the bindless model, with a register index that isn't guaranteed
	 * to be uniform. This presumably checks if the indices are equal and
	 * splits up the load/store, because it works the way you would
	 * expect.
	 */
	CAT6_BINDLESS_NONUNIFORM = 6,
} cat6_desc_mode_t;

/**
 * For atomic ops (which return a value):
 *
 *    pad1=1, pad3=6, pad5=3
 *    src1    - vecN offset/coords
 *    src2.x  - is actually dest register
 *    src2.y  - is 'data' except for cmpxchg where src2.y is 'compare'
 *              and src2.z is 'data'
 *
 * For stib (which does not return a value):
 *    pad1=0, pad3=6, pad5=2
 *    src1    - vecN offset/coords
 *    src2    - value to store
 *
 * For ldib:
 *    pad1=1, pad3=6, pad5=2
 *    src1    - vecN offset/coords
 *
 * for ldc (load from UBO using descriptor):
 *    pad1=0, pad3=4, pad5=2
 *
 * pad2 and pad5 are only observed to be 0.
 */
typedef struct PACKED {
	/* dword0: */
	uint32_t pad1     : 1;
	uint32_t base     : 3;
	uint32_t pad2     : 2;
	uint32_t desc_mode : 3;
	uint32_t d        : 2;
	uint32_t typed    : 1;
	uint32_t type_size : 2;
	uint32_t opc      : 6;
	uint32_t pad3     : 4;
	uint32_t src1     : 8;  /* coordinate/offset */

	/* dword1: */
	uint32_t src2     : 8;  /* or the dst for load instructions */
	uint32_t pad4     : 1;  //mustbe0 ??
	uint32_t ssbo     : 8;  /* ssbo/image binding point */
	uint32_t type     : 3;
	uint32_t pad5     : 7;
	uint32_t jmp_tgt  : 1;
	uint32_t sync     : 1;
	uint32_t opc_cat  : 3;
} instr_cat6_a6xx_t;

typedef struct PACKED {
	/* dword0: */
	uint32_t pad1     : 32;

	/* dword1: */
	uint32_t pad2     : 12;
	uint32_t ss       : 1;  /* maybe in the encoding, but blob only uses (sy) */
	uint32_t pad3     : 6;
	uint32_t w        : 1;  /* write */
	uint32_t r        : 1;  /* read */
	uint32_t l        : 1;  /* local */
	uint32_t g        : 1;  /* global */
	uint32_t opc      : 4;  /* presumed, but only a couple known OPCs */
	uint32_t jmp_tgt  : 1;  /* (jp) */
	uint32_t sync     : 1;  /* (sy) */
	uint32_t opc_cat  : 3;
} instr_cat7_t;

typedef union PACKED {
	instr_cat0_t cat0;
	instr_cat1_t cat1;
	instr_cat2_t cat2;
	instr_cat3_t cat3;
	instr_cat4_t cat4;
	instr_cat5_t cat5;
	instr_cat6_t cat6;
	instr_cat6_a6xx_t cat6_a6xx;
	instr_cat7_t cat7;
	struct PACKED {
		/* dword0: */
		uint32_t pad1     : 32;

		/* dword1: */
		uint32_t pad2     : 12;
		uint32_t ss       : 1;  /* cat1-cat4 (cat0??) and cat7 (?) */
		uint32_t ul       : 1;  /* cat2-cat4 (and cat1 in blob.. which may be bug??) */
		uint32_t pad3     : 13;
		uint32_t jmp_tgt  : 1;
		uint32_t sync     : 1;
		uint32_t opc_cat  : 3;

	};
} instr_t;

static inline uint32_t instr_repeat(instr_t *instr)
{
	switch (instr->opc_cat) {
	case 0:  return instr->cat0.repeat;
	case 1:  return instr->cat1.repeat;
	case 2:  return instr->cat2.repeat;
	case 3:  return instr->cat3.repeat;
	case 4:  return instr->cat4.repeat;
	default: return 0;
	}
}

static inline bool instr_sat(instr_t *instr)
{
	switch (instr->opc_cat) {
	case 2:  return instr->cat2.sat;
	case 3:  return instr->cat3.sat;
	case 4:  return instr->cat4.sat;
	default: return false;
	}
}

static inline bool is_sat_compatible(opc_t opc)
{
	/* On a6xx saturation doesn't work on cat4 */
	if (opc_cat(opc) != 2 && opc_cat(opc) != 3)
		return false;

	switch (opc) {
	/* On a3xx and a6xx saturation doesn't work on bary.f */
	case OPC_BARY_F:
	/* On a6xx saturation doesn't work on sel.* */
	case OPC_SEL_B16:
	case OPC_SEL_B32:
	case OPC_SEL_S16:
	case OPC_SEL_S32:
	case OPC_SEL_F16:
	case OPC_SEL_F32:
		return false;
	default:
		return true;
	}
}

/* We can probably drop the gpu_id arg, but keeping it for now so we can
 * assert if we see something we think should be new encoding on an older
 * gpu.
 */
static inline bool is_cat6_legacy(instr_t *instr, unsigned gpu_id)
{
	instr_cat6_a6xx_t *cat6 = &instr->cat6_a6xx;

	if (gpu_id < 600)
		return true;

	/* At least one of these two bits is pad in all the possible
	 * "legacy" cat6 encodings, and a analysis of all the pre-a6xx
	 * cmdstream traces I have indicates that the pad bit is zero
	 * in all cases.  So we can use this to detect new encoding:
	 */
	if ((cat6->pad3 & 0x4) && (cat6->pad5 & 0x2)) {
		ir3_assert(instr->cat6.opc == 0);
		return false;
	}

	return true;
}

static inline uint32_t instr_opc(instr_t *instr, unsigned gpu_id)
{
	switch (instr->opc_cat) {
	case 0:  return instr->cat0.opc | instr->cat0.opc_hi << 4;
	case 1:  return instr->cat1.opc;
	case 2:  return instr->cat2.opc;
	case 3:  return instr->cat3.opc;
	case 4:  return instr->cat4.opc;
	case 5:  return instr->cat5.opc;
	case 6:
		if (!is_cat6_legacy(instr, gpu_id))
			return instr->cat6_a6xx.opc;
		return instr->cat6.opc;
	case 7:  return instr->cat7.opc;
	default: return 0;
	}
}

static inline bool is_mad(opc_t opc)
{
	switch (opc) {
	case OPC_MAD_U16:
	case OPC_MAD_S16:
	case OPC_MAD_U24:
	case OPC_MAD_S24:
	case OPC_MAD_F16:
	case OPC_MAD_F32:
		return true;
	default:
		return false;
	}
}

static inline bool is_madsh(opc_t opc)
{
	switch (opc) {
	case OPC_MADSH_U16:
	case OPC_MADSH_M16:
		return true;
	default:
		return false;
	}
}

static inline bool is_atomic(opc_t opc)
{
	switch (opc) {
	case OPC_ATOMIC_ADD:
	case OPC_ATOMIC_SUB:
	case OPC_ATOMIC_XCHG:
	case OPC_ATOMIC_INC:
	case OPC_ATOMIC_DEC:
	case OPC_ATOMIC_CMPXCHG:
	case OPC_ATOMIC_MIN:
	case OPC_ATOMIC_MAX:
	case OPC_ATOMIC_AND:
	case OPC_ATOMIC_OR:
	case OPC_ATOMIC_XOR:
		return true;
	default:
		return false;
	}
}

static inline bool is_ssbo(opc_t opc)
{
	switch (opc) {
	case OPC_RESFMT:
	case OPC_RESINFO:
	case OPC_LDGB:
	case OPC_STGB:
	case OPC_STIB:
		return true;
	default:
		return false;
	}
}

static inline bool is_isam(opc_t opc)
{
	switch (opc) {
	case OPC_ISAM:
	case OPC_ISAML:
	case OPC_ISAMM:
		return true;
	default:
		return false;
	}
}


static inline bool is_cat2_float(opc_t opc)
{
	switch (opc) {
	case OPC_ADD_F:
	case OPC_MIN_F:
	case OPC_MAX_F:
	case OPC_MUL_F:
	case OPC_SIGN_F:
	case OPC_CMPS_F:
	case OPC_ABSNEG_F:
	case OPC_CMPV_F:
	case OPC_FLOOR_F:
	case OPC_CEIL_F:
	case OPC_RNDNE_F:
	case OPC_RNDAZ_F:
	case OPC_TRUNC_F:
		return true;

	default:
		return false;
	}
}

static inline bool is_cat3_float(opc_t opc)
{
	switch (opc) {
	case OPC_MAD_F16:
	case OPC_MAD_F32:
	case OPC_SEL_F16:
	case OPC_SEL_F32:
		return true;
	default:
		return false;
	}
}

#endif /* INSTR_A3XX_H_ */
