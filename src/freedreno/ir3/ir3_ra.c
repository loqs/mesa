/*
 * Copyright (C) 2021 Valve Corporation
 * Copyright (C) 2014 Rob Clark <robclark@freedesktop.org>
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

#include "ir3_ra.h"
#include "ir3_shader.h"
#include "util/rb_tree.h"
#include "util/u_math.h"

/* This file implements an SSA-based register allocator. Unlike other
 * SSA-based allocators, it handles vector split/collect "smartly," meaning
 * that multiple values may share the same register interval. From the
 * perspective of the allocator itself, only the top-level intervals matter,
 * and the allocator is only concerned with allocating top-level intervals,
 * which may mean moving other top-level intervals around. Other intervals,
 * like the destination of a split instruction or the source of a collect
 * instruction, are "locked" to their parent interval. The details of this are
 * mostly handled by ir3_merge_regs and ir3_reg_ctx.
 *
 * We currently don't do any backtracking, but we do use the merge sets as a
 * form of affinity to try to avoid moves from phis/splits/collects. Each
 * merge set is what a more "classic" graph-coloring or live-range based
 * allocator would consider a single register, but here we use it as merely a
 * hint, except when multiple overlapping values are live at the same time.
 * Each merge set has a "preferred" register, and we try to honor that when
 * allocating values in the merge set.
 */

/* ir3_reg_ctx implementation. */

static int
ir3_reg_interval_cmp(const struct rb_node *node, const void *data)
{
	physreg_t reg = *(const physreg_t *)data;
	const struct ir3_reg_interval *interval = ir3_rb_node_to_interval_const(node);
	if (interval->reg->interval_start > reg)
		return -1;
	else if (interval->reg->interval_end <= reg)
		return 1;
	else
		return 0;
}

static struct ir3_reg_interval *
ir3_reg_interval_search(struct rb_tree *tree, unsigned offset)
{
	struct rb_node *node = rb_tree_search(tree, &offset, ir3_reg_interval_cmp);
	return node ? ir3_rb_node_to_interval(node) : NULL;
}

static struct ir3_reg_interval *
ir3_reg_interval_search_sloppy(struct rb_tree *tree, unsigned offset)
{
	struct rb_node *node = rb_tree_search_sloppy(tree, &offset, ir3_reg_interval_cmp);
	return node ? ir3_rb_node_to_interval(node) : NULL;
}

/* Get the interval covering the reg, or the closest to the right if it
 * doesn't exist.
 */
static struct ir3_reg_interval *
ir3_reg_interval_search_right(struct rb_tree *tree, unsigned offset)
{
	struct ir3_reg_interval *interval = ir3_reg_interval_search_sloppy(tree, offset);
	if (!interval) {
		return NULL;
	} else if (interval->reg->interval_end > offset) {
		return interval;
	} else {
		/* There is no interval covering reg, and ra_file_search_sloppy()
		 * returned the closest range to the left, so the next interval to the
		 * right should be the closest to the right.
		 */
		return ir3_reg_interval_next_or_null(interval);
	}
}

static int
ir3_reg_interval_insert_cmp(const struct rb_node *_a, const struct rb_node *_b)
{
	const struct ir3_reg_interval *a = ir3_rb_node_to_interval_const(_a);
	const struct ir3_reg_interval *b = ir3_rb_node_to_interval_const(_b);
	return b->reg->interval_start - a->reg->interval_start;
}

static void
interval_insert(struct ir3_reg_ctx *ctx, struct rb_tree *tree,
				struct ir3_reg_interval *interval)
{
	struct ir3_reg_interval *right =
		ir3_reg_interval_search_right(tree, interval->reg->interval_start);
	if (right && right->reg->interval_start < interval->reg->interval_end) {
		/* We disallow trees where different members have different half-ness.
		 * This means that we can't treat bitcasts as copies like normal
		 * split/collect, so something like this would require an extra copy
		 * in mergedregs mode, and count as 4 half-units of register pressure
		 * instead of 2:
		 *
		 * f16vec2 foo = unpackFloat2x16(bar)
		 * ... = foo.x
		 * ... = bar
		 *
		 * However, relaxing this rule would open a huge can of worms. What
		 * happens when there's a vector of 16 things, and the fifth element
		 * has been bitcasted as a half-reg? Would that element alone have to
		 * be small enough to be used as a half-reg source? Let's keep that
		 * can of worms firmly shut for now.
		 */
		assert((interval->reg->flags & IR3_REG_HALF) ==
			   (right->reg->flags & IR3_REG_HALF));

		if (right->reg->interval_end <= interval->reg->interval_end &&
			right->reg->interval_start >= interval->reg->interval_start) {
			/* Check if we're inserting something that's already inserted */
			assert(interval != right);

			/* "right" is contained in "interval" and must become a child of
			 * it. There may be further children too.
			 */
			for (struct ir3_reg_interval *next = ir3_reg_interval_next(right);
				 right && right->reg->interval_start < interval->reg->interval_end;
				 right = next, next = ir3_reg_interval_next_or_null(next)) {
				/* "right" must be contained in "interval." */
				assert(right->reg->interval_end <= interval->reg->interval_end);
				assert((interval->reg->flags & IR3_REG_HALF) ==
					   (right->reg->flags & IR3_REG_HALF));
				if (!right->parent)
					ctx->interval_delete(ctx, right);
				right->parent = interval;
				rb_tree_remove(tree, &right->node);
				rb_tree_insert(&interval->children, &right->node,
							   ir3_reg_interval_insert_cmp);
			}
		} else {
			/* "right" must contain "interval," since intervals must form a
			 * tree.
			 */
			assert(right->reg->interval_start <= interval->reg->interval_start);
			interval->parent = right;
			interval_insert(ctx, &right->children, interval);
			return;
		}
	}

	if (!interval->parent)
		ctx->interval_add(ctx, interval);
	rb_tree_insert(tree, &interval->node, ir3_reg_interval_insert_cmp);
	interval->inserted = true;
}

void
ir3_reg_interval_insert(struct ir3_reg_ctx *ctx, struct ir3_reg_interval *interval)
{
	interval_insert(ctx, &ctx->intervals, interval);
}

void
ir3_reg_interval_remove(struct ir3_reg_ctx *ctx, struct ir3_reg_interval *interval)
{
	if (interval->parent) {
		rb_tree_remove(&interval->parent->children, &interval->node);
	} else {
		ctx->interval_delete(ctx, interval);
		rb_tree_remove(&ctx->intervals, &interval->node);
	}

	rb_tree_foreach_safe(struct ir3_reg_interval, child, &interval->children, node) {
		rb_tree_remove(&interval->children, &child->node);
		child->parent = interval->parent;

		if (interval->parent) {
			rb_tree_insert(&child->parent->children, &child->node,
						   ir3_reg_interval_insert_cmp);
		} else {
			ctx->interval_readd(ctx, interval, child);
			rb_tree_insert(&ctx->intervals, &child->node,
						   ir3_reg_interval_insert_cmp);
		}
	}

	interval->inserted = false;
}

void
ir3_reg_interval_remove_all(struct ir3_reg_ctx *ctx, struct ir3_reg_interval *interval)
{
	assert(!interval->parent);

	ctx->interval_delete(ctx, interval);
	rb_tree_remove(&ctx->intervals, &interval->node);
}

static void
interval_dump(struct ir3_reg_interval *interval, unsigned indent)
{
	for (unsigned i = 0; i < indent; i++)
		printf("\t");
	printf("reg %u start %u\n", interval->reg->name, interval->reg->interval_start);

	rb_tree_foreach(struct ir3_reg_interval, child, &interval->children, node) {
		interval_dump(child, indent + 1);
	}

	for (unsigned i = 0; i < indent; i++)
		printf("\t");
	printf("reg %u end %u\n", interval->reg->name, interval->reg->interval_end);
}

void
ir3_reg_interval_dump(struct ir3_reg_interval *interval)
{
	interval_dump(interval, 0);
}

/* These are the core datastructures used by the register allocator. First
 * ra_interval and ra_file, which are used for intra-block tracking and use
 * the ir3_reg_ctx infrastructure:
 */

struct ra_interval {
	struct ir3_reg_interval interval;

	struct rb_node physreg_node;
	physreg_t physreg_start, physreg_end;

	/* True if this is a source of the current instruction which is entirely
	 * killed. This means we can allocate the dest over it, but we can't break
	 * it up.
	 */
	bool is_killed;

