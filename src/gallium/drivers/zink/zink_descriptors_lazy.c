/*
 * Copyright © 2021 Valve Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */
#include "tgsi/tgsi_from_mesa.h"



#include "zink_context.h"
#include "zink_compiler.h"
#include "zink_descriptors.h"
#include "zink_program.h"
#include "zink_resource.h"
#include "zink_screen.h"

struct zink_descriptor_data_lazy {
   struct zink_descriptor_data base;
   VkDescriptorUpdateTemplateEntry push_entries[PIPE_SHADER_TYPES];
   bool push_state_changed[2]; //gfx, compute
   uint8_t state_changed[2]; //gfx, compute
};

struct zink_descriptor_pool {
   VkDescriptorPool pool;
   VkDescriptorSet sets[ZINK_DEFAULT_MAX_DESCS];
   unsigned set_idx;
   unsigned sets_alloc;
};

struct zink_batch_descriptor_data_lazy {
   struct zink_batch_descriptor_data base;
   struct hash_table pools[ZINK_DESCRIPTOR_TYPES];
   struct zink_descriptor_pool *push_pool[2];
   struct zink_program *pg[2]; //gfx, compute
   VkDescriptorSetLayout dsl[2][ZINK_DESCRIPTOR_TYPES];
   unsigned push_usage[2];
};

ALWAYS_INLINE static struct zink_descriptor_data_lazy *
dd_lazy(struct zink_context *ctx)
{
   return (struct zink_descriptor_data_lazy*)ctx->dd;
}

ALWAYS_INLINE static struct zink_batch_descriptor_data_lazy *
bdd_lazy(struct zink_batch_state *bs)
{
   return (struct zink_batch_descriptor_data_lazy*)bs->dd;
}

static void
init_template_entry(struct zink_shader *shader, enum zink_descriptor_type type,
                    unsigned idx, unsigned offset, VkDescriptorUpdateTemplateEntry *entry, unsigned *entry_idx, bool flatten_dynamic)
{
    int index = shader->bindings[type][idx].index;
    enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
    entry->dstArrayElement = 0;
    entry->dstBinding = shader->bindings[type][idx].binding;
    if (shader->bindings[type][idx].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && flatten_dynamic)
       /* filter out DYNAMIC type here */
       entry->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    else
       entry->descriptorType = shader->bindings[type][idx].type;
    switch (shader->bindings[type][idx].type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
       entry->descriptorCount = 1;
       entry->offset = offsetof(struct zink_context, di.ubos[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorBufferInfo);
       break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.textures[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorImageInfo);
       break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.tbos[stage][index + offset]);
       entry->stride = sizeof(VkBufferView);
       break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
       entry->descriptorCount = 1;
       entry->offset = offsetof(struct zink_context, di.ssbos[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorBufferInfo);
       break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.images[stage][index + offset]);
       entry->stride = sizeof(VkDescriptorImageInfo);
       break;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
       entry->descriptorCount = shader->bindings[type][idx].size;
       entry->offset = offsetof(struct zink_context, di.texel_images[stage][index + offset]);
       entry->stride = sizeof(VkBufferView);
       break;
    default:
       unreachable("unknown type");
    }
    (*entry_idx)++;
}

