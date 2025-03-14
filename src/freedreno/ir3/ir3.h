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

#ifndef IR3_H_
#define IR3_H_

#include <stdint.h>
#include <stdbool.h>

#include "compiler/shader_enums.h"

#include "util/bitscan.h"
#include "util/list.h"
#include "util/set.h"
#include "util/u_debug.h"

#include "instr-a3xx.h"

/* low level intermediate representation of an adreno shader program */

struct ir3_compiler;
struct ir3;
struct ir3_instruction;
struct ir3_block;

struct ir3_info {
	void *data;              /* used internally in ir3 assembler */
	/* Size in bytes of the shader binary, including NIR constants and
	 * padding
	 */
	uint32_t size;
	/* byte offset from start of the shader to the NIR constant data. */
	uint32_t constant_data_offset;
	/* Size in dwords of the instructions. */
	uint16_t sizedwords;
	uint16_t instrs_count;   /* expanded to account for rpt's */
	uint16_t nops_count;     /* # of nop instructions, including nopN */
	uint16_t mov_count;
	uint16_t cov_count;
	/* NOTE: max_reg, etc, does not include registers not touched
	 * by the shader (ie. vertex fetched via VFD_DECODE but not
	 * touched by shader)
	 */
	int8_t   max_reg;   /* highest GPR # used by shader */
	int8_t   max_half_reg;
	int16_t  max_const;
	/* This is the maximum # of waves that can executed at once in one core,
	 * assuming that they are all executing this shader.
	 */
	int8_t   max_waves;
	bool     double_threadsize;
	bool     multi_dword_ldp_stp;

	/* number of sync bits: */
	uint16_t ss, sy;

	/* estimate of number of cycles stalled on (ss) */
	uint16_t sstall;

	uint16_t last_baryf;     /* instruction # of last varying fetch */

	/* Number of instructions of a given category: */
	uint16_t instrs_per_cat[8];
};

struct ir3_merge_set {
	uint16_t preferred_reg;
	uint16_t size;
	uint16_t alignment;

	unsigned interval_start;

	unsigned regs_count;
	struct ir3_register **regs;
};

struct ir3_register {
	enum {
		IR3_REG_CONST  = 0x001,
		IR3_REG_IMMED  = 0x002,
		IR3_REG_HALF   = 0x004,
		/* Shared registers have the same value for all threads when read.
		 * They can only be written when one thread is active (that is, inside
		 * a "getone" block).
		 */
		IR3_REG_SHARED = 0x008,
		IR3_REG_RELATIV= 0x010,
		IR3_REG_R      = 0x020,
		/* Most instructions, it seems, can do float abs/neg but not
		 * integer.  The CP pass needs to know what is intended (int or
		 * float) in order to do the right thing.  For this reason the
		 * abs/neg flags are split out into float and int variants.  In
		 * addition, .b (bitwise) operations, the negate is actually a
		 * bitwise not, so split that out into a new flag to make it
		 * more clear.
		 */
		IR3_REG_FNEG   = 0x040,
		IR3_REG_FABS   = 0x080,
		IR3_REG_SNEG   = 0x100,
		IR3_REG_SABS   = 0x200,
		IR3_REG_BNOT   = 0x400,
		/* (ei) flag, end-input?  Set on last bary, presumably to signal
		 * that the shader needs no more input:
		 */
		IR3_REG_EI     = 0x2000,
		/* meta-flags, for intermediate stages of IR, ie.
		 * before register assignment is done:
		 */
		IR3_REG_SSA    = 0x4000,   /* 'instr' is ptr to assigning instr */
		IR3_REG_ARRAY  = 0x8000,

		IR3_REG_KILL = 0x10000,
		IR3_REG_FIRST_KILL = 0x20000,
		IR3_REG_UNUSED = 0x40000,
	} flags;

	/* used for cat5 instructions, but also for internal/IR level
	 * tracking of what registers are read/written by an instruction.
	 * wrmask may be a bad name since it is used to represent both
	 * src and dst that touch multiple adjacent registers.
	 */
	unsigned wrmask : 16;  /* up to vec16 */

	/* for relative addressing, 32bits for array size is too small,
	 * but otoh we don't need to deal with disjoint sets, so instead
	 * use a simple size field (number of scalar components).
	 *
	 * Note the size field isn't important for relative const (since
	 * we don't have to do register allocation for constants).
	 */
	unsigned size : 16;

	/* normal registers:
	 * the component is in the low two bits of the reg #, so
	 * rN.x becomes: (N << 2) | x
	 */
	uint16_t num;
	uint16_t name;
	union {
		/* immediate: */
		int32_t  iim_val;
		uint32_t uim_val;
		float    fim_val;
		/* relative: */
		struct {
			uint16_t id;
			int16_t offset;
			uint16_t base;
		} array;
	};


	/* For IR3_REG_DEST, pointer back to the instruction containing this
	 * register.
	 */
	struct ir3_instruction *instr;

	/* For IR3_REG_SSA, src registers contain ptr back to assigning
	 * instruction.
	 *
	 * For IR3_REG_ARRAY, the pointer is back to the last dependent
	 * array access (although the net effect is the same, it points
	 * back to a previous instruction that we depend on).
	 */
	struct ir3_register *def;

	/* Pointer to another register in the instruction that must share the same
	 * physical register. Each destination can be tied with one source, and
	 * they must have "tied" pointing to each other.
	 */
	struct ir3_register *tied;

	unsigned merge_set_offset;
	struct ir3_merge_set *merge_set;
	unsigned interval_start, interval_end;
};

/*
 * Stupid/simple growable array implementation:
 */
#define DECLARE_ARRAY(type, name) \
	unsigned name ## _count, name ## _sz; \
	type * name;