	/* True if this interval cannot be moved from its position. This is only
	 * used for precolored inputs to ensure that other inputs don't get
	 * allocated on top of them.
	 */
	bool frozen;
};

struct ra_file {
	struct ir3_reg_ctx reg_ctx;

	BITSET_DECLARE(available, RA_MAX_FILE_SIZE);
	BITSET_DECLARE(available_to_evict, RA_MAX_FILE_SIZE);

	struct rb_tree physreg_intervals;

	unsigned size;
	unsigned start;
};

/* State for inter-block tracking. When we split a live range to make space
 * for a vector, we may need to insert fixup code when a block has multiple
 * predecessors that have moved the same live value to different registers.
 * This keeps track of state required to do that.
 */

struct ra_block_state {
	/* Map of defining ir3_register -> physreg it was allocated to at the end
	 * of the block.
	 */
	struct hash_table *renames;

	/* For loops, we need to process a block before all its predecessors have
	 * been processed. In particular, we need to pick registers for values
	 * without knowing if all the predecessors have been renamed. This keeps
	 * track of the registers we chose so that when we visit the back-edge we
	 * can move them appropriately. If all predecessors have been visited
	 * before this block is visited then we don't need to fill this out. This
	 * is a map from ir3_register -> physreg.
	 */
	struct hash_table *entry_regs;

	/* True if the block has been visited and "renames" is complete.
	 */
	bool visited;
};

struct ra_parallel_copy {
	struct ra_interval *interval;
	physreg_t src;
};

/* The main context: */

struct ra_ctx {
	/* r0.x - r47.w. On a6xx with merged-regs, hr0.x-hr47.w go into the bottom
	 * half of this file too.
	 */
	struct ra_file full;

	/* hr0.x - hr63.w, only used without merged-regs. */
	struct ra_file half;

	/* Shared regs. */
	struct ra_file shared;

	struct ir3_liveness *live;

	struct ir3_block *block;

	const struct ir3_compiler *compiler;
	gl_shader_stage stage;

	/* Pending moves of top-level intervals that will be emitted once we're
	 * finished:
	 */
	DECLARE_ARRAY(struct ra_parallel_copy, parallel_copies);

	struct ra_interval *intervals;
	struct ra_block_state *blocks;

	bool merged_regs;
};

#define foreach_interval(interval, file) \
	rb_tree_foreach(struct ra_interval, interval, &(file)->physreg_intervals, physreg_node)
#define foreach_interval_rev(interval, file) \
	rb_tree_foreach(struct ra_interval, interval, &(file)->physreg_intervals, physreg_node)
#define foreach_interval_safe(interval, file) \
	rb_tree_foreach_safe(struct ra_interval, interval, &(file)->physreg_intervals, physreg_node)
#define foreach_interval_rev_safe(interval, file) \
	rb_tree_foreach_rev_safe(struct ra_interval, interval, &(file)->physreg_intervals, physreg_node)

static struct ra_interval *
rb_node_to_interval(struct rb_node *node)
{
	return rb_node_data(struct ra_interval, node, physreg_node);
}

static const struct ra_interval *
rb_node_to_interval_const(const struct rb_node *node)
{
	return rb_node_data(struct ra_interval, node, physreg_node);
}

static struct ra_interval *
ra_interval_next(struct ra_interval *interval)
{
	struct rb_node *next = rb_node_next(&interval->physreg_node);
	return next ? rb_node_to_interval(next) : NULL;
}

static struct ra_interval *
ra_interval_next_or_null(struct ra_interval *interval)
{
	return interval ? ra_interval_next(interval) : NULL;
}

static int
ra_interval_cmp(const struct rb_node *node, const void *data)
{
	physreg_t reg = *(const physreg_t *)data;
	const struct ra_interval *interval = rb_node_to_interval_const(node);
	if (interval->physreg_start > reg)
		return -1;
	else if (interval->physreg_end <= reg)
		return 1;
	else
		return 0;
}

static struct ra_interval *
ra_interval_search_sloppy(struct rb_tree *tree, physreg_t reg)
{
	struct rb_node *node = rb_tree_search_sloppy(tree, &reg, ra_interval_cmp);
	return node ? rb_node_to_interval(node) : NULL;
}

/* Get the interval covering the reg, or the closest to the right if it
 * doesn't exist.
 */
static struct ra_interval *
ra_interval_search_right(struct rb_tree *tree, physreg_t reg)
{
	struct ra_interval *interval = ra_interval_search_sloppy(tree, reg);
	if (!interval) {
		return NULL;
	} else if (interval->physreg_end > reg) {
		return interval;
	} else {
		/* There is no interval covering reg, and ra_file_search_sloppy()
		 * returned the closest range to the left, so the next interval to the
		 * right should be the closest to the right.
		 */
		return ra_interval_next_or_null(interval);
	}
}

static struct ra_interval *
ra_file_search_right(struct ra_file *file, physreg_t reg)
{
	return ra_interval_search_right(&file->physreg_intervals, reg);
}

static int
ra_interval_insert_cmp(const struct rb_node *_a, const struct rb_node *_b)
{
	const struct ra_interval *a = rb_node_to_interval_const(_a);
	const struct ra_interval *b = rb_node_to_interval_const(_b);
	return b->physreg_start - a->physreg_start;
}

static struct ra_interval *
ir3_reg_interval_to_ra_interval(struct ir3_reg_interval *interval)
{
	return rb_node_data(struct ra_interval, interval, interval);
}

static struct ra_file *
ir3_reg_ctx_to_file(struct ir3_reg_ctx *ctx)
{
	return rb_node_data(struct ra_file, ctx, reg_ctx);
}

static void
interval_add(struct ir3_reg_ctx *ctx, struct ir3_reg_interval *_interval)
{
	struct ra_interval *interval = ir3_reg_interval_to_ra_interval(_interval);
	struct ra_file *file = ir3_reg_ctx_to_file(ctx);

	/* We can assume in this case that physreg_start/physreg_end is already
	 * initialized.
	 */
	for (physreg_t i = interval->physreg_start; i < interval->physreg_end; i++) {
		BITSET_CLEAR(file->available, i);
		BITSET_CLEAR(file->available_to_evict, i);
	}

	rb_tree_insert(&file->physreg_intervals, &interval->physreg_node,
				   ra_interval_insert_cmp);
}

static void
interval_delete(struct ir3_reg_ctx *ctx, struct ir3_reg_interval *_interval)
{
	struct ra_interval *interval = ir3_reg_interval_to_ra_interval(_interval);
	struct ra_file *file = ir3_reg_ctx_to_file(ctx);

	for (physreg_t i = interval->physreg_start; i < interval->physreg_end; i++) {
		BITSET_SET(file->available, i);
		BITSET_SET(file->available_to_evict, i);
	}

	rb_tree_remove(&file->physreg_intervals, &interval->physreg_node);
}

static void
interval_readd(struct ir3_reg_ctx *ctx, struct ir3_reg_interval *_parent,
			   struct ir3_reg_interval *_child)
{
	struct ra_interval *parent = ir3_reg_interval_to_ra_interval(_parent);
	struct ra_interval *child = ir3_reg_interval_to_ra_interval(_child);

	child->physreg_start = parent->physreg_start +
		(child->interval.reg->interval_start - parent->interval.reg->interval_start);
	child->physreg_end = child->physreg_start +
		(child->interval.reg->interval_end - child->interval.reg->interval_start);

	interval_add(ctx, _child);
}


static void
ra_file_init(struct ra_file *file)
{
	for (unsigned i = 0; i < file->size; i++) {
		BITSET_SET(file->available, i);
		BITSET_SET(file->available_to_evict, i);
	}

	file->start = 0;

	rb_tree_init(&file->reg_ctx.intervals);
	rb_tree_init(&file->physreg_intervals);

	file->reg_ctx.interval_add = interval_add;
	file->reg_ctx.interval_delete = interval_delete;
	file->reg_ctx.interval_readd = interval_readd;
}

static void
ra_file_insert(struct ra_file *file, struct ra_interval *interval)
{
	assert(interval->physreg_start < interval->physreg_end);
	assert(interval->physreg_end <= file->size);
	if (interval->interval.reg->flags & IR3_REG_HALF)
		assert(interval->physreg_end <= RA_HALF_SIZE);

	ir3_reg_interval_insert(&file->reg_ctx, &interval->interval);
}

