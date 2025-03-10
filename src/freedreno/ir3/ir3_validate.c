/*
 * Copyright © 2020 Google, Inc.
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

#include <stdlib.h>

#include "util/ralloc.h"

#include "ir3.h"

struct ir3_validate_ctx {
	struct ir3 *ir;

	/* Current instruction being validated: */
	struct ir3_instruction *current_instr;

	/* Set of instructions found so far, used to validate that we
	 * don't have SSA uses that occure before def's
	 */
	struct set *defs;
};

static void
validate_error(struct ir3_validate_ctx *ctx, const char *condstr)
{
	fprintf(stderr, "validation fail: %s\n", condstr);
	fprintf(stderr, "  -> for instruction: ");
	ir3_print_instr(ctx->current_instr);
	abort();
}

#define validate_assert(ctx, cond) do { \
	if (!(cond)) { \
		validate_error(ctx, #cond); \
	} } while (0)

static unsigned
reg_class_flags(struct ir3_register *reg)
{
	return reg->flags & (IR3_REG_HALF | IR3_REG_SHARED);
}

static void
validate_src(struct ir3_validate_ctx *ctx, struct ir3_instruction *instr,
			 struct ir3_register *reg)
{
	struct ir3_instruction *src = ssa(reg);

	if (!src)
		return;

	validate_assert(ctx, _mesa_set_search(ctx->defs, src));
	validate_assert(ctx, src->dsts[0]->wrmask == reg->wrmask);
	validate_assert(ctx, reg_class_flags(src->dsts[0]) == reg_class_flags(reg));

	if (reg->tied) {
		validate_assert(ctx, reg->tied->tied == reg);
		bool found = false;
		for (unsigned i = 0; i < instr->dsts_count; i++) {
			if (instr->dsts[i] == reg->tied) {
				found = true;
				break;
			}
		}
		validate_assert(ctx, found && "tied register not in the same instruction");
	}
}

/* phi sources are logically read at the end of the predecessor basic block,
 * and we have to validate them then in order to correctly validate that the
 * use comes after the definition for loop phis.
 */
static void
validate_phi_src(struct ir3_validate_ctx *ctx, struct ir3_block *block, struct ir3_block *pred)
{
	unsigned pred_idx = ir3_block_get_pred_index(block, pred);

	foreach_instr (phi, &block->instr_list) {
		if (phi->opc != OPC_META_PHI)
			break;

		ctx->current_instr = phi;
		validate_assert(ctx, phi->srcs_count == block->predecessors_count);
		validate_src(ctx, phi, phi->srcs[pred_idx]);
	}
}

static void
validate_phi(struct ir3_validate_ctx *ctx, struct ir3_instruction *phi)
{
	_mesa_set_add(ctx->defs, phi);
	validate_assert(ctx, writes_gpr(phi));
}

static void
validate_dst(struct ir3_validate_ctx *ctx, struct ir3_instruction *instr,
			 struct ir3_register *reg)
{
	if (reg->tied) {
		validate_assert(ctx, reg->tied->tied == reg);
		validate_assert(ctx, reg_class_flags(reg->tied) == reg_class_flags(reg));
		validate_assert(ctx, reg->tied->wrmask == reg->wrmask);
		if (reg->flags & IR3_REG_ARRAY) {
			validate_assert(ctx, reg->tied->array.base == reg->array.base);
			validate_assert(ctx, reg->tied->size == reg->size);
		}
		bool found = false;
		for (unsigned i = 0; i < instr->srcs_count; i++) {
			if (instr->srcs[i] == reg->tied) {
				found = true;
				break;
			}
		}
		validate_assert(ctx, found && "tied register not in the same instruction");
	}

	if (reg->flags & IR3_REG_SSA)
		validate_assert(ctx, reg->instr == instr);
}

#define validate_reg_size(ctx, reg, type) \
	validate_assert(ctx, type_size(type) == (((reg)->flags & IR3_REG_HALF) ? 16 : 32))

