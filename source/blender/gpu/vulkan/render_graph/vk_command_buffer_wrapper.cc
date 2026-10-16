/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_command_buffer_wrapper.hh"
#include "vk_backend.hh"
#include "vk_device.hh"

#ifdef __ANDROID__
#  include <android/log.h>
#  include <cstdlib>
#  include <sys/system_properties.h>
#endif

namespace blender::gpu::render_graph {

/* Command counters for the reporting window. Only the submission thread records commands, so no
 * synchronization is needed. Taken (and reset) by VKCommandBuilder::record_commands. */
static VKCmdCounters cmd_counters;

VKCmdCounters vk_cmd_counters_take()
{
  const VKCmdCounters result = cmd_counters;
  cmd_counters = {};
  return result;
}

VKCommandBufferWrapper::VKCommandBufferWrapper(VkCommandBuffer vk_command_buffer,
                                               const VolkDeviceTable &functions,
                                               const VKExtensions &extensions,
                                               const VKWorkarounds &workarounds)
    : vk_command_buffer_(vk_command_buffer), functions(&functions)
{
  use_dynamic_rendering_local_read = extensions.dynamic_rendering_local_read;
  use_multi_draw_indirect = extensions.multi_draw_indirect;
  use_dynamic_state_viewport_scissor = !workarounds.static_viewport_scissor;
}

void VKCommandBufferWrapper::begin_recording()
{
  VkCommandBufferBeginInfo vk_command_buffer_begin_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      nullptr,
      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      nullptr};
  functions->vkBeginCommandBuffer(vk_command_buffer_, &vk_command_buffer_begin_info);
}

void VKCommandBufferWrapper::end_recording()
{
  functions->vkEndCommandBuffer(vk_command_buffer_);
}

void VKCommandBufferWrapper::bind_pipeline(VkPipelineBindPoint pipeline_bind_point,
                                           VkPipeline pipeline)
{
  cmd_counters.bind_pipeline++;
  functions->vkCmdBindPipeline(vk_command_buffer_, pipeline_bind_point, pipeline);
}

void VKCommandBufferWrapper::bind_descriptor_sets(VkPipelineBindPoint pipeline_bind_point,
                                                  VkPipelineLayout layout,
                                                  uint32_t first_set,
                                                  uint32_t descriptor_set_count,
                                                  const VkDescriptorSet *p_descriptor_sets,
                                                  uint32_t dynamic_offset_count,
                                                  const uint32_t *p_dynamic_offsets)
{
  cmd_counters.bind_descriptor_sets++;
  functions->vkCmdBindDescriptorSets(vk_command_buffer_,
                                     pipeline_bind_point,
                                     layout,
                                     first_set,
                                     descriptor_set_count,
                                     p_descriptor_sets,
                                     dynamic_offset_count,
                                     p_dynamic_offsets);
}

void VKCommandBufferWrapper::bind_index_buffer(VkBuffer buffer,
                                               VkDeviceSize offset,
                                               VkIndexType index_type)
{
  cmd_counters.bind_index_buffer++;
  functions->vkCmdBindIndexBuffer(vk_command_buffer_, buffer, offset, index_type);
}

void VKCommandBufferWrapper::bind_vertex_buffers(uint32_t first_binding,
                                                 uint32_t binding_count,
                                                 const VkBuffer *p_buffers,
                                                 const VkDeviceSize *p_offsets)
{
  cmd_counters.bind_vertex_buffers++;
  functions->vkCmdBindVertexBuffers(
      vk_command_buffer_, first_binding, binding_count, p_buffers, p_offsets);
}

void VKCommandBufferWrapper::draw(uint32_t vertex_count,
                                  uint32_t instance_count,
                                  uint32_t first_vertex,
                                  uint32_t first_instance)
{
  cmd_counters.draw_calls++;
  functions->vkCmdDraw(
      vk_command_buffer_, vertex_count, instance_count, first_vertex, first_instance);
}