static void
ra_file_remove(struct ra_file *file, struct ra_interval *interval)
{
	ir3_reg_interval_remove(&file->reg_ctx, &interval->interval);
}

static void
ra_file_mark_killed(struct ra_file *file, struct ra_interval *interval)
{
	assert(!interval->interval.parent);

	for (physreg_t i = interval->physreg_start; i < interval->physreg_end; i++) {
		BITSET_SET(file->available, i);
	}

	interval->is_killed = true;
}

static physreg_t
ra_interval_get_physreg(const struct ra_interval *interval)
{
	unsigned child_start = interval->interval.reg->interval_start;

	while (interval->interval.parent) {
		interval = ir3_reg_interval_to_ra_interval(interval->interval.parent);
	}

	return interval->physreg_start +
		(child_start - interval->interval.reg->interval_start);
}

static unsigned
ra_interval_get_num(const struct ra_interval *interval)
{
	return ra_physreg_to_num(ra_interval_get_physreg(interval),
							 interval->interval.reg->flags);
}

static void
ra_interval_init(struct ra_interval *interval, struct ir3_register *reg)
{
	ir3_reg_interval_init(&interval->interval, reg);
	interval->is_killed = false;
	interval->frozen = false;
}

static void
ra_interval_dump(struct ra_interval *interval)
{
	printf("physreg %u ", interval->physreg_start);

	ir3_reg_interval_dump(&interval->interval);
}

static void
ra_file_dump(struct ra_file *file)
{
	rb_tree_foreach(struct ra_interval, interval, &file->physreg_intervals, physreg_node) {
		ra_interval_dump(interval);
	}

	unsigned start, end;
	printf("available:\n");
	BITSET_FOREACH_RANGE(start, end, file->available, file->size) {
		printf("%u-%u ", start, end);
	}
	printf("\n");

	printf("available to evict:\n");
	BITSET_FOREACH_RANGE(start, end, file->available_to_evict, file->size) {
		printf("%u-%u ", start, end);
	}
	printf("\n");
	printf("start: %u\n", file->start);
}

static void
ra_ctx_dump(struct ra_ctx *ctx)
{
	printf("full:\n");
	ra_file_dump(&ctx->full);
	printf("half:\n");
	ra_file_dump(&ctx->half);
	printf("shared:\n");
	ra_file_dump(&ctx->shared);
}

static unsigned
reg_file_size(struct ra_file *file, struct ir3_register *reg)
{
	/* Half-regs can only take up the first half of the combined regfile */
	if (reg->flags & IR3_REG_HALF)
		return MIN2(file->size, RA_HALF_SIZE);
	else
		return file->size;
}

/* ra_pop_interval/ra_push_interval provide an API to shuffle around multiple
 * top-level intervals at once. Pop multiple intervals, then push them back in
 * any order.
 */

struct ra_removed_interval {
	struct ra_interval *interval;
	unsigned size;
};

static struct ra_removed_interval
ra_pop_interval(struct ra_ctx *ctx, struct ra_file *file,
		        struct ra_interval *interval)
{
	assert(!interval->interval.parent);

	/* Check if we've already moved this reg before */
	unsigned pcopy_index;
	for (pcopy_index = 0; pcopy_index < ctx->parallel_copies_count; pcopy_index++) {
		if (ctx->parallel_copies[pcopy_index].interval == interval)
			break;
	}

	if (pcopy_index == ctx->parallel_copies_count) {
		array_insert(ctx, ctx->parallel_copies, (struct ra_parallel_copy) {
				.interval = interval,
				.src = interval->physreg_start,
		});
	}

	ir3_reg_interval_remove_all(&file->reg_ctx, &interval->interval);

	return (struct ra_removed_interval) {
		.interval = interval,
		.size = interval->physreg_end - interval->physreg_start,
	};
}

static void
ra_push_interval(struct ra_ctx *ctx, struct ra_file *file,
				 const struct ra_removed_interval *removed, physreg_t dst)
{
	struct ra_interval *interval = removed->interval;

	interval->physreg_start = dst;
	interval->physreg_end = dst + removed->size;

	ir3_reg_interval_insert(&file->reg_ctx, &interval->interval);
}

/* Pick up the interval and place it at "dst". */
static void
ra_move_interval(struct ra_ctx *ctx, struct ra_file *file,
				 struct ra_interval *interval, physreg_t dst)
{
	struct ra_removed_interval temp = ra_pop_interval(ctx, file, interval);
	ra_push_interval(ctx, file, &temp, dst);
}

static bool
get_reg_specified(struct ra_file *file, struct ir3_register *reg, physreg_t physreg, bool is_source)
{
	for (unsigned i = 0; i < reg_size(reg); i++) {
		if (!BITSET_TEST(is_source ? file->available_to_evict : file->available, physreg + i))
			return false;
	}

	return true;
}

/* Try to evict any registers conflicting with the proposed spot "physreg" for
 * "reg". That is, move them to other places so that we can allocate "physreg"
 * here.
 */

static bool
try_evict_regs(struct ra_ctx *ctx, struct ra_file *file,
		       struct ir3_register *reg, physreg_t physreg,
			   unsigned *_eviction_count, bool is_source, bool speculative)
{
	BITSET_DECLARE(available_to_evict, RA_MAX_FILE_SIZE);
	memcpy(available_to_evict, file->available_to_evict, sizeof(available_to_evict));

	for (unsigned i = 0; i < reg_size(reg); i++)
		BITSET_CLEAR(available_to_evict, physreg + i);
	
	unsigned eviction_count = 0;
	/* Iterate over each range conflicting with physreg */
	for (struct ra_interval *conflicting = ra_file_search_right(file, physreg),
		 *next = ra_interval_next_or_null(conflicting);
		 conflicting != NULL && conflicting->physreg_start < physreg + reg_size(reg);
		 conflicting = next, next = ra_interval_next_or_null(next)) {
		if (!is_source && conflicting->is_killed)
			continue;

		if (conflicting->frozen) {
			assert(speculative);
			return false;
		}

		unsigned avail_start, avail_end;
		bool evicted = false;
		BITSET_FOREACH_RANGE(avail_start, avail_end, available_to_evict,
							 reg_file_size(file, conflicting->interval.reg)) {
			unsigned size = avail_end - avail_start;

			/* non-half registers must be aligned */
			if (!(conflicting->interval.reg->flags & IR3_REG_HALF) && avail_start % 2 == 1) {
				avail_start++;
				size--;
			}

			if (size >= conflicting->physreg_end - conflicting->physreg_start) {
				for (unsigned i = 0; i < conflicting->physreg_end - conflicting->physreg_start; i++)
					BITSET_CLEAR(available_to_evict, avail_start + i);
				eviction_count += conflicting->physreg_end - conflicting->physreg_start;
				if (!speculative)
					ra_move_interval(ctx, file, conflicting, avail_start);
				evicted = true;
				break;
			}
		}

		if (!evicted)
			return false;
	}

	*_eviction_count = eviction_count;
	return true;
}

static int removed_interval_cmp(const void *_i1, const void *_i2)
{
	const struct ra_removed_interval *i1 = _i1;
	const struct ra_removed_interval *i2 = _i2;

	/* We sort the registers as follows:
	 *
	 * |--------------------------------------------------------------------|
	 * |                    |             |             |                   |
	 * |  Half live-through | Half killed | Full killed | Full live-through |
	 * |                    |             |             |                   |
	 * |--------------------------------------------------------------------|
	 *                        |                 |
	 *                        |   Destination   |
	 *                        |                 |
	 *                        |-----------------|
	 *
	 * Half-registers have to be first so that they stay in the low half of
	 * the register file. Then half and full killed must stay together so that
	 * there's a contiguous range where we can put the register. With this
	 * structure we should be able to accomodate any collection of intervals
	 * such that the total number of half components is within the half limit
	 * and the combined components are within the full limit.
	 */

	unsigned i1_align = reg_elem_size(i1->interval->interval.reg);
	unsigned i2_align = reg_elem_size(i2->interval->interval.reg);
	if (i1_align > i2_align)
		return 1;
	if (i1_align < i2_align)
		return -1;

	if (i1_align == 1) {
		if (i2->interval->is_killed)
			return -1;
		if (i1->interval->is_killed)
			return 1;
	} else {
		if (i2->interval->is_killed)
			return 1;
		if (i1->interval->is_killed)
			return -1;
	}

	return 0;
}