#define array_insert(ctx, arr, ...) do { \
		if (arr ## _count == arr ## _sz) { \
			arr ## _sz = MAX2(2 * arr ## _sz, 16); \
			arr = reralloc_size(ctx, arr, arr ## _sz * sizeof(arr[0])); \
		} \
		arr[arr ##_count++] = __VA_ARGS__; \
	} while (0)

struct ir3_instruction {
	struct ir3_block *block;
	opc_t opc;
	enum {
		/* (sy) flag is set on first instruction, and after sample
		 * instructions (probably just on RAW hazard).
		 */
		IR3_INSTR_SY    = 0x001,
		/* (ss) flag is set on first instruction, and first instruction
		 * to depend on the result of "long" instructions (RAW hazard):
		 *
		 *   rcp, rsq, log2, exp2, sin, cos, sqrt
		 *
		 * It seems to synchronize until all in-flight instructions are
		 * completed, for example:
		 *
		 *   rsq hr1.w, hr1.w
		 *   add.f hr2.z, (neg)hr2.z, hc0.y
		 *   mul.f hr2.w, (neg)hr2.y, (neg)hr2.y
		 *   rsq hr2.x, hr2.x
		 *   (rpt1)nop
		 *   mad.f16 hr2.w, hr2.z, hr2.z, hr2.w
		 *   nop
		 *   mad.f16 hr2.w, (neg)hr0.w, (neg)hr0.w, hr2.w
		 *   (ss)(rpt2)mul.f hr1.x, (r)hr1.x, hr1.w
		 *   (rpt2)mul.f hr0.x, (neg)(r)hr0.x, hr2.x
		 *
		 * The last mul.f does not have (ss) set, presumably because the
		 * (ss) on the previous instruction does the job.
		 *
		 * The blob driver also seems to set it on WAR hazards, although
		 * not really clear if this is needed or just blob compiler being
		 * sloppy.  So far I haven't found a case where removing the (ss)
		 * causes problems for WAR hazard, but I could just be getting
		 * lucky:
		 *
		 *   rcp r1.y, r3.y
		 *   (ss)(rpt2)mad.f32 r3.y, (r)c9.x, r1.x, (r)r3.z
		 *
		 */
		IR3_INSTR_SS    = 0x002,
		/* (jp) flag is set on jump targets:
		 */
		IR3_INSTR_JP    = 0x004,
		IR3_INSTR_UL    = 0x008,
		IR3_INSTR_3D    = 0x010,
		IR3_INSTR_A     = 0x020,
		IR3_INSTR_O     = 0x040,
		IR3_INSTR_P     = 0x080,
		IR3_INSTR_S     = 0x100,
		IR3_INSTR_S2EN  = 0x200,
		IR3_INSTR_G     = 0x400,
		IR3_INSTR_SAT   = 0x800,
		/* (cat5/cat6) Bindless */
		IR3_INSTR_B     = 0x1000,
		/* (cat5/cat6) nonuniform */
		IR3_INSTR_NONUNIF    = 0x02000,
		/* (cat5-only) Get some parts of the encoding from a1.x */
		IR3_INSTR_A1EN       = 0x04000,
		/* meta-flags, for intermediate stages of IR, ie.
		 * before register assignment is done:
		 */
		IR3_INSTR_MARK       = 0x08000,
		IR3_INSTR_UNUSED     = 0x10000,
	} flags;
	uint8_t repeat;
	uint8_t nop;
#ifdef DEBUG
	unsigned srcs_max, dsts_max;
#endif
	unsigned srcs_count, dsts_count;
	struct ir3_register **dsts;
	struct ir3_register **srcs;
	union {
		struct {
			char inv1, inv2;
			char comp1, comp2;
			int  immed;
			struct ir3_block *target;
			const char *target_label;
			brtype_t brtype;
			unsigned idx;  /* for brac.N */
		} cat0;
		struct {
			type_t src_type, dst_type;
			round_t round;
		} cat1;
		struct {
			enum {
				IR3_COND_LT = 0,
				IR3_COND_LE = 1,
				IR3_COND_GT = 2,
				IR3_COND_GE = 3,
				IR3_COND_EQ = 4,
				IR3_COND_NE = 5,
			} condition;
		} cat2;
		struct {
			unsigned samp, tex;
			unsigned tex_base : 3;
			type_t type;
		} cat5;
		struct {
			type_t type;
			/* TODO remove dst_offset and handle as a ir3_register
			 * which might be IMMED, similar to how src_offset is
			 * handled.
			 */
			int dst_offset;
			int iim_val : 3;      /* for ldgb/stgb, # of components */
			unsigned d : 3;       /* for ldc, component offset */
			bool typed : 1;
			unsigned base : 3;
		} cat6;
		struct {
			unsigned w : 1;       /* write */
			unsigned r : 1;       /* read */
			unsigned l : 1;       /* local */
			unsigned g : 1;       /* global */
		} cat7;
		/* for meta-instructions, just used to hold extra data
		 * before instruction scheduling, etc
		 */
		struct {
			int off;              /* component/offset */
		} split;
		struct {
			/* Per-source index back to the entry in the
			 * ir3_shader_variant::outputs table.
			 */
			unsigned *outidxs;
		} end;
		struct {
			/* used to temporarily hold reference to nir_phi_instr
			 * until we resolve the phi srcs
			 */
			void *nphi;
		} phi;
		struct {
			unsigned samp, tex;
			unsigned input_offset;
			unsigned samp_base : 3;
			unsigned tex_base : 3;
		} prefetch;
		struct {
			/* maps back to entry in ir3_shader_variant::inputs table: */
			int inidx;
			/* for sysvals, identifies the sysval type.  Mostly so we can
			 * identify the special cases where a sysval should not be DCE'd
			 * (currently, just pre-fs texture fetch)
			 */
			gl_system_value sysval;
		} input;
	};

	/* When we get to the RA stage, we need instruction's position/name: */
	uint16_t ip;
	uint16_t name;

	/* used for per-pass extra instruction data.
	 *
	 * TODO we should remove the per-pass data like this and 'use_count'
	 * and do something similar to what RA does w/ ir3_ra_instr_data..
	 * ie. use the ir3_count_instructions pass, and then use instr->ip
	 * to index into a table of pass-private data.
	 */
	void *data;

	/**
	 * Valid if pass calls ir3_find_ssa_uses().. see foreach_ssa_use()
	 */
	struct set *uses;

	int use_count;      /* currently just updated/used by cp */

	/* an instruction can reference at most one address register amongst
	 * it's src/dst registers.  Beyond that, you need to insert mov's.
	 *
	 * NOTE: do not write this directly, use ir3_instr_set_address()
	 */
	struct ir3_register *address;

	/* Tracking for additional dependent instructions.  Used to handle
	 * barriers, WAR hazards for arrays/SSBOs/etc.
	 */
	DECLARE_ARRAY(struct ir3_instruction *, deps);

	/*
	 * From PoV of instruction scheduling, not execution (ie. ignores global/
	 * local distinction):
	 *                            shared  image  atomic  SSBO  everything
	 *   barrier()/            -   R/W     R/W    R/W     R/W       X
	 *     groupMemoryBarrier()
	 *     memoryBarrier()
	 *     (but only images declared coherent?)
	 *   memoryBarrierAtomic() -                  R/W
	 *   memoryBarrierBuffer() -                          R/W
	 *   memoryBarrierImage()  -           R/W
	 *   memoryBarrierShared() -   R/W
	 *
	 * TODO I think for SSBO/image/shared, in cases where we can determine
	 * which variable is accessed, we don't need to care about accesses to
	 * different variables (unless declared coherent??)
	 */
	enum {
		IR3_BARRIER_EVERYTHING = 1 << 0,
		IR3_BARRIER_SHARED_R   = 1 << 1,
		IR3_BARRIER_SHARED_W   = 1 << 2,
		IR3_BARRIER_IMAGE_R    = 1 << 3,
		IR3_BARRIER_IMAGE_W    = 1 << 4,
		IR3_BARRIER_BUFFER_R   = 1 << 5,
		IR3_BARRIER_BUFFER_W   = 1 << 6,
		IR3_BARRIER_ARRAY_R    = 1 << 7,
		IR3_BARRIER_ARRAY_W    = 1 << 8,
		IR3_BARRIER_PRIVATE_R  = 1 << 9,
		IR3_BARRIER_PRIVATE_W  = 1 << 10,
	} barrier_class, barrier_conflict;

	/* Entry in ir3_block's instruction list: */
	struct list_head node;

	uint32_t serialno;

	// TODO only computerator/assembler:
	int line;
};

struct ir3 {
	struct ir3_compiler *compiler;
	gl_shader_stage type;

	DECLARE_ARRAY(struct ir3_instruction *, inputs);

	/* Track bary.f (and ldlv) instructions.. this is needed in
	 * scheduling to ensure that all varying fetches happen before
	 * any potential kill instructions.  The hw gets grumpy if all
	 * threads in a group are killed before the last bary.f gets
	 * a chance to signal end of input (ei).
	 */
	DECLARE_ARRAY(struct ir3_instruction *, baryfs);

	/* Track all indirect instructions (read and write).  To avoid
	 * deadlock scenario where an address register gets scheduled,
	 * but other dependent src instructions cannot be scheduled due
	 * to dependency on a *different* address register value, the
	 * scheduler needs to ensure that all dependencies other than
	 * the instruction other than the address register are scheduled
	 * before the one that writes the address register.  Having a
	 * convenient list of instructions that reference some address
	 * register simplifies this.
	 */
	DECLARE_ARRAY(struct ir3_instruction *, a0_users);

	/* same for a1.x: */
	DECLARE_ARRAY(struct ir3_instruction *, a1_users);

	/* and same for instructions that consume predicate register: */
	DECLARE_ARRAY(struct ir3_instruction *, predicates);

	/* Track texture sample instructions which need texture state
	 * patched in (for astc-srgb workaround):
	 */
	DECLARE_ARRAY(struct ir3_instruction *, astc_srgb);

	/* List of blocks: */
	struct list_head block_list;

	/* List of ir3_array's: */
	struct list_head array_list;

#ifdef DEBUG
	unsigned block_count;
#endif
	unsigned instr_count;
};

struct ir3_array {
	struct list_head node;
	unsigned length;
	unsigned id;

	struct nir_register *r;

	/* To avoid array write's from getting DCE'd, keep track of the
	 * most recent write.  Any array access depends on the most
	 * recent write.  This way, nothing depends on writes after the
	 * last read.  But all the writes that happen before that have
	 * something depending on them
	 */
	struct ir3_register *last_write;

	/* extra stuff used in RA pass: */
	unsigned base;      /* base vreg name */
	unsigned reg;       /* base physical reg */
	uint16_t start_ip, end_ip;

	/* Indicates if half-precision */
	bool half;

	bool unused;
};

struct ir3_array * ir3_lookup_array(struct ir3 *ir, unsigned id);

struct ir3_block {
	struct list_head node;
	struct ir3 *shader;

	const struct nir_block *nblock;

	struct list_head instr_list;  /* list of ir3_instruction */

	/* each block has either one or two successors.. in case of
	 * two successors, 'condition' decides which one to follow.
	 * A block preceding an if/else has two successors.
	 */
	struct ir3_instruction *condition;
	struct ir3_block *successors[2];

	DECLARE_ARRAY(struct ir3_block *, predecessors);

	uint16_t start_ip, end_ip;

	/* Track instructions which do not write a register but other-
	 * wise must not be discarded (such as kill, stg, etc)
	 */
	DECLARE_ARRAY(struct ir3_instruction *, keeps);

	/* used for per-pass extra block data.  Mainly used right
	 * now in RA step to track livein/liveout.
	 */
	void *data;

	uint32_t index;

	struct ir3_block *imm_dom;
	DECLARE_ARRAY(struct ir3_block *, dom_children);

	uint32_t dom_pre_index;
	uint32_t dom_post_index;

#ifdef DEBUG
	uint32_t serialno;
#endif
};

static inline uint32_t
block_id(struct ir3_block *block)
{
#ifdef DEBUG
	return block->serialno;
#else
	return (uint32_t)(unsigned long)block;
#endif
}

static inline struct ir3_block *
ir3_start_block(struct ir3 *ir)
{
	return list_first_entry(&ir->block_list, struct ir3_block, node);
}

void ir3_block_add_predecessor(struct ir3_block *block, struct ir3_block *pred);
void ir3_block_remove_predecessor(struct ir3_block *block, struct ir3_block *pred);
unsigned ir3_block_get_pred_index(struct ir3_block *block, struct ir3_block *pred);

void ir3_calc_dominance(struct ir3 *ir);
bool ir3_block_dominates(struct ir3_block *a, struct ir3_block *b);

struct ir3_shader_variant;

struct ir3 * ir3_create(struct ir3_compiler *compiler, struct ir3_shader_variant *v);
void ir3_destroy(struct ir3 *shader);

void ir3_collect_info(struct ir3_shader_variant *v);
void * ir3_alloc(struct ir3 *shader, int sz);

unsigned ir3_get_reg_dependent_max_waves(const struct ir3_compiler *compiler,
										 unsigned reg_count, bool double_threadsize);

unsigned ir3_get_reg_independent_max_waves(struct ir3_shader_variant *v,
										   bool double_threadsize);

bool ir3_should_double_threadsize(struct ir3_shader_variant *v,
								  unsigned regs_count);

struct ir3_block * ir3_block_create(struct ir3 *shader);

struct ir3_instruction * ir3_instr_create(struct ir3_block *block,
		opc_t opc, int ndst, int nsrc);
struct ir3_instruction * ir3_instr_clone(struct ir3_instruction *instr);
void ir3_instr_add_dep(struct ir3_instruction *instr, struct ir3_instruction *dep);
const char *ir3_instr_name(struct ir3_instruction *instr);

struct ir3_register * ir3_src_create(struct ir3_instruction *instr,
		int num, int flags);
struct ir3_register * ir3_dst_create(struct ir3_instruction *instr,
		int num, int flags);
struct ir3_register * ir3_reg_clone(struct ir3 *shader,
		struct ir3_register *reg);

static inline void ir3_reg_tie(struct ir3_register *dst, struct ir3_register *src)
{
	assert(!dst->tied && !src->tied);
	dst->tied = src;
	src->tied = dst;
}

void ir3_reg_set_last_array(struct ir3_instruction *instr,
							struct ir3_register *reg,
							struct ir3_register *last_write);

void ir3_instr_set_address(struct ir3_instruction *instr,
		struct ir3_instruction *addr);

static inline bool ir3_instr_check_mark(struct ir3_instruction *instr)
{
	if (instr->flags & IR3_INSTR_MARK)
		return true;  /* already visited */
	instr->flags |= IR3_INSTR_MARK;
	return false;
}

void ir3_block_clear_mark(struct ir3_block *block);
void ir3_clear_mark(struct ir3 *shader);

unsigned ir3_count_instructions(struct ir3 *ir);
unsigned ir3_count_instructions_ra(struct ir3 *ir);

/**
 * Move 'instr' to just before 'after'
 */
static inline void
ir3_instr_move_before(struct ir3_instruction *instr,
		struct ir3_instruction *after)
{
	list_delinit(&instr->node);
	list_addtail(&instr->node, &after->node);
}

/**
 * Move 'instr' to just after 'before':
 */
static inline void
ir3_instr_move_after(struct ir3_instruction *instr,
		struct ir3_instruction *before)
{
	list_delinit(&instr->node);
	list_add(&instr->node, &before->node);
}

void ir3_find_ssa_uses(struct ir3 *ir, void *mem_ctx, bool falsedeps);

void ir3_set_dst_type(struct ir3_instruction *instr, bool half);
void ir3_fixup_src_type(struct ir3_instruction *instr);

bool ir3_valid_flags(struct ir3_instruction *instr, unsigned n, unsigned flags);

#include "util/set.h"
#define foreach_ssa_use(__use, __instr) \
	for (struct ir3_instruction *__use = (void *)~0; \
	     __use && (__instr)->uses; __use = NULL) \
		set_foreach ((__instr)->uses, __entry) \
			if ((__use = (void *)__entry->key))

static inline uint32_t reg_num(const struct ir3_register *reg)
{
	return reg->num >> 2;
}

static inline uint32_t reg_comp(const struct ir3_register *reg)
{
	return reg->num & 0x3;
}

static inline bool is_flow(struct ir3_instruction *instr)
{
	return (opc_cat(instr->opc) == 0);
}

static inline bool is_kill_or_demote(struct ir3_instruction *instr)
{
	return instr->opc == OPC_KILL || instr->opc == OPC_DEMOTE;
}

static inline bool is_nop(struct ir3_instruction *instr)
{
	return instr->opc == OPC_NOP;
}

static inline bool is_same_type_reg(struct ir3_register *reg1,
		struct ir3_register *reg2)
{
	unsigned type_reg1 = (reg1->flags & (IR3_REG_SHARED | IR3_REG_HALF));
	unsigned type_reg2 = (reg2->flags & (IR3_REG_SHARED | IR3_REG_HALF));

	if (type_reg1 ^ type_reg2)
		return false;
	else
		return true;
}

/* Is it a non-transformative (ie. not type changing) mov?  This can
 * also include absneg.s/absneg.f, which for the most part can be
 * treated as a mov (single src argument).
 */
static inline bool is_same_type_mov(struct ir3_instruction *instr)
{
	struct ir3_register *dst;

	switch (instr->opc) {
	case OPC_MOV:
		if (instr->cat1.src_type != instr->cat1.dst_type)
			return false;
		/* If the type of dest reg and src reg are different,
		 * it shouldn't be considered as same type mov
		 */
		if (!is_same_type_reg(instr->dsts[0], instr->srcs[0]))
			return false;
		break;
	case OPC_ABSNEG_F:
	case OPC_ABSNEG_S:
		if (instr->flags & IR3_INSTR_SAT)
			return false;
		/* If the type of dest reg and src reg are different,
		 * it shouldn't be considered as same type mov
		 */
		if (!is_same_type_reg(instr->dsts[0], instr->srcs[0]))
			return false;
		break;
	default:
		return false;
	}

	dst = instr->dsts[0];

	/* mov's that write to a0 or p0.x are special: */
	if (dst->num == regid(REG_P0, 0))
		return false;
	if (reg_num(dst) == REG_A0)
		return false;

	if (dst->flags & (IR3_REG_RELATIV | IR3_REG_ARRAY))
		return false;

	return true;
}

/* A move from const, which changes size but not type, can also be
 * folded into dest instruction in some cases.
 */
static inline bool is_const_mov(struct ir3_instruction *instr)
{
	if (instr->opc != OPC_MOV)
		return false;

	if (!(instr->srcs[0]->flags & IR3_REG_CONST))
		return false;

	type_t src_type = instr->cat1.src_type;
	type_t dst_type = instr->cat1.dst_type;

	return (type_float(src_type) && type_float(dst_type)) ||
		(type_uint(src_type) && type_uint(dst_type)) ||
		(type_sint(src_type) && type_sint(dst_type));
}

static inline bool is_alu(struct ir3_instruction *instr)
{
	return (1 <= opc_cat(instr->opc)) && (opc_cat(instr->opc) <= 3);
}

static inline bool is_sfu(struct ir3_instruction *instr)
{
	return (opc_cat(instr->opc) == 4);
}

static inline bool is_tex(struct ir3_instruction *instr)
{
	return (opc_cat(instr->opc) == 5);
}

static inline bool is_tex_or_prefetch(struct ir3_instruction *instr)
{
	return is_tex(instr) || (instr->opc == OPC_META_TEX_PREFETCH);
}

static inline bool is_mem(struct ir3_instruction *instr)
{
	return (opc_cat(instr->opc) == 6);
}

static inline bool is_barrier(struct ir3_instruction *instr)
{
	return (opc_cat(instr->opc) == 7);
}

static inline bool
is_half(struct ir3_instruction *instr)
{
	return !!(instr->dsts[0]->flags & IR3_REG_HALF);
}

static inline bool
is_shared(struct ir3_instruction *instr)
{
	return !!(instr->dsts[0]->flags & IR3_REG_SHARED);
}

static inline bool
is_store(struct ir3_instruction *instr)
{
	/* these instructions, the "destination" register is
	 * actually a source, the address to store to.
	 */
	switch (instr->opc) {
	case OPC_STG:
	case OPC_STG_A:
	case OPC_STGB:
	case OPC_STIB:
	case OPC_STP:
	case OPC_STL:
	case OPC_STLW:
	case OPC_L2G:
	case OPC_G2L:
		return true;
	default:
		return false;
	}
}

static inline bool is_load(struct ir3_instruction *instr)
{
	switch (instr->opc) {
	case OPC_LDG:
	case OPC_LDG_A:
	case OPC_LDGB:
	case OPC_LDIB:
	case OPC_LDL:
	case OPC_LDP:
	case OPC_L2G:
	case OPC_LDLW:
	case OPC_LDC:
	case OPC_LDLV:
		/* probably some others too.. */
		return true;
	default:
		return false;
	}
}

static inline bool is_input(struct ir3_instruction *instr)
{
	/* in some cases, ldlv is used to fetch varying without
	 * interpolation.. fortunately inloc is the first src
	 * register in either case
	 */
	switch (instr->opc) {
	case OPC_LDLV:
	case OPC_BARY_F:
		return true;
	default:
		return false;
	}
}

static inline bool is_bool(struct ir3_instruction *instr)
{
	switch (instr->opc) {
	case OPC_CMPS_F:
	case OPC_CMPS_S:
	case OPC_CMPS_U:
		return true;
	default:
		return false;
	}
}

static inline opc_t
cat3_half_opc(opc_t opc)
{
	switch (opc) {
	case OPC_MAD_F32: return OPC_MAD_F16;
	case OPC_SEL_B32: return OPC_SEL_B16;
	case OPC_SEL_S32: return OPC_SEL_S16;
	case OPC_SEL_F32: return OPC_SEL_F16;
	case OPC_SAD_S32: return OPC_SAD_S16;
	default:          return opc;
	}
}

static inline opc_t
cat3_full_opc(opc_t opc)
{
	switch (opc) {
	case OPC_MAD_F16: return OPC_MAD_F32;
	case OPC_SEL_B16: return OPC_SEL_B32;
	case OPC_SEL_S16: return OPC_SEL_S32;
	case OPC_SEL_F16: return OPC_SEL_F32;
	case OPC_SAD_S16: return OPC_SAD_S32;
	default:          return opc;
	}
}

static inline opc_t
cat4_half_opc(opc_t opc)
{
	switch (opc) {
	case OPC_RSQ:  return OPC_HRSQ;
	case OPC_LOG2: return OPC_HLOG2;
	case OPC_EXP2: return OPC_HEXP2;
	default:       return opc;
	}
}

static inline opc_t
cat4_full_opc(opc_t opc)
{
	switch (opc) {
	case OPC_HRSQ:  return OPC_RSQ;
	case OPC_HLOG2: return OPC_LOG2;
	case OPC_HEXP2: return OPC_EXP2;
	default:        return opc;
	}
}

static inline bool is_meta(struct ir3_instruction *instr)
{
	return (opc_cat(instr->opc) == -1);
}

static inline unsigned reg_elems(const struct ir3_register *reg)
{
	if (reg->flags & IR3_REG_ARRAY)
		return reg->size;
	else
		return util_last_bit(reg->wrmask);
}

static inline unsigned
reg_elem_size(const struct ir3_register *reg)
{
	return (reg->flags & IR3_REG_HALF) ? 1 : 2;
}

static inline unsigned
reg_size(const struct ir3_register *reg)
{
	return reg_elems(reg) * reg_elem_size(reg);
}

static inline unsigned dest_regs(struct ir3_instruction *instr)
{
	if ((instr->dsts_count == 0) || is_store(instr) || is_flow(instr))
		return 0;

	return util_last_bit(instr->dsts[0]->wrmask);
}

static inline bool
writes_gpr(struct ir3_instruction *instr)
{
	if (dest_regs(instr) == 0)
		return false;
	/* is dest a normal temp register: */
	struct ir3_register *reg = instr->dsts[0];
	debug_assert(!(reg->flags & (IR3_REG_CONST | IR3_REG_IMMED)));
	if ((reg_num(reg) == REG_A0) ||
			(reg->num == regid(REG_P0, 0)))
		return false;
	return true;
}

static inline bool writes_addr0(struct ir3_instruction *instr)
{
	if (instr->dsts_count > 0) {
		struct ir3_register *dst = instr->dsts[0];
		return dst->num == regid(REG_A0, 0);
	}
	return false;
}

static inline bool writes_addr1(struct ir3_instruction *instr)
{
	if (instr->dsts_count > 0) {
		struct ir3_register *dst = instr->dsts[0];
		return dst->num == regid(REG_A0, 1);
	}
	return false;
}

static inline bool writes_pred(struct ir3_instruction *instr)
{
	if (instr->dsts_count > 0) {
		struct ir3_register *dst = instr->dsts[0];
		return reg_num(dst) == REG_P0;
	}
	return false;
}

/* Is it something other than a normal register. Shared regs, p0, and a0/a1
 * are considered special here. Special registers are always accessed with one
 * size and never alias normal registers, even though a naive calculation
 * would sometimes make it seem like e.g. r30.z aliases a0.x.
 */
static inline bool is_reg_special(const struct ir3_register *reg)
{
	return (reg->flags & IR3_REG_SHARED) ||
		(reg_num(reg) == REG_A0) || (reg_num(reg) == REG_P0);
}

/* returns defining instruction for reg */
/* TODO better name */
static inline struct ir3_instruction *ssa(struct ir3_register *reg)
{
	if ((reg->flags & (IR3_REG_SSA | IR3_REG_ARRAY)) && reg->def)
		return reg->def->instr;
	return NULL;
}

static inline bool conflicts(struct ir3_register *a,
		struct ir3_register *b)
{
	return (a && b) && (a->def != b->def);
}

static inline bool reg_gpr(struct ir3_register *r)
{
	if (r->flags & (IR3_REG_CONST | IR3_REG_IMMED))
		return false;
	if ((reg_num(r) == REG_A0) || (reg_num(r) == REG_P0))
		return false;
	return true;
}

static inline type_t half_type(type_t type)
{
	switch (type) {
	case TYPE_F32: return TYPE_F16;
	case TYPE_U32: return TYPE_U16;
	case TYPE_S32: return TYPE_S16;
	case TYPE_F16:
	case TYPE_U16:
	case TYPE_S16:
		return type;
	default:
		assert(0);
		return ~0;
	}
}

static inline type_t full_type(type_t type)
{
	switch (type) {
	case TYPE_F16: return TYPE_F32;
	case TYPE_U16: return TYPE_U32;
	case TYPE_S16: return TYPE_S32;
	case TYPE_F32:
	case TYPE_U32:
	case TYPE_S32:
		return type;
	default:
		assert(0);
		return ~0;
	}
}

/* some cat2 instructions (ie. those which are not float) can embed an
 * immediate:
 */
static inline bool ir3_cat2_int(opc_t opc)
{
	switch (opc) {
	case OPC_ADD_U:
	case OPC_ADD_S:
	case OPC_SUB_U:
	case OPC_SUB_S:
	case OPC_CMPS_U:
	case OPC_CMPS_S:
	case OPC_MIN_U:
	case OPC_MIN_S:
	case OPC_MAX_U:
	case OPC_MAX_S:
	case OPC_CMPV_U:
	case OPC_CMPV_S:
	case OPC_MUL_U24:
	case OPC_MUL_S24:
	case OPC_MULL_U:
	case OPC_CLZ_S:
	case OPC_ABSNEG_S:
	case OPC_AND_B:
	case OPC_OR_B:
	case OPC_NOT_B:
	case OPC_XOR_B:
	case OPC_BFREV_B:
	case OPC_CLZ_B:
	case OPC_SHL_B:
	case OPC_SHR_B:
	case OPC_ASHR_B:
	case OPC_MGEN_B:
	case OPC_GETBIT_B:
	case OPC_CBITS_B:
	case OPC_BARY_F:
		return true;

	default:
		return false;
	}
}

/* map cat2 instruction to valid abs/neg flags: */
static inline unsigned ir3_cat2_absneg(opc_t opc)
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
	case OPC_BARY_F:
		return IR3_REG_FABS | IR3_REG_FNEG;

	case OPC_ADD_U:
	case OPC_ADD_S:
	case OPC_SUB_U:
	case OPC_SUB_S:
	case OPC_CMPS_U:
	case OPC_CMPS_S:
	case OPC_MIN_U:
	case OPC_MIN_S:
	case OPC_MAX_U:
	case OPC_MAX_S:
	case OPC_CMPV_U:
	case OPC_CMPV_S:
	case OPC_MUL_U24:
	case OPC_MUL_S24:
	case OPC_MULL_U:
	case OPC_CLZ_S:
		return 0;

	case OPC_ABSNEG_S:
		return IR3_REG_SABS | IR3_REG_SNEG;

	case OPC_AND_B:
	case OPC_OR_B:
	case OPC_NOT_B:
	case OPC_XOR_B:
	case OPC_BFREV_B:
	case OPC_CLZ_B:
	case OPC_SHL_B:
	case OPC_SHR_B:
	case OPC_ASHR_B:
	case OPC_MGEN_B:
	case OPC_GETBIT_B:
	case OPC_CBITS_B:
		return IR3_REG_BNOT;

	default:
		return 0;
	}
}

