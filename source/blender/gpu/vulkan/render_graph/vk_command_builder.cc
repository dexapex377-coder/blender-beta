/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_command_builder.hh"
#include "BLI_index_range.hh"
#include "vk_backend.hh"
#include "vk_render_graph.hh"
#include "vk_render_pass_fallback.hh"
#include "vk_to_string.hh"

#include <sstream>

#ifdef __ANDROID__
#  include <android/log.h>
#  include <cstdlib>
#  include <sys/system_properties.h>
#endif

namespace blender::gpu::render_graph {

static VkImageLayout to_default_image_layout(VkImageAspectFlags aspect, bool use_local_read)
{
  if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
    if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  }
  if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
    return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
  }
  return use_local_read ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR :
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

/* -------------------------------------------------------------------- */
/** \name Build nodes
 * \{ */

void VKCommandBuilder::build_nodes(VKRenderGraph &render_graph,
                                   VKCommandBufferInterface &command_buffer,
                                   Span<NodeHandle> node_handles)
{
  groups_init(render_graph, node_handles);
  groups_extract_barriers(
      render_graph, node_handles, command_buffer.use_dynamic_rendering_local_read);
}

#ifdef __ANDROID__
void VKCommandBuilder::scope_detail(const VKBeginRenderingData &begin_data) const
{
  static const bool log_enabled = []() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.blender.perf", value) > 0 && atoi(value) != 0;
  }();
  if (!log_enabled) {
    return;
  }

  /* Identity of the attachment set, so two scopes printing the same tag are candidates for being
   * merged into one render pass. The views are the framebuffer's identity; the render area and the
   * attachment count have to agree as well, since a merge also requires the same subpass shape. */
  uint64_t view_tag = 1469598103934665603ull;
  auto mix = [&view_tag](uint64_t v) {
    view_tag = (view_tag ^ v) * 1099511628211ull;
  };
  for (uint32_t i = 0; i < begin_data.vk_rendering_info.colorAttachmentCount; i++) {
    mix(reinterpret_cast<uintptr_t>(begin_data.color_attachments[i].imageView));
  }
  mix(reinterpret_cast<uintptr_t>(begin_data.depth_attachment.imageView));
  mix(reinterpret_cast<uintptr_t>(begin_data.stencil_attachment.imageView));

  const VkRect2D &area = begin_data.vk_rendering_info.renderArea;
  uint32_t depth_op = 0;
  if (begin_data.vk_rendering_info.pDepthAttachment != nullptr) {
    depth_op = begin_data.depth_attachment.loadOp;
  }

  __android_log_print(ANDROID_LOG_WARN,
                      "blender-scope",
                      "  scope set=%016llx area=%dx%d+%d+%d color=%u depth=%u loadOp=%u "
                      "color0=%016llx",
                      (unsigned long long)view_tag,
                      area.extent.width,
                      area.extent.height,
                      area.offset.x,
                      area.offset.y,
                      begin_data.vk_rendering_info.colorAttachmentCount,
                      begin_data.vk_rendering_info.pDepthAttachment != nullptr ? 1 : 0,
                      depth_op,
                      (unsigned long long)reinterpret_cast<uintptr_t>(
                          begin_data.color_attachments[0].imageView));
}
#endif

void VKCommandBuilder::record_commands(VKRenderGraph &render_graph,
                                       VKCommandBufferInterface &command_buffer,
                                       Span<NodeHandle> node_handles)
{
  groups_build_commands(render_graph, command_buffer, node_handles);

#ifdef __ANDROID__
  /* Report every 60 frames. Averaged, because a single frame of a rotating viewport has a very
   * different scope count from an idle one, and the interesting comparison is the steady state
   * under the same interaction. */
  if (++scope_report_frames >= 60) {
    static const bool log_enabled = []() {
      char value[PROP_VALUE_MAX] = {};
      return __system_property_get("debug.blender.perf", value) > 0 && atoi(value) != 0;
    }();
    if (log_enabled) {
      __android_log_print(ANDROID_LOG_WARN,
                          "blender-scope",
                          "scopes/frame=%u pixels/frame=%llu MPix/scope=%llu "
                          "loadOp: clear=%u load=%u dontcare=%u  fallback=%d  "
                          "adjacent=%u mergeable=%u loadok=%u repeat=%u repeatsep=%u",
                          uint32_t(scope_count / scope_report_frames),
                          (unsigned long long)(scope_pixels / scope_report_frames),
                          scope_count ?
                              (unsigned long long)(scope_pixels / scope_count / 1000) :
                              0ull,
                          uint32_t(scope_load_clear / scope_report_frames),
                          uint32_t(scope_load_keep / scope_report_frames),
                          uint32_t(scope_load_none / scope_report_frames),
                          int(VKRenderPassFallback::is_active()),
                          uint32_t(scope_adjacent / scope_report_frames),
                          uint32_t(scope_mergeable / scope_report_frames),
                          uint32_t(scope_mergeable_loadok / scope_report_frames),
                          uint32_t(scope_repeat / scope_report_frames),
                          uint32_t(scope_repeat_foreign / scope_report_frames));
    }
    scope_pixels = 0;
    scope_count = 0;
    scope_load_clear = 0;
    scope_load_keep = 0;
    scope_load_none = 0;
    scope_adjacent = 0;
    scope_mergeable = 0;
    scope_mergeable_loadok = 0;
    scope_repeat = 0;
    scope_repeat_foreign = 0;
    scope_report_frames = 0;
  }
#endif
}

void VKCommandBuilder::groups_init(const VKRenderGraph &render_graph,
                                   Span<NodeHandle> node_handles)
{
  group_nodes_.clear();
  IndexRange nodes_range = node_handles.index_range();
  while (!nodes_range.is_empty()) {
    IndexRange node_group = nodes_range.slice(0, 1);
    NodeHandle node_handle = node_handles[nodes_range.first()];
    const VKRenderGraphNode &node = render_graph.nodes_[node_handle];
    while (node_type_is_rendering(node.type) && node_group.size() < nodes_range.size()) {
      NodeHandle node_handle = node_handles[nodes_range[node_group.size()]];
      const VKRenderGraphNode &node = render_graph.nodes_[node_handle];
      if (!node_type_is_rendering(node.type) || node.type == VKNodeType::BEGIN_RENDERING) {
        break;
      }
      node_group = nodes_range.slice(0, node_group.size() + 1);
    }

    group_nodes_.append(node_group);
    nodes_range = nodes_range.drop_front(node_group.size());
  }
}