/* "Compress" all the live intervals so that there is enough space for the
 * destination register. As there can be gaps when a more-aligned interval
 * follows a less-aligned interval, this also sorts them to remove such
 * "padding", which may be required when space is very tight.  This isn't
 * amazing, but should be used only as a last resort in case the register file
 * is almost full and badly fragmented.
 *
 * Return the physreg to use.
 */
static physreg_t
compress_regs_left(struct ra_ctx *ctx, struct ra_file *file, unsigned size,
				   unsigned align, bool is_source)
{
	DECLARE_ARRAY(struct ra_removed_interval, intervals);
	intervals_count = intervals_sz = 0;
	intervals = NULL;

	unsigned removed_full_size = 0;
	unsigned removed_half_size = 0;
	unsigned file_size = align == 1 ? MIN2(file->size, RA_HALF_SIZE) : file->size;
	physreg_t start_reg = 0;

	foreach_interval_rev_safe(interval, file) {
		/* Check if we can sort the intervals *after* this one and have
		 * enough space leftover to accomodate "size" units.
		 */
		if (align == 1) {
			if (interval->physreg_end + removed_half_size <= file_size - size) {
				start_reg = interval->physreg_end;
				break;
			}
		} else {
			if (interval->physreg_end + removed_half_size <= file_size -
					removed_full_size - size) {
				start_reg = interval->physreg_end;
				break;
			}
		}

		/* We assume that all frozen intervals are at the start and that we
		 * can avoid popping them.
		 */
		assert(!interval->frozen);

		/* Killed sources don't count because they go at the end and can
		 * overlap the register we're trying to add.
		 */
		if (!interval->is_killed && !is_source) {
			if (interval->interval.reg->flags & IR3_REG_HALF)
				removed_half_size += interval->physreg_end - interval->physreg_start;
			else
				removed_full_size += interval->physreg_end - interval->physreg_start;
		}

		/* Now that we've done the accounting, pop this off */
		d("popping interval %u physreg %u\n", interval->interval.reg->name, interval->physreg_start);
		array_insert(ctx, intervals, ra_pop_interval(ctx, file, interval));
	}

	/* TODO: In addition to skipping registers at the beginning that are
	 * well-packed, we should try to skip registers at the end.
	 */

	qsort(intervals, intervals_count, sizeof(*intervals), removed_interval_cmp);

	physreg_t physreg = start_reg;
	physreg_t ret_reg = (physreg_t) ~0;
	for (unsigned i = 0; i < intervals_count; i++) {
		if (ret_reg == (physreg_t) ~0 &&
			((intervals[i].interval->is_killed && !is_source) ||
			!(intervals[i].interval->interval.reg->flags & IR3_REG_HALF))) {
			ret_reg = ALIGN(physreg, align);
		}

		if (ret_reg != (physreg_t) ~0 &&
			(is_source || !intervals[i].interval->is_killed)) {
			physreg = MAX2(physreg, ret_reg + size);
		}

		if (!(intervals[i].interval->interval.reg->flags & IR3_REG_HALF)) {
			physreg = ALIGN(physreg, 2);
		}

		if (physreg + intervals[i].size >
			reg_file_size(file, intervals[i].interval->interval.reg)) {
			d("ran out of room for interval %u!\n", intervals[i].interval->interval.reg->name);
			unreachable("reg pressure calculation was wrong!");
			return 0;
		}

		d("pushing interval %u physreg %u\n", intervals[i].interval->interval.reg->name, physreg);
		ra_push_interval(ctx, file, &intervals[i], physreg);

		physreg += intervals[i].size;
	}

	if (ret_reg == (physreg_t) ~0)
		ret_reg = physreg;

	ret_reg = ALIGN(ret_reg, align);
	if (ret_reg + size > file_size) {
		d("ran out of room for the new interval!\n");
		unreachable("reg pressure calculation was wrong!");
		return 0;
	}

	return ret_reg;
}

static void
update_affinity(struct ir3_register *reg, physreg_t physreg)
{
	if (!reg->merge_set || reg->merge_set->preferred_reg != (physreg_t) ~0)
		return;

	if (physreg < reg->merge_set_offset)
		return;

	reg->merge_set->preferred_reg = physreg - reg->merge_set_offset;
}

/* Try to find free space for a register without shuffling anything. This uses
 * a round-robin algorithm to reduce false dependencies.
 */
static physreg_t
find_best_gap(struct ra_file *file, unsigned file_size,
		      unsigned size, unsigned align, bool is_source)
{
	BITSET_WORD *available = is_source ? file->available_to_evict : file->available;

	unsigned start = ALIGN(file->start, align) % (file_size - size + align);
	unsigned candidate = start;
	do {
		bool is_available = true;
		for (unsigned i = 0; i < size; i++) {
			if (!BITSET_TEST(available, candidate + i)) {
				is_available = false;
				break;
			}
		}

		if (is_available) {
			file->start = (candidate + size) % file_size;
			return candidate;
		}

		candidate += align;
		if (candidate + size > file_size)
			candidate = 0;
	} while (candidate != start);
	
	return (physreg_t) ~0;
}

static struct ra_file *
ra_get_file(struct ra_ctx *ctx, struct ir3_register *reg)
{
	if (reg->flags & IR3_REG_SHARED)
		return &ctx->shared;
	else if (ctx->merged_regs || !(reg->flags & IR3_REG_HALF))
		return &ctx->full;
	else
		return &ctx->half;
}

/* This is the main entrypoint for picking a register. Pick a free register
 * for "reg", shuffling around sources if necessary. In the normal case where
 * "is_source" is false, this register can overlap with killed sources
 * (intervals with "is_killed == true"). If "is_source" is true, then
 * is_killed is ignored and the register returned must not overlap with killed
 * sources. This must be used for tied registers, because we're actually
 * allocating the destination and the tied source at the same time.
 */

static physreg_t
get_reg(struct ra_ctx *ctx, struct ra_file *file, struct ir3_register *reg,
		bool is_source)
{
	unsigned file_size = reg_file_size(file, reg);
	if (reg->merge_set && reg->merge_set->preferred_reg != (physreg_t) ~0) {
		physreg_t preferred_reg =
			reg->merge_set->preferred_reg + reg->merge_set_offset;
		if (preferred_reg < file_size &&
			preferred_reg % reg_elem_size(reg) == 0 &&
			get_reg_specified(file, reg, preferred_reg, is_source))
			return preferred_reg;
	}

	/* If this register is a subset of a merge set which we have not picked a
	 * register for, first try to allocate enough space for the entire merge
	 * set.
	 */
	unsigned size = reg_size(reg);
	if (reg->merge_set && reg->merge_set->preferred_reg == (physreg_t)~0 &&
		size < reg->merge_set->size) {
		physreg_t best_reg =
			find_best_gap(file, file_size, reg->merge_set->size, reg->merge_set->alignment, is_source);
		if (best_reg != (physreg_t) ~0u) {
			best_reg += reg->merge_set_offset;
			return best_reg;
		}
	}

	/* For ALU and SFU instructions, if the src reg is avail to pick, use it.
	 * Because this doesn't introduce unnecessary dependencies, and it
	 * potentially avoids needing (ss) syncs for write after read hazards for
	 * SFU instructions:
	 */
	if (is_sfu(reg->instr) || is_alu(reg->instr)) {
		for (unsigned i = 0; i < reg->instr->srcs_count; i++) {
			struct ir3_register *src = reg->instr->srcs[i];
			if (!ra_reg_is_src(src))
				continue;
			if (ra_get_file(ctx, src) == file && reg_size(src) >= size) {
				struct ra_interval *src_interval =
					&ctx->intervals[src->def->name];
				physreg_t src_physreg = ra_interval_get_physreg(src_interval);
				if (src_physreg % reg_elem_size(reg) == 0 &&
					src_physreg + size <= file_size &&
					get_reg_specified(file, reg, src_physreg, is_source))
					return src_physreg;
			}
		}
	}

	physreg_t best_reg =
		find_best_gap(file, file_size, size, reg_elem_size(reg), is_source);
	if (best_reg != (physreg_t) ~0u) {
		return best_reg;
	}

	/* Ok, we couldn't find anything that fits. Here is where we have to start
	 * moving things around to make stuff fit. First try solely evicting
	 * registers in the way.
	 */
	unsigned best_eviction_count = ~0;
	for (physreg_t i = 0; i + size <= file_size; i += reg_elem_size(reg)) {
		unsigned eviction_count;
		if (try_evict_regs(ctx, file, reg, i, &eviction_count, is_source, true)) {
			if (eviction_count < best_eviction_count) {
				best_eviction_count = eviction_count;
				best_reg = i;
			}
		}
	}
	
	if (best_eviction_count != ~0) {
		ASSERTED bool result =
			try_evict_regs(ctx, file, reg, best_reg, &best_eviction_count, is_source, false);
		assert(result);
		return best_reg;
	}

	/* Use the dumb fallback only if try_evict_regs() fails. */
	return compress_regs_left(ctx, file, reg_size(reg), reg_elem_size(reg), is_source);
}

