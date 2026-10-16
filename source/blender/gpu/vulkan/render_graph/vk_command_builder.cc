/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_command_builder.hh"
#include "BLI_index_range.hh"
#include "BLI_perf_probe.hh"
#include "vk_backend.hh"
#include "vk_command_buffer_wrapper.hh"
#include "vk_render_graph.hh"
#include "vk_render_pass_fallback.hh"
#include "vk_to_string.hh"

#include <sstream>
#include <cstdio>

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

/* Identity of the attachment set, so two scopes printing the same tag are candidates for being
 * merged into one render pass. The views are the framebuffer's identity; the render area and the
 * attachment count have to agree as well, since a merge also requires the same subpass shape. */
static uint64_t scope_view_tag(const VKBeginRenderingData &begin_data)
{
  uint64_t view_tag = 1469598103934665603ull;
  auto mix = [&view_tag](uint64_t v) { view_tag = (view_tag ^ v) * 1099511628211ull; };
  for (uint32_t i = 0; i < begin_data.vk_rendering_info.colorAttachmentCount; i++) {
    mix(reinterpret_cast<uintptr_t>(begin_data.color_attachments[i].imageView));
  }
  mix(reinterpret_cast<uintptr_t>(begin_data.depth_attachment.imageView));
  mix(reinterpret_cast<uintptr_t>(begin_data.stencil_attachment.imageView));
  return view_tag;
}

#ifdef __ANDROID__
void VKCommandBuilder::scope_detail(const VKBeginRenderingData &begin_data,
                                    const int prev_node_type,
                                    const bool directly_adjacent,
                                    const bool matches_previous) const
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
  const uint64_t view_tag = scope_view_tag(begin_data);

  const VkRect2D &area = begin_data.vk_rendering_info.renderArea;
  uint32_t depth_op = 0;
  if (begin_data.vk_rendering_info.pDepthAttachment != nullptr) {
    depth_op = begin_data.depth_attachment.loadOp;
  }

  __android_log_print(ANDROID_LOG_WARN,
                      "blender-scope",
                      "  scope set=%016llx area=%dx%d+%d+%d color=%u depth=%u loadOp=%u "
                      "color0=%016llx prev=%d adj=%d match=%d",
                      (unsigned long long)view_tag,
                      area.extent.width,
                      area.extent.height,
                      area.offset.x,
                      area.offset.y,
                      begin_data.vk_rendering_info.colorAttachmentCount,
                      begin_data.vk_rendering_info.pDepthAttachment != nullptr ? 1 : 0,
                      depth_op,
                      (unsigned long long)reinterpret_cast<uintptr_t>(
                          begin_data.color_attachments[0].imageView),
                      prev_node_type,
                      directly_adjacent ? 1 : 0,
                      matches_previous ? 1 : 0);
}
#endif