void VKCommandBuilder::groups_extract_barriers(VKRenderGraph &render_graph,
                                               Span<NodeHandle> node_handles,
                                               bool use_local_read)
{
  barrier_list_.clear();
  vk_buffer_memory_barriers_.clear();
  vk_image_memory_barriers_.clear();

  ImageTracker image_tracker(*this);

  /* Extract barriers. */
  group_pre_barriers_.clear();
  group_post_barriers_.clear();
  node_pre_barriers_.resize(node_handles.size());

  /* Keep track of the post barriers that needs to be added. The pre barriers will be stored
   * directly in `barrier_list_` but may not mingle with the pre barriers. Most barriers are
   * group pre barriers. */
  Vector<Barrier> post_barriers;
  /* Keep track of the node pre barriers that needs to be added. The pre barriers will be stored
   * directly in `barrier_list_` but may not mingle with the group barriers. */
  Vector<Barrier> node_pre_barriers;

  NodeHandle rendering_scope;
  bool rendering_active = false;

  for (const int64_t group_index : group_nodes_.index_range()) {
    /* Extract the pre-barriers of this group. */
    Barriers group_pre_barriers(barrier_list_.size(), 0);
    const GroupNodes &node_group = group_nodes_[group_index];
    for (const int64_t group_node_index : node_group) {
      NodeHandle node_handle = node_handles[group_node_index];
      VKRenderGraphNode &node = render_graph.nodes_[node_handle];
      Barrier barrier = {};
      build_pipeline_barriers(
          render_graph, node_handle, node.pipeline_stage_get(), image_tracker, barrier);
      if (!barrier.is_empty()) {
#if 0
        std::cout << __func__ << ": node_group=" << group_index
                  << ", node_group_range=" << node_group.first() << "-" << node_group.last()
                  << ", node_handle=" << node_handle << ", node_type=" << node.type
                  << ", debug_group=" << render_graph.full_debug_group(node_handle) << "\n";
        std::cout << __func__ << ": " << to_string_barrier(barrier);
#endif
        barrier_list_.append(barrier);
      }

      /* Reset the tracked layout of aliased images to undefined once their memory can be reused.
       */
      if (node.type == VKNodeType::SYNCHRONIZATION &&
          node.synchronization.reset_layout_to_undefined)
      {
        for (const VKRenderGraphImage &link : render_graph.linked_images(node_handle)) {
          VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
              link.resource.handle);
          resource.barrier_state.image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
      }

      /* Check for additional barriers when resuming rendering.
       *
       * Between suspending rendering and resuming the state/layout of resources can change and
       * require additional barriers.
       */
      else if (node.type == VKNodeType::BEGIN_RENDERING) {
        /* Begin rendering scope. */
        BLI_assert(!rendering_active);
        rendering_scope = node_handle;
        rendering_active = true;
        image_tracker.begin(render_graph, node_handle);
      }

      else if (node.type == VKNodeType::END_RENDERING) {
        /* End rendering scope. */
        BLI_assert(rendering_active);

        /* Save rendering scope handle for resource state update below. */
        const NodeHandle end_rendering_scope = rendering_scope;
        rendering_scope = 0;
        rendering_active = false;

        /* The render pass storeOp writes each attachment with COLOR_ATTACHMENT_WRITE_BIT (or
         * DEPTH_STENCIL_ATTACHMENT_WRITE_BIT). END_RENDERING has VKResourceType::NONE and
         * declares no resource dependencies, so these implicit writes are invisible to the
         * resource state tracker. Propagate them here so the next scope's pipeline barrier
         * correctly synchronizes with the storeOp, avoiding WRITE_AFTER_WRITE hazards. */
        for (const VKRenderGraphImage &link : render_graph.linked_images(end_rendering_scope)) {
          VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
              link.resource.handle);
          VKResourceBarrierState &state = resource.barrier_state;
          if (link.vk_access_flags & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) {
            state.vk_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          }
          if (link.vk_access_flags & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
            state.vk_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
          }
        }

        /* Any specific layout changes needs to be reverted, so the global resource state tracker
         * reflects the correct state. These barriers needs to be added as node post barriers. We
         * assume that END_RENDERING is always the last node of a group. */
        Barrier barrier = {};
        image_tracker.end(barrier, use_local_read);
        if (!barrier.is_empty()) {
          post_barriers.append(barrier);
        }

        /* After image_tracker.end() reverts tracked image layouts back to the default layout,
         * sync the global resource state tracker so subsequent barriers use the correct
         * oldLayout. */
        for (const VKRenderGraphImage &link : render_graph.linked_images(end_rendering_scope)) {
          VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
              link.resource.handle);
          if (resource.use_subresource_tracking()) {
            VKResourceBarrierState &state = resource.barrier_state;
            state.image_layout = to_default_image_layout(link.vk_image_aspect, use_local_read);
          }
        }
      }

      else if (rendering_active && !node_type_is_within_rendering(node.type)) {
        /* Suspend active rendering scope. */
        rendering_active = false;

        /* Any specific layout changes needs to be reverted, so the global resource state tracker
         * reflects the correct state. These barriers needs to be added as node post barriers.
         */
        Barrier barrier = {};
        image_tracker.suspend(barrier, use_local_read);
        if (!barrier.is_empty()) {
          post_barriers.append(barrier);
        }

        /* Sync resource state layout after suspend reverts tracked images. */
        for (const VKRenderGraphImage &link : render_graph.linked_images(rendering_scope)) {
          VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
              link.resource.handle);
          if (resource.use_subresource_tracking()) {
            VKResourceBarrierState &state = resource.barrier_state;
            state.image_layout = to_default_image_layout(link.vk_image_aspect, use_local_read);
          }
        }
      }

      else if (!rendering_active && node_type_is_within_rendering(node.type)) {
        /* Resume rendering scope. */
        VKRenderGraphNode &rendering_node = render_graph.nodes_[rendering_scope];
        Barrier barrier = {};
        build_pipeline_barriers(render_graph,
                                rendering_scope,
                                rendering_node.pipeline_stage_get(),
                                image_tracker,
                                barrier);
        if (!barrier.is_empty()) {
          barrier_list_.append(barrier);
        }

        /* Resume layered tracking. Each layer that has an override will be transition back to
         * the layer specific image layout. */
        barrier = {};
        image_tracker.resume(barrier, use_local_read);
        if (!barrier.is_empty()) {
          barrier_list_.append(barrier);
        }

        rendering_active = true;
      }

      /* Extract pre barriers for nodes. */
      if (use_local_read && node_type_is_within_rendering(node.type) &&
          node_has_input_attachments(render_graph, node_handle))
      {
        Barrier barrier = {};
        build_pipeline_barriers(
            render_graph, node_handle, node.pipeline_stage_get(), image_tracker, barrier, true);
        if (!barrier.is_empty()) {
          node_pre_barriers.append(barrier);
        }
      }
    }
    if (rendering_active) {
      /* Suspend layered image tracker. When active the next group will always be a compute/data
       * transfer group.
       *
       * Any specific layout changes needs to be reverted, so the global resource state tracker
       * reflects the correct state. These barriers needs to be added as node post barriers.
       */
      Barrier barrier = {};
      image_tracker.suspend(barrier, use_local_read);
      if (!barrier.is_empty()) {
        post_barriers.append(barrier);
      }

      /* Sync resource state layout after the tracker reverts tracked images. */
      for (const VKRenderGraphImage &link : render_graph.linked_images(rendering_scope)) {
        VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
            link.resource.handle);
        if (resource.use_subresource_tracking()) {
          VKResourceBarrierState &state = resource.barrier_state;
          state.image_layout = to_default_image_layout(link.vk_image_aspect, use_local_read);
        }
      }

      rendering_active = false;
    }

    /* After all node barriers have been extracted, check if depth/stencil attachments in
     * BEGIN_RENDERING were transitioned to a non-optimal layout by group pre-barriers. If so,
     * fix up VkRenderingAttachmentInfo.imageLayout to match the actual GPU layout, and add an
     * acquire barrier when LOAD_OP_LOAD is used to ensure memory visibility. */
    {
      NodeHandle first_handle = node_handles[node_group.first()];
      VKRenderGraphNode &first_node = render_graph.nodes_[first_handle];
      if (first_node.type == VKNodeType::BEGIN_RENDERING) {
        VKBeginRenderingData &begin_data =
            render_graph.storage_.begin_rendering[first_node.storage_index];

        for (const VKRenderGraphImage &link : render_graph.linked_images(first_handle)) {
          if (!(link.vk_image_aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))
          {
            continue;
          }
          VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
              link.resource.handle);
          VkImageLayout gpu_layout = resource.barrier_state.image_layout;
          VkImageLayout optimal_layout = to_default_image_layout(link.vk_image_aspect,
                                                                 use_local_read);
          if (gpu_layout == optimal_layout) {
            continue;
          }

          /* Fix up the imageLayout to reflect the actual GPU layout after all pre-barriers. */
          if (link.vk_image_aspect & VK_IMAGE_ASPECT_DEPTH_BIT &&
              begin_data.vk_rendering_info.pDepthAttachment != nullptr)
          {
            begin_data.depth_attachment.imageLayout = gpu_layout;
          }
          if (link.vk_image_aspect & VK_IMAGE_ASPECT_STENCIL_BIT &&
              begin_data.vk_rendering_info.pStencilAttachment != nullptr)
          {
            begin_data.stencil_attachment.imageLayout = gpu_layout;
          }

          /* When LOAD_OP_LOAD is used with a non-optimal layout, the existing layout transition
           * barriers may not provide proper memory visibility for the load operation. Add an
           * acquire barrier to ensure prior writes are visible before the depth/stencil test. */
          bool depth_load = (link.vk_image_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                            begin_data.vk_rendering_info.pDepthAttachment != nullptr &&
                            begin_data.depth_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
          bool stencil_load = (link.vk_image_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                              begin_data.vk_rendering_info.pStencilAttachment != nullptr &&
                              begin_data.stencil_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
          if (!depth_load && !stencil_load) {
            continue;
          }

          Barrier barrier = {};
          reset_barriers(barrier);
          barrier.src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
          barrier.dst_stage_mask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
          barrier.image_memory_barriers = IndexRange(vk_image_memory_barriers_.size(), 0);
          add_image_barrier(resource.image.vk_image,
                            barrier,
                            VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                            gpu_layout,
                            gpu_layout,
                            link.vk_image_aspect,
                            {});
          barrier.image_memory_barriers = barrier.image_memory_barriers.with_new_end(
              vk_image_memory_barriers_.size());
          barrier_list_.append(barrier);
        }
      }
    }

    /* Update the group pre and post barriers. Pre barriers are already stored in the
     * barrier_list_. The post barriers are appended after the pre barriers. */
    int64_t barrier_list_size = barrier_list_.size();
    group_pre_barriers_.append(group_pre_barriers.with_new_end(barrier_list_size));
    barrier_list_.extend(std::move(post_barriers));
    post_barriers = {};
    group_post_barriers_.append(
        IndexRange::from_begin_end(barrier_list_size, barrier_list_.size()));
    if (!node_pre_barriers.is_empty()) {
      barrier_list_size = barrier_list_.size();
      barrier_list_.extend(std::move(node_pre_barriers));
      node_pre_barriers = {};
      /* Shift all node pre barrier references to the new location in the barrier_list_. */
      for (const int64_t group_node_index : node_group) {
        NodeHandle node_handle = node_handles[group_node_index];
        if (!node_pre_barriers_[node_handle].is_empty()) {
          node_pre_barriers_[node_handle] = IndexRange::from_begin_size(
              node_pre_barriers_[node_handle].start() + barrier_list_size, 1);
        }
      }
    }
  }

  BLI_assert(group_pre_barriers_.size() == group_nodes_.size());
  BLI_assert(group_post_barriers_.size() == group_nodes_.size());
}