static void
assign_reg(struct ir3_instruction *instr, struct ir3_register *reg, unsigned num)
{
	if (reg->flags & IR3_REG_ARRAY) {
		reg->array.base = num;
		if (reg->flags & IR3_REG_RELATIV)
			reg->array.offset += num;
		else
			reg->num = num + reg->array.offset;
	} else {
		reg->num = num;
	}
}

static void
mark_src_killed(struct ra_ctx *ctx, struct ir3_register *src)
{
	struct ra_interval *interval = &ctx->intervals[src->def->name];

	if (!(src->flags & IR3_REG_FIRST_KILL) || interval->is_killed ||
		interval->interval.parent || !rb_tree_is_empty(&interval->interval.children))
		return;
	
	ra_file_mark_killed(ra_get_file(ctx, src), interval);
}

static void
insert_dst(struct ra_ctx *ctx, struct ir3_register *dst)
{
	struct ra_file *file = ra_get_file(ctx, dst);
	struct ra_interval *interval = &ctx->intervals[dst->name];

	d("insert dst %u physreg %u", dst->name, ra_interval_get_physreg(interval));

	if (!(dst->flags & IR3_REG_UNUSED))
		ra_file_insert(file, interval);

	assign_reg(dst->instr, dst, ra_interval_get_num(interval));
}

static void
allocate_dst_fixed(struct ra_ctx *ctx, struct ir3_register *dst, physreg_t physreg)
{
	struct ra_interval *interval = &ctx->intervals[dst->name];
	update_affinity(dst, physreg);

	ra_interval_init(interval, dst);
	interval->physreg_start = physreg;
	interval->physreg_end = physreg + reg_size(dst);
}

static void
allocate_dst(struct ra_ctx *ctx, struct ir3_register *dst)
{
	struct ra_file *file = ra_get_file(ctx, dst);

	struct ir3_register *tied = dst->tied;
	if (tied) {
		struct ra_interval *tied_interval = &ctx->intervals[tied->def->name];
		struct ra_interval *dst_interval = &ctx->intervals[dst->name];
		physreg_t tied_physreg = ra_interval_get_physreg(tied_interval);
		if (tied_interval->is_killed) {
			/* The easy case: the source is killed, so we can just reuse it
			 * for the destination.
			 */
			allocate_dst_fixed(ctx, dst, ra_interval_get_physreg(tied_interval));
		} else {
			/* The source is live-through, so we need to get a free register
			 * (which is free for both the source and destination!), copy the
			 * original source to it, then use that for the source and
			 * destination.
			 */
			physreg_t physreg = get_reg(ctx, file, dst, true);
			allocate_dst_fixed(ctx, dst, physreg);
			array_insert(ctx, ctx->parallel_copies, (struct ra_parallel_copy) {
					.interval = dst_interval,
					.src = tied_physreg,
			});
		}

		return;
	}

	/* All the hard work is done by get_reg here. */
	physreg_t physreg = get_reg(ctx, file, dst, false);

	allocate_dst_fixed(ctx, dst, physreg);
}

static void
assign_src(struct ra_ctx *ctx, struct ir3_instruction *instr, struct ir3_register *src)
{
	struct ra_interval *interval = &ctx->intervals[src->def->name];
	struct ra_file *file = ra_get_file(ctx, src);

	struct ir3_register *tied = src->tied;
	physreg_t physreg;
	if (tied) {
		struct ra_interval *tied_interval = &ctx->intervals[tied->name];
		physreg = ra_interval_get_physreg(tied_interval);
	} else {
		physreg = ra_interval_get_physreg(interval);
	}

	assign_reg(instr, src, ra_physreg_to_num(physreg, src->flags));

	if (src->flags & IR3_REG_FIRST_KILL)
		ra_file_remove(file, interval);
}

/* Insert a parallel copy instruction before the instruction with the parallel
 * copy entries we've built up.
 */
static void
insert_parallel_copy_instr(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	if (ctx->parallel_copies_count == 0)
		return;

	struct ir3_instruction *pcopy =
		ir3_instr_create(instr->block, OPC_META_PARALLEL_COPY,
						 ctx->parallel_copies_count,
						 ctx->parallel_copies_count);

	for (unsigned i = 0; i < ctx->parallel_copies_count; i++) {
		struct ra_parallel_copy *entry = &ctx->parallel_copies[i];
		struct ir3_register *reg =
			ir3_dst_create(pcopy, INVALID_REG,
						   entry->interval->interval.reg->flags & ~IR3_REG_SSA);
		reg->size = entry->interval->interval.reg->size;
		reg->wrmask = entry->interval->interval.reg->wrmask;
		assign_reg(pcopy, reg, ra_interval_get_num(entry->interval));
	}

	for (unsigned i = 0; i < ctx->parallel_copies_count; i++) {
		struct ra_parallel_copy *entry = &ctx->parallel_copies[i];
		struct ir3_register *reg =
			ir3_src_create(pcopy, INVALID_REG,
						   entry->interval->interval.reg->flags & ~IR3_REG_SSA);
		reg->size = entry->interval->interval.reg->size;
		reg->wrmask = entry->interval->interval.reg->wrmask;
		assign_reg(pcopy, reg, ra_physreg_to_num(entry->src, reg->flags));
	}

	list_del(&pcopy->node);
	list_addtail(&pcopy->node, &instr->node);
	ctx->parallel_copies_count = 0;
}

static void
handle_normal_instr(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	/* First, mark sources as going-to-be-killed while allocating the dest. */
	ra_foreach_src(src, instr) {
		mark_src_killed(ctx, src);
	}

	/* Allocate the destination. */
	ra_foreach_dst(dst, instr) {
		allocate_dst(ctx, dst);
	}

	/* Now handle sources. Go backward so that in case there are multiple
	 * sources with the same def and that def is killed we only remove it at
	 * the end.
	 */
	ra_foreach_src_rev(src, instr) {
		assign_src(ctx, instr, src);
	}

	/* Now finally insert the destination into the map. */
	ra_foreach_dst(dst, instr) {
		insert_dst(ctx, dst);
	}

	insert_parallel_copy_instr(ctx, instr);
}

static void
handle_split(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	struct ir3_register *dst = instr->dsts[0];
	struct ir3_register *src = instr->srcs[0];

	if (dst->merge_set == NULL || src->def->merge_set != dst->merge_set) {
		handle_normal_instr(ctx, instr);
		return;
	}

	struct ra_interval *src_interval = &ctx->intervals[src->def->name];

	physreg_t physreg = ra_interval_get_physreg(src_interval);
	assign_src(ctx, instr, src);

	allocate_dst_fixed(ctx, dst, physreg - src->def->merge_set_offset + dst->merge_set_offset);
	insert_dst(ctx, dst);
}