bool
zink_descriptor_program_init_lazy(struct zink_context *ctx, struct zink_program *pg)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[ZINK_DESCRIPTOR_TYPES][PIPE_SHADER_TYPES * 32];
   VkDescriptorUpdateTemplateEntry entries[ZINK_DESCRIPTOR_TYPES][PIPE_SHADER_TYPES * 32];
   unsigned num_bindings[ZINK_DESCRIPTOR_TYPES] = {0};
   uint8_t has_bindings = 0;

   struct zink_shader **stages;
   if (pg->is_compute)
      stages = &((struct zink_compute_program*)pg)->shader;
   else
      stages = ((struct zink_gfx_program*)pg)->shaders;

   if (!pg->dd)
      pg->dd = (void*)rzalloc(pg, struct zink_program_descriptor_data);
   if (!pg->dd)
      return false;

   unsigned push_count = 0;
   unsigned entry_idx[ZINK_DESCRIPTOR_TYPES] = {0};

   unsigned num_shaders = pg->is_compute ? 1 : ZINK_SHADER_COUNT;
   bool have_push = screen->info.have_KHR_push_descriptor;
   for (int i = 0; i < num_shaders; i++) {
      struct zink_shader *shader = stages[i];
      if (!shader)
         continue;

      enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
      VkShaderStageFlagBits stage_flags = zink_shader_stage(stage);
      for (int j = 0; j < ZINK_DESCRIPTOR_TYPES; j++) {
         for (int k = 0; k < shader->num_bindings[j]; k++) {
            /* dynamic ubos handled in push */
            if (shader->bindings[j][k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
               pg->dd->push_usage |= BITFIELD64_BIT(stage);

               push_count++;
               continue;
            }

            assert(num_bindings[j] < ARRAY_SIZE(bindings[j]));
            VkDescriptorSetLayoutBinding *binding = &bindings[j][num_bindings[j]];
            binding->binding = shader->bindings[j][k].binding;
            binding->descriptorType = shader->bindings[j][k].type;
            binding->descriptorCount = shader->bindings[j][k].size;
            binding->stageFlags = stage_flags;
            binding->pImmutableSamplers = NULL;

            enum zink_descriptor_size_index idx = zink_vktype_to_size_idx(shader->bindings[j][k].type);
            pg->dd->sizes[idx].descriptorCount += shader->bindings[j][k].size;
            pg->dd->sizes[idx].type = shader->bindings[j][k].type;
            switch (shader->bindings[j][k].type) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
               init_template_entry(shader, j, k, 0, &entries[j][entry_idx[j]], &entry_idx[j], screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY);
               break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
               for (unsigned l = 0; l < shader->bindings[j][k].size; l++)
                  init_template_entry(shader, j, k, l, &entries[j][entry_idx[j]], &entry_idx[j], screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY);
               break;
            default:
               break;
            }
            num_bindings[j]++;
            has_bindings |= BITFIELD_BIT(j);
         }
      }
   }
   pg->dd->binding_usage = has_bindings;
   if (!has_bindings && !push_count) {
      ralloc_free(pg->dd);
      pg->dd = NULL;

      pg->layout = zink_pipeline_layout_create(screen, pg);
      return !!pg->layout;
   }

   pg->dsl[pg->num_dsl++] = push_count ? ctx->dd->push_dsl[pg->is_compute]->layout : ctx->dd->dummy_dsl->layout;
   if (has_bindings) {
      u_foreach_bit(type, has_bindings) {
         for (unsigned i = 0; i < type; i++) {
            /* push set is always 0 */
            if (!pg->dsl[i + 1]) {
               /* inject a null dsl */
               pg->dsl[pg->num_dsl++] = ctx->dd->dummy_dsl->layout;
               pg->dd->binding_usage |= BITFIELD_BIT(i);
            }
         }
         pg->dd->layouts[pg->num_dsl] = zink_descriptor_util_layout_get(ctx, type, bindings[type], num_bindings[type], &pg->dd->layout_key[type]);
         pg->dd->layout_key[type]->use_count++;
         pg->dsl[pg->num_dsl] = pg->dd->layouts[pg->num_dsl]->layout;
         pg->num_dsl++;
      }
      for (unsigned i = 0; i < ARRAY_SIZE(pg->dd->sizes); i++)
         pg->dd->sizes[i].descriptorCount *= ZINK_DEFAULT_MAX_DESCS;
   }

   pg->layout = zink_pipeline_layout_create(screen, pg);
   if (!pg->layout)
      return false;
   if (!screen->info.have_KHR_descriptor_update_template || screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_NOTEMPLATES)
      return true;

   VkDescriptorUpdateTemplateCreateInfo template[ZINK_DESCRIPTOR_TYPES + 1] = {0};
   /* type of template */
   VkDescriptorUpdateTemplateType types[ZINK_DESCRIPTOR_TYPES + 1] = {VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET};
   if (have_push && screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY)
      types[0] = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;

   /* number of descriptors in template */
   unsigned wd_count[ZINK_DESCRIPTOR_TYPES + 1];
   if (push_count)
      wd_count[0] = pg->is_compute ? 1 : ZINK_SHADER_COUNT;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++)
      wd_count[i + 1] = pg->dd->layout_key[i] ? pg->dd->layout_key[i]->num_descriptors : 0;

   VkDescriptorUpdateTemplateEntry *push_entries[2] = {
      dd_lazy(ctx)->push_entries,
      &dd_lazy(ctx)->push_entries[PIPE_SHADER_COMPUTE],
   };
   for (unsigned i = 0; i < pg->num_dsl; i++) {
      bool is_push = i == 0;
      /* no need for empty templates */
      if (pg->dsl[i] == ctx->dd->dummy_dsl->layout ||
          (!is_push && pg->dd->layouts[i]->template))
         continue;
      template[i].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
      assert(wd_count[i]);
      template[i].descriptorUpdateEntryCount = wd_count[i];
      if (is_push)
         template[i].pDescriptorUpdateEntries = push_entries[pg->is_compute];
      else
         template[i].pDescriptorUpdateEntries = entries[i - 1];
      template[i].templateType = types[i];
      template[i].descriptorSetLayout = pg->dsl[i];
      template[i].pipelineBindPoint = pg->is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
      template[i].pipelineLayout = pg->layout;
      template[i].set = i;
      VkDescriptorUpdateTemplateKHR t;
      if (screen->vk.CreateDescriptorUpdateTemplate(screen->dev, &template[i], NULL, &t) != VK_SUCCESS)
         return false;
      if (is_push)
         pg->dd->push_template = t;
      else
         pg->dd->layouts[i]->template = t;
   }
   return true;
}