void VKCommandBuilder::groups_build_commands(VKRenderGraph &render_graph,
                                             VKCommandBufferInterface &command_buffer,
                                             Span<NodeHandle> node_handles)
{
  DebugGroups debug_groups = {};
  VKBoundPipelines active_pipelines = {};

  NodeHandle rendering_scope = 0;
  bool rendering_active = false;

  for (int64_t group_index : group_nodes_.index_range()) {
    IndexRange group_nodes = group_nodes_[group_index];
    Span<NodeHandle> group_node_handles = node_handles.slice(group_nodes);

    /* Record group pre barriers. */
    for (BarrierIndex barrier_index : group_pre_barriers_[group_index]) {
      BLI_assert_msg(!rendering_active,
                     "Pre group barriers must be executed outside a rendering scope.");
      Barrier &barrier = barrier_list_[barrier_index];
#if 0
      std::cout << __func__ << ": node_group=" << group_index
                << ", node_group_range=" << group_node_handles.first() << "-"
                << group_node_handles.last() << ", pre_barrier=(" << to_string_barrier(barrier)
                << ")\n";
#endif
      send_pipeline_barriers(command_buffer, barrier, false);
    }

    /* Record group node commands. */
    for (NodeHandle node_handle : group_node_handles) {
      VKRenderGraphNode &node = render_graph.nodes_[node_handle];

      if (G.debug & G_DEBUG_GPU) {
        activate_debug_group(render_graph, command_buffer, debug_groups, node_handle);
      }

      if (node.type == VKNodeType::BEGIN_RENDERING) {
        /* Per-frame accounting of render scopes. On a tile-based GPU each scope is a tile flush,
         * and the flush cost tracks the pixels in the attachments rather than the number of draws,
         * so scopes and pixels are the two numbers that decide where optimisation pays. Reported
         * through the existing android.perf switch rather than a build, so the numbers can be
         * The per-scope lines answer what the totals cannot: whether repeated scopes share an
         * attachment set. Those can be merged into one render pass and the tile flushes
         * disappear; scopes targeting different images cannot be merged and only get cheaper by
         * rendering less. Different fixes, so the distinction is worth the log lines. */
        {
          const VKBeginRenderingData &begin_data =
              render_graph.storage_.begin_rendering[node.storage_index];
          const VkRect2D &area = begin_data.vk_rendering_info.renderArea;
          const uint32_t color_count = begin_data.vk_rendering_info.colorAttachmentCount;
          scope_pixels += uint64_t(area.extent.width) * uint64_t(area.extent.height) *
                          (color_count + 1);
          auto count_load_op = [this](VkAttachmentLoadOp load_op) {
            switch (load_op) {
              case VK_ATTACHMENT_LOAD_OP_CLEAR:
                scope_load_clear++;
                break;
              case VK_ATTACHMENT_LOAD_OP_LOAD:
                scope_load_keep++;
                break;
              default:
                scope_load_none++;
                break;
            }
          };
          for (uint32_t i = 0; i < color_count; i++) {
            /* Empty slots carry a DONT_CARE placeholder and a null view. The format array cannot
             * be the filter here: it is only filled when the render pass fallback is active,
             * which left this accounting reading all zeros on the native path. */
            if (begin_data.color_attachments[i].imageView == VK_NULL_HANDLE) {
              continue;
            }
            count_load_op(begin_data.color_attachments[i].loadOp);
          }
          if (begin_data.vk_rendering_info.pDepthAttachment != nullptr) {
            count_load_op(begin_data.depth_attachment.loadOp);
          }
          scope_detail(begin_data);
          /* Measurement only. A scope targeting the same attachments as the previous one is a
           * merge candidate, and there are two kinds of those: adjacent (the END/BEGIN pair is
           * dropped and both draw runs become one scope) and separated by other work (the
           * commands in between have to be moved before or after the merged scope first, which is
           * a scheduler change and not something the builder can do on its own). They are counted
           * apart for that reason. Neither is the removed fusion, which held a scope open across
           * transfer or dispatch work; that is not a legal command stream -- vkCmdCopyBuffer must
           * only be called outside a render pass instance (VUID-vkCmdCopyBuffer-renderpass), and
           * the same holds for dispatch. Nothing is built on these numbers yet. The previous
           * scope's begin node is still in rendering_scope here; nothing else has overwritten it
           * since its END_RENDERING ran. */
          const int64_t flat_index = group_nodes.first();
          if (rendering_scope != 0) {
            const bool directly_adjacent =
                flat_index > 0 && render_graph.nodes_[node_handles[flat_index - 1]].type ==
                                      VKNodeType::END_RENDERING;
            if (directly_adjacent) {
              scope_adjacent++;
            }
            const Span<VKRenderGraphImage> previous =
                render_graph.linked_images(render_graph.nodes_[rendering_scope]);
            const Span<VKRenderGraphImage> current = render_graph.linked_images(node);
            bool same_attachments = previous.size() == current.size();
            for (const VKRenderGraphImage &current_link : current) {
              if (!same_attachments) {
                break;
              }
              bool found = false;
              for (const VKRenderGraphImage &previous_link : previous) {
                if (previous_link.resource.handle == current_link.resource.handle &&
                    previous_link.vk_image_aspect == current_link.vk_image_aspect)
                {
                  found = true;
                  break;
                }
              }
              same_attachments = found;
            }
            if (same_attachments) {
              /* The render area has to match as well: merging different rects would also merge
               * their scissors and clear rects. */
              const VKBeginRenderingData &previous_begin =
                  render_graph.storage_.begin_rendering[render_graph.nodes_[rendering_scope]
                                                             .storage_index];
              const VkRect2D &previous_area = previous_begin.vk_rendering_info.renderArea;
              same_attachments = previous_area.offset.x == area.offset.x &&
                                 previous_area.offset.y == area.offset.y &&
                                 previous_area.extent.width == area.extent.width &&
                                 previous_area.extent.height == area.extent.height;
            }
            if (same_attachments) {
              scope_repeat++;
              if (!directly_adjacent) {
                scope_repeat_foreign++;
              }
              else {
                scope_mergeable++;
                /* A clear on the second scope means its contents do not survive the boundary, so
                 * merging would have to keep the clear; counted apart from the plain cases. */
                bool keeps_contents = true;
                for (uint32_t i = 0; i < color_count; i++) {
                  if (begin_data.color_attachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                    keeps_contents = false;
                    break;
                  }
                }
                if (keeps_contents) {
                  scope_mergeable_loadok++;
                }
              }
            }
          }
        }
        rendering_scope = node_handle;
        rendering_active = true;
        scope_count++;
      }

      else if (node.type == VKNodeType::END_RENDERING) {
        rendering_active = false;
      }
      else if (node_type_is_within_rendering(node.type)) {
        if (!rendering_active) {
          /* Restart rendering scope. */
          VKRenderGraphNode &rendering_node = render_graph.nodes_[rendering_scope];
          VKBeginRenderingNode::reconfigure_for_restart(
              render_graph.storage_.begin_rendering[rendering_node.storage_index]);
          rendering_node.build_commands(command_buffer, render_graph.storage_, active_pipelines);
          rendering_active = true;
        }
      }

      /* Record group node barriers. (VK_EXT_dynamic_rendering_local_read) */
      for (BarrierIndex node_pre_barrier_index : node_pre_barriers_[node_handle]) {
        Barrier &barrier = barrier_list_[node_pre_barrier_index];
#if 0
      std::cout << __func__ << ": node_group=" << group_index
                << ", node_group_range=" << group_node_handles.first() << "-"
                << group_node_handles.last() << ", node_pre_barrier=(" << to_string_barrier(barrier)
                << ")\n";
#endif
        /* TODO: Barrier should already contain the changes for local read. */
        send_pipeline_barriers(command_buffer, barrier, true);
      }

#if 0
      std::cout << __func__ << ": node_group=" << group_index
                << ", node_group_range=" << group_node_handles.first() << "-"
                << group_node_handles.last() << ", node_handle=" << node_handle
                << ", node_type=" << node.type
                << ", debug group=" << render_graph.full_debug_group(node_handle) << "\n";
#endif
      node.build_commands(command_buffer, render_graph.storage_, active_pipelines);
    }

    if (rendering_active) {
      /* Suspend rendering as the next node group will contain data transfer/dispatch commands.
       */
      rendering_active = false;
      /* Not the END_RENDERING node: this closes a scope the builder suspended between node
       * groups, so it bypasses the node that would otherwise route through the fallback. Calling
       * end_rendering() here reached vkCmdEndRenderingKHR unguarded, which is null without
       * VK_KHR_dynamic_rendering -- a SIGSEGV at 0x0 on the first frame. Mirrors what
       * VKBeginRenderingNode/VKEndRenderingNode do. */
      if (VKRenderPassFallback::is_active()) {
        end_rendering_fallback(command_buffer);
      }
      else {
        command_buffer.end_rendering();
      }
    }

    /* Record group post barriers. */
    for (BarrierIndex barrier_index : group_post_barriers_[group_index]) {
      BLI_assert_msg(!rendering_active,
                     "Post group barriers must be executed outside a rendering scope.");
      Barrier &barrier = barrier_list_[barrier_index];
#if 0
      std::cout << __func__ << ": node_group=" << group_index
                << ", node_group_range=" << group_node_handles.first() << "-"
                << group_node_handles.last() << ", post_barrier=(" << to_string_barrier(barrier)
                << ")\n";
#endif
      send_pipeline_barriers(command_buffer, barrier, false);
    }
  }

  finish_debug_groups(command_buffer, debug_groups);
}

