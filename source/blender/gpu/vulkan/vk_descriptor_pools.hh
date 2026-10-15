/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_vector.hh"

#include "vk_descriptor_set.hh"

namespace blender::gpu {
class VKDevice;

/**
 * List of VkDescriptorPools.
 *
 * In Vulkan a pool is constructed with a fixed size per resource type. When more resources are
 * needed it a next pool should be created. VKDescriptorPools will keep track of those pools and
 * construct new pools when the previous one is exhausted.
 */
class VKDescriptorPools {
  /**
   * Pool sizes to use. When one descriptor pool is requested to allocate a descriptor but isn't
   * able to do so, it will fail.
   *
   * See VKDescriptorSetTracker::upload_descriptor_sets for rebalancing the pool sizes.
   */
  static constexpr uint32_t POOL_SIZE_STORAGE_BUFFER = 1000;
  static constexpr uint32_t POOL_SIZE_DESCRIPTOR_SETS = 250;
  static constexpr uint32_t POOL_SIZE_STORAGE_IMAGE = 250;
  static constexpr uint32_t POOL_SIZE_COMBINED_IMAGE_SAMPLER = 250;
  static constexpr uint32_t POOL_SIZE_UNIFORM_BUFFER = 500;
  static constexpr uint32_t POOL_SIZE_UNIFORM_TEXEL_BUFFER = 100;
  static constexpr uint32_t POOL_SIZE_INPUT_ATTACHMENT = 100;
  static constexpr uint32_t POOL_SIZE_ACCELERATION_STRUCTURE = 100;

  /**
   * Unused recycled pools.
   *
   * When a pool is full it is being discarded (for reuse). After all descriptor sets of the pool
   * are unused the descriptor pool can be reused.
   * Note: descriptor pools/sets are pinned to a single thread so the pools should always return to
   * the instance it was created on.
   */
  Vector<VkDescriptorPool> recycled_pools_;
  /** Active descriptor pool. Should always be a valid handle. */
  VkDescriptorPool vk_descriptor_pool_ = VK_NULL_HANDLE;
  Mutex mutex_;

 public:
  ~VKDescriptorPools();

  void init(const VKDevice &vk_device);

  /**
   * Allocate a new descriptor set.
   *
   * When the active descriptor pool is full it is discarded and another descriptor pool is
   * ensured.
   */
  VkDescriptorSet allocate(const VkDescriptorSetLayout descriptor_set_layout);

  /**
   * Allocate with bounded retry on pool-exhausted errors.
   *
   * MTK/Mali drivers report an exhausted pool as VK_ERROR_OUT_OF_DEVICE_MEMORY instead of
   * VK_ERROR_OUT_OF_POOL_MEMORY. Without retry on all three error codes, the full pool stays
   * active and every later allocation records a VK_NULL_HANDLE set, crashing the driver in the
   * submission thread within minutes.
   */
  VkDescriptorSet allocate_with_retries(const VkDescriptorSetLayout descriptor_set_layout);
  /**
   * After a descriptor pool has been discarded and recycled, force the GPU to finish
   * and invalidate any cached descriptor set handles so they are not reused from a
   * stale pool. Returns a small struct with the post-sync state.
   */
  struct AfterPoolDiscard {
    bool state_was_dirty;
    uint32_t descriptor_sets_invalidated;
  };
  static AfterPoolDiscard sync_after_pool_discard(VKContext &context);

  /**
   * Recycle a previous discarded descriptor pool.
   */
  void recycle(VkDescriptorPool vk_descriptor_pool);

 private:
  void add_new_pool(const VKDevice &device);
  void discard_active_pool(VKContext &vk_context);
  void ensure_pool(const VKDevice &device);

  /** Bounded recovery budget for allocate_with_retries(). */
  static constexpr uint32_t MAX_POOL_ALLOCATION_RETRIES = 8;
  uint32_t allocation_retries_ = MAX_POOL_ALLOCATION_RETRIES;
};
}  // namespace blender::gpu
