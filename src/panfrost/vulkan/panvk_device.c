/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_private.h"

#include "panfrost-quirks.h"
#include "pan_blitter.h"
#include "pan_bo.h"
#include "pan_encoder.h"
#include "pan_util.h"
#include "decode.h"

#include <fcntl.h>
#include <libsync.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm-uapi/panfrost_drm.h"

#include "util/debug.h"
#include "util/strtod.h"
#include "vk_format.h"
#include "vk_util.h"

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include <wayland-client.h>
#include "wayland-drm-client-protocol.h"
#endif

#include "panvk_cs.h"

VkResult
_panvk_device_set_lost(struct panvk_device *device,
                       const char *file, int line,
                       const char *msg, ...)
{
   /* Set the flag indicating that waits should return in finite time even
    * after device loss.
    */
   p_atomic_inc(&device->_lost);

   /* TODO: Report the log message through VkDebugReportCallbackEXT instead */
   fprintf(stderr, "%s:%d: ", file, line);
   va_list ap;
   va_start(ap, msg);
   vfprintf(stderr, msg, ap);
   va_end(ap);

   if (env_var_as_boolean("PANVK_ABORT_ON_DEVICE_LOSS", false))
      abort();

   return VK_ERROR_DEVICE_LOST;
}

static int
panvk_device_get_cache_uuid(uint16_t family, void *uuid)
{
   uint32_t mesa_timestamp;
   uint16_t f = family;
   memset(uuid, 0, VK_UUID_SIZE);
   memcpy(uuid, &mesa_timestamp, 4);
   memcpy((char *) uuid + 4, &f, 2);
   snprintf((char *) uuid + 6, VK_UUID_SIZE - 10, "pan");
   return 0;
}

static void
panvk_get_driver_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
   snprintf(uuid, VK_UUID_SIZE, "panfrost");
}

static void
panvk_get_device_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
}

static const struct debug_control panvk_debug_options[] = {
   { "startup", PANVK_DEBUG_STARTUP },
   { "nir", PANVK_DEBUG_NIR },
   { "trace", PANVK_DEBUG_TRACE },
   { "sync", PANVK_DEBUG_SYNC },
   { "afbc", PANVK_DEBUG_AFBC },
   { "linear", PANVK_DEBUG_LINEAR },
   { NULL, 0 }
};

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#define PANVK_USE_WSI_PLATFORM
#endif

#define PANVK_API_VERSION VK_MAKE_VERSION(1, 1, VK_HEADER_VERSION)

VkResult
panvk_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
    *pApiVersion = PANVK_API_VERSION;
    return VK_SUCCESS;
}

static const struct vk_instance_extension_table panvk_instance_extensions = {
#ifdef PANVK_USE_WSI_PLATFORM
   .KHR_surface = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
};

static void
panvk_get_device_extensions(const struct panvk_physical_device *device,
                            struct vk_device_extension_table *ext)
{
   *ext = (struct vk_device_extension_table) {
#ifdef PANVK_USE_WSI_PLATFORM
      .KHR_swapchain = true,
#endif
      .EXT_custom_border_color = true,
   };
}

VkResult
panvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance)
{
   struct panvk_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   pAllocator = pAllocator ? : vk_default_allocator();
   instance = vk_zalloc(pAllocator, sizeof(*instance), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;

   vk_instance_dispatch_table_from_entrypoints(&dispatch_table,
                                               &panvk_instance_entrypoints,
                                               true);
   result = vk_instance_init(&instance->vk,
                             &panvk_instance_extensions,
                             &dispatch_table,
                             pCreateInfo,
                             pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->physical_device_count = -1;
   instance->debug_flags = parse_debug_string(getenv("PANVK_DEBUG"),
                                              panvk_debug_options);

   if (instance->debug_flags & PANVK_DEBUG_STARTUP)
      panvk_logi("Created an instance");

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = panvk_instance_to_handle(instance);

   return VK_SUCCESS;
}

static void
panvk_physical_device_finish(struct panvk_physical_device *device)
{
   panvk_wsi_finish(device);

   panvk_meta_cleanup(device);
   pan_blitter_cleanup(&device->pdev);
   panfrost_close_device(&device->pdev);
   close(device->local_fd);
   if (device->master_fd != -1)
      close(device->master_fd);

   vk_physical_device_finish(&device->vk);
}

void
panvk_DestroyInstance(VkInstance _instance,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_instance, instance, _instance);

   if (!instance)
      return;

   for (int i = 0; i < instance->physical_device_count; ++i) {
      panvk_physical_device_finish(instance->physical_devices + i);
   }

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

static VkResult
panvk_physical_device_init(struct panvk_physical_device *device,
                           struct panvk_instance *instance,
                           drmDevicePtr drm_device)
{
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result = VK_SUCCESS;
   drmVersionPtr version;
   int fd;
   int master_fd = -1;

   if (!getenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER")) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "WARNING: panvk is not a conformant vulkan implementation, "
                       "pass PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 if you know what you're doing.");
   }

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to open device %s", path);
   }

   version = drmGetVersion(fd);
   if (!version) {
      close(fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to query kernel driver version for device %s",
                       path);
   }

   if (strcmp(version->name, "panfrost")) {
      drmFreeVersion(version);
      close(fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "device %s does not use the panfrost kernel driver", path);
   }

   drmFreeVersion(version);

   if (instance->debug_flags & PANVK_DEBUG_STARTUP)
      panvk_logi("Found compatible device '%s'.", path);

   struct vk_device_extension_table supported_extensions;
   panvk_get_device_extensions(device, &supported_extensions);

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &panvk_physical_device_entrypoints,
                                                      true);

   result = vk_physical_device_init(&device->vk, &instance->vk,
                                    &supported_extensions,
                                    &dispatch_table);

   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }

   device->instance = instance;
   assert(strlen(path) < ARRAY_SIZE(device->path));
   strncpy(device->path, path, ARRAY_SIZE(device->path));

   if (instance->vk.enabled_extensions.KHR_display) {
      master_fd = open(drm_device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      if (master_fd >= 0) {
         /* TODO: free master_fd is accel is not working? */
      }
   }

   device->master_fd = master_fd;
   device->local_fd = fd;
   device->pdev.debug = PAN_DBG_TRACE;
   panfrost_open_device(NULL, fd, &device->pdev);

   if (device->pdev.quirks & MIDGARD_SFBD) {
      result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "%s not supported",
                         panfrost_model_name(device->pdev.gpu_id));
      goto fail;
   }

   pan_blitter_init(&device->pdev);
   panvk_meta_init(device);

   memset(device->name, 0, sizeof(device->name));
   sprintf(device->name, "%s", panfrost_model_name(device->pdev.gpu_id));

   if (panvk_device_get_cache_uuid(device->pdev.gpu_id, device->cache_uuid)) {
      result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                         "cannot generate UUID");
      goto fail;
   }

   fprintf(stderr, "WARNING: panvk is not a conformant vulkan implementation, "
                   "testing use only.\n");

   panvk_get_driver_uuid(&device->device_uuid);
   panvk_get_device_uuid(&device->device_uuid);

   result = panvk_wsi_init(device);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }

   return VK_SUCCESS;

fail:
   close(fd);
   if (master_fd != -1)
      close(master_fd);
   return result;
}

static VkResult
panvk_enumerate_devices(struct panvk_instance *instance)
{
   /* TODO: Check for more devices ? */
   drmDevicePtr devices[8];
   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   int max_devices;

   instance->physical_device_count = 0;

   max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));

   if (instance->debug_flags & PANVK_DEBUG_STARTUP)
      panvk_logi("Found %d drm nodes", max_devices);

   if (max_devices < 1)
      return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);

   for (unsigned i = 0; i < (unsigned) max_devices; i++) {
      if ((devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) &&
          devices[i]->bustype == DRM_BUS_PLATFORM) {

         result = panvk_physical_device_init(instance->physical_devices +
                                           instance->physical_device_count,
                                           instance, devices[i]);
         if (result == VK_SUCCESS)
            ++instance->physical_device_count;
         else if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
            break;
      }
   }
   drmFreeDevices(devices, max_devices);

   return result;
}

