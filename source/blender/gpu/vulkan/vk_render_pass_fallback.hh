/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Emulates VK_KHR_dynamic_rendering using classic VkRenderPass/VkFramebuffer for
 * devices that don't support it (e.g. Vulkan 1.1 Adreno GPUs).
 *
 * All images in the Vulkan backend are single-sample, so there is no multisample
 * resolve to emulate. Render passes are layout-neutral (initialLayout ==
 * finalLayout == the attachment layout the render graph already transitioned to),
 * so the render graph keeps owning every barrier - the render pass performs no
 * layout transitions of its own.
 */

#pragma once

#include <atomic>
#include <cstring>
#include <mutex>

#include "xxhash.h"

#include "BLI_map.hh"
#include "BLI_span.hh"

#include "vk_common.hh"

namespace blender::gpu {

class VKDiscardPool;

namespace render_graph {
struct VKBeginRenderingData;
/* VKCommandBufferInterface lives in this namespace (vk_command_buffer_wrapper.hh), not in
 * blender::gpu; declaring it in the enclosing namespace would define an unrelated type that no
 * call site can bind to. */
class VKCommandBufferInterface;

/**
 * Begin/end a render scope through a VkRenderPass instead of dynamic rendering. Defined in the .cc
 * (they need the concrete VKBeginRenderingData) and called from the begin/end rendering nodes when
 * VKRenderPassFallback::is_active().
 */
void begin_rendering_fallback(VKCommandBufferInterface &command_buffer,
                              const VKBeginRenderingData &data);
void end_rendering_fallback(VKCommandBufferInterface &command_buffer);
}  // namespace render_graph

class VKRenderPassFallback {
 private:
  VkDevice vk_device_ = VK_NULL_HANDLE;
  const VolkDeviceTable *functions_ = nullptr;

 public:
  /**
   * True when the device lacks VK_KHR_dynamic_rendering and every render scope must go through
   * VkRenderPass instead.
   *
   * Written once during device init on the main thread, then read on the submission thread (every
   * render scope) and on the view-discard thread, so it is atomic rather than a plain bool -- a
   * plain read of a value another thread wrote is a data race, and this one decides which Vulkan
   * entry point a command buffer uses. Relaxed is enough and free: init publishes it before the
   * submission thread is created, so there is nothing to synchronise against.
   */
  static bool is_active()
  {
    return is_active_.load(std::memory_order_relaxed);
  }
  static void set_active(bool active)
  {
    is_active_.store(active, std::memory_order_relaxed);
  }

 private:
  static std::atomic<bool> is_active_;

  /* POD keys: zero-initialized then filled, hashed/compared over their bytes so
   * padding stays deterministic. */
  struct RenderPassKey {
    uint32_t color_count;
    VkFormat color_formats[8];
    uint8_t color_load[8];
    uint8_t color_store[8];
    /** Per attachment: attachments in one pass can be in different layouts. */
    VkImageLayout color_layouts[8];
    VkFormat depth_format;
    VkFormat stencil_format;
    uint8_t depth_load, depth_store, stencil_load, stencil_store;
    VkImageLayout depth_layout;

    uint64_t hash() const
    {
      return XXH3_64bits(this, sizeof(*this));
    }
    bool operator==(const RenderPassKey &o) const
    {
      return memcmp(this, &o, sizeof(*this)) == 0;
    }
  };

  struct FramebufferKey {
    VkRenderPass render_pass;
    VkImageView views[9];
    uint32_t view_count;
    uint32_t width, height, layers;

    uint64_t hash() const
    {
      return XXH3_64bits(this, sizeof(*this));
    }
    bool operator==(const FramebufferKey &o) const
    {
      return memcmp(this, &o, sizeof(*this)) == 0;
    }
  };

  Map<RenderPassKey, VkRenderPass> render_passes_;
  Map<FramebufferKey, VkFramebuffer> framebuffers_;

  /**
   * One-entry cache in front of each map, checked before hashing.
   *
   * A frame is mostly many render scopes (the Redmi logged 3701 of them in a few seconds), and
   * consecutive scopes very often share the same attachment set -- the viewport's own scope
   * repeats every frame, and a deferred renderer draws many sub-passes against the same target.
   * Comparing the POD key is a memcmp of ~100 bytes against a value already in cache, against an
   * XXH3 over the same bytes plus a map probe. Both entries are only touched under `mutex_`, so
   * this does not change the locking contract.
   */
  RenderPassKey last_render_pass_key_;
  VkRenderPass last_render_pass_ = VK_NULL_HANDLE;
  FramebufferKey last_framebuffer_key_;
  VkFramebuffer last_framebuffer_ = VK_NULL_HANDLE;

  /** Guards the caches (accessed from the submission thread and view-discard). */
  std::mutex mutex_;

  /** Caller must hold `mutex_`. */
  VkRenderPass render_pass_from_key(const RenderPassKey &key);
  void render_pass_key_from_data(const render_graph::VKBeginRenderingData &data,
                                 RenderPassKey &r_key) const;
  /** Leaves `render_pass` at VK_NULL_HANDLE; the caller fills it once it has the pass. */
  void framebuffer_key_from_data(const render_graph::VKBeginRenderingData &data,
                                 FramebufferKey &r_key) const;

 public:
  void init(VkDevice vk_device, const VolkDeviceTable &functions)
  {
    vk_device_ = vk_device;
    functions_ = &functions;
  }
  void deinit();

  /** Render pass matching the actual load/store ops, for vkCmdBeginRenderPass. */
  VkRenderPass render_pass_get(const render_graph::VKBeginRenderingData &data);

  /**
   * Render pass and framebuffer for a scope, in one call. The only entry point for render scopes;
   * pipelines use compat_render_pass_get() instead.
   *
   * begin_rendering_fallback() needs both for every scope, and each lookup takes `mutex_`. Doing
   * them together halves the lock traffic and lets the one-entry caches absorb the repeat case
   * with a single lock acquisition per scope.
   */
  void render_scope_get(const render_graph::VKBeginRenderingData &data,
                        VkRenderPass *r_vk_render_pass,
                        VkFramebuffer *r_vk_framebuffer);

  /**
   * A render pass compatible (same formats/attachment layout) with the one above,
   * built with generic LOAD/STORE ops, for graphics-pipeline creation.
   */
  VkRenderPass compat_render_pass_get(Span<VkFormat> color_formats,
                                      VkFormat depth_format,
                                      VkFormat stencil_format);

  /** Framebuffer wrapping the attachment image views for `data`. */
  VkFramebuffer framebuffer_get(VkRenderPass vk_render_pass,
                                const render_graph::VKBeginRenderingData &data);

  /**
   * Retire (into `pool`, deferred to its timeline) any cached framebuffer that
   * references `vk_image_view`. Called when a view is discarded so a recycled
   * view handle can't match a stale cached framebuffer.
   */
  void discard_framebuffers_for_view(VkImageView vk_image_view, VKDiscardPool &pool);

  int64_t render_pass_count() const
  {
    return render_passes_.size();
  }
  int64_t framebuffer_count() const
  {
    return framebuffers_.size();
  }
};

}  // namespace blender::gpu