static void
handle_collect(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	struct ir3_merge_set *dst_set = instr->dsts[0]->merge_set;
	unsigned dst_offset = instr->dsts[0]->merge_set_offset;
	
	if (!dst_set || dst_set->regs_count == 1) {
		handle_normal_instr(ctx, instr);
		return;
	}

	/* We need to check if any of the sources are contained in an interval
	 * that is at least as large as the vector. In this case, we should put
	 * the vector inside that larger interval. (There should be one
	 * unambiguous place to put it, because values sharing the same merge set
	 * should be allocated together.) This can happen in a case like:
	 *
	 * ssa_1 (wrmask=0xf) = ...
	 * ssa_2 = split ssa_1 off:0
	 * ssa_3 = split ssa_1 off:1
	 * ssa_4 (wrmask=0x3) = collect (kill)ssa_2, (kill)ssa_3
	 * ... = (kill)ssa_1
	 * ... = (kill)ssa_4
	 *
	 * ssa_4 will be coalesced with ssa_1 and needs to be allocated inside it.
	 */
	physreg_t dst_fixed = (physreg_t) ~0u;

	for (unsigned i = 0; i < instr->srcs_count; i++) {
		if (!ra_reg_is_src(instr->srcs[i]))
			continue;

		if (instr->srcs[i]->flags & IR3_REG_FIRST_KILL) {
			mark_src_killed(ctx, instr->srcs[i]);
		}

		struct ir3_register *src = instr->srcs[i];
		struct ra_interval *interval = &ctx->intervals[src->def->name];

		if (src->def->merge_set != dst_set || interval->is_killed)
			continue;
		while (interval->interval.parent != NULL) {
			interval = ir3_reg_interval_to_ra_interval(interval->interval.parent);
		}
		if (reg_size(interval->interval.reg) >= reg_size(instr->dsts[0])) {
			dst_fixed = interval->physreg_start - interval->interval.reg->merge_set_offset + dst_offset;
		} else {
			/* For sources whose root interval is smaller than the
			 * destination (i.e. the normal case), we will shuffle them
			 * around after allocating the destination. Mark them killed so
			 * that the destination can be allocated over them, even if they
			 * aren't actually killed.
			 */
			ra_file_mark_killed(ra_get_file(ctx, src), interval);
		}
	}

	if (dst_fixed != (physreg_t) ~0u)
		allocate_dst_fixed(ctx, instr->dsts[0], dst_fixed);
	else
		allocate_dst(ctx, instr->dsts[0]);

	/* Remove the temporary is_killed we added */
	for (unsigned i = 0; i < instr->srcs_count; i++) {
		if (!ra_reg_is_src(instr->srcs[i]))
			continue;

		struct ir3_register *src = instr->srcs[i];
		struct ra_interval *interval = &ctx->intervals[src->def->name];
		while (interval->interval.parent != NULL) {
			interval = ir3_reg_interval_to_ra_interval(interval->interval.parent);
		}

		/* Filter out cases where it actually should be killed */
		if (interval != &ctx->intervals[src->def->name] ||
			!(src->flags & IR3_REG_KILL))
			interval->is_killed = false;
	}


	ra_foreach_src_rev(src, instr) {
		assign_src(ctx, instr, src);
	}

	/* We need to do this before insert_dst(), so that children of the
	 * destination which got marked as killed and then shuffled around to make
	 * space for the destination have the correct pcopy destination that
	 * matches what we assign the source of the collect to in assign_src().
	 *
	 * TODO: In this case we'll wind up copying the value in the pcopy and
	 * then again in the collect. We could avoid one of those by updating the
	 * pcopy destination to match up with the final location of the source
	 * after the collect and making the collect a no-op. However this doesn't
	 * seem to happen often.
	 */
	insert_parallel_copy_instr(ctx, instr);

	/* Note: insert_dst will automatically shuffle around any intervals that
	 * are a child of the collect by making them children of the collect.
	 */

	insert_dst(ctx, instr->dsts[0]);
}

/* Parallel copies before RA should only be at the end of the block, for
 * phi's. For these we only need to fill in the sources, and then we fill in
 * the destinations in the successor block.
 */
static void
handle_pcopy(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	ra_foreach_src_rev(src, instr) {
		assign_src(ctx, instr, src);
	}
}

/* Some inputs may need to be precolored. We need to handle those first, so
 * that other non-precolored inputs don't accidentally get allocated over
 * them. Inputs are the very first thing in the shader, so it shouldn't be a
 * problem to allocate them to a specific physreg.
 */

static void
handle_precolored_input(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	if (instr->dsts[0]->num == INVALID_REG)
		return;

	struct ra_interval *interval = &ctx->intervals[instr->dsts[0]->name];
	physreg_t physreg = ra_reg_get_physreg(instr->dsts[0]);
	allocate_dst_fixed(ctx, instr->dsts[0], physreg);
	insert_dst(ctx, instr->dsts[0]);
	interval->frozen = true;
}

static void
handle_input(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	if (instr->dsts[0]->num != INVALID_REG)
		return;

	allocate_dst(ctx, instr->dsts[0]);

	struct ra_file *file = ra_get_file(ctx, instr->dsts[0]);
	struct ra_interval *interval = &ctx->intervals[instr->dsts[0]->name];
	ra_file_insert(file, interval);
}

static void
assign_input(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	struct ra_interval *interval = &ctx->intervals[instr->dsts[0]->name];
	struct ra_file *file = ra_get_file(ctx, instr->dsts[0]);

	if (instr->dsts[0]->num == INVALID_REG) {
		assign_reg(instr, instr->dsts[0], ra_interval_get_num(interval));
	} else {
		interval->frozen = false;
	}

	if (instr->dsts[0]->flags & IR3_REG_UNUSED)
		ra_file_remove(file, interval);

	ra_foreach_src_rev(src, instr)
		assign_src(ctx, instr, src);
}

/* chmask is a bit weird, because it has pre-colored sources due to the need
 * to pass some registers to the next stage. Fortunately there are only at
 * most two, and there should be no other live values by the time we get to
 * this instruction, so we only have to do the minimum and don't need any
 * fancy fallbacks.
 *
 * TODO: Add more complete handling of precolored sources, e.g. for function
 * argument handling. We'd need a way to mark sources as fixed so that they
 * don't get moved around when placing other sources in the fallback case, and
 * a duplication of much of the logic in get_reg(). This also opens another
 * can of worms, e.g. what if the precolored source is a split of a vector
 * which is still live -- this breaks our assumption that splits don't incur
 * any "extra" register requirements and we'd have to break it out of the
 * parent ra_interval.
 */

static void
handle_precolored_source(struct ra_ctx *ctx, struct ir3_register *src)
{
	struct ra_file *file = ra_get_file(ctx, src);
	struct ra_interval *interval = &ctx->intervals[src->def->name];
	physreg_t physreg = ra_reg_get_physreg(src);

	if (ra_interval_get_num(interval) == src->num)
		return;

	/* Try evicting stuff in our way if it isn't free. This won't move
	 * anything unless it overlaps with our precolored physreg, so we don't
	 * have to worry about evicting other precolored sources.
	 */
	if (!get_reg_specified(file, src, physreg, true)) {
		unsigned eviction_count;
		if (!try_evict_regs(ctx, file, src, physreg, &eviction_count, true, false)) {
			unreachable("failed to evict for precolored source!");
			return;
		}
	}

	ra_move_interval(ctx, file, interval, physreg);
}

static void
handle_chmask(struct ra_ctx *ctx, struct ir3_instruction *instr)
{
	/* Note: we purposely don't mark sources as killed, so that we can reuse
	 * some of the get_reg() machinery as-if the source is a destination.
	 * Marking it as killed would make e.g. get_reg_specified() wouldn't work
	 * correctly.
	 */
	ra_foreach_src(src, instr) {
		assert(src->num != INVALID_REG);
		handle_precolored_source(ctx, src);
	}

	ra_foreach_src(src, instr) {
		struct ra_file *file = ra_get_file(ctx, src);
		struct ra_interval *interval = &ctx->intervals[src->def->name];
		if (src->flags & IR3_REG_FIRST_KILL)
			ra_file_remove(file, interval);
	}

	/* add dummy destination for validation */
	assign_reg(instr, instr->dsts[0], 0);

	insert_parallel_copy_instr(ctx, instr);
}

static physreg_t
read_register(struct ra_ctx *ctx, struct ir3_block *block, struct ir3_register *def)
{
	struct ra_block_state *state = &ctx->blocks[block->index];
	if (state->renames) {
		struct hash_entry *entry = _mesa_hash_table_search(state->renames, def);
		if (entry) {
			return (physreg_t)(uintptr_t)entry->data;
		}
	}

	return ra_reg_get_physreg(def);
}

static void
handle_live_in(struct ra_ctx *ctx, struct ir3_register *def)
{
	physreg_t physreg = ~0;
	for (unsigned i = 0; i < ctx->block->predecessors_count; i++) {
		struct ir3_block *pred = ctx->block->predecessors[i];
		struct ra_block_state *pred_state = &ctx->blocks[pred->index];

		if (!pred_state->visited)
			continue;

		physreg = read_register(ctx, pred, def);
		break;
	}

	assert(physreg != (physreg_t)~0);

	struct ra_interval *interval = &ctx->intervals[def->name];
	struct ra_file *file = ra_get_file(ctx, def);
	ra_interval_init(interval, def);
	interval->physreg_start = physreg;
	interval->physreg_end = physreg + reg_size(def);
	ra_file_insert(file, interval);
}