#ifdef __ANDROID__
namespace {

/** Short token for the attachment formats this backend produces, so the scope lines stay
 * readable. Unknown formats fall back to their numeric value. */
std::string scope_format_short_name(VkFormat format)
{
  switch (format) {
    case VK_FORMAT_UNDEFINED:
      return "undef";
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return "RGBA16F";
    case VK_FORMAT_R16G16_SFLOAT:
      return "RG16F";
    case VK_FORMAT_R32_SFLOAT:
      return "R32F";
    case VK_FORMAT_R8G8B8A8_UNORM:
      return "RGBA8";
    case VK_FORMAT_B8G8R8A8_UNORM:
      return "BGRA8";
    case VK_FORMAT_R8G8B8A8_SRGB:
      return "RGBA8s";
    case VK_FORMAT_B8G8R8A8_SRGB:
      return "BGRA8s";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return "RGB10A2";
    case VK_FORMAT_R16_UINT:
      return "R16U";
    case VK_FORMAT_R32_UINT:
      return "R32U";
    case VK_FORMAT_R16G16B16A16_UINT:
      return "RGBA16U";
    case VK_FORMAT_D32_SFLOAT:
      return "D32";
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return "D32S8";
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return "D24S8";
    case VK_FORMAT_D16_UNORM:
      return "D16";
    default:
      return "f" + std::to_string(int(format));
  }
}

const char *scope_node_short_name(const VKNodeType type)
{
  switch (type) {
    case VKNodeType::UNUSED:
      return "??";
    case VKNodeType::BEGIN_QUERY:
      return "BQ";
    case VKNodeType::BEGIN_RENDERING:
      return "BR";
    case VKNodeType::BLIT_IMAGE:
      return "BL";
    case VKNodeType::BUILD_ACCELERATION_STRUCTURE:
      return "AS";
    case VKNodeType::CLEAR_ATTACHMENTS:
      return "CLa";
    case VKNodeType::CLEAR_COLOR_IMAGE:
      return "CLc";
    case VKNodeType::CLEAR_DEPTH_STENCIL_IMAGE:
      return "CLd";
    case VKNodeType::COPY_BUFFER:
      return "CPb";
    case VKNodeType::COPY_IMAGE:
      return "CPi";
    case VKNodeType::COPY_IMAGE_TO_BUFFER:
      return "Ci2b";
    case VKNodeType::COPY_BUFFER_TO_IMAGE:
      return "Cb2i";
    case VKNodeType::DISPATCH:
      return "DI";
    case VKNodeType::DISPATCH_INDIRECT:
      return "DIi";
    case VKNodeType::DRAW:
      return "DR";
    case VKNodeType::DRAW_INDEXED:
      return "DRx";
    case VKNodeType::DRAW_INDEXED_INDIRECT:
      return "DRxi";
    case VKNodeType::DRAW_INDIRECT:
      return "DRi";
    case VKNodeType::END_QUERY:
      return "EQ";
    case VKNodeType::END_RENDERING:
      return "ER";
    case VKNodeType::FILL_BUFFER:
      return "FB";
    case VKNodeType::RESET_QUERY_POOL:
      return "RQ";
    case VKNodeType::SYNCHRONIZATION:
      return "SY";
    case VKNodeType::UPDATE_BUFFER:
      return "UP";
    case VKNodeType::UPDATE_MIPMAPS:
      return "UM";
  }
  return "??";
}

/** Identity of the buffer nodes inside a scope run: buffer handles and byte counts, so the log
 * separates pass-local command uploads from copies of scene data. */
void scope_node_identity(const VKRenderGraphNode &node, std::string &r_out)
{
  char buffer[96];
  switch (node.type) {
    case VKNodeType::COPY_BUFFER:
      snprintf(buffer,
               sizeof(buffer),
               "(%llx>%llx:%llu)",
               (unsigned long long)(uintptr_t)node.copy_buffer.src_buffer.vk_handle,
               (unsigned long long)(uintptr_t)node.copy_buffer.dst_buffer.vk_handle,
               (unsigned long long)node.copy_buffer.region.size);
      break;
    case VKNodeType::UPDATE_BUFFER:
      snprintf(buffer,
               sizeof(buffer),
               "(%llx@%llu:%llu)",
               (unsigned long long)(uintptr_t)node.update_buffer.dst_buffer.vk_handle,
               (unsigned long long)node.update_buffer.dst_offset,
               (unsigned long long)node.update_buffer.data_size);
      break;
    case VKNodeType::FILL_BUFFER:
      snprintf(buffer,
               sizeof(buffer),
               "(%llx:%llu)",
               (unsigned long long)(uintptr_t)node.fill_buffer.buffer.vk_handle,
               (unsigned long long)node.fill_buffer.size);
      break;
    default:
      return;
  }
  r_out += buffer;
}

}  // namespace