void VKCommandBufferWrapper::draw_indexed(uint32_t index_count,
                                          uint32_t instance_count,
                                          uint32_t first_index,
                                          int32_t vertex_offset,
                                          uint32_t first_instance)
{
  cmd_counters.draw_calls++;
  functions->vkCmdDrawIndexed(
      vk_command_buffer_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VKCommandBufferWrapper::draw_indirect(VkBuffer buffer,
                                           VkDeviceSize offset,
                                           uint32_t draw_count,
                                           uint32_t stride)
{
  cmd_counters.draw_calls++;
  functions->vkCmdDrawIndirect(vk_command_buffer_, buffer, offset, draw_count, stride);
}

void VKCommandBufferWrapper::draw_indexed_indirect(VkBuffer buffer,
                                                   VkDeviceSize offset,
                                                   uint32_t draw_count,
                                                   uint32_t stride)
{
  cmd_counters.draw_calls++;
  functions->vkCmdDrawIndexedIndirect(vk_command_buffer_, buffer, offset, draw_count, stride);
}

void VKCommandBufferWrapper::dispatch(uint32_t group_count_x,
                                      uint32_t group_count_y,
                                      uint32_t group_count_z)
{
  cmd_counters.dispatch_calls++;
  functions->vkCmdDispatch(vk_command_buffer_, group_count_x, group_count_y, group_count_z);
}

void VKCommandBufferWrapper::dispatch_indirect(VkBuffer buffer, VkDeviceSize offset)
{
  cmd_counters.dispatch_calls++;
  functions->vkCmdDispatchIndirect(vk_command_buffer_, buffer, offset);
}

void VKCommandBufferWrapper::update_buffer(VkBuffer dst_buffer,
                                           VkDeviceSize dst_offset,
                                           VkDeviceSize data_size,
                                           const void *p_data)
{
  cmd_counters.update_buffer++;
  functions->vkCmdUpdateBuffer(vk_command_buffer_, dst_buffer, dst_offset, data_size, p_data);
}

void VKCommandBufferWrapper::copy_buffer(VkBuffer src_buffer,
                                         VkBuffer dst_buffer,
                                         uint32_t region_count,
                                         const VkBufferCopy *p_regions)
{
  cmd_counters.copy_calls++;
  functions->vkCmdCopyBuffer(vk_command_buffer_, src_buffer, dst_buffer, region_count, p_regions);
}

void VKCommandBufferWrapper::copy_image(VkImage src_image,
                                        VkImageLayout src_image_layout,
                                        VkImage dst_image,
                                        VkImageLayout dst_image_layout,
                                        uint32_t region_count,
                                        const VkImageCopy *p_regions)
{
  cmd_counters.copy_calls++;
  functions->vkCmdCopyImage(vk_command_buffer_,
                            src_image,
                            src_image_layout,
                            dst_image,
                            dst_image_layout,
                            region_count,
                            p_regions);
}

void VKCommandBufferWrapper::blit_image(VkImage src_image,
                                        VkImageLayout src_image_layout,
                                        VkImage dst_image,
                                        VkImageLayout dst_image_layout,
                                        uint32_t region_count,
                                        const VkImageBlit *p_regions,
                                        VkFilter filter)
{
  cmd_counters.copy_calls++;
  functions->vkCmdBlitImage(vk_command_buffer_,
                            src_image,
                            src_image_layout,
                            dst_image,
                            dst_image_layout,
                            region_count,
                            p_regions,
                            filter);
}

void VKCommandBufferWrapper::copy_buffer_to_image(VkBuffer src_buffer,
                                                  VkImage dst_image,
                                                  VkImageLayout dst_image_layout,
                                                  uint32_t region_count,
                                                  const VkBufferImageCopy *p_regions)
{
  cmd_counters.copy_calls++;
  functions->vkCmdCopyBufferToImage(
      vk_command_buffer_, src_buffer, dst_image, dst_image_layout, region_count, p_regions);
}

void VKCommandBufferWrapper::copy_image_to_buffer(VkImage src_image,
                                                  VkImageLayout src_image_layout,
                                                  VkBuffer dst_buffer,
                                                  uint32_t region_count,
                                                  const VkBufferImageCopy *p_regions)
{
  cmd_counters.copy_calls++;
  functions->vkCmdCopyImageToBuffer(
      vk_command_buffer_, src_image, src_image_layout, dst_buffer, region_count, p_regions);
}

void VKCommandBufferWrapper::fill_buffer(VkBuffer dst_buffer,
                                         VkDeviceSize dst_offset,
                                         VkDeviceSize size,
                                         uint32_t data)
{
  cmd_counters.clear_calls++;
  functions->vkCmdFillBuffer(vk_command_buffer_, dst_buffer, dst_offset, size, data);
}

void VKCommandBufferWrapper::clear_color_image(VkImage image,
                                               VkImageLayout image_layout,
                                               const VkClearColorValue *p_color,
                                               uint32_t range_count,
                                               const VkImageSubresourceRange *p_ranges)
{
  cmd_counters.clear_calls++;
  functions->vkCmdClearColorImage(
      vk_command_buffer_, image, image_layout, p_color, range_count, p_ranges);
}
void VKCommandBufferWrapper::clear_depth_stencil_image(
    VkImage image,
    VkImageLayout image_layout,
    const VkClearDepthStencilValue *p_depth_stencil,
    uint32_t range_count,
    const VkImageSubresourceRange *p_ranges)
{
  cmd_counters.clear_calls++;
  functions->vkCmdClearDepthStencilImage(
      vk_command_buffer_, image, image_layout, p_depth_stencil, range_count, p_ranges);
}

void VKCommandBufferWrapper::clear_attachments(uint32_t attachment_count,
                                               const VkClearAttachment *p_attachments,
                                               uint32_t rect_count,
                                               const VkClearRect *p_rects)
{
  cmd_counters.clear_calls++;
  functions->vkCmdClearAttachments(
      vk_command_buffer_, attachment_count, p_attachments, rect_count, p_rects);
}

void VKCommandBufferWrapper::pipeline_barrier(
    VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count,
    const VkMemoryBarrier *p_memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier *p_buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier *p_image_memory_barriers)
{
  cmd_counters.barrier_calls++;
  cmd_counters.barriers += uint64_t(memory_barrier_count) + uint64_t(buffer_memory_barrier_count) +
                           uint64_t(image_memory_barrier_count);
  functions->vkCmdPipelineBarrier(vk_command_buffer_,
                                  src_stage_mask,
                                  dst_stage_mask,
                                  dependency_flags,
                                  memory_barrier_count,
                                  p_memory_barriers,
                                  buffer_memory_barrier_count,
                                  p_buffer_memory_barriers,
                                  image_memory_barrier_count,
                                  p_image_memory_barriers);
}

void VKCommandBufferWrapper::push_constants(VkPipelineLayout layout,
                                            VkShaderStageFlags stage_flags,
                                            uint32_t offset,
                                            uint32_t size,
                                            const void *p_values)
{
  cmd_counters.push_constants++;
  functions->vkCmdPushConstants(vk_command_buffer_, layout, stage_flags, offset, size, p_values);
}

void VKCommandBufferWrapper::set_viewport(const Vector<VkViewport> viewports)
{
  if (!use_dynamic_state_viewport_scissor) {
    return;
  }
  cmd_counters.set_viewport++;
  functions->vkCmdSetViewport(vk_command_buffer_, 0, viewports.size(), viewports.data());
}

void VKCommandBufferWrapper::set_scissor(const Vector<VkRect2D> scissors)
{
  if (!use_dynamic_state_viewport_scissor) {
    return;
  }
  cmd_counters.set_scissor++;
  functions->vkCmdSetScissor(vk_command_buffer_, 0, scissors.size(), scissors.data());
}
void VKCommandBufferWrapper::set_line_width(const float line_width)
{
  functions->vkCmdSetLineWidth(vk_command_buffer_, line_width);
}
void VKCommandBufferWrapper::set_stencil_compare_mask(const uint32_t compare_mask)
{
  functions->vkCmdSetStencilCompareMask(
      vk_command_buffer_, VK_STENCIL_FACE_FRONT_AND_BACK, compare_mask);
}
void VKCommandBufferWrapper::set_stencil_write_mask(const uint32_t write_mask)
{
  functions->vkCmdSetStencilWriteMask(
      vk_command_buffer_, VK_STENCIL_FACE_FRONT_AND_BACK, write_mask);
}
void VKCommandBufferWrapper::set_stencil_reference(const uint32_t reference)
{
  functions->vkCmdSetStencilReference(
      vk_command_buffer_, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}
void VKCommandBufferWrapper::set_front_face(const VkFrontFace front_face)
{
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdSetFrontFaceEXT);
  device.functions.vkCmdSetFrontFaceEXT(vk_command_buffer_, front_face);
}
void VKCommandBufferWrapper::set_vertex_input(
    Span<VkVertexInputBindingDescription2EXT> vertex_binding_descriptions,
    Span<VkVertexInputAttributeDescription2EXT> vertex_attribute_descriptions)
{
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdSetVertexInputEXT);
  device.functions.vkCmdSetVertexInputEXT(vk_command_buffer_,
                                          vertex_binding_descriptions.size(),
                                          vertex_binding_descriptions.data(),
                                          vertex_attribute_descriptions.size(),
                                          vertex_attribute_descriptions.data());
}

void VKCommandBufferWrapper::begin_rendering(const VkRenderingInfo *p_rendering_info)
{
  if (VKRenderPassFallback::is_active()) {
    /* Reached only if a call site bypasses VKBeginRenderingNode, which is where the fallback is
     * normally entered. The render pass for the scope has not been built at that point, so there
     * is nothing correct to do here; skipping turns what used to be a SIGSEGV at 0x0 into a
     * missing render scope that the validation layer reports. */
    return;
  }
  BLI_assert(functions->vkCmdBeginRenderingKHR);
  cmd_counters.begin_rendering++;
  functions->vkCmdBeginRenderingKHR(vk_command_buffer_, p_rendering_info);
}

void VKCommandBufferWrapper::end_rendering()
{
  if (VKRenderPassFallback::is_active()) {
    /* Same reasoning as begin_rendering(). Not the end of a fallback scope -- that goes through
     * end_rendering_fallback() -- so ending the render pass here would close the wrong one. */
    return;
  }
  BLI_assert(functions->vkCmdEndRenderingKHR);
  cmd_counters.end_rendering++;
  functions->vkCmdEndRenderingKHR(vk_command_buffer_);
}

void VKCommandBufferWrapper::begin_render_pass(VkRenderPass vk_render_pass,
                                               VkFramebuffer vk_framebuffer,
                                               const VkRect2D &render_area,
                                               Span<VkClearValue> clear_values)
{
  VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin_info.renderPass = vk_render_pass;
  begin_info.framebuffer = vk_framebuffer;
  begin_info.renderArea = render_area;
  begin_info.clearValueCount = uint32_t(clear_values.size());
  begin_info.pClearValues = clear_values.data();
  /* Inline contents. Spelled as a cast rather than VK_SUBPASS_CONTENTS_INLINE so it does not depend
   * on which revision of the Vulkan headers the NDK ships: VK_NULL_HANDLE expands to nullptr under
   * C++11 and later, and nullptr cannot initialise the VkSubpassContents enum that this parameter
   * became when VK_KHR_maintenance1 promoted it out of void*. The cast is the same value, 0. */
  const VkSubpassContents inline_contents = static_cast<VkSubpassContents>(0);
#ifdef __ANDROID__
  /* Every Vulkan call the fallback path makes is reachable through this wrapper, and each one
   * dereferences a volk pointer that a 1.1 driver may have left null -- a SIGSEGV at 0x0 with an
   * otherwise empty backtrace. Trace the fallback's own calls so the crash log says which one
   * fired instead of leaving it to be guessed. */
  {
    static const bool log_enabled = []() {
      char value[PROP_VALUE_MAX] = {};
      return __system_property_get("debug.blender.log", value) > 0 && atoi(value) != 0;
    }();
    static int counter = 0;
    if (log_enabled && (counter++ % 20) == 0) {
      __android_log_print(ANDROID_LOG_WARN,
                          "blender-rpfb",
                          "begin_render_pass #%d rp=%p fb=%p clear=%u",
                          counter,
                          (const void *)vk_render_pass,
                          (const void *)vk_framebuffer,
                          uint32_t(clear_values.size()));
    }
  }
#endif
  functions->vkCmdBeginRenderPass(vk_command_buffer_, &begin_info, inline_contents);
  cmd_counters.begin_render_pass++;
}

void VKCommandBufferWrapper::end_render_pass()
{
  functions->vkCmdEndRenderPass(vk_command_buffer_);
  cmd_counters.end_render_pass++;
}

void VKCommandBufferWrapper::begin_query(VkQueryPool vk_query_pool,
                                         uint32_t query_index,
                                         VkQueryControlFlags vk_query_control_flags)
{
  functions->vkCmdBeginQuery(
      vk_command_buffer_, vk_query_pool, query_index, vk_query_control_flags);
}

void VKCommandBufferWrapper::end_query(VkQueryPool vk_query_pool, uint32_t query_index)
{
  functions->vkCmdEndQuery(vk_command_buffer_, vk_query_pool, query_index);
}

void VKCommandBufferWrapper::reset_query_pool(VkQueryPool vk_query_pool,
                                              uint32_t first_query,
                                              uint32_t query_count)
{
  functions->vkCmdResetQueryPool(vk_command_buffer_, vk_query_pool, first_query, query_count);
}

void VKCommandBufferWrapper::write_timestamp(VkQueryPool vk_query_pool,
                                             uint32_t query_index,
                                             VkPipelineStageFlagBits stage)
{
  functions->vkCmdWriteTimestamp(vk_command_buffer_, stage, vk_query_pool, query_index);
}

void VKCommandBufferWrapper::begin_debug_utils_label(
    const VkDebugUtilsLabelEXT *vk_debug_utils_label)
{
  if (vkCmdBeginDebugUtilsLabelEXT) {
    vkCmdBeginDebugUtilsLabelEXT(vk_command_buffer_, vk_debug_utils_label);
  }
}

void VKCommandBufferWrapper::end_debug_utils_label()
{
  if (vkCmdEndDebugUtilsLabelEXT) {
    vkCmdEndDebugUtilsLabelEXT(vk_command_buffer_);
  }
}

/* VK_KHR_ray_tracing */
void VKCommandBufferWrapper::build_acceleration_structure(
    const VkAccelerationStructureBuildGeometryInfoKHR *p_infos,
    const VkAccelerationStructureBuildRangeInfoKHR *p_build_range_infos)
{
  const VKDevice &device = VKBackend::get().device;
  device.functions.vkCmdBuildAccelerationStructuresKHR(
      vk_command_buffer_, 1, p_infos, &p_build_range_infos);
}

}  // namespace blender::gpu::render_graph