/* map cat3 instructions to valid abs/neg flags: */
static inline unsigned ir3_cat3_absneg(opc_t opc)
{
	switch (opc) {
	case OPC_MAD_F16:
	case OPC_MAD_F32:
	case OPC_SEL_F16:
	case OPC_SEL_F32:
		return IR3_REG_FNEG;

	case OPC_MAD_U16:
	case OPC_MADSH_U16:
	case OPC_MAD_S16:
	case OPC_MADSH_M16:
	case OPC_MAD_U24:
	case OPC_MAD_S24:
	case OPC_SEL_S16:
	case OPC_SEL_S32:
	case OPC_SAD_S16:
	case OPC_SAD_S32:
		/* neg *may* work on 3rd src.. */

	case OPC_SEL_B16:
	case OPC_SEL_B32:

	default:
		return 0;
	}
}

/* Return the type (float, int, or uint) the op uses when converting from the
 * internal result of the op (which is assumed to be the same size as the
 * sources) to the destination when they are not the same size. If F32 it does
 * a floating-point conversion, if U32 it does a truncation/zero-extension, if
 * S32 it does a truncation/sign-extension. "can_fold" will be false if it
 * doesn't do anything sensible or is unknown.
 */
static inline type_t
ir3_output_conv_type(struct ir3_instruction *instr, bool *can_fold)
{
	*can_fold = true;
	switch (instr->opc) {
	case OPC_ADD_F:
	case OPC_MUL_F:
	case OPC_BARY_F:
	case OPC_MAD_F32:
	case OPC_MAD_F16:
		return TYPE_F32;

	case OPC_ADD_U:
	case OPC_SUB_U:
	case OPC_MIN_U:
	case OPC_MAX_U:
	case OPC_AND_B:
	case OPC_OR_B:
	case OPC_NOT_B:
	case OPC_XOR_B:
	case OPC_MUL_U24:
	case OPC_MULL_U:
	case OPC_SHL_B:
	case OPC_SHR_B:
	case OPC_ASHR_B:
	case OPC_MAD_U24:
	/* Comparison ops zero-extend/truncate their results, so consider them as
	 * unsigned here.
	 */
	case OPC_CMPS_F:
	case OPC_CMPV_F:
	case OPC_CMPS_U:
	case OPC_CMPS_S:
		return TYPE_U32;

	case OPC_ADD_S:
	case OPC_SUB_S:
	case OPC_MIN_S:
	case OPC_MAX_S:
	case OPC_ABSNEG_S:
	case OPC_MUL_S24:
	case OPC_MAD_S24:
		return TYPE_S32;

	/* We assume that any move->move folding that could be done was done by
	 * NIR.
	 */
	case OPC_MOV:
	default:
		*can_fold = false;
		return TYPE_U32;
	}
}