void VKCommandBuilder::scope_names_detail(VKRenderGraph &render_graph,
                                          const Span<NodeHandle> node_handles,
                                          const int64_t flat_index,
                                          const NodeHandle node_handle,
                                          const VKBeginRenderingData &begin_data) const
{
  if (!blender::perf::scope_names_enabled()) {
    return;
  }

  const std::string name = render_graph.full_debug_group(node_handle);

  /* The ordered node run between the previous scope's close and this begin. That run is what a
   * join would have to move across, so its contents decide what is fusible. Collected walking
   * backwards and printed in chronological order, with buffer identities for the upload nodes. */
  NodeHandle run[64];
  int run_len = 0;
  bool closed_by_scope = false;
  for (int64_t i = flat_index - 1; i >= 0 && run_len < 64; i--) {
    const NodeHandle handle = node_handles[i];
    if (render_graph.nodes_[handle].type == VKNodeType::END_RENDERING) {
      closed_by_scope = true;
      break;
    }
    run[run_len++] = handle;
  }
  std::string between;
  for (int j = run_len - 1; j >= 0; j--) {
    const VKRenderGraphNode &node = render_graph.nodes_[run[j]];
    if (!between.empty()) {
      between += ',';
    }
    between += scope_node_short_name(node.type);
    scope_node_identity(node, between);
    if (between.size() > 300) {
      between += ",..";
      break;
    }
  }
  if (!closed_by_scope && !between.empty()) {
    between = "|start," + between;
  }
  if (between.empty()) {
    between = "(none)";
  }

  /* Nodes inside this scope: draws are the visible work, anything else decides whether the
   * scope could be dropped or joined. */
  int draws = 0;
  int inside_nodes = 0;
  std::string inside;
  for (int64_t i = flat_index + 1; i < int64_t(node_handles.size()); i++) {
    const VKRenderGraphNode &node = render_graph.nodes_[node_handles[i]];
    if (node.type == VKNodeType::END_RENDERING) {
      break;
    }
    inside_nodes++;
    if (inside.size() < 60) {
      if (!inside.empty()) {
        inside += ',';
      }
      inside += scope_node_short_name(node.type);
    }
    if (node.type == VKNodeType::DRAW || node.type == VKNodeType::DRAW_INDEXED ||
        node.type == VKNodeType::DRAW_INDIRECT || node.type == VKNodeType::DRAW_INDEXED_INDIRECT)
    {
      draws++;
    }
  }

  /* Attachment formats and load/store ops, straight off the begin node: what the pass table needs
   * without guessing. Empty slots (null view) are skipped, matching the set= line.
   * `lo`: 0=LOAD, 1=CLEAR, 2=DONT_CARE. `st`: 0=STORE, 1=DONT_CARE. */
  std::string attachments;
  char att_chunk[96];
  const uint32_t color_attachment_count = begin_data.vk_rendering_info.colorAttachmentCount;
  for (uint32_t i = 0; i < color_attachment_count; i++) {
    if (begin_data.color_attachments[i].imageView == VK_NULL_HANDLE) {
      continue;
    }
    snprintf(att_chunk,
             sizeof(att_chunk),
             " c%u=%s lo=%u st=%u",
             i,
             scope_format_short_name(begin_data.color_formats[i]).c_str(),
             uint32_t(begin_data.color_attachments[i].loadOp),
             uint32_t(begin_data.color_attachments[i].storeOp));
    attachments += att_chunk;
  }
  if (begin_data.vk_rendering_info.pDepthAttachment != nullptr) {
    snprintf(att_chunk,
             sizeof(att_chunk),
             " d=%s lo=%u st=%u",
             scope_format_short_name(begin_data.depth_format).c_str(),
             uint32_t(begin_data.depth_attachment.loadOp),
             uint32_t(begin_data.depth_attachment.storeOp));
    attachments += att_chunk;
  }
  if (begin_data.vk_rendering_info.pStencilAttachment != nullptr) {
    snprintf(att_chunk,
             sizeof(att_chunk),
             " s=%s lo=%u st=%u",
             scope_format_short_name(begin_data.stencil_format).c_str(),
             uint32_t(begin_data.stencil_attachment.loadOp),
             uint32_t(begin_data.stencil_attachment.storeOp));
    attachments += att_chunk;
  }

  __android_log_print(ANDROID_LOG_WARN,
                      "blender-scope",
                      "    scope2 name=\"%s\" draws=%d nodes=%d inside=%s between=%s att=%s",
                      name.c_str(),
                      draws,
                      inside_nodes,
                      inside.empty() ? "(none)" : inside.c_str(),
                      between.c_str(),
                      attachments.empty() ? "(none)" : attachments.c_str());
}
#endif

/* Everything a scope join rests on: the two scopes have to fit one render pass (same attachments,
 * aspects, mip/layer ranges, views and depth/stencil shape) and the following scope must not
 * clear, since a clear would have to become a mid-pass vkCmdClearAttachments to survive the join.
 * The render area of the second scope has to fit into the first one's; a sub-rect is allowed when
 * every command the second scope records clips itself to that sub-rect. These are read-only checks
 * over the graph, so they can be done while the groups are walked. */

static bool rect_equal(const VkRect2D &a, const VkRect2D &b)
{
  return a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
         a.extent.width == b.extent.width && a.extent.height == b.extent.height;
}

static bool rect_contains(const VkRect2D &outer, const VkRect2D &inner)
{
  return inner.offset.x >= outer.offset.x && inner.offset.y >= outer.offset.y &&
         int64_t(inner.offset.x) + int64_t(inner.extent.width) <=
             int64_t(outer.offset.x) + int64_t(outer.extent.width) &&
         int64_t(inner.offset.y) + int64_t(inner.extent.height) <=
             int64_t(outer.offset.y) + int64_t(outer.extent.height);
}

/** The scissor rects a draw node carries, empty when it carries none. */
static Span<VkRect2D> draw_node_scissors(VKRenderGraphStorage &storage, VKRenderGraphNode &node)
{
  switch (node.type) {
    case VKNodeType::DRAW:
      return VKDrawNode::storage_data(storage, node.storage_index).graphics.viewport.scissors;
    case VKNodeType::DRAW_INDEXED:
      return VKDrawIndexedNode::storage_data(storage, node.storage_index).graphics.viewport.scissors;
    case VKNodeType::DRAW_INDIRECT:
      return VKDrawIndirectNode::storage_data(storage, node.storage_index).graphics.viewport.scissors;
    case VKNodeType::DRAW_INDEXED_INDIRECT:
      return VKDrawIndexedIndirectNode::storage_data(storage, node.storage_index)
          .graphics.viewport.scissors;
    default:
      return {};
  }
}