static void
handle_live_out(struct ra_ctx *ctx, struct ir3_register *def)
{
	/* Skip parallelcopy's which in the original program are only used as phi
	 * arguments. Even though phi arguments are live out, they are only
	 * assigned when the phi is.
	 */
	if (def->instr->opc == OPC_META_PARALLEL_COPY)
		return;

	struct ra_block_state *state = &ctx->blocks[ctx->block->index];
	struct ra_interval *interval = &ctx->intervals[def->name];
	physreg_t physreg = ra_interval_get_physreg(interval);
	if (physreg != ra_reg_get_physreg(def)) {
		if (!state->renames)
			state->renames = _mesa_pointer_hash_table_create(ctx);
		_mesa_hash_table_insert(state->renames, def, (void *)(uintptr_t)physreg);
	}
}

static void
handle_phi(struct ra_ctx *ctx, struct ir3_register *def)
{
	struct ra_file *file = ra_get_file(ctx, def);
	struct ra_interval *interval = &ctx->intervals[def->name];

	/* phis are always scalar, so they should already be the smallest possible 
	 * size. However they may be coalesced with other live-in values/phi
	 * nodes, so check for that here.
	 */
	struct ir3_reg_interval *parent_ir3 =
		ir3_reg_interval_search(&file->reg_ctx.intervals, def->interval_start);
	physreg_t physreg;
	if (parent_ir3) {
		struct ra_interval *parent = ir3_reg_interval_to_ra_interval(parent_ir3);
		physreg = ra_interval_get_physreg(parent) +
			(def->interval_start - parent_ir3->reg->interval_start);
	} else {
		physreg = get_reg(ctx, file, def, false);
	}

	allocate_dst_fixed(ctx, def, physreg);

	ra_file_insert(file, interval);
}

static void
assign_phi(struct ra_ctx *ctx, struct ir3_instruction *phi)
{
	struct ra_file *file = ra_get_file(ctx, phi->dsts[0]);
	struct ra_interval *interval = &ctx->intervals[phi->dsts[0]->name];
	assert(!interval->interval.parent);
	unsigned num = ra_interval_get_num(interval);
	assign_reg(phi, phi->dsts[0], num);

	/* Assign the parallelcopy sources of this phi */
	for (unsigned i = 0; i < phi->srcs_count; i++) {
		if (phi->srcs[i]->def) {
			assign_reg(phi, phi->srcs[i], num);
			assign_reg(phi, phi->srcs[i]->def, num);
		}
	}

	if (phi->dsts[0]->flags & IR3_REG_UNUSED)
		ra_file_remove(file, interval);
}

/* When we split a live range, we sometimes need to emit fixup code at the end
 * of a block. For example, something like:
 *
 * a = ...
 * if (...) {
 *    ...
 *    a' = a
 *    b = ... // a evicted to make room for b
 *    ...
 * }
 * ... = a
 *
 * When we insert the copy to a' in insert_parallel_copy_instr(), this forces
 * to insert another copy "a = a'" at the end of the if. Normally this would
 * also entail adding a phi node, but since we're about to go out of SSA
 * anyway we just insert an extra move. Note, however, that "b" might be used
 * in a phi node at the end of the if and share registers with "a", so we
 * have to be careful to extend any preexisting parallelcopy instruction
 * instead of creating our own in order to guarantee that they properly get
 * swapped.
 */

static void
insert_liveout_copy(struct ir3_block *block, physreg_t dst, physreg_t src,
					struct ir3_register *reg)
{
	struct ir3_instruction *old_pcopy = NULL;
	if (!list_is_empty(&block->instr_list)) {
		struct ir3_instruction *last =
			LIST_ENTRY(struct ir3_instruction, block->instr_list.prev, node);
		if (last->opc == OPC_META_PARALLEL_COPY)
			old_pcopy = last;
	}

	unsigned old_pcopy_srcs = old_pcopy ? old_pcopy->srcs_count : 0;
	struct ir3_instruction *pcopy =
		ir3_instr_create(block, OPC_META_PARALLEL_COPY,
						 old_pcopy_srcs + 1, old_pcopy_srcs + 1);

	for (unsigned i = 0; i < old_pcopy_srcs; i++) {
		old_pcopy->dsts[i]->instr = pcopy;
		pcopy->dsts[pcopy->dsts_count++] = old_pcopy->dsts[i];
	}

	struct ir3_register *dst_reg =
		ir3_dst_create(pcopy, INVALID_REG,
					   reg->flags & ~IR3_REG_SSA);
	dst_reg->wrmask = reg->wrmask;
	dst_reg->size = reg->size;
	assign_reg(pcopy, dst_reg, ra_physreg_to_num(dst, reg->flags));

	for (unsigned i = 0; i < old_pcopy_srcs; i++) {
		pcopy->srcs[pcopy->srcs_count++] = old_pcopy->srcs[i];
	}

	struct ir3_register *src_reg =
		ir3_src_create(pcopy, INVALID_REG, reg->flags & ~IR3_REG_SSA);
	src_reg->wrmask = reg->wrmask;
	src_reg->size = reg->size;
	assign_reg(pcopy, src_reg, ra_physreg_to_num(src, reg->flags));

	if (old_pcopy)
		list_del(&old_pcopy->node);
}

static void
insert_live_in_move(struct ra_ctx *ctx, struct ra_interval *interval)
{
	physreg_t physreg = ra_interval_get_physreg(interval);
	
	for (unsigned i = 0; i < ctx->block->predecessors_count; i++) {
		struct ir3_block *pred = ctx->block->predecessors[i];
		struct ra_block_state *pred_state = &ctx->blocks[pred->index];

		if (!pred_state->visited)
			continue;

		physreg_t pred_reg = read_register(ctx, pred, interval->interval.reg);
		if (pred_reg != physreg) {
			insert_liveout_copy(pred, physreg, pred_reg, interval->interval.reg);
		}
	}
}

static void
insert_file_live_in_moves(struct ra_ctx *ctx, struct ra_file *file)
{
	BITSET_WORD *live_in = ctx->live->live_in[ctx->block->index];
	rb_tree_foreach(struct ra_interval, interval, &file->physreg_intervals, physreg_node) {
		/* Skip phi nodes. This needs to happen after phi nodes are allocated,
		 * because we may have to move live-ins around to make space for phi
		 * nodes, but we shouldn't be handling phi nodes here.
		 */
		if (BITSET_TEST(live_in, interval->interval.reg->name))
			insert_live_in_move(ctx, interval);
	}
}

static void
insert_entry_regs(struct ra_block_state *state, struct ra_file *file)
{
	rb_tree_foreach(struct ra_interval, interval, &file->physreg_intervals, physreg_node) {
		_mesa_hash_table_insert(state->entry_regs, interval->interval.reg,
				(void *)(uintptr_t)interval->physreg_start);
	}
}

static void
insert_live_in_moves(struct ra_ctx *ctx)
{
	insert_file_live_in_moves(ctx, &ctx->full);
	insert_file_live_in_moves(ctx, &ctx->half);
	insert_file_live_in_moves(ctx, &ctx->shared);

	/* If not all predecessors are visited, insert live-in regs so that
	 * insert_live_out_moves() will work.
	 */
	bool all_preds_visited = true;
	for (unsigned i = 0; i < ctx->block->predecessors_count; i++) {
		if (!ctx->blocks[ctx->block->predecessors[i]->index].visited) {
			all_preds_visited = false;
			break;
		}
	}

	if (!all_preds_visited) {
		struct ra_block_state *state = &ctx->blocks[ctx->block->index];
		state->entry_regs = _mesa_pointer_hash_table_create(ctx);
		
		insert_entry_regs(state, &ctx->full);
		insert_entry_regs(state, &ctx->half);
		insert_entry_regs(state, &ctx->shared);
	}
}

static void
insert_live_out_move(struct ra_ctx *ctx, struct ra_interval *interval)
{
	for (unsigned i = 0; i < 2; i++) {
		if (!ctx->block->successors[i])
			continue;

		struct ir3_block *succ = ctx->block->successors[i];
		struct ra_block_state *succ_state = &ctx->blocks[succ->index];

		if (!succ_state->visited)
			continue;

		struct hash_entry *entry =
			_mesa_hash_table_search(succ_state->entry_regs, interval->interval.reg);
		if (!entry)
			continue;

		physreg_t new_reg = (physreg_t)(uintptr_t)entry->data;
		if (new_reg != interval->physreg_start) {
			insert_liveout_copy(ctx->block, new_reg, interval->physreg_start,
								interval->interval.reg);
		}
	}
}