bool VKCommandBuilder::node_has_input_attachments(const VKRenderGraph &render_graph,
                                                  NodeHandle node_handle)
{
  const Span<VKRenderGraphImage> images = render_graph.linked_images(node_handle);
  return std::ranges::any_of(images.begin(), images.end(), [](const VKRenderGraphImage &input) {
    return input.vk_access_flags & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
  });
}

void VKCommandBuilder::activate_debug_group(VKRenderGraph &render_graph,
                                            VKCommandBufferInterface &command_buffer,
                                            DebugGroups &debug_groups,
                                            NodeHandle node_handle)
{
  VKRenderGraph::DebugGroupID debug_group = render_graph.debug_.node_group_map[node_handle];
  if (debug_group == debug_groups.active_debug_group_id) {
    return;
  }

  /* Determine the number of pops and pushes that will happen on the debug stack. */
  int num_ends = 0;
  int num_begins = 0;

  if (debug_group == -1) {
    num_ends = debug_groups.debug_level;
  }
  else {
    Vector<VKRenderGraph::DebugGroupNameID> &to_group =
        render_graph.debug_.used_groups[debug_group];
    if (debug_groups.active_debug_group_id != -1) {
      Vector<VKRenderGraph::DebugGroupNameID> &from_group =
          render_graph.debug_.used_groups[debug_groups.active_debug_group_id];

      num_ends = max_ii(from_group.size() - to_group.size(), 0);
      int num_checks = min_ii(from_group.size(), to_group.size());
      for (int index : IndexRange(num_checks)) {
        if (from_group[index] != to_group[index]) {
          num_ends += num_checks - index;
          break;
        }
      }
    }

    num_begins = to_group.size() - (debug_groups.debug_level - num_ends);
  }

  /* Perform the pops from the debug stack. */
  for (int index = 0; index < num_ends; index++) {
    command_buffer.end_debug_utils_label();
  }
  debug_groups.debug_level -= num_ends;

  /* Perform the pushes to the debug stack. */
  if (num_begins > 0) {
    Vector<VKRenderGraph::DebugGroupNameID> &to_group =
        render_graph.debug_.used_groups[debug_group];
    VkDebugUtilsLabelEXT debug_utils_label = {};
    debug_utils_label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    for (int index : IndexRange(debug_groups.debug_level, num_begins)) {
      const VKRenderGraph::DebugGroup &debug_group = render_graph.debug_.groups[to_group[index]];
      debug_utils_label.pLabelName = debug_group.name.c_str();
      copy_v4_v4(debug_utils_label.color, debug_group.color);
      command_buffer.begin_debug_utils_label(&debug_utils_label);
    }
  }

  debug_groups.debug_level += num_begins;
  debug_groups.active_debug_group_id = debug_group;
}