VkResult
panvk_EnumeratePhysicalDevices(VkInstance _instance,
                               uint32_t *pPhysicalDeviceCount,
                               VkPhysicalDevice *pPhysicalDevices)
{
   VK_FROM_HANDLE(panvk_instance, instance, _instance);
   VK_OUTARRAY_MAKE(out, pPhysicalDevices, pPhysicalDeviceCount);

   VkResult result;

   if (instance->physical_device_count < 0) {
      result = panvk_enumerate_devices(instance);
      if (result != VK_SUCCESS && result != VK_ERROR_INCOMPATIBLE_DRIVER)
         return result;
   }

   for (uint32_t i = 0; i < instance->physical_device_count; ++i) {
      vk_outarray_append(&out, p)
      {
         *p = panvk_physical_device_to_handle(instance->physical_devices + i);
      }
   }

   return vk_outarray_status(&out);
}

VkResult
panvk_EnumeratePhysicalDeviceGroups(VkInstance _instance,
                                    uint32_t *pPhysicalDeviceGroupCount,
                                    VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
   VK_FROM_HANDLE(panvk_instance, instance, _instance);
   VK_OUTARRAY_MAKE(out, pPhysicalDeviceGroupProperties,
                    pPhysicalDeviceGroupCount);
   VkResult result;

   if (instance->physical_device_count < 0) {
      result = panvk_enumerate_devices(instance);
      if (result != VK_SUCCESS && result != VK_ERROR_INCOMPATIBLE_DRIVER)
         return result;
   }

   for (uint32_t i = 0; i < instance->physical_device_count; ++i) {
      vk_outarray_append(&out, p)
      {
         p->physicalDeviceCount = 1;
         p->physicalDevices[0] =
            panvk_physical_device_to_handle(instance->physical_devices + i);
         p->subsetAllocation = false;
      }
   }

   return VK_SUCCESS;
}

void
panvk_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceFeatures2 *pFeatures)
{
   vk_foreach_struct(ext, pFeatures->pNext)
   {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
         VkPhysicalDeviceVulkan11Features *features = (void *) ext;
         features->storageBuffer16BitAccess            = false;
         features->uniformAndStorageBuffer16BitAccess  = false;
         features->storagePushConstant16               = false;
         features->storageInputOutput16                = false;
         features->multiview                           = false;
         features->multiviewGeometryShader             = false;
         features->multiviewTessellationShader         = false;
         features->variablePointersStorageBuffer       = true;
         features->variablePointers                    = true;
         features->protectedMemory                     = false;
         features->samplerYcbcrConversion              = false;
         features->shaderDrawParameters                = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
         VkPhysicalDeviceVulkan12Features *features = (void *) ext;
         features->samplerMirrorClampToEdge            = false;
         features->drawIndirectCount                   = false;
         features->storageBuffer8BitAccess             = false;
         features->uniformAndStorageBuffer8BitAccess   = false;
         features->storagePushConstant8                = false;
         features->shaderBufferInt64Atomics            = false;
         features->shaderSharedInt64Atomics            = false;
         features->shaderFloat16                       = false;
         features->shaderInt8                          = false;

         features->descriptorIndexing                                 = false;
         features->shaderInputAttachmentArrayDynamicIndexing          = false;
         features->shaderUniformTexelBufferArrayDynamicIndexing       = false;
         features->shaderStorageTexelBufferArrayDynamicIndexing       = false;
         features->shaderUniformBufferArrayNonUniformIndexing         = false;
         features->shaderSampledImageArrayNonUniformIndexing          = false;
         features->shaderStorageBufferArrayNonUniformIndexing         = false;
         features->shaderStorageImageArrayNonUniformIndexing          = false;
         features->shaderInputAttachmentArrayNonUniformIndexing       = false;
         features->shaderUniformTexelBufferArrayNonUniformIndexing    = false;
         features->shaderStorageTexelBufferArrayNonUniformIndexing    = false;
         features->descriptorBindingUniformBufferUpdateAfterBind      = false;
         features->descriptorBindingSampledImageUpdateAfterBind       = false;
         features->descriptorBindingStorageImageUpdateAfterBind       = false;
         features->descriptorBindingStorageBufferUpdateAfterBind      = false;
         features->descriptorBindingUniformTexelBufferUpdateAfterBind = false;
         features->descriptorBindingStorageTexelBufferUpdateAfterBind = false;
         features->descriptorBindingUpdateUnusedWhilePending          = false;
         features->descriptorBindingPartiallyBound                    = false;
         features->descriptorBindingVariableDescriptorCount           = false;
         features->runtimeDescriptorArray                             = false;

         features->samplerFilterMinmax                 = false;
         features->scalarBlockLayout                   = false;
         features->imagelessFramebuffer                = false;
         features->uniformBufferStandardLayout         = false;
         features->shaderSubgroupExtendedTypes         = false;
         features->separateDepthStencilLayouts         = false;
         features->hostQueryReset                      = false;
         features->timelineSemaphore                   = false;
         features->bufferDeviceAddress                 = false;
         features->bufferDeviceAddressCaptureReplay    = false;
         features->bufferDeviceAddressMultiDevice      = false;
         features->vulkanMemoryModel                   = false;
         features->vulkanMemoryModelDeviceScope        = false;
         features->vulkanMemoryModelAvailabilityVisibilityChains = false;
         features->shaderOutputViewportIndex           = false;
         features->shaderOutputLayer                   = false;
         features->subgroupBroadcastDynamicId          = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
         VkPhysicalDeviceVariablePointersFeatures *features = (void *) ext;
         features->variablePointersStorageBuffer = true;
         features->variablePointers = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
         VkPhysicalDeviceMultiviewFeatures *features =
            (VkPhysicalDeviceMultiviewFeatures *) ext;
         features->multiview = false;
         features->multiviewGeometryShader = false;
         features->multiviewTessellationShader = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
         VkPhysicalDeviceShaderDrawParametersFeatures *features =
            (VkPhysicalDeviceShaderDrawParametersFeatures *) ext;
         features->shaderDrawParameters = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: {
         VkPhysicalDeviceProtectedMemoryFeatures *features =
            (VkPhysicalDeviceProtectedMemoryFeatures *) ext;
         features->protectedMemory = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
         VkPhysicalDevice16BitStorageFeatures *features =
            (VkPhysicalDevice16BitStorageFeatures *) ext;
         features->storageBuffer16BitAccess = false;
         features->uniformAndStorageBuffer16BitAccess = false;
         features->storagePushConstant16 = false;
         features->storageInputOutput16 = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
         VkPhysicalDeviceSamplerYcbcrConversionFeatures *features =
            (VkPhysicalDeviceSamplerYcbcrConversionFeatures *) ext;
         features->samplerYcbcrConversion = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT: {
         VkPhysicalDeviceDescriptorIndexingFeaturesEXT *features =
            (VkPhysicalDeviceDescriptorIndexingFeaturesEXT *) ext;
         features->shaderInputAttachmentArrayDynamicIndexing = false;
         features->shaderUniformTexelBufferArrayDynamicIndexing = false;
         features->shaderStorageTexelBufferArrayDynamicIndexing = false;
         features->shaderUniformBufferArrayNonUniformIndexing = false;
         features->shaderSampledImageArrayNonUniformIndexing = false;
         features->shaderStorageBufferArrayNonUniformIndexing = false;
         features->shaderStorageImageArrayNonUniformIndexing = false;
         features->shaderInputAttachmentArrayNonUniformIndexing = false;
         features->shaderUniformTexelBufferArrayNonUniformIndexing = false;
         features->shaderStorageTexelBufferArrayNonUniformIndexing = false;
         features->descriptorBindingUniformBufferUpdateAfterBind = false;
         features->descriptorBindingSampledImageUpdateAfterBind = false;
         features->descriptorBindingStorageImageUpdateAfterBind = false;
         features->descriptorBindingStorageBufferUpdateAfterBind = false;
         features->descriptorBindingUniformTexelBufferUpdateAfterBind = false;
         features->descriptorBindingStorageTexelBufferUpdateAfterBind = false;
         features->descriptorBindingUpdateUnusedWhilePending = false;
         features->descriptorBindingPartiallyBound = false;
         features->descriptorBindingVariableDescriptorCount = false;
         features->runtimeDescriptorArray = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT: {
         VkPhysicalDeviceConditionalRenderingFeaturesEXT *features =
            (VkPhysicalDeviceConditionalRenderingFeaturesEXT *) ext;
         features->conditionalRendering = false;
         features->inheritedConditionalRendering = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
         VkPhysicalDeviceTransformFeedbackFeaturesEXT *features =
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT *) ext;
         features->transformFeedback = false;
         features->geometryStreams = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
         VkPhysicalDeviceIndexTypeUint8FeaturesEXT *features =
            (VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)ext;
         features->indexTypeUint8 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *features =
            (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)ext;
         features->vertexAttributeInstanceRateDivisor = true;
         features->vertexAttributeInstanceRateZeroDivisor = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES_EXT: {
         VkPhysicalDevicePrivateDataFeaturesEXT *features =
            (VkPhysicalDevicePrivateDataFeaturesEXT *)ext;
         features->privateData = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipEnableFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)ext;
         features->depthClipEnable = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
         VkPhysicalDevice4444FormatsFeaturesEXT *features = (void *)ext;
         features->formatA4R4G4B4 = true;
         features->formatA4B4G4R4 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
         VkPhysicalDeviceCustomBorderColorFeaturesEXT *features = (void *) ext;
         features->customBorderColors = true;
         features->customBorderColorWithoutFormat = true;
         break;
      }
      default:
         break;
      }
   }

   pFeatures->features = (VkPhysicalDeviceFeatures) {
      .fullDrawIndexUint32 = true,
      .independentBlend = true,
      .wideLines = true,
      .largePoints = true,
      .textureCompressionETC2 = true,
      .textureCompressionASTC_LDR = true,
      .shaderUniformBufferArrayDynamicIndexing = true,
      .shaderSampledImageArrayDynamicIndexing = true,
      .shaderStorageBufferArrayDynamicIndexing = true,
      .shaderStorageImageArrayDynamicIndexing = true,
   };
}