static void
insert_file_live_out_moves(struct ra_ctx *ctx, struct ra_file *file)
{
	rb_tree_foreach(struct ra_interval, interval, &file->physreg_intervals, physreg_node) {
		insert_live_out_move(ctx, interval);
	}
}

static void
insert_live_out_moves(struct ra_ctx *ctx)
{
	insert_file_live_out_moves(ctx, &ctx->full);
	insert_file_live_out_moves(ctx, &ctx->half);
	insert_file_live_out_moves(ctx, &ctx->shared);
}

static void
handle_block(struct ra_ctx *ctx, struct ir3_block *block)
{
	ctx->block = block;

	/* Reset the register files from the last block */
	ra_file_init(&ctx->full);
	ra_file_init(&ctx->half);
	ra_file_init(&ctx->shared);

	/* Handle live-ins, phis, and input meta-instructions. These all appear
	 * live at the beginning of the block, and interfere with each other
	 * therefore need to be allocated "in parallel". This means that we
	 * have to allocate all of them, inserting them into the file, and then
	 * delay updating the IR until all of them are allocated.
	 *
	 * Handle precolored inputs first, because we need to make sure that other
	 * inputs don't overwrite them. We shouldn't have both live-ins/phi nodes
	 * and inputs at the same time, because the first block doesn't have
	 * predecessors. Therefore handle_live_in doesn't have to worry about
	 * them.
	 */

	foreach_instr (instr, &block->instr_list) {
		if (instr->opc == OPC_META_INPUT)
			handle_precolored_input(ctx, instr);
		else
			break;
	}

	unsigned name;
	BITSET_FOREACH_SET(name, ctx->live->live_in[block->index],
					   ctx->live->definitions_count) {
		struct ir3_register *reg = ctx->live->definitions[name];
		handle_live_in(ctx, reg);
	}

	foreach_instr (instr, &block->instr_list) {
		if (instr->opc == OPC_META_PHI)
			handle_phi(ctx, instr->dsts[0]);
		else if (instr->opc == OPC_META_INPUT || instr->opc == OPC_META_TEX_PREFETCH)
			handle_input(ctx, instr);
		else
			break;
	}

	/* After this point, every live-in/phi/input has an interval assigned to
	 * it. We delay actually assigning values until everything has been
	 * allocated, so we can simply ignore any parallel copy entries created
	 * when shuffling them around.
	 */
	ctx->parallel_copies_count = 0;

	insert_live_in_moves(ctx);

	if (RA_DEBUG) {
		printf("after live-in block %u:\n", block->index);
		ra_ctx_dump(ctx);
	}

	/* Now we're done with processing live-ins, and can handle the body of the
	 * block.
	 */
	foreach_instr (instr, &block->instr_list) {
		if (RA_DEBUG) {
			printf("processing: ");
			ir3_print_instr(instr);
		}

		if (instr->opc == OPC_META_PHI)
			assign_phi(ctx, instr);
		else if (instr->opc == OPC_META_INPUT || instr->opc == OPC_META_TEX_PREFETCH)
			assign_input(ctx, instr);
		else if (instr->opc == OPC_META_SPLIT)
			handle_split(ctx, instr);
		else if (instr->opc == OPC_META_COLLECT)
			handle_collect(ctx, instr);
		else if (instr->opc == OPC_META_PARALLEL_COPY)
			handle_pcopy(ctx, instr);
		else if (instr->opc == OPC_CHMASK)
			handle_chmask(ctx, instr);
		else
			handle_normal_instr(ctx, instr);

		if (RA_DEBUG)
			ra_ctx_dump(ctx);
	}

	insert_live_out_moves(ctx);

	BITSET_FOREACH_SET(name, ctx->live->live_out[block->index],
					   ctx->live->definitions_count) {
		struct ir3_register *reg = ctx->live->definitions[name];
		handle_live_out(ctx, reg);
	}

	ctx->blocks[block->index].visited = true;

	for (unsigned i = 0; i < block->dom_children_count; i++) {
		handle_block(ctx, block->dom_children[i]);
	}
}

static unsigned
calc_target_full_pressure(struct ir3_shader_variant *v, unsigned pressure)
{
	/* Registers are allocated in units of vec4, so switch from units of
	 * half-regs to vec4.
	 */
	unsigned reg_count = DIV_ROUND_UP(pressure, 2 * 4);

	bool double_threadsize = ir3_should_double_threadsize(v, reg_count);

	unsigned target = reg_count;
	unsigned reg_independent_max_waves =
		ir3_get_reg_independent_max_waves(v, double_threadsize);
	unsigned reg_dependent_max_waves =
		ir3_get_reg_dependent_max_waves(v->shader->compiler, reg_count,
				double_threadsize);
	unsigned target_waves =
		MIN2(reg_independent_max_waves, reg_dependent_max_waves);

	while (target <= RA_FULL_SIZE / (2 * 4) &&
		   ir3_should_double_threadsize(v, target) == double_threadsize &&
		   ir3_get_reg_dependent_max_waves(v->shader->compiler, target,
										   double_threadsize) >= target_waves)
		target++;

	return (target - 1) * 2 * 4;
}

int
ir3_ra(struct ir3_shader_variant *v)
{
	ir3_calc_dominance(v->ir);

	ir3_create_parallel_copies(v->ir);

	struct ir3_liveness *live = ir3_calc_liveness(v);

	ir3_debug_print(v->ir, "AFTER: create_parallel_copies");

	ir3_merge_regs(live, v->ir);

	struct ir3_pressure max_pressure;
	ir3_calc_pressure(v, live, &max_pressure);
	d("max pressure:");
	d("\tfull: %u", max_pressure.full);
	d("\thalf: %u", max_pressure.half);
	d("\tshared: %u", max_pressure.shared);

	if (v->mergedregs) {
		max_pressure.full += max_pressure.half;
		max_pressure.half = 0;
	}

	if (max_pressure.full > RA_FULL_SIZE ||
		max_pressure.half > RA_HALF_SIZE ||
		max_pressure.shared > RA_SHARED_SIZE) {
		d("max pressure exceeded!");
		return 1;
	}

	struct ra_ctx *ctx = rzalloc(NULL, struct ra_ctx);

	ctx->merged_regs = v->mergedregs;
	ctx->compiler = v->shader->compiler;
	ctx->stage = v->type;
	ctx->live = live;
	ctx->intervals = rzalloc_array(ctx, struct ra_interval, live->definitions_count);
	ctx->blocks = rzalloc_array(ctx, struct ra_block_state, live->block_count);

	ctx->full.size = calc_target_full_pressure(v, max_pressure.full);
	d("full size: %u", ctx->full.size);
	
	if (!v->mergedregs)
		ctx->half.size = RA_HALF_SIZE;

	ctx->shared.size = RA_SHARED_SIZE;

	handle_block(ctx, ir3_start_block(v->ir));

	ir3_ra_validate(v, ctx->full.size, ctx->half.size, live->block_count);

	/* Strip array-ness and SSA-ness at the end, because various helpers still
	 * need to work even on definitions that have already been assigned. For
	 * example, we need to preserve array-ness so that array live-ins have the
	 * right size.
	 */
	foreach_block (block, &v->ir->block_list) {
		foreach_instr (instr, &block->instr_list) {
			for (unsigned i = 0; i < instr->dsts_count; i++) {
				instr->dsts[i]->flags &= ~IR3_REG_SSA;

				/* Parallel copies of array registers copy the whole register,
				 * and we need some way to let the parallel copy code know
				 * that this was an array whose size is determined by
				 * reg->size. So keep the array flag on those.
				 */
				if (!is_meta(instr))
					instr->dsts[i]->flags &= ~IR3_REG_ARRAY;
			}

			for (unsigned i = 0; i < instr->srcs_count; i++) {
				instr->srcs[i]->flags &= ~IR3_REG_SSA;

				if (!is_meta(instr))
					instr->srcs[i]->flags &= ~IR3_REG_ARRAY;
			}
		}
	}

	ir3_debug_print(v->ir, "AFTER: register allocation");

	ir3_lower_copies(v);

	ir3_debug_print(v->ir, "AFTER: ir3_lower_copies");

	ralloc_free(ctx);
	ralloc_free(live);
	return 0;
}