/* Return the src and dst types for the conversion which is already folded
 * into the op. We can assume that instr has folded in a conversion from
 * ir3_output_conv_src_type() to ir3_output_conv_dst_type(). Only makes sense
 * to call if ir3_output_conv_type() returns can_fold = true.
 */
static inline type_t
ir3_output_conv_src_type(struct ir3_instruction *instr, type_t base_type)
{
	switch (instr->opc) {
	case OPC_CMPS_F:
	case OPC_CMPV_F:
	case OPC_CMPS_U:
	case OPC_CMPS_S:
		/* Comparisons only return 0/1 and the size of the comparison sources
		 * is irrelevant, never consider them as having an output conversion
		 * by returning a type with the dest size here:
		 */
		return (instr->dsts[0]->flags & IR3_REG_HALF) ? half_type(base_type) :
			full_type(base_type);

	case OPC_BARY_F:
		/* bary.f doesn't have an explicit source, but we can assume here that
		 * the varying data it reads is in fp32.
		 *
		 * This may be fp16 on older gen's depending on some register
		 * settings, but it's probably not worth plumbing that through for a
		 * small improvement that NIR would hopefully handle for us anyway.
		 */
		return TYPE_F32;

	default:
		return (instr->dsts[1]->flags & IR3_REG_HALF) ? half_type(base_type) :
			full_type(base_type);
	}
}