void
panvk_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceProperties2 *pProperties)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);

   vk_foreach_struct(ext, pProperties->pNext)
   {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
         VkPhysicalDevicePushDescriptorPropertiesKHR *properties = (VkPhysicalDevicePushDescriptorPropertiesKHR *)ext;
         properties->maxPushDescriptors = MAX_PUSH_DESCRIPTORS;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
         VkPhysicalDeviceIDProperties *properties = (VkPhysicalDeviceIDProperties *)ext;
         memcpy(properties->driverUUID, pdevice->driver_uuid, VK_UUID_SIZE);
         memcpy(properties->deviceUUID, pdevice->device_uuid, VK_UUID_SIZE);
         properties->deviceLUIDValid = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
         VkPhysicalDeviceMultiviewProperties *properties = (VkPhysicalDeviceMultiviewProperties *)ext;
         properties->maxMultiviewViewCount = 0;
         properties->maxMultiviewInstanceIndex = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES: {
         VkPhysicalDevicePointClippingProperties *properties = (VkPhysicalDevicePointClippingProperties *)ext;
         properties->pointClippingBehavior =
            VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
         VkPhysicalDeviceMaintenance3Properties *properties = (VkPhysicalDeviceMaintenance3Properties *)ext;
         /* Make sure everything is addressable by a signed 32-bit int, and
          * our largest descriptors are 96 bytes. */
         properties->maxPerSetDescriptors = (1ull << 31) / 96;
         /* Our buffer size fields allow only this much */
         properties->maxMemoryAllocationSize = 0xFFFFFFFFull;
         break;
      }
      default:
         break;
      }
   }

   VkSampleCountFlags sample_counts = 0xf;

   /* make sure that the entire descriptor set is addressable with a signed
    * 32-bit int. So the sum of all limits scaled by descriptor size has to
    * be at most 2 GiB. the combined image & samples object count as one of
    * both. This limit is for the pipeline layout, not for the set layout, but
    * there is no set limit, so we just set a pipeline limit. I don't think
    * any app is going to hit this soon. */
   size_t max_descriptor_set_size =
      ((1ull << 31) - 16 * MAX_DYNAMIC_BUFFERS) /
      (32 /* uniform buffer, 32 due to potential space wasted on alignment */ +
       32 /* storage buffer, 32 due to potential space wasted on alignment */ +
       32 /* sampler, largest when combined with image */ +
       64 /* sampled image */ + 64 /* storage image */);

   VkPhysicalDeviceLimits limits = {
      .maxImageDimension1D = (1 << 14),
      .maxImageDimension2D = (1 << 14),
      .maxImageDimension3D = (1 << 11),
      .maxImageDimensionCube = (1 << 14),
      .maxImageArrayLayers = (1 << 11),
      .maxTexelBufferElements = 128 * 1024 * 1024,
      .maxUniformBufferRange = UINT32_MAX,
      .maxStorageBufferRange = UINT32_MAX,
      .maxPushConstantsSize = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount = UINT32_MAX,
      .maxSamplerAllocationCount = 64 * 1024,
      .bufferImageGranularity = 64,          /* A cache line */
      .sparseAddressSpaceSize = 0xffffffffu, /* buffer max size */
      .maxBoundDescriptorSets = MAX_SETS,
      .maxPerStageDescriptorSamplers = max_descriptor_set_size,
      .maxPerStageDescriptorUniformBuffers = max_descriptor_set_size,
      .maxPerStageDescriptorStorageBuffers = max_descriptor_set_size,
      .maxPerStageDescriptorSampledImages = max_descriptor_set_size,
      .maxPerStageDescriptorStorageImages = max_descriptor_set_size,
      .maxPerStageDescriptorInputAttachments = max_descriptor_set_size,
      .maxPerStageResources = max_descriptor_set_size,
      .maxDescriptorSetSamplers = max_descriptor_set_size,
      .maxDescriptorSetUniformBuffers = max_descriptor_set_size,
      .maxDescriptorSetUniformBuffersDynamic = MAX_DYNAMIC_UNIFORM_BUFFERS,
      .maxDescriptorSetStorageBuffers = max_descriptor_set_size,
      .maxDescriptorSetStorageBuffersDynamic = MAX_DYNAMIC_STORAGE_BUFFERS,
      .maxDescriptorSetSampledImages = max_descriptor_set_size,
      .maxDescriptorSetStorageImages = max_descriptor_set_size,
      .maxDescriptorSetInputAttachments = max_descriptor_set_size,
      .maxVertexInputAttributes = 32,
      .maxVertexInputBindings = 32,
      .maxVertexInputAttributeOffset = 2047,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 128,
      .maxTessellationGenerationLevel = 64,
      .maxTessellationPatchSize = 32,
      .maxTessellationControlPerVertexInputComponents = 128,
      .maxTessellationControlPerVertexOutputComponents = 128,
      .maxTessellationControlPerPatchOutputComponents = 120,
      .maxTessellationControlTotalOutputComponents = 4096,
      .maxTessellationEvaluationInputComponents = 128,
      .maxTessellationEvaluationOutputComponents = 128,
      .maxGeometryShaderInvocations = 127,
      .maxGeometryInputComponents = 64,
      .maxGeometryOutputComponents = 128,
      .maxGeometryOutputVertices = 256,
      .maxGeometryTotalOutputComponents = 1024,
      .maxFragmentInputComponents = 128,
      .maxFragmentOutputAttachments = 8,
      .maxFragmentDualSrcAttachments = 1,
      .maxFragmentCombinedOutputResources = 8,
      .maxComputeSharedMemorySize = 32768,
      .maxComputeWorkGroupCount = { 65535, 65535, 65535 },
      .maxComputeWorkGroupInvocations = 2048,
      .maxComputeWorkGroupSize = { 2048, 2048, 2048 },
      .subPixelPrecisionBits = 4 /* FIXME */,
      .subTexelPrecisionBits = 4 /* FIXME */,
      .mipmapPrecisionBits = 4 /* FIXME */,
      .maxDrawIndexedIndexValue = UINT32_MAX,
      .maxDrawIndirectCount = UINT32_MAX,
      .maxSamplerLodBias = 16,
      .maxSamplerAnisotropy = 16,
      .maxViewports = MAX_VIEWPORTS,
      .maxViewportDimensions = { (1 << 14), (1 << 14) },
      .viewportBoundsRange = { INT16_MIN, INT16_MAX },
      .viewportSubPixelBits = 8,
      .minMemoryMapAlignment = 4096, /* A page */
      .minTexelBufferOffsetAlignment = 1,
      .minUniformBufferOffsetAlignment = 4,
      .minStorageBufferOffsetAlignment = 4,
      .minTexelOffset = -32,
      .maxTexelOffset = 31,
      .minTexelGatherOffset = -32,
      .maxTexelGatherOffset = 31,
      .minInterpolationOffset = -2,
      .maxInterpolationOffset = 2,
      .subPixelInterpolationOffsetBits = 8,
      .maxFramebufferWidth = (1 << 14),
      .maxFramebufferHeight = (1 << 14),
      .maxFramebufferLayers = (1 << 10),
      .framebufferColorSampleCounts = sample_counts,
      .framebufferDepthSampleCounts = sample_counts,
      .framebufferStencilSampleCounts = sample_counts,
      .framebufferNoAttachmentsSampleCounts = sample_counts,
      .maxColorAttachments = MAX_RTS,
      .sampledImageColorSampleCounts = sample_counts,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .sampledImageDepthSampleCounts = sample_counts,
      .sampledImageStencilSampleCounts = sample_counts,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .timestampComputeAndGraphics = true,
      .timestampPeriod = 1,
      .maxClipDistances = 8,
      .maxCullDistances = 8,
      .maxCombinedClipAndCullDistances = 8,
      .discreteQueuePriorities = 1,
      .pointSizeRange = { 0.125, 255.875 },
      .lineWidthRange = { 0.0, 7.9921875 },
      .pointSizeGranularity = (1.0 / 8.0),
      .lineWidthGranularity = (1.0 / 128.0),
      .strictLines = false, /* FINISHME */
      .standardSampleLocations = true,
      .optimalBufferCopyOffsetAlignment = 128,
      .optimalBufferCopyRowPitchAlignment = 128,
      .nonCoherentAtomSize = 64,
   };

   pProperties->properties = (VkPhysicalDeviceProperties) {
      .apiVersion = PANVK_API_VERSION,
      .driverVersion = vk_get_driver_version(),
      .vendorID = 0, /* TODO */
      .deviceID = 0,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = { 0 },
   };

   strcpy(pProperties->properties.deviceName, pdevice->name);
   memcpy(pProperties->properties.pipelineCacheUUID, pdevice->cache_uuid, VK_UUID_SIZE);
}