void VKCommandBuilder::finish_debug_groups(VKCommandBufferInterface &command_buffer,
                                           DebugGroups &debug_groups)
{
  for (int i = 0; i < debug_groups.debug_level; i++) {
    command_buffer.end_debug_utils_label();
  }
  debug_groups.debug_level = 0;
}

void VKCommandBuilder::build_pipeline_barriers(VKRenderGraph &render_graph,
                                               NodeHandle node_handle,
                                               VkPipelineStageFlags pipeline_stage,
                                               ImageTracker &image_tracker,
                                               Barrier &r_barrier,
                                               bool within_rendering)
{
  reset_barriers(r_barrier);
  add_image_barriers(
      render_graph, node_handle, pipeline_stage, image_tracker, r_barrier, within_rendering);
  add_buffer_barriers(render_graph, node_handle, pipeline_stage, r_barrier);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipeline barriers
 * \{ */

void VKCommandBuilder::reset_barriers(Barrier &r_barrier)
{
  r_barrier.dst_stage_mask = r_barrier.src_stage_mask = VK_PIPELINE_STAGE_NONE;
}

void VKCommandBuilder::send_pipeline_barriers(VKCommandBufferInterface &command_buffer,
                                              const Barrier &barrier,
                                              bool within_rendering)
{
  if (barrier.is_empty()) {
    return;
  }

  /* When no resources have been used, we can start the barrier at the top of the pipeline.
   * It is not allowed to set it to None. */
  /* TODO: VK_KHR_synchronization2 allows setting src_stage_mask to NONE. */
  /* When no resources have been used, we can start the barrier at the top of the pipeline.
   * It is not allowed to set it to None. */
  VkPipelineStageFlags src_stage_mask = (barrier.src_stage_mask == VK_PIPELINE_STAGE_NONE) ?
                                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
                                            VkPipelineStageFlagBits(barrier.src_stage_mask);

  VkPipelineStageFlags dst_stage_mask = barrier.dst_stage_mask;
  /* TODO: this should be done during barrier extraction making within_rendering obsolete. */
  if (within_rendering) {
    /* See: VUID - `vkCmdPipelineBarrier` - `srcStageMask` - 09556
     * If `vkCmdPipelineBarrier` is called within a render pass instance started with
     * `vkCmdBeginRendering`, this command must only specify frame-buffer-space stages in
     * `srcStageMask` and `dstStageMask`. */
    src_stage_mask = dst_stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }

  Span<VkBufferMemoryBarrier> buffer_barriers = vk_buffer_memory_barriers_.as_span().slice(
      barrier.buffer_memory_barriers);
  Span<VkImageMemoryBarrier> image_barriers = vk_image_memory_barriers_.as_span().slice(
      barrier.image_memory_barriers);

  command_buffer.pipeline_barrier(src_stage_mask,
                                  dst_stage_mask,
                                  VK_DEPENDENCY_BY_REGION_BIT,
                                  0,
                                  nullptr,
                                  buffer_barriers.size(),
                                  buffer_barriers.data(),
                                  image_barriers.size(),
                                  image_barriers.data());
}

void VKCommandBuilder::add_buffer_barriers(VKRenderGraph &render_graph,
                                           NodeHandle node_handle,
                                           VkPipelineStageFlags node_stages,
                                           Barrier &r_barrier)
{
  r_barrier.buffer_memory_barriers = IndexRange(vk_buffer_memory_barriers_.size(), 0);
  add_buffer_read_barriers(render_graph, node_handle, node_stages, r_barrier);
  add_buffer_write_barriers(render_graph, node_handle, node_stages, r_barrier);
  r_barrier.buffer_memory_barriers = r_barrier.buffer_memory_barriers.with_new_end(
      vk_buffer_memory_barriers_.size());
}

void VKCommandBuilder::add_buffer_read_barriers(VKRenderGraph &render_graph,
                                                NodeHandle node_handle,
                                                VkPipelineStageFlags node_stages,
                                                Barrier &r_barrier)
{
  for (const VKRenderGraphBuffer &link : render_graph.linked_buffers(node_handle)) {
    if (link.has_write_access()) {
      continue;
    }
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.get_buffer_resource(
        versioned_resource.handle);
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const bool is_first_read = resource_state.is_new_stamp();
    if (!is_first_read &&
        (resource_state.vk_access & link.vk_access_flags) == link.vk_access_flags &&
        (resource_state.vk_pipeline_stages & node_stages) == node_stages)
    {
      /* Has already been covered in a previous call no need to add this one. */
      continue;
    }

    const VkAccessFlags wait_access = resource_state.vk_access;

    r_barrier.src_stage_mask |= resource_state.vk_pipeline_stages;
    r_barrier.dst_stage_mask |= node_stages;

    if (is_first_read) {
      resource_state.vk_access = link.vk_access_flags;
      resource_state.vk_pipeline_stages = node_stages;
    }
    else {
      resource_state.vk_access |= link.vk_access_flags;
      resource_state.vk_pipeline_stages |= node_stages;
    }

    if (wait_access != VK_ACCESS_NONE) {
      add_buffer_barrier(resource.buffer.vk_buffer, r_barrier, wait_access, link.vk_access_flags);
    }
  }
}

void VKCommandBuilder::add_buffer_write_barriers(VKRenderGraph &render_graph,
                                                 NodeHandle node_handle,
                                                 VkPipelineStageFlags node_stages,
                                                 Barrier &r_barrier)
{
  for (const VKRenderGraphBuffer &link : render_graph.linked_buffers(node_handle)) {
    if (!link.has_write_access()) {
      continue;
    }
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.get_buffer_resource(
        versioned_resource.handle);
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const VkAccessFlags wait_access = resource_state.vk_access;

    r_barrier.src_stage_mask |= resource_state.vk_pipeline_stages;
    r_barrier.dst_stage_mask |= node_stages;

    resource_state.vk_access = link.vk_access_flags;
    resource_state.vk_pipeline_stages = node_stages;

    if (wait_access != VK_ACCESS_NONE) {
      add_buffer_barrier(resource.buffer.vk_buffer, r_barrier, wait_access, link.vk_access_flags);
    }
  }
}

void VKCommandBuilder::add_buffer_barrier(VkBuffer vk_buffer,
                                          Barrier &r_barrier,
                                          VkAccessFlags src_access_mask,
                                          VkAccessFlags dst_access_mask)
{
  for (VkBufferMemoryBarrier &vk_buffer_memory_barrier :
       vk_buffer_memory_barriers_.as_mutable_span().drop_front(
           r_barrier.buffer_memory_barriers.start()))
  {
    if (vk_buffer_memory_barrier.buffer == vk_buffer) {
      /* When registering read/write buffers, it can be that the node internally requires
       * read/write. In this case we adjust the dstAccessMask of the read barrier. */
      if ((vk_buffer_memory_barrier.dstAccessMask & src_access_mask) == src_access_mask) {
        vk_buffer_memory_barrier.dstAccessMask |= dst_access_mask;
        return;
      }
      /* When re-registering resources we can skip if access mask already contain all the flags.
       */
      if ((vk_buffer_memory_barrier.dstAccessMask & dst_access_mask) == dst_access_mask &&
          (vk_buffer_memory_barrier.srcAccessMask & src_access_mask) == src_access_mask)
      {
        return;
      }
    }
  }

  vk_buffer_memory_barriers_.append({VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                     nullptr,
                                     src_access_mask,
                                     dst_access_mask,
                                     VK_QUEUE_FAMILY_IGNORED,
                                     VK_QUEUE_FAMILY_IGNORED,
                                     vk_buffer,
                                     0,
                                     VK_WHOLE_SIZE});
}

void VKCommandBuilder::add_image_barriers(VKRenderGraph &render_graph,
                                          NodeHandle node_handle,
                                          VkPipelineStageFlags node_stages,
                                          ImageTracker &image_tracker,
                                          Barrier &r_barrier,
                                          bool within_rendering)
{
  r_barrier.image_memory_barriers = IndexRange(vk_image_memory_barriers_.size(), 0);
  add_image_read_barriers(
      render_graph, node_handle, node_stages, image_tracker, r_barrier, within_rendering);
  add_image_write_barriers(
      render_graph, node_handle, node_stages, image_tracker, r_barrier, within_rendering);
  r_barrier.image_memory_barriers = r_barrier.image_memory_barriers.with_new_end(
      vk_image_memory_barriers_.size());
}

void VKCommandBuilder::add_image_read_barriers(VKRenderGraph &render_graph,
                                               NodeHandle node_handle,
                                               VkPipelineStageFlags node_stages,
                                               ImageTracker &image_tracker,
                                               Barrier &r_barrier,
                                               bool within_rendering)
{
  for (const VKRenderGraphImage &link : render_graph.linked_images(node_handle)) {
    if (link.has_write_access()) {
      continue;
    }
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
        versioned_resource.handle);
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const bool is_first_read = resource_state.is_new_stamp();
    if ((!is_first_read) &&
        (resource_state.vk_access & link.vk_access_flags) == link.vk_access_flags &&
        (resource_state.vk_pipeline_stages & node_stages) == node_stages &&
        resource_state.image_layout == link.vk_image_layout)
    {
      /* Has already been covered in previous barrier no need to add this one. */
      continue;
    }
    if (within_rendering && link.vk_image_layout != VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR) {
      /* Allow only local read barriers inside rendering scope */
      continue;
    }

    /* Check if this image is being tracked as layered color attachment. In that case we are not
     * allowed to update the resource state as it will be reverted by the image tracker. */
    if (image_tracker.contains(resource.image.vk_image)) {
      if (resource_state.image_layout != link.vk_image_layout) {
        image_tracker.update(resource.image.vk_image,
                             link.subimage,
                             resource_state.image_layout,
                             link.vk_image_layout,
                             link.vk_image_aspect,
                             r_barrier);
        resource_state.image_layout = link.vk_image_layout;
      }
      /* Image tracker only tracks layout changes. Access flag synchronization is still needed
       * to avoid WRITE->READ hazards between rendering scopes. See #158501. */
      VkAccessFlags wait_access = resource_state.vk_access;
      r_barrier.src_stage_mask |= resource_state.vk_pipeline_stages;
      r_barrier.dst_stage_mask |= node_stages;
      if (is_first_read) {
        resource_state.vk_access = link.vk_access_flags;
        resource_state.vk_pipeline_stages = node_stages;
      }
      else {
        resource_state.vk_access |= link.vk_access_flags;
        resource_state.vk_pipeline_stages |= node_stages;
      }
      if (wait_access != VK_ACCESS_NONE && (wait_access & ~link.vk_access_flags) != 0) {
        add_image_barrier(resource.image.vk_image,
                          r_barrier,
                          wait_access,
                          link.vk_access_flags,
                          resource_state.image_layout,
                          link.vk_image_layout,
                          link.vk_image_aspect,
                          {});
      }
      continue;
    }

    VkAccessFlags wait_access = resource_state.vk_access;

    r_barrier.src_stage_mask |= resource_state.vk_pipeline_stages;
    r_barrier.dst_stage_mask |= node_stages;

    if (is_first_read) {
      resource_state.vk_access = link.vk_access_flags;
      resource_state.vk_pipeline_stages = node_stages;
    }
    else {
      resource_state.vk_access |= link.vk_access_flags;
      resource_state.vk_pipeline_stages |= node_stages;
    }

    if (wait_access != VK_ACCESS_NONE || link.vk_image_layout != resource_state.image_layout) {
      add_image_barrier(resource.image.vk_image,
                        r_barrier,
                        wait_access,
                        link.vk_access_flags,
                        resource_state.image_layout,
                        link.vk_image_layout,
                        link.vk_image_aspect,
                        {});
      resource_state.image_layout = link.vk_image_layout;
    }
  }
}

void VKCommandBuilder::add_image_write_barriers(VKRenderGraph &render_graph,
                                                NodeHandle node_handle,
                                                VkPipelineStageFlags node_stages,
                                                ImageTracker &image_tracker,
                                                Barrier &r_barrier,
                                                bool within_rendering)
{
  for (const VKRenderGraphImage &link : render_graph.linked_images(node_handle)) {
    if (!link.has_write_access()) {
      continue;
    }
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
        versioned_resource.handle);
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const VkAccessFlags wait_access = resource_state.vk_access;
    if (within_rendering && link.vk_image_layout != VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR) {
      /* Allow only local read barriers inside rendering scope */
      continue;
    }

    /* Check if this image is being tracked as layered color attachment. In that case we are not
     * allowed to update the resource state as it will be reverted by the image tracker. */
    if (image_tracker.contains(resource.image.vk_image)) {
      if (resource_state.image_layout != link.vk_image_layout) {
        image_tracker.update(resource.image.vk_image,
                             link.subimage,
                             resource_state.image_layout,
                             link.vk_image_layout,
                             link.vk_image_aspect,
                             r_barrier);
        resource_state.image_layout = link.vk_image_layout;
      }
      /* Image tracker only tracks layout changes. Access flag synchronization is still needed
       * to avoid READ->WRITE hazards between rendering scopes. See #158501. */
      r_barrier.src_stage_mask |= resource_state.vk_pipeline_stages;
      r_barrier.dst_stage_mask |= node_stages;
      resource_state.vk_access = link.vk_access_flags;
      resource_state.vk_pipeline_stages = node_stages;
      if (wait_access != VK_ACCESS_NONE && (wait_access & ~link.vk_access_flags) != 0) {
        add_image_barrier(resource.image.vk_image,
                          r_barrier,
                          wait_access,
                          link.vk_access_flags,
                          resource_state.image_layout,
                          link.vk_image_layout,
                          link.vk_image_aspect,
                          {});
      }
      continue;
    }

    r_barrier.src_stage_mask |= resource_state.vk_pipeline_stages;
    r_barrier.dst_stage_mask |= node_stages;

    resource_state.vk_access = link.vk_access_flags;
    resource_state.vk_pipeline_stages = node_stages;

    if (wait_access != VK_ACCESS_NONE || link.vk_image_layout != resource_state.image_layout) {
      add_image_barrier(resource.image.vk_image,
                        r_barrier,
                        wait_access,
                        link.vk_access_flags,
                        resource_state.image_layout,
                        link.vk_image_layout,
                        link.vk_image_aspect,
                        {});
      resource_state.image_layout = link.vk_image_layout;
    }
  }
}