static void
validate_instr(struct ir3_validate_ctx *ctx, struct ir3_instruction *instr)
{
	struct ir3_register *last_reg = NULL;

	if (writes_gpr(instr)) {
		if (instr->dsts[0]->flags & IR3_REG_RELATIV) {
			validate_assert(ctx, instr->address);
		}
	}

	foreach_src_n (reg, n, instr) {
		if (reg->flags & IR3_REG_RELATIV)
			validate_assert(ctx, instr->address);

		validate_src(ctx, instr, reg);

		/* Validate that all src's are either half of full.
		 *
		 * Note: tex instructions w/ .s2en are a bit special in that the
		 * tex/samp src reg is half-reg for non-bindless and full for
		 * bindless, irrespective of the precision of other srcs. The
		 * tex/samp src is the first src reg when .s2en is set
		 */
		if (reg->tied) {
			/* must have the same size as the destination, handled in
			 * validate_reg().
			 */
		} else if (reg == instr->address) {
			validate_assert(ctx, reg->flags & IR3_REG_HALF);
		} else if ((instr->flags & IR3_INSTR_S2EN) && (n < 2)) {
			if (n == 0) {
				if (instr->flags & IR3_INSTR_B)
					validate_assert(ctx, !(reg->flags & IR3_REG_HALF));
				else
					validate_assert(ctx, reg->flags & IR3_REG_HALF);
			}
		} else if (opc_cat(instr->opc) == 6) {
			/* handled below */
		} else if (opc_cat(instr->opc) == 0) {
			/* end/chmask/etc are allowed to have different size sources */
		} else if (n > 0) {
			validate_assert(ctx, (last_reg->flags & IR3_REG_HALF) == (reg->flags & IR3_REG_HALF));
		}

		last_reg = reg;
	}
	
	for (unsigned i = 0; i < instr->dsts_count; i++) {
		struct ir3_register *reg = instr->dsts[i];

		validate_dst(ctx, instr, reg);
	}

	_mesa_set_add(ctx->defs, instr);

	/* Check that src/dst types match the register types, and for
	 * instructions that have different opcodes depending on type,
	 * that the opcodes are correct.
	 */
	switch (opc_cat(instr->opc)) {
	case 1: /* move instructions */
		if (instr->opc == OPC_MOVMSK) {
			validate_assert(ctx, instr->dsts_count == 1);
			validate_assert(ctx, instr->srcs_count == 0);
			validate_assert(ctx, instr->dsts[0]->flags & IR3_REG_SHARED);
			validate_assert(ctx, !(instr->dsts[0]->flags & IR3_REG_HALF));
			validate_assert(ctx, util_is_power_of_two_or_zero(instr->dsts[0]->wrmask + 1));
		} else {
			validate_reg_size(ctx, instr->dsts[0], instr->cat1.dst_type);
			validate_reg_size(ctx, instr->srcs[0], instr->cat1.src_type);
		}
		break;
	case 3:
		/* Validate that cat3 opc matches the src type.  We've already checked that all
		 * the src regs are same type
		 */
		if (instr->srcs[0]->flags & IR3_REG_HALF) {
			validate_assert(ctx, instr->opc == cat3_half_opc(instr->opc));
		} else {
			validate_assert(ctx, instr->opc == cat3_full_opc(instr->opc));
		}
		break;
	case 4:
		/* Validate that cat4 opc matches the dst type: */
		if (instr->dsts[0]->flags & IR3_REG_HALF) {
			validate_assert(ctx, instr->opc == cat4_half_opc(instr->opc));
		} else {
			validate_assert(ctx, instr->opc == cat4_full_opc(instr->opc));
		}
		break;
	case 5:
		validate_reg_size(ctx, instr->dsts[0], instr->cat5.type);
		break;
	case 6:
		switch (instr->opc) {
		case OPC_RESINFO:
		case OPC_RESFMT:
			validate_reg_size(ctx, instr->dsts[0], instr->cat6.type);
			validate_reg_size(ctx, instr->srcs[0], instr->cat6.type);
			break;
		case OPC_L2G:
		case OPC_G2L:
			validate_assert(ctx, !(instr->dsts[0]->flags & IR3_REG_HALF));
			validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
			break;
		case OPC_STG:
			validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
			validate_assert(ctx, !(instr->srcs[1]->flags & IR3_REG_HALF));
			validate_reg_size(ctx, instr->srcs[2], instr->cat6.type);
			validate_assert(ctx, !(instr->srcs[3]->flags & IR3_REG_HALF));
			break;
		case OPC_STG_A:
			validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
			validate_assert(ctx, !(instr->srcs[2]->flags & IR3_REG_HALF));
			validate_assert(ctx, !(instr->srcs[3]->flags & IR3_REG_HALF));
			validate_reg_size(ctx, instr->srcs[4], instr->cat6.type);
			validate_assert(ctx, !(instr->srcs[5]->flags & IR3_REG_HALF));
			break;
		case OPC_STL:
		case OPC_STP:
		case OPC_STLW:
			validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
			validate_reg_size(ctx, instr->srcs[1], instr->cat6.type);
			validate_assert(ctx, !(instr->srcs[2]->flags & IR3_REG_HALF));
			break;
		case OPC_STIB:
			if (instr->flags & IR3_INSTR_B) {
				validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
				validate_assert(ctx, !(instr->srcs[1]->flags & IR3_REG_HALF));
				validate_reg_size(ctx, instr->srcs[2], instr->cat6.type);
			} else {
				validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
				validate_reg_size(ctx, instr->srcs[1], instr->cat6.type);
				validate_assert(ctx, !(instr->srcs[2]->flags & IR3_REG_HALF));
			}
			break;
		default:
			validate_reg_size(ctx, instr->dsts[0], instr->cat6.type);
			validate_assert(ctx, !(instr->srcs[0]->flags & IR3_REG_HALF));
			if (instr->srcs_count > 1)
				validate_assert(ctx, !(instr->srcs[1]->flags & IR3_REG_HALF));
			break;
		}
	}
}

void
ir3_validate(struct ir3 *ir)
{
#ifdef NDEBUG
#  define VALIDATE 0
#else
#  define VALIDATE 1
#endif

	if (!VALIDATE)
		return;

	struct ir3_validate_ctx *ctx = ralloc_size(NULL, sizeof(*ctx));

	ctx->ir = ir;
	ctx->defs = _mesa_pointer_set_create(ctx);

	foreach_block (block, &ir->block_list) {
		/* We require that the first block does not have any predecessors,
		 * which allows us to assume that phi nodes and meta:input's do not
		 * appear in the same basic block.
		 */
		validate_assert(ctx,
				block != ir3_start_block(ir) || block->predecessors_count == 0);

		struct ir3_instruction *prev = NULL;
		foreach_instr (instr, &block->instr_list) {
			ctx->current_instr = instr;
			if (instr->opc == OPC_META_PHI) {
				/* phis must be the first in the block */
				validate_assert(ctx, prev == NULL || prev->opc == OPC_META_PHI);
				validate_phi(ctx, instr);
			} else {
				validate_instr(ctx, instr);
			}
			prev = instr;
		}

		for (unsigned i = 0; i < 2; i++) {
			if (block->successors[i])
				validate_phi_src(ctx, block->successors[i], block);
		}
	}

	ralloc_free(ctx);
}