static inline type_t
ir3_output_conv_dst_type(struct ir3_instruction *instr, type_t base_type)
{
	return (instr->dsts[0]->flags & IR3_REG_HALF) ? half_type(base_type) :
		full_type(base_type);
}

/* Some instructions have signed/unsigned variants which are identical except
 * for whether the folded conversion sign-extends or zero-extends, and we can
 * fold in a mismatching move by rewriting the opcode. Return the opcode to
 * switch signedness, and whether one exists.
 */
static inline opc_t
ir3_try_swap_signedness(opc_t opc, bool *can_swap)
{
	switch (opc) {
#define PAIR(u, s)		\
	case OPC_##u:		\
		return OPC_##s;	\
	case OPC_##s:		\
		return OPC_##u;
	PAIR(ADD_U, ADD_S)
	PAIR(SUB_U, SUB_S)
	/* Note: these are only identical when the sources are half, but that's
	 * the only case we call this function for anyway.
	 */
	PAIR(MUL_U24, MUL_S24)

	default:
		*can_swap = false;
		return opc;
	}
}

#define MASK(n) ((1 << (n)) - 1)

/* iterator for an instructions's sources (reg), also returns src #: */
#define foreach_src_n(__srcreg, __n, __instr) \
	if ((__instr)->srcs_count) \
		for (struct ir3_register *__srcreg = (void *)~0; __srcreg; __srcreg = NULL) \
			for (unsigned __cnt = (__instr)->srcs_count, __n = 0; __n < __cnt; __n++) \
				if ((__srcreg = (__instr)->srcs[__n]))