void
zink_descriptor_program_deinit_lazy(struct zink_screen *screen, struct zink_program *pg)
{
   for (unsigned i = 0; pg->num_dsl && i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (pg->dd->layout_key[i])
         pg->dd->layout_key[i]->use_count--;
   }
   if (pg->dd && pg->dd->push_template)
      screen->vk.DestroyDescriptorUpdateTemplate(screen->dev, pg->dd->push_template, NULL);
   ralloc_free(pg->dd);
}

static VkDescriptorPool
create_pool(struct zink_screen *screen, unsigned num_type_sizes, VkDescriptorPoolSize *sizes, unsigned flags)
{
   VkDescriptorPool pool;
   VkDescriptorPoolCreateInfo dpci = {0};
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = num_type_sizes;
   dpci.flags = flags;
   dpci.maxSets = ZINK_DEFAULT_MAX_DESCS;
   if (vkCreateDescriptorPool(screen->dev, &dpci, 0, &pool) != VK_SUCCESS) {
      debug_printf("vkCreateDescriptorPool failed\n");
      return VK_NULL_HANDLE;
   }
   return pool;
}

static struct zink_descriptor_pool *
get_descriptor_pool_lazy(struct zink_context *ctx, struct zink_program *pg, enum zink_descriptor_type type, struct zink_batch_state *bs)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct hash_entry *he = _mesa_hash_table_search(&bdd_lazy(bs)->pools[type], pg->dd->layout_key[type]);
   if (he)
      return he->data;
   struct zink_descriptor_pool *pool = rzalloc(bs, struct zink_descriptor_pool);
   if (!pool)
      return NULL;
   unsigned idx = zink_descriptor_type_to_size_idx(type);
   VkDescriptorPoolSize *size = &pg->dd->sizes[idx];
   /* this is a sampler/image set with no images only texels */
   if (!size->descriptorCount)
      size++;
   pool->pool = create_pool(screen, zink_descriptor_program_num_sizes(pg, type), size, 0);
   if (!pool->pool) {
      ralloc_free(pool);
      return NULL;
   }
   _mesa_hash_table_insert(&bdd_lazy(bs)->pools[type], pg->dd->layout_key[type], pool);
   return pool;
}

static VkDescriptorSet
get_descriptor_set_lazy(struct zink_context *ctx, struct zink_program *pg, enum zink_descriptor_type type, struct zink_descriptor_pool *pool, bool is_compute)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (!pool)
      return VK_NULL_HANDLE;

   if (pool->set_idx < pool->sets_alloc)
      return pool->sets[pool->set_idx++];

   /* allocate up to $current * 10, e.g., 10 -> 100 or 100 -> 1000 */
   unsigned sets_to_alloc = MIN2(MAX2(pool->sets_alloc * 10, 10), ZINK_DEFAULT_MAX_DESCS) - pool->sets_alloc;
   if (!sets_to_alloc) {//pool full
      zink_fence_wait(&ctx->base);
      return get_descriptor_set_lazy(ctx, pg, type, pool, is_compute);
   }
   if (!zink_descriptor_util_alloc_sets(screen, pg ? pg->dsl[type + 1] : ctx->dd->push_dsl[is_compute]->layout,
                                        pool->pool, &pool->sets[pool->sets_alloc], sets_to_alloc))
      return VK_NULL_HANDLE;
   pool->sets_alloc += sets_to_alloc;
   return pool->sets[pool->set_idx++];
}