static const VkQueueFamilyProperties panvk_queue_family_properties = {
   .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 64,
   .minImageTransferGranularity = { 1, 1, 1 },
};

void
panvk_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                             uint32_t *pQueueFamilyPropertyCount,
                                             VkQueueFamilyProperties *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append(&out, p) { *p = panvk_queue_family_properties; }
}

void
panvk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                              uint32_t *pQueueFamilyPropertyCount,
                                              VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append(&out, p)
   {
      p->queueFamilyProperties = panvk_queue_family_properties;
   }
}

static uint64_t
panvk_get_system_heap_size()
{
   struct sysinfo info;
   sysinfo(&info);

   uint64_t total_ram = (uint64_t)info.totalram * info.mem_unit;

   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024 * 1024 * 1024)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   return available_ram;
}

void
panvk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                         VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   pMemoryProperties->memoryProperties = (VkPhysicalDeviceMemoryProperties) {
      .memoryHeapCount = 1,
      .memoryHeaps[0].size = panvk_get_system_heap_size(),
      .memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
      .memoryTypeCount = 1,
      .memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      .memoryTypes[0].heapIndex = 0,
   };
}

static VkResult
panvk_queue_init(struct panvk_device *device,
                 struct panvk_queue *queue,
                 uint32_t queue_family_index,
                 int idx,
                 VkDeviceQueueCreateFlags flags)
{
   const struct panfrost_device *pdev = &device->physical_device->pdev;

   vk_object_base_init(&device->vk, &queue->base, VK_OBJECT_TYPE_QUEUE);
   queue->device = device;
   queue->queue_family_index = queue_family_index;
   queue->flags = flags;

   struct drm_syncobj_create create = {
      .flags = DRM_SYNCOBJ_CREATE_SIGNALED,
   };

   int ret = drmIoctl(pdev->fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
   if (ret)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   queue->sync = create.handle;
   return VK_SUCCESS;
}

static void
panvk_queue_finish(struct panvk_queue *queue)
{
}

VkResult
panvk_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDevice *pDevice)
{
   VK_FROM_HANDLE(panvk_physical_device, physical_device, physicalDevice);
   VkResult result;
   struct panvk_device *device;

   /* Check enabled features */
   if (pCreateInfo->pEnabledFeatures) {
      VkPhysicalDeviceFeatures2 supported_features = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      };
      panvk_GetPhysicalDeviceFeatures2(physicalDevice, &supported_features);
      VkBool32 *supported_feature = (VkBool32 *) &supported_features.features;
      VkBool32 *enabled_feature = (VkBool32 *) pCreateInfo->pEnabledFeatures;
      unsigned num_features =
         sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
      for (uint32_t i = 0; i < num_features; i++) {
         if (enabled_feature[i] && !supported_feature[i])
            return vk_error(physical_device->instance,
                            VK_ERROR_FEATURE_NOT_PRESENT);
      }
   }

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &panvk_device_entrypoints,
                                             true);
   result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return vk_errorf(physical_device->instance, result, "vk_device_init failed");
   }

   device->instance = physical_device->instance;
   device->physical_device = physical_device;

   for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_create =
         &pCreateInfo->pQueueCreateInfos[i];
      uint32_t qfi = queue_create->queueFamilyIndex;
      device->queues[qfi] =
         vk_alloc(&device->vk.alloc,
                  queue_create->queueCount * sizeof(struct panvk_queue),
                  8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!device->queues[qfi]) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      memset(device->queues[qfi], 0,
             queue_create->queueCount * sizeof(struct panvk_queue));

      device->queue_count[qfi] = queue_create->queueCount;

      for (unsigned q = 0; q < queue_create->queueCount; q++) {
         result = panvk_queue_init(device, &device->queues[qfi][q], qfi, q,
                                   queue_create->flags);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   *pDevice = panvk_device_to_handle(device);
   return VK_SUCCESS;

fail:
   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   vk_free(&device->vk.alloc, device);
   return result;
}

void
panvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);

   if (!device)
      return;

   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   vk_free(&device->vk.alloc, device);
}

VkResult
panvk_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                       VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

void
panvk_GetDeviceQueue2(VkDevice _device,
                      const VkDeviceQueueInfo2 *pQueueInfo,
                      VkQueue *pQueue)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_queue *queue;

   queue = &device->queues[pQueueInfo->queueFamilyIndex][pQueueInfo->queueIndex];
   if (pQueueInfo->flags != queue->flags) {
      /* From the Vulkan 1.1.70 spec:
       *
       * "The queue returned by vkGetDeviceQueue2 must have the same
       * flags value from this structure as that used at device
       * creation time in a VkDeviceQueueCreateInfo instance. If no
       * matching flags were specified at device creation time then
       * pQueue will return VK_NULL_HANDLE."
       */
      *pQueue = VK_NULL_HANDLE;
      return;
   }

   *pQueue = panvk_queue_to_handle(queue);
}