/* iterator for an instructions's sources (reg): */
#define foreach_src(__srcreg, __instr) \
	foreach_src_n(__srcreg, __i, __instr)

static inline unsigned __ssa_src_cnt(struct ir3_instruction *instr)
{
	return instr->srcs_count + instr->deps_count;
}

static inline bool __is_false_dep(struct ir3_instruction *instr, unsigned n)
{
	if (n >= instr->srcs_count)
		return true;
	return false;
}

static inline struct ir3_instruction **
__ssa_srcp_n(struct ir3_instruction *instr, unsigned n)
{
	if (__is_false_dep(instr, n))
		return &instr->deps[n - instr->srcs_count];
	if (ssa(instr->srcs[n]))
		return &instr->srcs[n]->def->instr;
	return NULL;
}

#define foreach_ssa_srcp_n(__srcp, __n, __instr) \
	for (struct ir3_instruction **__srcp = (void *)~0; __srcp; __srcp = NULL) \
		for (unsigned __cnt = __ssa_src_cnt(__instr), __n = 0; __n < __cnt; __n++) \
			if ((__srcp = __ssa_srcp_n(__instr, __n)))

#define foreach_ssa_srcp(__srcp, __instr) \
	foreach_ssa_srcp_n(__srcp, __i, __instr)

/* iterator for an instruction's SSA sources (instr), also returns src #: */
#define foreach_ssa_src_n(__srcinst, __n, __instr) \
	for (struct ir3_instruction *__srcinst = (void *)~0; __srcinst; __srcinst = NULL) \
		foreach_ssa_srcp_n(__srcp, __n, __instr) \
			if ((__srcinst = *__srcp))

/* iterator for an instruction's SSA sources (instr): */
#define foreach_ssa_src(__srcinst, __instr) \
	foreach_ssa_src_n(__srcinst, __i, __instr)

/* iterators for shader inputs: */
#define foreach_input_n(__ininstr, __cnt, __ir) \
	for (struct ir3_instruction *__ininstr = (void *)~0; __ininstr; __ininstr = NULL) \
		for (unsigned __cnt = 0; __cnt < (__ir)->inputs_count; __cnt++) \
			if ((__ininstr = (__ir)->inputs[__cnt]))
#define foreach_input(__ininstr, __ir) \
	foreach_input_n(__ininstr, __i, __ir)

/* iterators for instructions: */
#define foreach_instr(__instr, __list) \
	list_for_each_entry(struct ir3_instruction, __instr, __list, node)
#define foreach_instr_rev(__instr, __list) \
	list_for_each_entry_rev(struct ir3_instruction, __instr, __list, node)
#define foreach_instr_safe(__instr, __list) \
	list_for_each_entry_safe(struct ir3_instruction, __instr, __list, node)

/* iterators for blocks: */
#define foreach_block(__block, __list) \
	list_for_each_entry(struct ir3_block, __block, __list, node)
#define foreach_block_safe(__block, __list) \
	list_for_each_entry_safe(struct ir3_block, __block, __list, node)
#define foreach_block_rev(__block, __list) \
	list_for_each_entry_rev(struct ir3_block, __block, __list, node)

/* iterators for arrays: */
#define foreach_array(__array, __list) \
	list_for_each_entry(struct ir3_array, __array, __list, node)
#define foreach_array_safe(__array, __list) \
	list_for_each_entry_safe(struct ir3_array, __array, __list, node)

#define IR3_PASS(ir, pass, ...) ({ \
		bool progress = pass(ir, ##__VA_ARGS__); \
		if (progress) { \
			ir3_debug_print(ir, "AFTER: " #pass); \
			ir3_validate(ir); \
		} \
		progress; \
	})

/* validate: */
void ir3_validate(struct ir3 *ir);

/* dump: */
void ir3_print(struct ir3 *ir);
void ir3_print_instr(struct ir3_instruction *instr);

/* delay calculation: */
int ir3_delayslots(struct ir3_instruction *assigner,
		struct ir3_instruction *consumer, unsigned n, bool soft);
unsigned ir3_delay_calc_prera(struct ir3_block *block, struct ir3_instruction *instr);
unsigned ir3_delay_calc_postra(struct ir3_block *block, struct ir3_instruction *instr,
		bool soft, bool mergedregs);
unsigned ir3_delay_calc_exact(struct ir3_block *block, struct ir3_instruction *instr,
		bool mergedregs);
void ir3_remove_nops(struct ir3 *ir);

/* dead code elimination: */
struct ir3_shader_variant;
bool ir3_dce(struct ir3 *ir, struct ir3_shader_variant *so);

/* fp16 conversion folding */
bool ir3_cf(struct ir3 *ir);

/* copy-propagate: */
bool ir3_cp(struct ir3 *ir, struct ir3_shader_variant *so);
bool ir3_cp_postsched(struct ir3 *ir);

/* common subexpression elimination: */
bool ir3_cse(struct ir3 *ir);

/* Make arrays SSA */
bool ir3_array_to_ssa(struct ir3 *ir);

/* scheduling: */
bool ir3_sched_add_deps(struct ir3 *ir);
int ir3_sched(struct ir3 *ir);

struct ir3_context;
bool ir3_postsched(struct ir3 *ir, struct ir3_shader_variant *v);

/* register assignment: */
int ir3_ra(struct ir3_shader_variant *v);

/* legalize: */
bool ir3_legalize(struct ir3 *ir, struct ir3_shader_variant *so, int *max_bary);

static inline bool
ir3_has_latency_to_hide(struct ir3 *ir)
{
	/* VS/GS/TCS/TESS  co-exist with frag shader invocations, but we don't
	 * know the nature of the fragment shader.  Just assume it will have
	 * latency to hide:
	 */
	if (ir->type != MESA_SHADER_FRAGMENT)
		return true;

	foreach_block (block, &ir->block_list) {
		foreach_instr (instr, &block->instr_list) {
			if (is_tex_or_prefetch(instr))
				return true;

			if (is_load(instr)) {
				switch (instr->opc) {
				case OPC_LDLV:
				case OPC_LDL:
				case OPC_LDLW:
					break;
				default:
					return true;
				}
			}
		}
	}

	return false;
}

/* ************************************************************************* */
/* instruction helpers */

/* creates SSA src of correct type (ie. half vs full precision) */
static inline struct ir3_register * __ssa_src(struct ir3_instruction *instr,
		struct ir3_instruction *src, unsigned flags)
{
	struct ir3_register *reg;
	if (src->dsts[0]->flags & IR3_REG_HALF)
		flags |= IR3_REG_HALF;
	reg = ir3_src_create(instr, INVALID_REG, IR3_REG_SSA | flags);
	reg->def = src->dsts[0];
	reg->wrmask = src->dsts[0]->wrmask;
	return reg;
}

static inline struct ir3_register * __ssa_dst(struct ir3_instruction *instr)
{
	struct ir3_register *reg = ir3_dst_create(instr, INVALID_REG, IR3_REG_SSA);
	reg->instr = instr;
	return reg;
}