static bool
populate_sets(struct zink_context *ctx, struct zink_program *pg, uint8_t *changed_sets, bool need_push, VkDescriptorSet *sets)
{
   struct zink_batch_state *bs = ctx->batch.state;
   if (need_push && !zink_screen(ctx->base.screen)->info.have_KHR_push_descriptor) {
         struct zink_descriptor_pool *pool = bdd_lazy(bs)->push_pool[pg->is_compute];
         sets[0] = get_descriptor_set_lazy(ctx, NULL, 0, pool, pg->is_compute);
         if (!sets[0])
            return false;
   } else
      sets[0] = VK_NULL_HANDLE;
   /* may have flushed */
   if (bs != ctx->batch.state)
      *changed_sets = pg->dd->binding_usage;
   bs = ctx->batch.state;
   u_foreach_bit(type, *changed_sets) {
      if (pg->dd->layout_key[type]) {
         struct zink_descriptor_pool *pool = get_descriptor_pool_lazy(ctx, pg, type, bs);
         sets[type + 1] = get_descriptor_set_lazy(ctx, pg, type, pool, pg->is_compute);
         if (ctx->batch.state != bs && (sets[0] || type != ffs(*changed_sets))) {
               /* sets are allocated by batch state, so if flush occurs on anything
                * but the first set that has been fetched here, get all new sets
                */
               *changed_sets = pg->dd->binding_usage;
               if (pg->dd->push_usage)
                  need_push = true;
               return populate_sets(ctx, pg, changed_sets, need_push, sets);
         }
      } else
         sets[type + 1] = ctx->dd->dummy_set;
      if (!sets[type + 1])
         return false;
   }
   return true;
}

void
zink_descriptor_set_update_lazy(struct zink_context *ctx, struct zink_program *pg, enum zink_descriptor_type type, VkDescriptorSet set)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   screen->vk.UpdateDescriptorSetWithTemplate(screen->dev, set, pg->dd->layouts[type + 1]->template, ctx);
}