void
panvk_GetDeviceQueue(VkDevice _device,
                     uint32_t queueFamilyIndex,
                     uint32_t queueIndex,
                     VkQueue *pQueue)
{
   const VkDeviceQueueInfo2 info = (VkDeviceQueueInfo2) {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
      .queueFamilyIndex = queueFamilyIndex,
      .queueIndex = queueIndex
   };

   panvk_GetDeviceQueue2(_device, &info, pQueue);
}

static void
panvk_queue_submit_batch(struct panvk_queue *queue,
                         struct panvk_batch *batch,
                         uint32_t *bos, unsigned nr_bos,
                         uint32_t *in_fences,
                         unsigned nr_in_fences)
{
   const struct panvk_device *dev = queue->device;
   unsigned debug = dev->physical_device->instance->debug_flags;
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   int ret;

   /* Reset the batch if it's already been issued */
   if (batch->issued) {
      util_dynarray_foreach(&batch->jobs, void *, job)
         memset((*job), 0, 4 * 4);

      /* Reset the tiler before re-issuing the batch */
      if (pan_is_bifrost(pdev) && batch->tiler.bifrost_descs.cpu) {
         memcpy(batch->tiler.bifrost_descs.cpu, &batch->tiler.templ.bifrost,
                sizeof(batch->tiler.templ.bifrost));
      } else if (!pan_is_bifrost(pdev) && batch->fb.desc.cpu) {
         void *tiler = pan_section_ptr(batch->fb.desc.cpu, MULTI_TARGET_FRAMEBUFFER, TILER);
         memcpy(tiler, &batch->tiler.templ.midgard, sizeof(batch->tiler.templ.midgard));
         /* All weights set to 0, nothing to do here */
         pan_section_pack(batch->fb.desc.cpu, MULTI_TARGET_FRAMEBUFFER, TILER_WEIGHTS, w);
      }
   }

   if (batch->scoreboard.first_job) {
      struct drm_panfrost_submit submit = {
         .bo_handles = (uintptr_t)bos,
         .bo_handle_count = nr_bos,
         .in_syncs = (uintptr_t)in_fences,
         .in_sync_count = nr_in_fences,
         .out_sync = queue->sync,
         .jc = batch->scoreboard.first_job,
      };

      ret = drmIoctl(pdev->fd, DRM_IOCTL_PANFROST_SUBMIT, &submit);
      assert(!ret);

      if (debug & (PANVK_DEBUG_TRACE | PANVK_DEBUG_SYNC)) {
         ret = drmSyncobjWait(pdev->fd, &submit.out_sync, 1, INT64_MAX, 0, NULL);
         assert(!ret);
      }

      if (debug & PANVK_DEBUG_TRACE)
         pandecode_jc(batch->scoreboard.first_job, pan_is_bifrost(pdev), pdev->gpu_id);
   }

   if (batch->fragment_job) {
      struct drm_panfrost_submit submit = {
         .bo_handles = (uintptr_t)bos,
         .bo_handle_count = nr_bos,
         .out_sync = queue->sync,
         .jc = batch->fragment_job,
         .requirements = PANFROST_JD_REQ_FS,
      };

      if (batch->scoreboard.first_job) {
         submit.in_syncs = (uintptr_t)(&queue->sync);
         submit.in_sync_count = 1;
      } else {
         submit.in_syncs = (uintptr_t)in_fences;
         submit.in_sync_count = nr_in_fences;
      }

      ret = drmIoctl(pdev->fd, DRM_IOCTL_PANFROST_SUBMIT, &submit);
      assert(!ret);
      if (debug & (PANVK_DEBUG_TRACE | PANVK_DEBUG_SYNC)) {
         ret = drmSyncobjWait(pdev->fd, &submit.out_sync, 1, INT64_MAX, 0, NULL);
         assert(!ret);
      }

      if (debug & PANVK_DEBUG_TRACE)
         pandecode_jc(batch->fragment_job, pan_is_bifrost(pdev), pdev->gpu_id);
   }

   if (debug & PANVK_DEBUG_TRACE)
      pandecode_next_frame();

   batch->issued = true;
}

static void
panvk_queue_transfer_sync(struct panvk_queue *queue,
                          struct panvk_syncobj *dst)
{
   const struct panfrost_device *pdev = &queue->device->physical_device->pdev;
   int ret;

   struct drm_syncobj_handle handle = {
      .handle = queue->sync,
      .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
      .fd = -1,
   };

   ret = drmIoctl(pdev->fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
   assert(!ret);
   assert(handle.fd >= 0);

   handle.handle = dst->temporary ? : dst->permanent;
   ret = drmIoctl(pdev->fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);
   assert(!ret);

   close(handle.fd);
}

VkResult
panvk_QueueSubmit(VkQueue _queue,
                  uint32_t submitCount,
                  const VkSubmitInfo *pSubmits,
                  VkFence _fence)
{
   VK_FROM_HANDLE(panvk_queue, queue, _queue);
   VK_FROM_HANDLE(panvk_fence, fence, _fence);
   const struct panfrost_device *pdev = &queue->device->physical_device->pdev;

   for (uint32_t i = 0; i < submitCount; ++i) {
      const VkSubmitInfo *submit = pSubmits + i;
      unsigned nr_in_fences = submit->waitSemaphoreCount + 1;
      uint32_t in_fences[nr_in_fences];
      
      in_fences[0] = queue->sync;
      for (unsigned i = 0; i < submit->waitSemaphoreCount; i++) {
         VK_FROM_HANDLE(panvk_semaphore, sem, submit->pWaitSemaphores[i]);

         in_fences[i + 1] = sem->syncobj.temporary ? : sem->syncobj.permanent;
      }

      for (uint32_t j = 0; j < submit->commandBufferCount; ++j) {
         VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, (submit->pCommandBuffers[j]));

         list_for_each_entry(struct panvk_batch, batch, &cmdbuf->batches, node) {
            /* FIXME: should be done at the batch level */
            unsigned nr_bos =
               util_dynarray_num_elements(&cmdbuf->desc_pool.bos, struct panfrost_bo *) +
               util_dynarray_num_elements(&cmdbuf->varying_pool.bos, struct panfrost_bo *) +
               util_dynarray_num_elements(&cmdbuf->tls_pool.bos, struct panfrost_bo *) +
               (batch->fb.info ? batch->fb.info->attachment_count : 0) +
               (batch->blit.src ? 1 : 0) +
               (batch->blit.dst ? 1 : 0) +
               (batch->scoreboard.first_tiler ? 1 : 0) + 1;
            unsigned bo_idx = 0;
            uint32_t bos[nr_bos];

            util_dynarray_foreach(&cmdbuf->desc_pool.bos, struct panfrost_bo *, bo) {
               bos[bo_idx++] = (*bo)->gem_handle;
            }

            util_dynarray_foreach(&cmdbuf->varying_pool.bos, struct panfrost_bo *, bo) {
               bos[bo_idx++] = (*bo)->gem_handle;
            }

            util_dynarray_foreach(&cmdbuf->tls_pool.bos, struct panfrost_bo *, bo) {
               bos[bo_idx++] = (*bo)->gem_handle;
            }

            if (batch->fb.info) {
               for (unsigned i = 0; i < batch->fb.info->attachment_count; i++) {
                  bos[bo_idx++] = batch->fb.info->attachments[i].iview->pview.image->data.bo->gem_handle;
               }
            }

            if (batch->blit.src)
               bos[bo_idx++] = batch->blit.src->gem_handle;

            if (batch->blit.dst)
               bos[bo_idx++] = batch->blit.dst->gem_handle;

            if (batch->scoreboard.first_tiler)
               bos[bo_idx++] = pdev->tiler_heap->gem_handle;

            bos[bo_idx++] = pdev->sample_positions->gem_handle;
            assert(bo_idx == nr_bos);
            panvk_queue_submit_batch(queue, batch, bos, nr_bos, in_fences, nr_in_fences);
         }
      }

      /* Transfer the out fence to signal semaphores */
      for (unsigned i = 0; i < submit->signalSemaphoreCount; i++) {
         VK_FROM_HANDLE(panvk_semaphore, sem, submit->pSignalSemaphores[i]);
         panvk_queue_transfer_sync(queue, &sem->syncobj);
      }
   }