static inline struct ir3_instruction *
create_immed_typed(struct ir3_block *block, uint32_t val, type_t type)
{
	struct ir3_instruction *mov;
	unsigned flags = (type_size(type) < 32) ? IR3_REG_HALF : 0;

	mov = ir3_instr_create(block, OPC_MOV, 1, 1);
	mov->cat1.src_type = type;
	mov->cat1.dst_type = type;
	__ssa_dst(mov)->flags |= flags;
	ir3_src_create(mov, 0, IR3_REG_IMMED | flags)->uim_val = val;

	return mov;
}

static inline struct ir3_instruction *
create_immed(struct ir3_block *block, uint32_t val)
{
	return create_immed_typed(block, val, TYPE_U32);
}

static inline struct ir3_instruction *
create_uniform_typed(struct ir3_block *block, unsigned n, type_t type)
{
	struct ir3_instruction *mov;
	unsigned flags = (type_size(type) < 32) ? IR3_REG_HALF : 0;

	mov = ir3_instr_create(block, OPC_MOV, 1, 1);
	mov->cat1.src_type = type;
	mov->cat1.dst_type = type;
	__ssa_dst(mov)->flags |= flags;
	ir3_src_create(mov, n, IR3_REG_CONST | flags);

	return mov;
}

static inline struct ir3_instruction *
create_uniform(struct ir3_block *block, unsigned n)
{
	return create_uniform_typed(block, n, TYPE_F32);
}

static inline struct ir3_instruction *
create_uniform_indirect(struct ir3_block *block, int n, type_t type,
		struct ir3_instruction *address)
{
	struct ir3_instruction *mov;

	mov = ir3_instr_create(block, OPC_MOV, 1, 1);
	mov->cat1.src_type = type;
	mov->cat1.dst_type = type;
	__ssa_dst(mov);
	ir3_src_create(mov, 0, IR3_REG_CONST | IR3_REG_RELATIV)->array.offset = n;

	ir3_instr_set_address(mov, address);

	return mov;
}

static inline struct ir3_instruction *
ir3_MOV(struct ir3_block *block, struct ir3_instruction *src, type_t type)
{
	struct ir3_instruction *instr = ir3_instr_create(block, OPC_MOV, 1, 1);
	unsigned flags = (type_size(type) < 32) ? IR3_REG_HALF : 0;

	__ssa_dst(instr)->flags |= flags;
	if (src->dsts[0]->flags & IR3_REG_ARRAY) {
		struct ir3_register *src_reg = __ssa_src(instr, src, IR3_REG_ARRAY);
		src_reg->array = src->dsts[0]->array;
	} else {
		__ssa_src(instr, src, src->dsts[0]->flags & IR3_REG_SHARED);
	}
	debug_assert(!(src->dsts[0]->flags & IR3_REG_RELATIV));
	instr->cat1.src_type = type;
	instr->cat1.dst_type = type;
	return instr;
}

static inline struct ir3_instruction *
ir3_COV(struct ir3_block *block, struct ir3_instruction *src,
		type_t src_type, type_t dst_type)
{
	struct ir3_instruction *instr = ir3_instr_create(block, OPC_MOV, 1, 1);
	unsigned dst_flags = (type_size(dst_type) < 32) ? IR3_REG_HALF : 0;
	unsigned src_flags = (type_size(src_type) < 32) ? IR3_REG_HALF : 0;

	debug_assert((src->dsts[0]->flags & IR3_REG_HALF) == src_flags);

	__ssa_dst(instr)->flags |= dst_flags;
	__ssa_src(instr, src, 0);
	instr->cat1.src_type = src_type;
	instr->cat1.dst_type = dst_type;
	debug_assert(!(src->dsts[0]->flags & IR3_REG_ARRAY));
	return instr;
}

static inline struct ir3_instruction *
ir3_MOVMSK(struct ir3_block *block, unsigned components)
{
	struct ir3_instruction *instr = ir3_instr_create(block, OPC_MOVMSK, 1, 0);

	struct ir3_register *dst = __ssa_dst(instr);
	dst->flags |= IR3_REG_SHARED;
	dst->wrmask = (1 << components) - 1;
	return instr;
}

static inline struct ir3_instruction *
ir3_NOP(struct ir3_block *block)
{
	return ir3_instr_create(block, OPC_NOP, 0, 0);
}

#define IR3_INSTR_0 0

#define __INSTR0(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block)                                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 0);                              \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR0F(f, name)    __INSTR0(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR0(name)        __INSTR0(0, name, OPC_##name)