void
zink_descriptors_update_lazy(struct zink_context *ctx, bool is_compute)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch *batch = &ctx->batch;
   struct zink_batch_state *bs = ctx->batch.state;
   struct zink_program *pg = is_compute ? &ctx->curr_compute->base : &ctx->curr_program->base;

   bool batch_changed = !bdd_lazy(bs)->pg[is_compute];
   if (batch_changed) {
      /* update all sets and bind null sets */
      dd_lazy(ctx)->state_changed[is_compute] = pg->dd->binding_usage;
      dd_lazy(ctx)->push_state_changed[is_compute] = !!pg->dd->push_usage;
   }

   if (pg != bdd_lazy(bs)->pg[is_compute]) {
      /* if we don't already know that we have to update all sets,
       * check to see if any dsls changed
       *
       * also always update the dsl pointers on program change
       */
       for (unsigned i = 0; i < ARRAY_SIZE(bdd_lazy(bs)->dsl[is_compute]); i++) {
          /* push set is already detected, start at 1 */
          if (bdd_lazy(bs)->dsl[is_compute][i] != pg->dsl[i + 1])
             dd_lazy(ctx)->state_changed[is_compute] |= BITFIELD_BIT(i);
          bdd_lazy(bs)->dsl[is_compute][i] = pg->dsl[i + 1];
       }
       dd_lazy(ctx)->push_state_changed[is_compute] |= bdd_lazy(bs)->push_usage[is_compute] != pg->dd->push_usage;
       bdd_lazy(bs)->push_usage[is_compute] = pg->dd->push_usage;
   }
   bdd_lazy(bs)->pg[is_compute] = pg;

   VkDescriptorSet desc_sets[5];
   uint8_t changed_sets = pg->dd->binding_usage & dd_lazy(ctx)->state_changed[is_compute];
   bool need_push = pg->dd->push_usage &&
                    (dd_lazy(ctx)->push_state_changed[is_compute] || batch_changed);
   if (!populate_sets(ctx, pg, &changed_sets, need_push, desc_sets)) {
      debug_printf("ZINK: couldn't get descriptor sets!\n");
      return;
   }
   if (ctx->batch.state != bs) {
      /* recheck: populate may have overflowed the pool and triggered a flush */
      batch_changed = true;
      dd_lazy(ctx)->state_changed[is_compute] = pg->dd->binding_usage;
      changed_sets = pg->dd->binding_usage & dd_lazy(ctx)->state_changed[is_compute];
      dd_lazy(ctx)->push_state_changed[is_compute] = !!pg->dd->push_usage;
   }
   bs = ctx->batch.state;

   if (pg->dd->binding_usage && changed_sets) {
      u_foreach_bit(type, changed_sets) {
         if (pg->dd->layout_key[type])
            screen->vk.UpdateDescriptorSetWithTemplate(screen->dev, desc_sets[type + 1], pg->dd->layouts[type + 1]->template, ctx);
         assert(type + 1 < pg->num_dsl);
         vkCmdBindDescriptorSets(bs->cmdbuf,
                                 is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 /* set index incremented by 1 to account for push set */
                                 pg->layout, type + 1, 1, &desc_sets[type + 1],
                                 0, NULL);
      }
      dd_lazy(ctx)->state_changed[is_compute] = false;
   }

   if (pg->dd->push_usage && dd_lazy(ctx)->push_state_changed[is_compute]) {
      if (screen->info.have_KHR_push_descriptor)
         screen->vk.CmdPushDescriptorSetWithTemplateKHR(batch->state->cmdbuf, pg->dd->push_template,
                                                     pg->layout, 0, ctx);
      else {
         assert(desc_sets[0]);
         screen->vk.UpdateDescriptorSetWithTemplate(screen->dev, desc_sets[0], pg->dd->push_template, ctx);
         vkCmdBindDescriptorSets(batch->state->cmdbuf,
                                 is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 pg->layout, 0, 1, &desc_sets[0],
                                 0, NULL);
      }
      dd_lazy(ctx)->push_state_changed[is_compute] = false;
   } else if (dd_lazy(ctx)->push_state_changed[is_compute]) {
      vkCmdBindDescriptorSets(bs->cmdbuf,
                              is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pg->layout, 0, 1, &ctx->dd->dummy_set,
                              0, NULL);
      dd_lazy(ctx)->push_state_changed[is_compute] = false;
   }
   /* set again in case of flushing */
   bdd_lazy(bs)->pg[is_compute] = pg;
   ctx->dd->pg[is_compute] = pg;
}

void
zink_context_invalidate_descriptor_state_lazy(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type, unsigned start, unsigned count)
{
   if (type == ZINK_DESCRIPTOR_TYPE_UBO && !start)
      dd_lazy(ctx)->push_state_changed[shader == PIPE_SHADER_COMPUTE] = true;
   else
      dd_lazy(ctx)->state_changed[shader == PIPE_SHADER_COMPUTE] |= BITFIELD_BIT(type);
}

void
zink_batch_descriptor_deinit_lazy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs->dd)
      return;
   if (screen->info.have_KHR_descriptor_update_template) {
      for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
         hash_table_foreach(&bdd_lazy(bs)->pools[i], entry) {
            struct zink_descriptor_pool *pool = (void*)entry->data;
            vkDestroyDescriptorPool(screen->dev, pool->pool, NULL);
         }
      }
      if (bdd_lazy(bs)->push_pool[0])
         vkDestroyDescriptorPool(screen->dev, bdd_lazy(bs)->push_pool[0]->pool, NULL);
      if (bdd_lazy(bs)->push_pool[1])
         vkDestroyDescriptorPool(screen->dev, bdd_lazy(bs)->push_pool[1]->pool, NULL);
   }
   ralloc_free(bs->dd);
}