   if (fence) {
      /* Transfer the last out fence to the fence object */
      panvk_queue_transfer_sync(queue, &fence->syncobj);
   }

   return VK_SUCCESS;
}

VkResult
panvk_QueueWaitIdle(VkQueue _queue)
{
   VK_FROM_HANDLE(panvk_queue, queue, _queue);

   if (panvk_device_is_lost(queue->device))
      return VK_ERROR_DEVICE_LOST;

   const struct panfrost_device *pdev = &queue->device->physical_device->pdev;
   struct drm_syncobj_wait wait = {
      .handles = (uint64_t) (uintptr_t)(&queue->sync),
      .count_handles = 1,
      .timeout_nsec = INT64_MAX,
      .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
   };
   int ret;

   ret = drmIoctl(pdev->fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
   assert(!ret);

   return VK_SUCCESS;
}

VkResult
panvk_DeviceWaitIdle(VkDevice _device)
{
   VK_FROM_HANDLE(panvk_device, device, _device);

   if (panvk_device_is_lost(device))
      return VK_ERROR_DEVICE_LOST;

   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++) {
         panvk_QueueWaitIdle(panvk_queue_to_handle(&device->queues[i][q]));
      }
   }
   return VK_SUCCESS;
}

VkResult
panvk_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                           uint32_t *pPropertyCount,
                                           VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(&panvk_instance_extensions,
                                                     pPropertyCount, pProperties);
}

PFN_vkVoidFunction
panvk_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(panvk_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk,
                                    &panvk_instance_entrypoints,
                                    pName);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return panvk_GetInstanceProcAddr(instance, pName);
}

VkResult
panvk_AllocateMemory(VkDevice _device,
                     const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_device_memory *mem;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   if (pAllocateInfo->allocationSize == 0) {
      /* Apparently, this is allowed */
      *pMem = VK_NULL_HANDLE;
      return VK_SUCCESS;
   }

   mem = vk_object_alloc(&device->vk, pAllocator, sizeof(*mem),
                         VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (mem == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext,
                           IMPORT_MEMORY_FD_INFO_KHR);

   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (fd_info) {
      assert(fd_info->handleType ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
             fd_info->handleType ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      /*
       * TODO Importing the same fd twice gives us the same handle without
       * reference counting.  We need to maintain a per-instance handle-to-bo
       * table and add reference count to panvk_bo.
       */
      mem->bo = panfrost_bo_import(&device->physical_device->pdev, fd_info->fd);
      /* take ownership and close the fd */
      close(fd_info->fd);
   } else {
      mem->bo = panfrost_bo_create(&device->physical_device->pdev,
                                   pAllocateInfo->allocationSize, 0,
                                   "User-requested memory");
   }

   assert(mem->bo);

   *pMem = panvk_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

void
panvk_FreeMemory(VkDevice _device,
                 VkDeviceMemory _mem,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   panfrost_bo_unreference(mem->bo);
   vk_object_free(&device->vk, pAllocator, mem);
}

VkResult
panvk_MapMemory(VkDevice _device,
                VkDeviceMemory _memory,
                VkDeviceSize offset,
                VkDeviceSize size,
                VkMemoryMapFlags flags,
                void **ppData)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, _memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (!mem->bo->ptr.cpu)
      panfrost_bo_mmap(mem->bo);

   *ppData = mem->bo->ptr.cpu;

   if (*ppData) {
      *ppData += offset;
      return VK_SUCCESS;
   }

   return vk_error(device->instance, VK_ERROR_MEMORY_MAP_FAILED);
}

void
panvk_UnmapMemory(VkDevice _device, VkDeviceMemory _memory)
{
}

VkResult
panvk_FlushMappedMemoryRanges(VkDevice _device,
                              uint32_t memoryRangeCount,
                              const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VkResult
panvk_InvalidateMappedMemoryRanges(VkDevice _device,
                                   uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

void
panvk_GetBufferMemoryRequirements(VkDevice _device,
                                  VkBuffer _buffer,
                                  VkMemoryRequirements *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, _buffer);

   pMemoryRequirements->memoryTypeBits = 1;
   pMemoryRequirements->alignment = 64;
   pMemoryRequirements->size =
      align64(buffer->size, pMemoryRequirements->alignment);
}

void
panvk_GetBufferMemoryRequirements2(VkDevice device,
                                   const VkBufferMemoryRequirementsInfo2 *pInfo,
                                   VkMemoryRequirements2 *pMemoryRequirements)
{
   panvk_GetBufferMemoryRequirements(device, pInfo->buffer,
                                     &pMemoryRequirements->memoryRequirements);
}

void
panvk_GetImageMemoryRequirements(VkDevice _device,
                                 VkImage _image,
                                 VkMemoryRequirements *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_image, image, _image);

   pMemoryRequirements->memoryTypeBits = 1;
   pMemoryRequirements->size = panvk_image_get_total_size(image);
   pMemoryRequirements->alignment = 4096;
}

void
panvk_GetImageMemoryRequirements2(VkDevice device,
                                 const VkImageMemoryRequirementsInfo2 *pInfo,
                                 VkMemoryRequirements2 *pMemoryRequirements)
{
   panvk_GetImageMemoryRequirements(device, pInfo->image,
                                    &pMemoryRequirements->memoryRequirements);
}

void
panvk_GetImageSparseMemoryRequirements(VkDevice device, VkImage image,
                                       uint32_t *pSparseMemoryRequirementCount,
                                       VkSparseImageMemoryRequirements *pSparseMemoryRequirements)
{
   panvk_stub();
}

void
panvk_GetImageSparseMemoryRequirements2(VkDevice device,
                                        const VkImageSparseMemoryRequirementsInfo2 *pInfo,
                                        uint32_t *pSparseMemoryRequirementCount,
                                        VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   panvk_stub();
}

void
panvk_GetDeviceMemoryCommitment(VkDevice device,
                                VkDeviceMemory memory,
                                VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VkResult
panvk_BindBufferMemory2(VkDevice device,
                        uint32_t bindInfoCount,
                        const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(panvk_buffer, buffer, pBindInfos[i].buffer);

      if (mem) {
         buffer->bo = mem->bo;
         buffer->bo_offset = pBindInfos[i].memoryOffset;
      } else {
         buffer->bo = NULL;
      }
   }
   return VK_SUCCESS;
}

VkResult
panvk_BindBufferMemory(VkDevice device,
                       VkBuffer buffer,
                       VkDeviceMemory memory,
                       VkDeviceSize memoryOffset)
{
   const VkBindBufferMemoryInfo info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = buffer,
      .memory = memory,
      .memoryOffset = memoryOffset
   };

   return panvk_BindBufferMemory2(device, 1, &info);
}

VkResult
panvk_BindImageMemory2(VkDevice device,
                       uint32_t bindInfoCount,
                       const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(panvk_image, image, pBindInfos[i].image);
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);

      if (mem) {
         panfrost_bo_reference(mem->bo);
         image->pimage.data.bo = mem->bo;
         image->pimage.data.offset = pBindInfos[i].memoryOffset;
         /* Reset the AFBC headers */
         if (drm_is_afbc(image->pimage.layout.modifier)) {
            void *base = image->pimage.data.bo->ptr.cpu + image->pimage.data.offset;

            for (unsigned layer = 0; layer < image->pimage.layout.array_size; layer++) {
               for (unsigned level = 0; level < image->pimage.layout.nr_slices; level++) {
                  void *header = base +
                                 (layer * image->pimage.layout.array_stride) +
                                 image->pimage.layout.slices[level].offset;
                  memset(header, 0, image->pimage.layout.slices[level].afbc.header_size);
               }
            }
         }
      } else {
         panfrost_bo_unreference(image->pimage.data.bo);
         image->pimage.data.bo = NULL;
         image->pimage.data.offset = pBindInfos[i].memoryOffset;
      }
   }

   return VK_SUCCESS;
}