#define __INSTR1(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block,                                      \
		struct ir3_instruction *a, unsigned aflags)                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 1);                              \
	__ssa_dst(instr);                                                    \
	__ssa_src(instr, a, aflags);                                         \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR1F(f, name)    __INSTR1(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR1(name)        __INSTR1(0, name, OPC_##name)

#define __INSTR2(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block,                                      \
		struct ir3_instruction *a, unsigned aflags,                      \
		struct ir3_instruction *b, unsigned bflags)                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 2);                              \
	__ssa_dst(instr);                                                    \
	__ssa_src(instr, a, aflags);                                         \
	__ssa_src(instr, b, bflags);                                         \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR2F(f, name)    __INSTR2(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR2(name)        __INSTR2(0, name, OPC_##name)

#define __INSTR3(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block,                                      \
		struct ir3_instruction *a, unsigned aflags,                      \
		struct ir3_instruction *b, unsigned bflags,                      \
		struct ir3_instruction *c, unsigned cflags)                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 3);                              \
	__ssa_dst(instr);                                                    \
	__ssa_src(instr, a, aflags);                                         \
	__ssa_src(instr, b, bflags);                                         \
	__ssa_src(instr, c, cflags);                                         \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR3F(f, name)    __INSTR3(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR3(name)        __INSTR3(0, name, OPC_##name)

#define __INSTR4(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block,                                      \
		struct ir3_instruction *a, unsigned aflags,                      \
		struct ir3_instruction *b, unsigned bflags,                      \
		struct ir3_instruction *c, unsigned cflags,                      \
		struct ir3_instruction *d, unsigned dflags)                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 4);                              \
	__ssa_dst(instr);                                                    \
	__ssa_src(instr, a, aflags);                                         \
	__ssa_src(instr, b, bflags);                                         \
	__ssa_src(instr, c, cflags);                                         \
	__ssa_src(instr, d, dflags);                                         \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR4F(f, name)    __INSTR4(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR4(name)        __INSTR4(0, name, OPC_##name)

#define __INSTR5(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block,                                      \
		struct ir3_instruction *a, unsigned aflags,                      \
		struct ir3_instruction *b, unsigned bflags,                      \
		struct ir3_instruction *c, unsigned cflags,                      \
		struct ir3_instruction *d, unsigned dflags,                      \
		struct ir3_instruction *e, unsigned eflags)                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 5);                              \
	__ssa_dst(instr);                                                    \
	__ssa_src(instr, a, aflags);                                         \
	__ssa_src(instr, b, bflags);                                         \
	__ssa_src(instr, c, cflags);                                         \
	__ssa_src(instr, d, dflags);                                         \
	__ssa_src(instr, e, eflags);                                         \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR5F(f, name)    __INSTR5(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR5(name)        __INSTR5(0, name, OPC_##name)

#define __INSTR6(flag, name, opc)                                        \
static inline struct ir3_instruction *                                   \
ir3_##name(struct ir3_block *block,                                      \
		struct ir3_instruction *a, unsigned aflags,                      \
		struct ir3_instruction *b, unsigned bflags,                      \
		struct ir3_instruction *c, unsigned cflags,                      \
		struct ir3_instruction *d, unsigned dflags,                      \
		struct ir3_instruction *e, unsigned eflags,                      \
		struct ir3_instruction *f, unsigned fflags)                      \
{                                                                        \
	struct ir3_instruction *instr =                                      \
		ir3_instr_create(block, opc, 1, 6);                              \
	__ssa_dst(instr);                                                    \
	__ssa_src(instr, a, aflags);                                         \
	__ssa_src(instr, b, bflags);                                         \
	__ssa_src(instr, c, cflags);                                         \
	__ssa_src(instr, d, dflags);                                         \
	__ssa_src(instr, e, eflags);                                         \
	__ssa_src(instr, f, fflags);                                         \
	instr->flags |= flag;                                                \
	return instr;                                                        \
}
#define INSTR6F(f, name)    __INSTR6(IR3_INSTR_##f, name##_##f, OPC_##name)
#define INSTR6(name)        __INSTR6(0, name, OPC_##name)

/* cat0 instructions: */
INSTR1(B)
INSTR0(JUMP)
INSTR1(KILL)
INSTR1(DEMOTE)
INSTR0(END)
INSTR0(CHSH)
INSTR0(CHMASK)
INSTR1(PREDT)
INSTR0(PREDF)
INSTR0(PREDE)

/* cat2 instructions, most 2 src but some 1 src: */
INSTR2(ADD_F)
INSTR2(MIN_F)
INSTR2(MAX_F)
INSTR2(MUL_F)
INSTR1(SIGN_F)
INSTR2(CMPS_F)
INSTR1(ABSNEG_F)
INSTR2(CMPV_F)
INSTR1(FLOOR_F)
INSTR1(CEIL_F)
INSTR1(RNDNE_F)
INSTR1(RNDAZ_F)
INSTR1(TRUNC_F)
INSTR2(ADD_U)
INSTR2(ADD_S)
INSTR2(SUB_U)
INSTR2(SUB_S)
INSTR2(CMPS_U)
INSTR2(CMPS_S)
INSTR2(MIN_U)
INSTR2(MIN_S)
INSTR2(MAX_U)
INSTR2(MAX_S)
INSTR1(ABSNEG_S)
INSTR2(AND_B)
INSTR2(OR_B)
INSTR1(NOT_B)
INSTR2(XOR_B)
INSTR2(CMPV_U)
INSTR2(CMPV_S)
INSTR2(MUL_U24)
INSTR2(MUL_S24)
INSTR2(MULL_U)
INSTR1(BFREV_B)
INSTR1(CLZ_S)
INSTR1(CLZ_B)
INSTR2(SHL_B)
INSTR2(SHR_B)
INSTR2(ASHR_B)
INSTR2(BARY_F)
INSTR2(MGEN_B)
INSTR2(GETBIT_B)
INSTR1(SETRM)
INSTR1(CBITS_B)
INSTR2(SHB)
INSTR2(MSAD)

/* cat3 instructions: */
INSTR3(MAD_U16)
INSTR3(MADSH_U16)
INSTR3(MAD_S16)
INSTR3(MADSH_M16)
INSTR3(MAD_U24)
INSTR3(MAD_S24)
INSTR3(MAD_F16)
INSTR3(MAD_F32)
/* NOTE: SEL_B32 checks for zero vs nonzero */
INSTR3(SEL_B16)
INSTR3(SEL_B32)
INSTR3(SEL_S16)
INSTR3(SEL_S32)
INSTR3(SEL_F16)
INSTR3(SEL_F32)
INSTR3(SAD_S16)
INSTR3(SAD_S32)

/* cat4 instructions: */
INSTR1(RCP)
INSTR1(RSQ)
INSTR1(HRSQ)
INSTR1(LOG2)
INSTR1(HLOG2)
INSTR1(EXP2)
INSTR1(HEXP2)
INSTR1(SIN)
INSTR1(COS)
INSTR1(SQRT)

/* cat5 instructions: */
INSTR1(DSX)
INSTR1(DSXPP_MACRO)
INSTR1(DSY)
INSTR1(DSYPP_MACRO)
INSTR1F(3D, DSX)
INSTR1F(3D, DSY)
INSTR1(RGETPOS)

static inline struct ir3_instruction *
ir3_SAM(struct ir3_block *block, opc_t opc, type_t type,
		unsigned wrmask, unsigned flags, struct ir3_instruction *samp_tex,
		struct ir3_instruction *src0, struct ir3_instruction *src1)
{
	struct ir3_instruction *sam;
	unsigned nreg = 0;

	if (flags & IR3_INSTR_S2EN) {
		nreg++;
	}
	if (src0) {
		nreg++;
	}
	if (src1) {
		nreg++;
	}

	sam = ir3_instr_create(block, opc, 1, nreg);
	sam->flags |= flags;
	__ssa_dst(sam)->wrmask = wrmask;
	if (flags & IR3_INSTR_S2EN) {
		__ssa_src(sam, samp_tex, (flags & IR3_INSTR_B) ? 0 : IR3_REG_HALF);
	}
	if (src0) {
		__ssa_src(sam, src0, 0);
	}
	if (src1) {
		__ssa_src(sam, src1, 0);
	}
	sam->cat5.type  = type;

	return sam;
}

/* cat6 instructions: */
INSTR2(LDLV)
INSTR3(LDG)
INSTR3(LDL)
INSTR3(LDLW)
INSTR3(LDP)
INSTR4(STG)
INSTR3(STL)
INSTR3(STLW)
INSTR3(STP)
INSTR1(RESINFO)
INSTR1(RESFMT)
INSTR2(ATOMIC_ADD)
INSTR2(ATOMIC_SUB)
INSTR2(ATOMIC_XCHG)
INSTR2(ATOMIC_INC)
INSTR2(ATOMIC_DEC)
INSTR2(ATOMIC_CMPXCHG)
INSTR2(ATOMIC_MIN)
INSTR2(ATOMIC_MAX)
INSTR2(ATOMIC_AND)
INSTR2(ATOMIC_OR)
INSTR2(ATOMIC_XOR)
INSTR2(LDC)
#if GPU >= 600
INSTR3(STIB);
INSTR2(LDIB);
INSTR5(LDG_A);
INSTR6(STG_A);
INSTR3F(G, ATOMIC_ADD)
INSTR3F(G, ATOMIC_SUB)
INSTR3F(G, ATOMIC_XCHG)
INSTR3F(G, ATOMIC_INC)
INSTR3F(G, ATOMIC_DEC)
INSTR3F(G, ATOMIC_CMPXCHG)
INSTR3F(G, ATOMIC_MIN)
INSTR3F(G, ATOMIC_MAX)
INSTR3F(G, ATOMIC_AND)
INSTR3F(G, ATOMIC_OR)
INSTR3F(G, ATOMIC_XOR)
#elif GPU >= 400
INSTR3(LDGB)
INSTR4(STGB)
INSTR4(STIB)
INSTR4F(G, ATOMIC_ADD)
INSTR4F(G, ATOMIC_SUB)
INSTR4F(G, ATOMIC_XCHG)
INSTR4F(G, ATOMIC_INC)
INSTR4F(G, ATOMIC_DEC)
INSTR4F(G, ATOMIC_CMPXCHG)
INSTR4F(G, ATOMIC_MIN)
INSTR4F(G, ATOMIC_MAX)
INSTR4F(G, ATOMIC_AND)
INSTR4F(G, ATOMIC_OR)
INSTR4F(G, ATOMIC_XOR)
#endif

/* cat7 instructions: */
INSTR0(BAR)
INSTR0(FENCE)

/* ************************************************************************* */
#include "regmask.h"

static inline void regmask_set(regmask_t *regmask, struct ir3_register *reg)
{
	bool half = reg->flags & IR3_REG_HALF;
	if (reg->flags & IR3_REG_RELATIV) {
		for (unsigned i = 0; i < reg->size; i++)
			__regmask_set(regmask, half, reg->array.base + i);
	} else {
		for (unsigned mask = reg->wrmask, n = reg->num; mask; mask >>= 1, n++)
			if (mask & 1)
				__regmask_set(regmask, half, n);
	}
}

static inline bool regmask_get(regmask_t *regmask,
		struct ir3_register *reg)
{
	bool half = reg->flags & IR3_REG_HALF;
	if (reg->flags & IR3_REG_RELATIV) {
		for (unsigned i = 0; i < reg->size; i++)
			if (__regmask_get(regmask, half, reg->array.base + i))
				return true;
	} else {
		for (unsigned mask = reg->wrmask, n = reg->num; mask; mask >>= 1, n++)
			if (mask & 1)
				if (__regmask_get(regmask, half, n))
					return true;
	}
	return false;
}
/* ************************************************************************* */

#endif /* IR3_H_ */