void
zink_batch_descriptor_reset_lazy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!screen->info.have_KHR_descriptor_update_template)
      return;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      hash_table_foreach(&bdd_lazy(bs)->pools[i], entry) {
         const struct zink_descriptor_layout_key *key = entry->key;
         struct zink_descriptor_pool *pool = (void*)entry->data;
         if (key->use_count)
            pool->set_idx = 0;
         else {
            vkDestroyDescriptorPool(screen->dev, pool->pool, NULL);
            ralloc_free(pool);
            _mesa_hash_table_remove(&bdd_lazy(bs)->pools[i], entry);
         }
      }
   }
   for (unsigned i = 0; i < 2; i++) {
      bdd_lazy(bs)->pg[i] = NULL;
      if (bdd_lazy(bs)->push_pool[i])
         bdd_lazy(bs)->push_pool[i]->set_idx = 0;
   }
}

bool
zink_batch_descriptor_init_lazy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   bs->dd = (void*)rzalloc(bs, struct zink_batch_descriptor_data_lazy);
   if (!bs->dd)
      return false;
   if (!screen->info.have_KHR_descriptor_update_template)
      return true;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (!_mesa_hash_table_init(&bdd_lazy(bs)->pools[i], bs->dd, _mesa_hash_pointer, _mesa_key_pointer_equal))
         return false;
   }
   if (!screen->info.have_KHR_push_descriptor) {
      VkDescriptorPoolSize sizes;
      sizes.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      sizes.descriptorCount = ZINK_SHADER_COUNT * ZINK_DEFAULT_MAX_DESCS;
      bdd_lazy(bs)->push_pool[0] = rzalloc(bs, struct zink_descriptor_pool);
      bdd_lazy(bs)->push_pool[0]->pool = create_pool(screen, 1, &sizes, 0);
      sizes.descriptorCount  = ZINK_DEFAULT_MAX_DESCS;
      bdd_lazy(bs)->push_pool[1] = rzalloc(bs, struct zink_descriptor_pool);
      bdd_lazy(bs)->push_pool[1]->pool = create_pool(screen, 1, &sizes, 0);
   }
   return true;
}

bool
zink_descriptors_init_lazy(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   ctx->dd = (void*)rzalloc(ctx, struct zink_descriptor_data_lazy);
   if (!ctx->dd)
      return false;

   if (screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_NOTEMPLATES)
      printf("ZINK: CACHED/NOTEMPLATES DESCRIPTORS\n");
   else if (screen->info.have_KHR_descriptor_update_template) {
      for (unsigned i = 0; i < PIPE_SHADER_TYPES; i++) {
         VkDescriptorUpdateTemplateEntry *entry = &dd_lazy(ctx)->push_entries[i];
         entry->dstBinding = tgsi_processor_to_shader_stage(i);
         entry->descriptorCount = 1;
         entry->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
         entry->offset = offsetof(struct zink_context, di.ubos[i][0]);
         entry->stride = sizeof(VkDescriptorBufferInfo);
      }
      if (screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY)
         printf("ZINK: USING LAZY DESCRIPTORS\n");
   }
   struct zink_descriptor_layout_key *layout_key;
   if (!zink_descriptor_util_push_layouts_get(ctx, ctx->dd->push_dsl, ctx->dd->push_layout_keys))
      return false;

   ctx->dd->dummy_dsl = zink_descriptor_util_layout_get(ctx, 0, NULL, 0, &layout_key);
   if (!ctx->dd->dummy_dsl)
      return false;
   VkDescriptorPoolSize null_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
   ctx->dd->dummy_pool = create_pool(screen, 1, &null_size, 0);
   zink_descriptor_util_alloc_sets(screen, ctx->dd->dummy_dsl->layout,
                                   ctx->dd->dummy_pool, &ctx->dd->dummy_set, 1);
   zink_descriptor_util_init_null_set(ctx, ctx->dd->dummy_set);
   return true;
}

void
zink_descriptors_deinit_lazy(struct zink_context *ctx)
{
   if (ctx->dd) {
      struct zink_screen *screen = zink_screen(ctx->base.screen);
      if (ctx->dd->dummy_pool)
         vkDestroyDescriptorPool(screen->dev, ctx->dd->dummy_pool, NULL);
      if (screen->descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY &&
          screen->info.have_KHR_push_descriptor) {
         vkDestroyDescriptorSetLayout(screen->dev, ctx->dd->push_dsl[0]->layout, NULL);
         vkDestroyDescriptorSetLayout(screen->dev, ctx->dd->push_dsl[1]->layout, NULL);
      }
   }
   ralloc_free(ctx->dd);
}