VkResult
panvk_BindImageMemory(VkDevice device,
                      VkImage image,
                      VkDeviceMemory memory,
                      VkDeviceSize memoryOffset)
{
   const VkBindImageMemoryInfo info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .image = image,
      .memory = memory,
      .memoryOffset = memoryOffset
   };

   return panvk_BindImageMemory2(device, 1, &info);
}

VkResult
panvk_QueueBindSparse(VkQueue _queue,
                      uint32_t bindInfoCount,
                      const VkBindSparseInfo *pBindInfo,
                      VkFence _fence)
{
   return VK_SUCCESS;
}

VkResult
panvk_CreateEvent(VkDevice _device,
                  const VkEventCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkEvent *pEvent)
{
   panvk_stub();
   return VK_SUCCESS;
}

void
panvk_DestroyEvent(VkDevice _device,
                   VkEvent _event,
                   const VkAllocationCallbacks *pAllocator)
{
   panvk_stub();
}

VkResult
panvk_GetEventStatus(VkDevice _device, VkEvent _event)
{
   panvk_stub();
   return VK_EVENT_RESET;
}

VkResult
panvk_SetEvent(VkDevice _device, VkEvent _event)
{
   panvk_stub();
   return VK_SUCCESS;
}

VkResult
panvk_ResetEvent(VkDevice _device, VkEvent _event)
{
   panvk_stub();
   return VK_SUCCESS;
}

VkResult
panvk_CreateBuffer(VkDevice _device,
                   const VkBufferCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

   buffer = vk_object_alloc(&device->vk, pAllocator, sizeof(*buffer),
                            VK_OBJECT_TYPE_BUFFER);
   if (buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->flags = pCreateInfo->flags;

   *pBuffer = panvk_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

void
panvk_DestroyBuffer(VkDevice _device,
                    VkBuffer _buffer,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_object_free(&device->vk, pAllocator, buffer);
}

VkResult
panvk_CreateFramebuffer(VkDevice _device,
                        const VkFramebufferCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkFramebuffer *pFramebuffer)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_framebuffer *framebuffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

   size_t size = sizeof(*framebuffer) + sizeof(struct panvk_attachment_info) *
                                           pCreateInfo->attachmentCount;
   framebuffer = vk_object_alloc(&device->vk, pAllocator, size,
                                 VK_OBJECT_TYPE_FRAMEBUFFER);
   if (framebuffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   framebuffer->attachment_count = pCreateInfo->attachmentCount;
   framebuffer->width = pCreateInfo->width;
   framebuffer->height = pCreateInfo->height;
   framebuffer->layers = pCreateInfo->layers;
   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      VkImageView _iview = pCreateInfo->pAttachments[i];
      struct panvk_image_view *iview = panvk_image_view_from_handle(_iview);
      framebuffer->attachments[i].iview = iview;
   }

   *pFramebuffer = panvk_framebuffer_to_handle(framebuffer);
   return VK_SUCCESS;
}

void
panvk_DestroyFramebuffer(VkDevice _device,
                         VkFramebuffer _fb,
                         const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_framebuffer, fb, _fb);

   if (fb)
      vk_object_free(&device->vk, pAllocator, fb);
}

static enum mali_mipmap_mode
panvk_translate_sampler_mipmap_mode(VkSamplerMipmapMode mode)
{
   switch (mode) {
   case VK_SAMPLER_MIPMAP_MODE_NEAREST: return MALI_MIPMAP_MODE_NEAREST;
   case VK_SAMPLER_MIPMAP_MODE_LINEAR: return MALI_MIPMAP_MODE_TRILINEAR;
   default: unreachable("Invalid mipmap mode");
   }
}

static unsigned
panvk_translate_sampler_address_mode(VkSamplerAddressMode mode)
{
   switch (mode) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT: return MALI_WRAP_MODE_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MALI_WRAP_MODE_MIRRORED_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return MALI_WRAP_MODE_CLAMP_TO_EDGE;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MALI_WRAP_MODE_CLAMP_TO_BORDER;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return MALI_WRAP_MODE_MIRRORED_CLAMP_TO_EDGE;
   default: unreachable("Invalid wrap");
   }
}

static enum mali_func
panvk_translate_sampler_compare_func(const VkSamplerCreateInfo *pCreateInfo)
{
   if (!pCreateInfo->compareEnable)
      return MALI_FUNC_NEVER;

   enum mali_func f = panvk_translate_compare_func(pCreateInfo->compareOp);
   return panfrost_flip_compare_func(f);
}

static void
panvk_init_midgard_sampler(struct panvk_sampler *sampler,
                           const VkSamplerCreateInfo *pCreateInfo)
{
   const VkSamplerCustomBorderColorCreateInfoEXT *pBorderColor =
      vk_find_struct_const(pCreateInfo->pNext, SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT);

   pan_pack(&sampler->desc, MIDGARD_SAMPLER, cfg) {
      cfg.magnify_nearest = pCreateInfo->magFilter == VK_FILTER_NEAREST;
      cfg.minify_nearest = pCreateInfo->minFilter == VK_FILTER_NEAREST;
      cfg.mipmap_mode = panvk_translate_sampler_mipmap_mode(pCreateInfo->mipmapMode);
      cfg.normalized_coordinates = !pCreateInfo->unnormalizedCoordinates;
      cfg.lod_bias = FIXED_16(pCreateInfo->mipLodBias, true);
      cfg.minimum_lod = FIXED_16(pCreateInfo->minLod, false);
      cfg.maximum_lod = FIXED_16(pCreateInfo->maxLod, false);

      cfg.wrap_mode_s = panvk_translate_sampler_address_mode(pCreateInfo->addressModeU);
      cfg.wrap_mode_t = panvk_translate_sampler_address_mode(pCreateInfo->addressModeV);
      cfg.wrap_mode_r = panvk_translate_sampler_address_mode(pCreateInfo->addressModeW);
      cfg.compare_function = panvk_translate_sampler_compare_func(pCreateInfo);

      switch (pCreateInfo->borderColor) {
      case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
      case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
         cfg.border_color_r = fui(0.0);
         cfg.border_color_g = fui(0.0);
         cfg.border_color_b = fui(0.0);
         cfg.border_color_a =
            pCreateInfo->borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ?
            fui(1.0) : fui(0.0);
         break;
      case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
      case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
         cfg.border_color_r = 0;
         cfg.border_color_g = 0;
         cfg.border_color_b = 0;
         cfg.border_color_a =
            pCreateInfo->borderColor == VK_BORDER_COLOR_INT_OPAQUE_BLACK ?
            UINT_MAX : 0;
         break;
      case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
         cfg.border_color_r = fui(1.0);
         cfg.border_color_g = fui(1.0);
         cfg.border_color_b = fui(1.0);
         cfg.border_color_a = fui(1.0);
         break;
      case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
         cfg.border_color_r = UINT_MAX;
         cfg.border_color_g = UINT_MAX;
         cfg.border_color_b = UINT_MAX;
         cfg.border_color_a = UINT_MAX;
         break;
      case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT:
      case VK_BORDER_COLOR_INT_CUSTOM_EXT:
         cfg.border_color_r = pBorderColor->customBorderColor.int32[0];
         cfg.border_color_g = pBorderColor->customBorderColor.int32[1];
         cfg.border_color_b = pBorderColor->customBorderColor.int32[2];
         cfg.border_color_a = pBorderColor->customBorderColor.int32[3];
         break;
      default:
         unreachable("Invalid border color");
      }
   }
}