void VKCommandBuilder::add_image_barrier(VkImage vk_image,
                                         Barrier &r_barrier,
                                         VkAccessFlags src_access_mask,
                                         VkAccessFlags dst_access_mask,
                                         VkImageLayout old_image_layout,
                                         VkImageLayout new_image_layout,
                                         VkImageAspectFlags aspect_mask,
                                         const VKSubImageRange &subimage)
{
  BLI_assert(aspect_mask != VK_IMAGE_ASPECT_NONE);
  for (VkImageMemoryBarrier &vk_image_memory_barrier :
       vk_image_memory_barriers_.as_mutable_span().drop_front(
           r_barrier.image_memory_barriers.start()))
  {
    if (vk_image_memory_barrier.image == vk_image) {
      /* When registering read/write buffers, it can be that the node internally requires
       * read/write. In this case we adjust the dstAccessMask of the read barrier. An example is
       * EEVEE update HIZ compute shader and shadow tagging. */
      if ((vk_image_memory_barrier.dstAccessMask & src_access_mask) == src_access_mask) {
        vk_image_memory_barrier.dstAccessMask |= dst_access_mask;
        vk_image_memory_barrier.srcAccessMask |= src_access_mask;
        /* When the existing barrier's newLayout matches the new barrier's
         * oldLayout, update to the combined layout transition. For example,
         * a read barrier (UNDEFINED->TRANSFER_DST) followed by a write
         * barrier (TRANSFER_DST->GENERAL) becomes UNDEFINED->GENERAL. */
        if (old_image_layout == vk_image_memory_barrier.newLayout) {
          vk_image_memory_barrier.newLayout = new_image_layout;
        }
        return;
      }
      /* When re-registering resources we can skip if access mask already contain all the flags.
       */
      if ((vk_image_memory_barrier.dstAccessMask & dst_access_mask) == dst_access_mask &&
          (vk_image_memory_barrier.srcAccessMask & src_access_mask) == src_access_mask &&
          old_image_layout == new_image_layout)
      {
        return;
      }
    }
  }

  vk_image_memory_barriers_.append({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    nullptr,
                                    src_access_mask,
                                    dst_access_mask,
                                    old_image_layout,
                                    new_image_layout,
                                    VK_QUEUE_FAMILY_IGNORED,
                                    VK_QUEUE_FAMILY_IGNORED,
                                    vk_image,
                                    {aspect_mask,
                                     subimage.mipmap_level,
                                     subimage.mipmap_count,
                                     subimage.layer_base,
                                     subimage.layer_count}});
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sub-resource tracking
 * \{ */

void VKCommandBuilder::ImageTracker::begin(const VKRenderGraph &render_graph,
                                           NodeHandle node_handle)
{
  BLI_assert(render_graph.nodes_[node_handle].type == VKNodeType::BEGIN_RENDERING);
  tracked_attachments.clear();
  changes.clear();

  for (const VKRenderGraphImage &link : render_graph.linked_images(node_handle)) {
    VKResourceStateTracker::Resource &resource = render_graph.resources_.get_image_resource(
        link.resource.handle);
    if (resource.use_subresource_tracking()) {
      tracked_attachments.add(resource.image.vk_image);
    }
  }
}

void VKCommandBuilder::ImageTracker::update(VkImage vk_image,
                                            const VKSubImageRange &subimage,
                                            VkImageLayout old_layout,
                                            VkImageLayout new_layout,
                                            VkImageAspectFlags aspect,
                                            Barrier &r_barrier)
{
  for (const SubImageChange &change : changes) {
    if (change.vk_image == vk_image && ((subimage.layer_count != VK_REMAINING_ARRAY_LAYERS &&
                                         change.subimage.layer_base == subimage.layer_base) ||
                                        (subimage.mipmap_count != VK_REMAINING_MIP_LEVELS &&
                                         change.subimage.mipmap_level == subimage.mipmap_level)))
    {
      BLI_assert_msg(
          change.vk_image_layout == new_layout,
          "We don't support more that one change of the same subimage multiple times during a "
          "rendering scope.");
      /* Early exit as layer is in correct layout. This is a normal case as we expect multiple
       * draw commands to take place during a rendering scope with the same layer access. */
      return;
    }
  }

  changes.append({vk_image, new_layout, aspect, subimage});

  /* We should be able to do better. BOTTOM/TOP is really a worst case barrier. */
  r_barrier.src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  r_barrier.dst_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkAccessFlags dst_access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
    dst_access_mask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  }
  if (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
    dst_access_mask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  }
  /* NOTE: src_access_mask uses a conservative VK_ACCESS_TRANSFER_WRITE_BIT as a
   * placeholder. The actual prior access flags are added by the sync barrier logic
   * in add_image_read_barriers/add_image_write_barriers (via wait_access from the
   * resource state tracker) and merged into this barrier by add_image_barrier's merge.
   * TODO: Pass the actual tracked wait_access here instead, removing the redundant
   * sync barrier and this placeholder. */
  command_builder.add_image_barrier(vk_image,
                                    r_barrier,
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
                                    dst_access_mask,
                                    old_layout,
                                    new_layout,
                                    aspect,
                                    subimage);
}

void VKCommandBuilder::ImageTracker::end(Barrier &r_barrier, bool use_local_read)
{
  suspend(r_barrier, use_local_read);
  tracked_attachments.clear();
  changes.clear();
}

void VKCommandBuilder::ImageTracker::suspend(Barrier &r_barrier, bool use_local_read)

{
  if (changes.is_empty()) {
    return;
  }

  command_builder.reset_barriers(r_barrier);
  /* We should be able to do better. BOTTOM/TOP is really a worst case barrier. */
  r_barrier.src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  r_barrier.dst_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  int64_t start_index = command_builder.vk_image_memory_barriers_.size();
  r_barrier.image_memory_barriers = IndexRange::from_begin_size(start_index, 0);
  for (const SubImageChange &change : changes) {
    command_builder.add_image_barrier(
        change.vk_image,
        r_barrier,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        change.vk_image_layout,
        to_default_image_layout(change.vk_image_aspect, use_local_read),
        change.vk_image_aspect,
        change.subimage);
    r_barrier.image_memory_barriers = r_barrier.image_memory_barriers.with_new_end(
        command_builder.vk_image_memory_barriers_.size());

#if 0
    std::cout << __func__ << ": transition layout image=" << change.vk_image
              << ", layer=" << change.subimage.layer_base
              << ", count=" << change.subimage.layer_count
              << ", from_layout=" << to_string(change.vk_image_layout)
              << ", to_layout=" << to_string(
                     to_default_image_layout(change.vk_image_aspect, use_local_read))
              << "\n";
#endif
  }
}

void VKCommandBuilder::ImageTracker::resume(Barrier &r_barrier, bool use_local_read)
{
  if (changes.is_empty()) {
    return;
  }

  command_builder.reset_barriers(r_barrier);
  /* We should be able to do better. BOTTOM/TOP is really a worst case barrier. */
  r_barrier.src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  r_barrier.dst_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  int64_t start_index = command_builder.vk_image_memory_barriers_.size();
  r_barrier.image_memory_barriers = IndexRange::from_begin_size(start_index, 0);

  for (const SubImageChange &change : changes) {
    command_builder.add_image_barrier(
        change.vk_image,
        r_barrier,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        to_default_image_layout(change.vk_image_aspect, use_local_read),
        change.vk_image_layout,
        change.vk_image_aspect,
        change.subimage);
#if 0
    std::cout << __func__ << ": transition layout image=" << change.vk_image
              << ", layer=" << change.subimage.layer_base
              << ", count=" << change.subimage.layer_count
              << ", from_layout=" << to_string(
                     to_default_image_layout(change.vk_image_aspect, use_local_read))
              << ", to_layout=" << to_string(change.vk_image_layout) << "\n";
#endif
  }
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Debugging tools
 * \{ */

std::string VKCommandBuilder::to_string_barrier(const Barrier &barrier)
{
  std::stringstream ss;
  ss << "src_stage_mask=" << to_string_vk_pipeline_stage_flags(barrier.src_stage_mask)
     << ", dst_stage_mask=" << to_string_vk_pipeline_stage_flags(barrier.dst_stage_mask) << "\n";
  for (const VkBufferMemoryBarrier &buffer_memory_barrier :
       vk_buffer_memory_barriers_.as_span().slice(barrier.buffer_memory_barriers))
  {
    ss << "  - src_access_mask=" << to_string_vk_access_flags(buffer_memory_barrier.srcAccessMask)
       << ", dst_access_mask=" << to_string_vk_access_flags(buffer_memory_barrier.dstAccessMask)
       << ", vk_buffer=" << to_string(buffer_memory_barrier.buffer) << "\n";
  }

  for (const VkImageMemoryBarrier &image_memory_barrier :
       vk_image_memory_barriers_.as_span().slice(barrier.image_memory_barriers))
  {
    ss << "  - src_access_mask=" << to_string_vk_access_flags(image_memory_barrier.srcAccessMask)
       << ", dst_access_mask=" << to_string_vk_access_flags(image_memory_barrier.dstAccessMask)
       << ", vk_image=" << to_string(image_memory_barrier.image)
       << ", old_layout=" << to_string(image_memory_barrier.oldLayout)
       << ", new_layout=" << to_string(image_memory_barrier.newLayout)
       << ", subresource_range=" << to_string(image_memory_barrier.subresourceRange, 2) << "\n";
  }

  return ss.str();
}

/** \} */

}  // namespace blender::gpu::render_graph
