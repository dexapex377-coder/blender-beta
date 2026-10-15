/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_capabilities.hh"
#include "GPU_context.hh"

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_descriptor_pools.hh"
#include "vk_device.hh"
#include "vk_state_manager.hh"

namespace blender::gpu {

VKDescriptorPools::~VKDescriptorPools()
{
  const VKDevice &device = VKBackend::get().device;
  for (const VkDescriptorPool vk_descriptor_pool : recycled_pools_) {
    device.functions.vkDestroyDescriptorPool(device.vk_handle(), vk_descriptor_pool, nullptr);
  }
  recycled_pools_.clear();
  if (vk_descriptor_pool_ != VK_NULL_HANDLE) {
    device.functions.vkDestroyDescriptorPool(device.vk_handle(), vk_descriptor_pool_, nullptr);
    vk_descriptor_pool_ = VK_NULL_HANDLE;
  }
}

void VKDescriptorPools::init(const VKDevice &device)
{
  ensure_pool(device);
}

void VKDescriptorPools::ensure_pool(const VKDevice &device)
{
  if (vk_descriptor_pool_ != VK_NULL_HANDLE) {
    return;
  }

  std::scoped_lock lock(mutex_);
  if (!recycled_pools_.is_empty()) {
    vk_descriptor_pool_ = recycled_pools_.pop_last();
    return;
  }

  Vector<VkDescriptorPoolSize> pool_sizes = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, POOL_SIZE_STORAGE_BUFFER},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, POOL_SIZE_STORAGE_IMAGE},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POOL_SIZE_COMBINED_IMAGE_SAMPLER},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, POOL_SIZE_UNIFORM_BUFFER},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, POOL_SIZE_UNIFORM_TEXEL_BUFFER},
      {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, POOL_SIZE_INPUT_ATTACHMENT}};
  if (GPU_ray_query_support()) {
    pool_sizes.append(
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, POOL_SIZE_ACCELERATION_STRUCTURE});
  }
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = POOL_SIZE_DESCRIPTOR_SETS;
  pool_info.poolSizeCount = pool_sizes.size();
  pool_info.pPoolSizes = pool_sizes.data();
  device.functions.vkCreateDescriptorPool(
      device.vk_handle(), &pool_info, nullptr, &vk_descriptor_pool_);
}

void VKDescriptorPools::discard_active_pool(VKContext &context)
{
  context.discard_pool.discard_descriptor_pool_for_reuse(vk_descriptor_pool_, this);
  vk_descriptor_pool_ = VK_NULL_HANDLE;
  /* Ported from kronos3D. Without this, all descriptor sets of the discarded pool
   * (including the one VKDescriptorSetTracker caches for reuse) stay stale after the
   * pool is recycled and reset by the submission thread. The MTK driver crashes with
   * a null-deref inside upload_descriptor_sets when writing into a set whose pool was
   * reset -- SIGSEGV fault addr 0x38 in vulkan.mtk.so, killing the session every
   * ~2 minutes. Force a state refresh so the next draw re-allocates instead of
   * reusing a stale handle, and drop the tracker's cached set + pending writes. */
  context.state_manager_get().is_dirty = true;
  context.descriptor_set_get().invalidate();
}

VKDescriptorPools::AfterPoolDiscard VKDescriptorPools::sync_after_pool_discard(
    VKContext &context)
{
  /* GPU_finish() is a full barrier: the CPU waits for every previously submitted
   * command to complete before the caller reads anything the GPU writes. Without
   * it the statistics read in eevee_shadow.cc races against the shadow pass still
   * in flight, which on MTK/PowerVR surfaced as a black cube. */
  GPU_finish();

  AfterPoolDiscard result;
  result.state_was_dirty = context.state_manager_get().is_dirty;
  context.state_manager_get().is_dirty = true;
  context.descriptor_set_get().invalidate();
  result.descriptor_sets_invalidated = 1;
  return result;
}

void VKDescriptorPools::recycle(VkDescriptorPool vk_descriptor_pool)
{
  const VKDevice &device = VKBackend::get().device;
  device.functions.vkResetDescriptorPool(device.vk_handle(), vk_descriptor_pool, 0);
  std::scoped_lock lock(mutex_);
  recycled_pools_.append(vk_descriptor_pool);
}

VkDescriptorSet VKDescriptorPools::allocate(const VkDescriptorSetLayout descriptor_set_layout)
{
  /* Ported from kronos3D. Each top-level allocation owns a fresh recovery budget;
   * the recursive retry path only drains it for the duration of a single descriptor
   * set allocation, then hands back to a fresh top-level budget on the next call.
   * This keeps the loop bounded per allocation while allowing the whole recovery
   * cycle to run many times over a long session. */
  allocation_retries_ = MAX_POOL_ALLOCATION_RETRIES;
  return allocate_with_retries(descriptor_set_layout);
}

VkDescriptorSet VKDescriptorPools::allocate_with_retries(
    const VkDescriptorSetLayout descriptor_set_layout)
{
  BLI_assert(descriptor_set_layout != VK_NULL_HANDLE);
  BLI_assert(vk_descriptor_pool_ != VK_NULL_HANDLE);
  const VKDevice &device = VKBackend::get().device;

  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = vk_descriptor_pool_;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &descriptor_set_layout;
  VkDescriptorSet vk_descriptor_set = VK_NULL_HANDLE;
  VkResult result = device.functions.vkAllocateDescriptorSets(
      device.vk_handle(), &allocate_info, &vk_descriptor_set);

  /* Ported from kronos3D. MTK/Mali reports an exhausted pool as
   * VK_ERROR_OUT_OF_DEVICE_MEMORY instead of VK_ERROR_OUT_OF_POOL_MEMORY: in this
   * driver the pool's descriptor budget is backed by the device heap, so running out
   * surfaces as a heap error. Treating only OUT_OF_POOL_MEMORY / FRAGMENTED_POOL as
   * recoverable leaves the full pool permanently active; every later allocation
   * records a VK_NULL_HANDLE set in the render graph, which then crashes the driver
   * (null deref) inside the submission thread -- SIGSEGV fault addr 0x38, killing the
   * session every ~2 minutes. Recover from all three the same way: discard the full
   * pool, ensure a fresh one and retry (bounded, so a genuine device OOM does not spin). */
  if (ELEM(result,
           VK_ERROR_OUT_OF_POOL_MEMORY,
           VK_ERROR_FRAGMENTED_POOL,
           VK_ERROR_OUT_OF_DEVICE_MEMORY) &&
      allocation_retries_ > 0)
  {
    allocation_retries_--;
    {
      VKContext &context = *VKContext::get();
      discard_active_pool(context);
      ensure_pool(device);
    }
    return allocate_with_retries(descriptor_set_layout);
  }

  return vk_descriptor_set;
}

}  // namespace blender::gpu