static void
panvk_init_bifrost_sampler(struct panvk_sampler *sampler,
                           const VkSamplerCreateInfo *pCreateInfo)
{
   const VkSamplerCustomBorderColorCreateInfoEXT *pBorderColor =
      vk_find_struct_const(pCreateInfo->pNext, SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT);

   pan_pack(&sampler->desc, BIFROST_SAMPLER, cfg) {
      cfg.point_sample_magnify = pCreateInfo->magFilter == VK_FILTER_LINEAR;
      cfg.point_sample_minify = pCreateInfo->minFilter == VK_FILTER_LINEAR;
      cfg.mipmap_mode = panvk_translate_sampler_mipmap_mode(pCreateInfo->mipmapMode);
      cfg.normalized_coordinates = !pCreateInfo->unnormalizedCoordinates;

      cfg.lod_bias = FIXED_16(pCreateInfo->mipLodBias, true);
      cfg.minimum_lod = FIXED_16(pCreateInfo->minLod, false);
      cfg.maximum_lod = FIXED_16(pCreateInfo->maxLod, false);
      cfg.wrap_mode_s = panvk_translate_sampler_address_mode(pCreateInfo->addressModeU);
      cfg.wrap_mode_t = panvk_translate_sampler_address_mode(pCreateInfo->addressModeV);
      cfg.wrap_mode_r = panvk_translate_sampler_address_mode(pCreateInfo->addressModeW);
      cfg.compare_function = panvk_translate_sampler_compare_func(pCreateInfo);

      switch (pCreateInfo->borderColor) {
      case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
      case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
         cfg.border_color_r = fui(0.0);
         cfg.border_color_g = fui(0.0);
         cfg.border_color_b = fui(0.0);
         cfg.border_color_a =
            pCreateInfo->borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ?
            fui(1.0) : fui(0.0);
         break;
      case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
      case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
         cfg.border_color_r = 0;
         cfg.border_color_g = 0;
         cfg.border_color_b = 0;
         cfg.border_color_a =
            pCreateInfo->borderColor == VK_BORDER_COLOR_INT_OPAQUE_BLACK ?
            UINT_MAX : 0;
         break;
      case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
         cfg.border_color_r = fui(1.0);
         cfg.border_color_g = fui(1.0);
         cfg.border_color_b = fui(1.0);
         cfg.border_color_a = fui(1.0);
         break;
      case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
         cfg.border_color_r = UINT_MAX;
         cfg.border_color_g = UINT_MAX;
         cfg.border_color_b = UINT_MAX;
         cfg.border_color_a = UINT_MAX;
         break;
      case VK_BORDER_COLOR_FLOAT_CUSTOM_EXT:
      case VK_BORDER_COLOR_INT_CUSTOM_EXT:
         cfg.border_color_r = pBorderColor->customBorderColor.int32[0];
         cfg.border_color_g = pBorderColor->customBorderColor.int32[1];
         cfg.border_color_b = pBorderColor->customBorderColor.int32[2];
         cfg.border_color_a = pBorderColor->customBorderColor.int32[3];
         break;
      default:
         unreachable("Invalid border color");
      }
   }
}

static void
panvk_init_sampler(struct panvk_device *device,
                   struct panvk_sampler *sampler,
                   const VkSamplerCreateInfo *pCreateInfo)
{
   if (pan_is_bifrost(&device->physical_device->pdev))
      panvk_init_bifrost_sampler(sampler, pCreateInfo);
   else
      panvk_init_midgard_sampler(sampler, pCreateInfo);
}

VkResult
panvk_CreateSampler(VkDevice _device,
                    const VkSamplerCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkSampler *pSampler)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_sampler *sampler;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = vk_object_alloc(&device->vk, pAllocator, sizeof(*sampler),
                             VK_OBJECT_TYPE_SAMPLER);
   if (!sampler)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   panvk_init_sampler(device, sampler, pCreateInfo);
   *pSampler = panvk_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

void
panvk_DestroySampler(VkDevice _device,
                     VkSampler _sampler,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_sampler, sampler, _sampler);

   if (!sampler)
      return;

   vk_object_free(&device->vk, pAllocator, sampler);
}

/* vk_icd.h does not declare this function, so we declare it here to
 * suppress Wmissing-prototypes.
 */
PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion);

PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
   /* For the full details on loader interface versioning, see
    * <https://github.com/KhronosGroup/Vulkan-LoaderAndValidationLayers/blob/master/loader/LoaderAndLayerInterface.md>.
    * What follows is a condensed summary, to help you navigate the large and
    * confusing official doc.
    *
    *   - Loader interface v0 is incompatible with later versions. We don't
    *     support it.
    *
    *   - In loader interface v1:
    *       - The first ICD entrypoint called by the loader is
    *         vk_icdGetInstanceProcAddr(). The ICD must statically expose this
    *         entrypoint.
    *       - The ICD must statically expose no other Vulkan symbol unless it
    * is linked with -Bsymbolic.
    *       - Each dispatchable Vulkan handle created by the ICD must be
    *         a pointer to a struct whose first member is VK_LOADER_DATA. The
    *         ICD must initialize VK_LOADER_DATA.loadMagic to
    * ICD_LOADER_MAGIC.
    *       - The loader implements vkCreate{PLATFORM}SurfaceKHR() and
    *         vkDestroySurfaceKHR(). The ICD must be capable of working with
    *         such loader-managed surfaces.
    *
    *    - Loader interface v2 differs from v1 in:
    *       - The first ICD entrypoint called by the loader is
    *         vk_icdNegotiateLoaderICDInterfaceVersion(). The ICD must
    *         statically expose this entrypoint.
    *
    *    - Loader interface v3 differs from v2 in:
    *        - The ICD must implement vkCreate{PLATFORM}SurfaceKHR(),
    *          vkDestroySurfaceKHR(), and other API which uses VKSurfaceKHR,
    *          because the loader no longer does so.
    */
   *pSupportedVersion = MIN2(*pSupportedVersion, 3u);
   return VK_SUCCESS;
}

VkResult
panvk_GetMemoryFdKHR(VkDevice _device,
                     const VkMemoryGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   /* At the moment, we support only the below handle types. */
   assert(pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
          pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int prime_fd = panfrost_bo_export(memory->bo);
   if (prime_fd < 0)
      return vk_error(device->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VkResult
panvk_GetMemoryFdPropertiesKHR(VkDevice _device,
                               VkExternalMemoryHandleTypeFlagBits handleType,
                               int fd,
                               VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   assert(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   pMemoryFdProperties->memoryTypeBits = 1;
   return VK_SUCCESS;
}

void
panvk_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice,
                                                   const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
                                                   VkExternalSemaphoreProperties *pExternalSemaphoreProperties)
{
   if ((pExternalSemaphoreInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT ||
        pExternalSemaphoreInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)) {
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT |
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->compatibleHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT |
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
      pExternalSemaphoreProperties->compatibleHandleTypes = 0;
      pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
   }
}

void
panvk_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice,
                                               const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
                                               VkExternalFenceProperties *pExternalFenceProperties)
{
   pExternalFenceProperties->exportFromImportedHandleTypes = 0;
   pExternalFenceProperties->compatibleHandleTypes = 0;
   pExternalFenceProperties->externalFenceFeatures = 0;
}

void
panvk_GetDeviceGroupPeerMemoryFeatures(VkDevice device,
                                       uint32_t heapIndex,
                                       uint32_t localDeviceIndex,
                                       uint32_t remoteDeviceIndex,
                                       VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   assert(localDeviceIndex == remoteDeviceIndex);

   *pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT |
                          VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
                          VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT |
                          VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}