bool VKCommandBuilder::scope_begins_match(VKRenderGraph &render_graph,
                                          const Span<NodeHandle> node_handles,
                                          const NodeHandle first_begin,
                                          const NodeHandle second_begin,
                                          const int64_t second_begin_index)
{
  const Span<VKRenderGraphImage> first_links = render_graph.linked_images(first_begin);
  const Span<VKRenderGraphImage> second_links = render_graph.linked_images(second_begin);
  if (first_links.size() != second_links.size()) {
    return false;
  }
  for (const VKRenderGraphImage &second_link : second_links) {
    bool found = false;
    for (const VKRenderGraphImage &first_link : first_links) {
      if (first_link.resource.handle == second_link.resource.handle &&
          first_link.vk_image_aspect == second_link.vk_image_aspect &&
          first_link.subimage.mipmap_level == second_link.subimage.mipmap_level &&
          first_link.subimage.mipmap_count == second_link.subimage.mipmap_count &&
          first_link.subimage.layer_base == second_link.subimage.layer_base &&
          first_link.subimage.layer_count == second_link.subimage.layer_count)
      {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  const VKBeginRenderingData &first_data =
      render_graph.storage_.begin_rendering[render_graph.nodes_[first_begin].storage_index];
  const VKBeginRenderingData &second_data =
      render_graph.storage_.begin_rendering[render_graph.nodes_[second_begin].storage_index];

  /* The resource comparison above matches the image; two scopes can bind the same image through
   * different views (sRGB against linear), and one render pass can only carry one of them. The
   * begin node of the first scope is the one that continues to be recorded, so only its views
   * have to cover the second scope. */
  if (first_data.vk_rendering_info.colorAttachmentCount !=
      second_data.vk_rendering_info.colorAttachmentCount)
  {
    return false;
  }
  for (uint32_t i = 0; i < first_data.vk_rendering_info.colorAttachmentCount; i++) {
    if (first_data.color_attachments[i].imageView !=
        second_data.color_attachments[i].imageView)
    {
      return false;
    }
  }
  const bool first_has_depth = first_data.vk_rendering_info.pDepthAttachment != nullptr;
  const bool second_has_depth = second_data.vk_rendering_info.pDepthAttachment != nullptr;
  if (first_has_depth != second_has_depth) {
    return false;
  }
  if (first_has_depth && first_data.depth_attachment.imageView !=
                            second_data.depth_attachment.imageView)
  {
    return false;
  }
  const bool first_has_stencil = first_data.vk_rendering_info.pStencilAttachment != nullptr;
  const bool second_has_stencil = second_data.vk_rendering_info.pStencilAttachment != nullptr;
  if (first_has_stencil != second_has_stencil) {
    return false;
  }
  if (first_has_stencil && first_data.stencil_attachment.imageView !=
                              second_data.stencil_attachment.imageView)
  {
    return false;
  }

  const VkRect2D &first_area = first_data.vk_rendering_info.renderArea;
  const VkRect2D &second_area = second_data.vk_rendering_info.renderArea;
  if (rect_equal(first_area, second_area)) {
    /* Equal area: the render area clips the same pixels before and after the join, whatever
     * scissors the draws carry. */
    return true;
  }
  if (!rect_contains(first_area, second_area)) {
    return false;
  }
  /* The join widens the area the second scope records under, so every command it records has to
   * stay inside its own (smaller) area: only then do the rasterized pixels stay identical. The
   * draws of a scope normally carry exactly that area as their scissor, since both are built from
   * the same framebuffer scissor rect (see `VKFrameBuffer::render_area_update`). */
  for (int64_t i = second_begin_index + 1; i < int64_t(node_handles.size()); i++) {
    VKRenderGraphNode &node = render_graph.nodes_[node_handles[i]];
    if (node.type == VKNodeType::END_RENDERING) {
      return true;
    }
    if (node.type == VKNodeType::CLEAR_ATTACHMENTS) {
      /* A mid-pass clear rasterizes its own rects, which are not checked here. */
      return false;
    }
    if (!node_type_is_within_rendering(node.type)) {
      /* A transfer, a dispatch or a restart sits in between: not a plain draw run. */
      return false;
    }
    const Span<VkRect2D> scissors = draw_node_scissors(render_graph.storage_, node);
    if (scissors.is_empty()) {
      /* No scissor recorded: the clipping is pipeline state, not this render area. */
      return false;
    }
    for (const VkRect2D &scissor : scissors) {
      if (!rect_contains(second_area, scissor)) {
        return false;
      }
    }
  }
  /* The scope does not close inside this list. */
  return false;
}

static bool scope_begin_clears(const VKBeginRenderingData &begin_data)
{
  const uint32_t color_count = begin_data.vk_rendering_info.colorAttachmentCount;
  for (uint32_t i = 0; i < color_count; i++) {
    if (begin_data.color_attachments[i].imageView == VK_NULL_HANDLE) {
      continue;
    }
    if (begin_data.color_attachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      return true;
    }
  }
  if (begin_data.vk_rendering_info.pDepthAttachment != nullptr &&
      begin_data.depth_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
    return true;
  }
  if (begin_data.vk_rendering_info.pStencilAttachment != nullptr &&
      begin_data.stencil_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
    return true;
  }
  return false;
}

/* The joined scope stores what either scope needed stored, so the store ops of the following
 * scope are copied onto the begin node that continues to be recorded. Turning a DONT_CARE store
 * into STORE is always allowed; both sets of consumers stay correct. */
static void scope_begin_join_stores(VKBeginRenderingData &owner_data,
                                    const VKBeginRenderingData &next_data)
{
  const uint32_t owner_count = owner_data.vk_rendering_info.colorAttachmentCount;
  const uint32_t next_count = next_data.vk_rendering_info.colorAttachmentCount;
  const uint32_t color_count = owner_count < next_count ? owner_count : next_count;
  for (uint32_t i = 0; i < color_count; i++) {
    if (owner_data.color_attachments[i].imageView == VK_NULL_HANDLE) {
      continue;
    }
    if (next_data.color_attachments[i].storeOp == VK_ATTACHMENT_STORE_OP_STORE) {
      owner_data.color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
  }
  if (owner_data.vk_rendering_info.pDepthAttachment != nullptr &&
      next_data.vk_rendering_info.pDepthAttachment != nullptr &&
      next_data.depth_attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE)
  {
    owner_data.depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  if (owner_data.vk_rendering_info.pStencilAttachment != nullptr &&
      next_data.vk_rendering_info.pStencilAttachment != nullptr &&
      next_data.stencil_attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE)
  {
    owner_data.stencil_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
}

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
    /* Always taken so the window resets with the scope totals whether or not it is printed. */
    const VKCmdCounters cmds = vk_cmd_counters_take();
    const uint64_t fast_clears = vk_fast_clear_count_take();
    if (log_enabled) {
      __android_log_print(ANDROID_LOG_WARN,
                          "blender-scope",
                          "scopes/frame=%u pixels/frame=%llu MPix/scope=%llu "
                          "loadOp: clear=%u load=%u dontcare=%u  fallback=%d  "
                          "adjacent=%u mergeable=%u loadok=%u repeat=%u repeatsep=%u merged=%u "
                          "elided=%u fastclear=%u",
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
                          uint32_t(scope_repeat_foreign / scope_report_frames),
                          uint32_t(scope_merged / scope_report_frames),
                          uint32_t(scope_elided / scope_report_frames),
                          uint32_t(fast_clears / scope_report_frames));
      __android_log_print(ANDROID_LOG_WARN,
                          "blender-scope",
                          "cmds/frame: draw=%llu dispatch=%llu barrier=%llu(%llu) pipe=%llu "
                          "ds=%llu vbo=%llu ibo=%llu pc=%llu vp=%llu sc=%llu copy=%llu "
                          "clear=%llu upd=%llu | rend: dyn=%llu/%llu rp=%llu/%llu",
                          (unsigned long long)(cmds.draw_calls / scope_report_frames),
                          (unsigned long long)(cmds.dispatch_calls / scope_report_frames),
                          (unsigned long long)(cmds.barrier_calls / scope_report_frames),
                          (unsigned long long)(cmds.barriers / scope_report_frames),
                          (unsigned long long)(cmds.bind_pipeline / scope_report_frames),
                          (unsigned long long)(cmds.bind_descriptor_sets / scope_report_frames),
                          (unsigned long long)(cmds.bind_vertex_buffers / scope_report_frames),
                          (unsigned long long)(cmds.bind_index_buffer / scope_report_frames),
                          (unsigned long long)(cmds.push_constants / scope_report_frames),
                          (unsigned long long)(cmds.set_viewport / scope_report_frames),
                          (unsigned long long)(cmds.set_scissor / scope_report_frames),
                          (unsigned long long)(cmds.copy_calls / scope_report_frames),
                          (unsigned long long)(cmds.clear_calls / scope_report_frames),
                          (unsigned long long)(cmds.update_buffer / scope_report_frames),
                          (unsigned long long)(cmds.begin_rendering / scope_report_frames),
                          (unsigned long long)(cmds.end_rendering / scope_report_frames),
                          (unsigned long long)(cmds.begin_render_pass / scope_report_frames),
                          (unsigned long long)(cmds.end_render_pass / scope_report_frames));
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
    scope_merged = 0;
    scope_elided = 0;
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
  scope_merge_skip_.clear();
  scope_merge_dropped_.clear();
  scope_merges_.clear();
  collect_scope_elisions(render_graph, node_handles);

  /* The pass below runs with the marked scope joins applied, so the groups it walks and the
   * barriers it produces are the ones the recording sees. A join is only kept when it leaves
   * nothing to be recorded between the two nodes: a group barrier sitting there would be
   * recorded inside the still open scope. Dropping a join changes the tracked state, so the pass
   * is re-run from a snapshot. The loop ends by itself: every re-run keeps all remaining joins or
   * drops at least one of the candidates, and there is a finite number of those. */
  resource_states_before_extraction_ = render_graph.resources_.resources_;
  extract_barriers_pass(render_graph, node_handles, use_local_read);
  while (drop_unsafe_scope_merges()) {
    render_graph.resources_.resources_ = resource_states_before_extraction_;
    extract_barriers_pass(render_graph, node_handles, use_local_read);
  }

  apply_scope_merge_store_patches(render_graph);
}

void VKCommandBuilder::extract_barriers_pass(VKRenderGraph &render_graph,
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
      if (scope_merge_skipped(node_handle) || scope_elide_.contains(node_handle)) {
        /* Joined or elided scope: neither this node's barriers nor its scope bookkeeping
         * apply. */
        continue;
      }
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

      else if (node.type == VKNodeType::END_RENDERING && rendering_active &&
               try_scope_merge(
                   render_graph, node_handles, group_node_index, group_index, rendering_scope))
      {
        /* Joined with the following scope: kept open on purpose, so the close is skipped. */
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
    if (rendering_active && !scope_merge_skipped(node_handles[node_group.last()])) {
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

bool VKCommandBuilder::try_scope_merge(VKRenderGraph &render_graph,
                                       Span<NodeHandle> node_handles,
                                       int64_t group_node_index,
                                       int64_t group_index,
                                       NodeHandle owner_begin)
{
#ifdef __ANDROID__
  /* Joining changes the recorded command stream, so the switch allows a like-for-like fps
   * comparison without a rebuild: `setprop debug.blender.scopemerge 0` and restart. */
  static const bool scope_merge_enabled = []() {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get("debug.blender.scopemerge", value);
    return !(len > 0 && atoi(value) == 0);
  }();
  if (!scope_merge_enabled) {
    return false;
  }
#endif

  if (owner_begin == 0) {
    return false;
  }
  const int64_t begin_index = group_node_index + 1;
  if (begin_index >= int64_t(node_handles.size())) {
    return false;
  }
  const NodeHandle end_rendering = node_handles[group_node_index];
  const NodeHandle begin_rendering = node_handles[begin_index];
  if (render_graph.nodes_[begin_rendering].type != VKNodeType::BEGIN_RENDERING) {
    return false;
  }
  if (scope_elide_.contains(begin_rendering)) {
    /* An elided scope records no begin at all, so a join would keep the previous scope open
     * across its body instead. */
    return false;
  }
  if (scope_merge_dropped_.contains(end_rendering)) {
    return false;
  }
  if (!scope_begins_match(render_graph, node_handles, owner_begin, begin_rendering, begin_index)) {
    return false;
  }
  if (scope_begin_clears(render_graph.storage_.begin_rendering[render_graph.nodes_[begin_rendering]
                                                                    .storage_index]))
  {
    return false;
  }

  /* Nothing is recorded between the two nodes, so dropping the pair does not reorder any command:
   * the second draw run simply keeps the scope open. */
  scope_merge_skip_.add(end_rendering);
  scope_merge_skip_.add(begin_rendering);
  scope_merges_.append(
      ScopeMerge{end_rendering, begin_rendering, owner_begin, group_index});
  return true;
}

bool VKCommandBuilder::drop_unsafe_scope_merges()
{
  bool dropped = false;
  for (int64_t i = int64_t(scope_merges_.size()) - 1; i >= 0; i--) {
    const ScopeMerge &merge = scope_merges_[i];
    /* A barrier recorded between the END and the BEGIN would end up inside the open scope, and
     * the recording asserts against that. Leave that pair to the normal close and begin. */
    const bool clean_boundary = group_pre_barriers_[merge.group_index + 1].is_empty() &&
                                group_post_barriers_[merge.group_index].is_empty();
    if (clean_boundary) {
      continue;
    }
    scope_merge_skip_.remove(merge.end_rendering);
    scope_merge_skip_.remove(merge.begin_rendering);
    scope_merge_dropped_.add(merge.end_rendering);
    scope_merges_.remove_and_reorder(i);
    dropped = true;
  }
  return dropped;
}

void VKCommandBuilder::collect_scope_elisions(const VKRenderGraph &render_graph,
                                              Span<NodeHandle> node_handles)
{
  scope_elide_.clear();

#ifdef __ANDROID__
  /* Dropping the pair changes the recorded stream; the joins' switch turns it off with them
   * (`setprop debug.blender.scopemerge 0`), so both can be compared without a rebuild. */
  static const bool scope_elision_enabled = []() {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get("debug.blender.scopemerge", value);
    return !(len > 0 && atoi(value) == 0);
  }();
  if (!scope_elision_enabled) {
    return;
  }
#endif

  for (int64_t i = 0; i < int64_t(node_handles.size()); i++) {
    const NodeHandle begin_handle = node_handles[i];
    const VKRenderGraphNode &begin_node = render_graph.nodes_[begin_handle];
    if (begin_node.type != VKNodeType::BEGIN_RENDERING) {
      continue;
    }

    const VKBeginRenderingData &begin_data =
        render_graph.storage_.begin_rendering[begin_node.storage_index];

    /* A clear load operation does real work at the begin, so a scope that clears is never
     * dropped. Load and don't-care carry no writes of their own. */
    bool clears = false;
    for (uint32_t attachment = 0; attachment < begin_data.vk_rendering_info.colorAttachmentCount;
         attachment++)
    {
      if (begin_data.color_attachments[attachment].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        clears = true;
        break;
      }
    }
    if (begin_data.vk_rendering_info.pDepthAttachment != nullptr &&
        begin_data.depth_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
    {
      clears = true;
    }
    if (begin_data.vk_rendering_info.pStencilAttachment != nullptr &&
        begin_data.stencil_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
    {
      clears = true;
    }
    if (clears) {
      continue;
    }

    /* Walk the body: only synchronization nodes allowed, and the scope has to close in this
     * list (a begin that never closes would leave the state machine mid-scope). */
    bool closes = false;
    bool has_body_work = false;
    NodeHandle end_handle = 0;
    int body_nodes = 0;
    VKNodeType body_types[16];
    int body_type_count = 0;
    for (int64_t j = i + 1; j < int64_t(node_handles.size()); j++) {
      const NodeHandle inner_handle = node_handles[j];
      const VKRenderGraphNode &inner_node = render_graph.nodes_[inner_handle];
      if (inner_node.type == VKNodeType::END_RENDERING) {
        closes = true;
        end_handle = inner_handle;
        break;
      }
      if (inner_node.type != VKNodeType::SYNCHRONIZATION) {
        has_body_work = true;
      }
      body_nodes++;
      if (body_type_count < 16) {
        body_types[body_type_count++] = inner_node.type;
      }
    }
    if (!closes || has_body_work) {
      continue;
    }

    scope_elide_.add(begin_handle);
    scope_elide_.add(end_handle);

#ifdef __ANDROID__
    if (blender::perf::scope_names_enabled()) {
      std::string body;
      for (int k = 0; k < body_type_count && body.size() < 60; k++) {
        if (!body.empty()) {
          body += ',';
        }
        body += scope_node_short_name(body_types[k]);
      }
      const std::string name = render_graph.full_debug_group(begin_handle);
      __android_log_print(ANDROID_LOG_WARN,
                          "blender-scope",
                          "    scope3 name=\"%s\" ELIDED=1 nodes=%d inside=%s",
                          name.c_str(),
                          body_nodes,
                          body.empty() ? "(none)" : body.c_str());
    }
#endif
  }
}

void VKCommandBuilder::apply_scope_merge_store_patches(VKRenderGraph &render_graph)
{
  for (const ScopeMerge &merge : scope_merges_) {
    VKBeginRenderingData &owner_data =
        render_graph.storage_.begin_rendering[render_graph.nodes_[merge.owner_begin]
                                                  .storage_index];
    const VKBeginRenderingData &next_data =
        render_graph.storage_.begin_rendering[render_graph.nodes_[merge.begin_rendering]
                                                  .storage_index];
    scope_begin_join_stores(owner_data, next_data);
  }
}

/* -------------------------------------------------------------------- */
/** \name GPU scope timing
 * \{ */

void VKCommandBuilder::timestamp_scope_begin(VKCommandBufferInterface &command_buffer,
                                             const VKBeginRenderingData &begin_data)
{
  if (!timestamp_config_.enabled || timestamp_config_.pool == VK_NULL_HANDLE ||
      timestamp_config_.cursor == nullptr)
  {
    return;
  }

  /* One query pair per scope. The cursor is the caller's; it wraps so the pool is reused, and
   * every pair is reset before it is written. */
  uint32_t &cursor = *timestamp_config_.cursor;
  if (cursor + 2 > timestamp_config_.pool_size) {
    cursor = 0;
  }
  const uint32_t begin_query = cursor;
  cursor += 2;

  /* The reset has to happen outside a render pass instance; every begin is recorded before the
   * scope opens, so this is a legal place for it. */
  command_buffer.reset_query_pool(timestamp_config_.pool, begin_query, 2);
  command_buffer.write_timestamp(
      timestamp_config_.pool, begin_query, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

  const VkRect2D &area = begin_data.vk_rendering_info.renderArea;
  VKScopeTimestamp scope_timestamp;
  scope_timestamp.tag = scope_view_tag(begin_data);
  scope_timestamp.begin_query = begin_query;
  scope_timestamp.end_query = begin_query + 1;
  scope_timestamp.width = area.extent.width;
  scope_timestamp.height = area.extent.height;
  scope_timestamp.offset_x = area.offset.x;
  scope_timestamp.offset_y = area.offset.y;
  scope_timestamp.color_count = begin_data.vk_rendering_info.colorAttachmentCount;
  scope_timestamps_.append(scope_timestamp);
  timestamp_open_index_ = int64_t(scope_timestamps_.size()) - 1;
}

void VKCommandBuilder::timestamp_scope_end(VKCommandBufferInterface &command_buffer)
{
  if (timestamp_open_index_ < 0 || !timestamp_config_.enabled) {
    return;
  }
  command_buffer.write_timestamp(timestamp_config_.pool,
                                 scope_timestamps_[timestamp_open_index_].end_query,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  timestamp_open_index_ = -1;
}

/** \} */

void VKCommandBuilder::groups_build_commands(VKRenderGraph &render_graph,
                                             VKCommandBufferInterface &command_buffer,
                                             Span<NodeHandle> node_handles)
{
  DebugGroups debug_groups = {};
  VKBoundPipelines active_pipelines = {};

  NodeHandle rendering_scope = 0;
  bool rendering_active = false;
  /** Set while walking the nodes of the scope whose begin commands are still to be recorded. */
  const VKBeginRenderingData *pending_begin_data = nullptr;
  /** Set when the node being recorded closes an open scope (see VKEndRenderingNode). */
  bool pending_end_close = false;

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

      if (scope_merge_skipped(node_handle)) {
        /* Joined scope: the close or the begin node is dropped. A dropped close means the scope
         * stays open through this group. */
        if (node.type == VKNodeType::END_RENDERING) {
          scope_merged++;
        }
        continue;
      }

      if (scope_elide_.contains(node_handle)) {
        /* Elided scope: the begin/end pair carries no load/store work of its own, so both nodes
         * are dropped. Any synchronization node inside it is recorded where it is. */
        if (node.type == VKNodeType::BEGIN_RENDERING) {
          scope_elided++;
        }
        continue;
      }

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
            /* Empty slots carry a DONT_CARE placeholder and a null view, so the view is the
             * filter here. The format array is filled from the producer side and is only
             * identically zero for empty slots. */
            if (begin_data.color_attachments[i].imageView == VK_NULL_HANDLE) {
              continue;
            }
            count_load_op(begin_data.color_attachments[i].loadOp);
          }
          if (begin_data.vk_rendering_info.pDepthAttachment != nullptr) {
            count_load_op(begin_data.depth_attachment.loadOp);
          }
          const int64_t flat_index = group_nodes.first();
          const int prev_node_type = flat_index > 0 ?
                                         int(render_graph.nodes_[node_handles[flat_index - 1]].type) :
                                         -1;
          const bool directly_adjacent = prev_node_type == int(VKNodeType::END_RENDERING);
          const bool matches_previous = rendering_scope != 0 && scope_begins_match(render_graph,
                                                                                   node_handles,
                                                                                   rendering_scope,
                                                                                   node_handle,
                                                                                   flat_index);
          scope_detail(begin_data, prev_node_type, directly_adjacent, matches_previous);
          scope_names_detail(render_graph, node_handles, flat_index, node_handle, begin_data);
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
          if (rendering_scope != 0) {
            if (directly_adjacent) {
              scope_adjacent++;
            }
            if (matches_previous) {
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
        /* The begin commands are recorded further down (after the node barriers); write the begin
         * timestamp there, just before the scope opens. */
        pending_begin_data = &render_graph.storage_.begin_rendering[node.storage_index];
      }

      else if (node.type == VKNodeType::END_RENDERING) {
        /* The END node records the close further down. Remember whether it really closes an open
         * scope before the flag is cleared, so the timing pair matches the command stream. */
        pending_end_close = rendering_active;
        rendering_active = false;
      }
      else if (node_type_is_within_rendering(node.type)) {
        if (!rendering_active) {
          /* Restart rendering scope. */
          VKRenderGraphNode &rendering_node = render_graph.nodes_[rendering_scope];
          VKBeginRenderingNode::reconfigure_for_restart(
              render_graph.storage_.begin_rendering[rendering_node.storage_index]);
          timestamp_scope_begin(
              command_buffer, render_graph.storage_.begin_rendering[rendering_node.storage_index]);
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
      if (pending_begin_data != nullptr) {
        timestamp_scope_begin(command_buffer, *pending_begin_data);
        pending_begin_data = nullptr;
      }
      node.build_commands(command_buffer, render_graph.storage_, active_pipelines);
      if (pending_end_close) {
        timestamp_scope_end(command_buffer);
        pending_end_close = false;
      }
    }

    if (rendering_active && !scope_merge_skipped(group_node_handles.last())) {
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
      /* The scope is closed here, so its timing pair closes with it. */
      timestamp_scope_end(command_buffer);
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
