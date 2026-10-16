/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_set.hh"

#include "vk_common.hh"
#include "vk_render_graph_node.hh"
#include "vk_render_graph_links.hh"
#include "vk_scheduler.hh"

namespace blender::gpu::render_graph {
class VKRenderGraph;

struct LayeredImageBinding {
  VkImage vk_image;
  VkImageLayout vk_image_layout;
  uint32_t layer;
  uint32_t layer_count;
};

/**
 * GPU scope timing, driven from the submission thread. The command builder writes one timestamp
 * query pair around every recorded render scope; the caller owns the pool and the cursor and reads
 * the pairs back once the submission has completed. Disabled by default, so nothing changes unless
 * the on-device profiler turns it on.
 */
struct VKScopeTimestampConfig {
  VkQueryPool pool = VK_NULL_HANDLE;
  uint32_t pool_size = 0;
  /** Write cursor into the pool, advanced by two per scope. Owned by the caller. */
  uint32_t *cursor = nullptr;
  bool enabled = false;
};

/** One recorded render scope, tagged with the two queries written around its commands. */
struct VKScopeTimestamp {
  /** Attachment set identity, the same tag the scope accounting logs. */
  uint64_t tag = 0;
  uint32_t begin_query = 0;
  uint32_t end_query = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t offset_x = 0;
  uint32_t offset_y = 0;
  uint32_t color_count = 0;
};

/**
 * Build the command buffer for sending to the device queue.
 *
 * Determine which nodes needs to be scheduled, Then for each node generate the needed pipeline
 * barriers and commands.
 */
class VKCommandBuilder {
 private:
  /**
   * List of all extracted VkBufferMemoryBarriers. These barriers will be referenced by
   * Barrier::buffer_memory_barriers.
   */
  Vector<VkBufferMemoryBarrier> vk_buffer_memory_barriers_;
  /**
   * List of all extracted VkImageMemoryBarriers. These barriers will be referenced by
   * Barrier::image_memory_barriers.
   */
  Vector<VkImageMemoryBarrier> vk_image_memory_barriers_;

  /**
   * Per-frame render-scope accounting, reported through the android.perf switch.
   *
   * On a tile-based GPU every scope ends the tile, and what that costs tracks the pixels covered
   * by the attachments, not the number of draw calls. Counting both is what separates "many small
   * scopes that could be merged" from "few large ones where only the load/store ops would help".
   * Counted on both the dynamic rendering and the render pass fallback path, so the two can be
   * compared directly.
   */
  uint64_t scope_pixels = 0;
  uint32_t scope_count = 0;
  uint32_t scope_load_clear = 0;
  uint32_t scope_load_keep = 0;
  uint32_t scope_load_none = 0;
  /** Frames since the last report; the totals are averaged over this many. */
  uint32_t scope_report_frames = 0;
  /** Adjacent scopes measured while looking into where the tile flushes can legally be cut. */
  uint32_t scope_adjacent = 0;
  uint32_t scope_mergeable = 0;
  uint32_t scope_mergeable_loadok = 0;
  /** Scopes repeating the attachment set of the previous scope, adjacent or not. The separated
   * ones are the candidates that need the work in between moved out of the way first. */
  uint32_t scope_repeat = 0;
  uint32_t scope_repeat_foreign = 0;

  /** GPU scope timing. The configuration is set once; the list accumulates the scopes recorded
   * since the last submission and is drained by the submission thread when it submits. */
  VKScopeTimestampConfig timestamp_config_;
  Vector<VKScopeTimestamp> scope_timestamps_;
  /** Index into `scope_timestamps_` of the pair whose begin timestamp was written and whose end
   * timestamp is still pending, -1 when no scope is open. */
  int64_t timestamp_open_index_ = -1;

  /**
   * A join between two scopes: an END_RENDERING immediately followed by a BEGIN_RENDERING on the
   * same attachments and render area. Nothing is recorded between the two nodes, so the pair can
   * be dropped and the second draw run keeps the scope open instead of flushing its tiles.
   *
   * Filled by the barrier extraction, consumed by the recording phase.
   */
  struct ScopeMerge {
    NodeHandle end_rendering = 0;
    NodeHandle begin_rendering = 0;
    /** The begin node that opens the scope being kept open, which carries the store ops. */
    NodeHandle owner_begin = 0;
    /** Group of the END node. The joined group boundary is `group_index` to `group_index + 1`. */
    int64_t group_index = 0;
  };
  Set<NodeHandle> scope_merge_skip_;
  Set<NodeHandle> scope_merge_dropped_;
  Vector<ScopeMerge> scope_merges_;
  /** Number of joins applied, reported with the scope accounting. */
  uint32_t scope_merged = 0;
  /**
   * Resource states before the barrier extraction. The extraction advances them in place, so a
   * re-run (a join dropped for a dirty boundary) needs them restored first.
   */
  Vector<VKResourceStateTracker::Resource> resource_states_before_extraction_;

  /** Android only. One line per scope, behind the android.perf switch. */
  void scope_detail(const VKBeginRenderingData &begin_data,
                    int prev_node_type,
                    bool directly_adjacent,
                    bool matches_previous) const;

  /** Android debug only. One extra line per scope: pass name (debug group path), draw count,
   * node count inside, and the ordered node run between the previous scope and this begin.
   * Behind `debug.blender.scopenames`, off by default. */
  void scope_names_detail(VKRenderGraph &render_graph,
                          Span<NodeHandle> node_handles,
                          int64_t flat_index,
                          NodeHandle node_handle) const;

  /** Opens a timestamp pair for a scope that is about to record its begin. */
  void timestamp_scope_begin(VKCommandBufferInterface &command_buffer,
                             const VKBeginRenderingData &begin_data);
  /** Writes the end timestamp of the pair opened by `timestamp_scope_begin`, if any. */
  void timestamp_scope_end(VKCommandBufferInterface &command_buffer);

  struct Barrier {
    /** Index range into `VKCommandBuilder::vk_buffer_memory_barriers_` */
    IndexRange buffer_memory_barriers;
    /** Index range into `VKCommandBuilder::vk_image_memory_barriers_` */
    IndexRange image_memory_barriers;

    VkPipelineStageFlags src_stage_mask = VK_PIPELINE_STAGE_NONE;
    VkPipelineStageFlags dst_stage_mask = VK_PIPELINE_STAGE_NONE;

    bool is_empty() const
    {
      return buffer_memory_barriers.is_empty() && image_memory_barriers.is_empty();
    }
  };

  /**
   * An image can consist out of layers and mipmaps. In some cases each layer/mipmap can get
   * different VkImageLayouts. To not complicate the global state tracker we track sub-resources
   * only when building the barriers.
   */
  struct ImageTracker {
    /**
     * Local reference to the active command builder.
     *
     * The reference is used to add VkImageMemoryBarrier's to the command builder.
     */
    VKCommandBuilder &command_builder;
    ImageTracker(VKCommandBuilder &command_builder) : command_builder(command_builder) {}

    /**
     * All layered attachments of the last rendering scope (VKNodeType::BEGIN_RENDERING).
     *
     * When binding layer from these images we expect that they aren't used as attachment and can
     * be transitioned into a different image layout. These image layouts are stored in
     * `layered_bindings`.
     */
    Set<VkImage> tracked_attachments;

    /**
     * Keep track of the changes to images since the start of the rendering scope. This allows
     * reverting and rebuilding the correct state when ending or resuming the render scope.
     */
    struct SubImageChange {
      VkImage vk_image;
      VkImageLayout vk_image_layout;
      VkImageAspectFlags vk_image_aspect;
      VKSubImageRange subimage;
    };
    Vector<SubImageChange> changes;

    /**
     * Update the layered attachments list when beginning a new render scope.
     *
     * node_handle should be a handle that points to a VKNodeType::BEGIN_RENDERING.
     * Any attachments that are layered will be added to the `tracked_attachments` list.
     */
    void begin(const VKRenderGraph &render_graph, NodeHandle node_handle);

    /**
     * Is layered tracking enabled for the given vk_image.
     */
    inline bool contains(VkImage vk_image) const
    {
      return tracked_attachments.contains(vk_image);
    }

    /**
     * Ensure the layout of a layer.
     *
     * - `old_layout` should be the expected layout of the full image.
     */
    void update(VkImage vk_image,
                const VKSubImageRange &subimage,
                VkImageLayout old_layout,
                VkImageLayout new_layout,
                VkImageAspectFlags aspect,
                Barrier &r_barrier);
    /**
     * Ensure the layout of a mipmap level.
     *
     * - `old_layout` should be the expected layout of the full image.
     */
    void update(VkImage vk_image,
                uint32_t mipmap_level,
                VkImageLayout old_layout,
                VkImageLayout new_layout,
                VkImageAspectFlags aspect,
                Barrier &r_barrier);

    /**
     * End layer tracking.
     *
     * All modified layers (layer_tracking_update) will be changed back to the image layout of
     * the texture (most likely a `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`).
     *
     * Render suspension/resuming will not work after calling this method.
     */
    void end(Barrier &r_barrier, bool use_local_read);

    /**
     * Suspend layer tracking
     *
     * Temporarily suspend layer tracking. This transits all modified layers back to its original
     * layout.
     * NOTE: Only call this method when you the rendering will be resumed, otherwise use
     * `layer_tracking_end`.
     */
    void suspend(Barrier &r_barrier, bool use_local_read);

    /**
     * Resume suspended layer tracking.
     *
     * Resume suspended layer tracking. This transits all registered layers back to its modified
     * state.
     */
    void resume(Barrier &r_barrier, bool use_local_read);
  };

  /**
   * Index range of the nodes of a group.
   *
   * The indexes are to `VKScheduler::result_` that is passed along `Span<NodeHandle> nodes` of
   * `build_nodes`.
   */
  using GroupNodes = IndexRange;

  /**
   * Index range into barrier_list_;
   */
  using Barriers = IndexRange;
  using BarrierIndex = int64_t;

  /** Per group store the indices of the nodes. */
  Vector<GroupNodes> group_nodes_;
  /** Barriers that will be recorded just before the commands of a group are recorded. */
  Vector<Barriers> group_pre_barriers_;
  /** Barriers that will be recorded after a group is recorded. */
  Vector<Barriers> group_post_barriers_;
  /**
   * Barriers that will be recorded just before the commands of a specific node are recorded. The
   * barriers are stored per NodeHandle and in the same order as `Span<NodeHandle> nodes`.
   */
  Vector<Barriers> node_pre_barriers_;

  /** List of all generated barriers. */
  Vector<Barrier> barrier_list_;

 public:
  /**
   * Build execution groups and barriers.
   * This method should be performed when the resources are locked.
   */
  void build_nodes(VKRenderGraph &render_graph,
                   VKCommandBufferInterface &command_buffer,
                   Span<NodeHandle> node_handles);

  /**
   * Record commands of the nodes provided by the `node_handles` parameter. The commands are
   * recorded into the given `command_buffer`.
   *
   * `build_nodes` needs to be called before calling with exact the same parameters.
   */
  void record_commands(VKRenderGraph &render_graph,
                       VKCommandBufferInterface &command_buffer,
                       Span<NodeHandle> node_handles);

  /**
   * GPU scope timing. Configure before recording; the builder writes one query pair per scope.
   * `scope_timestamps_get` returns the pairs recorded since the last `scope_timestamps_clear`,
   * which the submission thread calls when it hands the pairs over for readback.
   */
  void timestamps_configure(const VKScopeTimestampConfig &config)
  {
    timestamp_config_ = config;
  }
  Span<VKScopeTimestamp> scope_timestamps_get() const
  {
    return scope_timestamps_;
  }
  void scope_timestamps_clear()
  {
    scope_timestamps_.clear();
    timestamp_open_index_ = -1;
  }

 private:
  /**
   *  Split the node_handles in logical groups.
   *
   * A new group is created when the next node is switching from data/compute to graphics and each
   * data/compute is also put in its own group.
   */
  void groups_init(const VKRenderGraph &render_graph, Span<NodeHandle> node_handles);

  /**
   * Extract the memory/buffer/image barriers from the command groups and add them to the pre/post
   * barriers.
   *
   * This process is single threaded as resource states change during the extraction process. The
   * result of this function would allow the sub builders to be built in parallel.
   */
  void groups_extract_barriers(VKRenderGraph &render_graph,
                               Span<NodeHandle> node_handles,
                               bool use_local_read);

  /**
   * One pass over the nodes, extracting the barriers with the marked scope joins applied. Called
   * again from a clean state whenever a join has to be dropped.
   */
  void extract_barriers_pass(VKRenderGraph &render_graph,
                             Span<NodeHandle> node_handles,
                             bool use_local_read);

  /**
   * Whether the scope the second begin opens can continue the scope the first begin opened: same
   * attachments (handle, aspect, mip/layer range), same views in the same slots, same depth and
   * stencil shape, and a render area that fits the first scope's.
   *
   * The render area of a scope is the framebuffer scissor rect whenever the scissor test is on
   * (see `VKFrameBuffer::render_area_update`) and the draws of the scope carry that same rect as
   * their dynamic scissor. A scope whose area is merely a sub-rect of the first one can join it
   * when every draw it records clips itself to that sub-rect: the draws rasterize the same pixels
   * inside the wider pass, which just keeps a larger load/store region. `second_begin_index` is
   * the position of the second begin in the flat node list, used to look at what that scope
   * records before it closes.
   */
  bool scope_begins_match(VKRenderGraph &render_graph,
                          Span<NodeHandle> node_handles,
                          NodeHandle first_begin,
                          NodeHandle second_begin,
                          int64_t second_begin_index);

  /**
   * Try to join the scope this END_RENDERING closes with the scope the immediately following
   * BEGIN_RENDERING opens, marking both nodes in `scope_merge_skip_` when the pair is legal.
   *
   * Returns true when the pair was marked, in which case the close is skipped.
   */
  bool try_scope_merge(VKRenderGraph &render_graph,
                       Span<NodeHandle> node_handles,
                       int64_t group_node_index,
                       int64_t group_index,
                       NodeHandle owner_begin);

  /**
   * Drop the marked joins whose boundary carries barriers. Recorded there they would sit inside
   * the still open scope. Returns true when at least one join was dropped.
   */
  bool drop_unsafe_scope_merges();

  /** Copy the store ops of each joined scope onto the begin node that will be recorded. */
  void apply_scope_merge_store_patches(VKRenderGraph &render_graph);

  inline bool scope_merge_skipped(NodeHandle node_handle) const
  {
    return scope_merge_skip_.contains(node_handle);
  }

  /**
   * Record all the commands for all the groups to the command buffer.
   */
  void groups_build_commands(VKRenderGraph &render_graph,
                             VKCommandBufferInterface &command_buffer,
                             Span<NodeHandle> node_handles);

  /**
   * Build the pipeline barriers that should be recorded before any other commands of the node
   * group the given node is part of is being recorded.
   */
  void build_pipeline_barriers(VKRenderGraph &render_graph,
                               NodeHandle node_handle,
                               VkPipelineStageFlags pipeline_stage,
                               ImageTracker &image_tracker,
                               Barrier &r_barrier,
                               bool within_rendering = false);
  void reset_barriers(Barrier &r_barrier);
  void send_pipeline_barriers(VKCommandBufferInterface &command_buffer,
                              const Barrier &barrier,
                              bool within_rendering);

  void add_buffer_barriers(VKRenderGraph &render_graph,
                           NodeHandle node_handle,
                           VkPipelineStageFlags node_stages,
                           Barrier &r_barrier);
  void add_buffer_barrier(VkBuffer vk_buffer,
                          Barrier &r_barrier,
                          VkAccessFlags src_access_mask,
                          VkAccessFlags dst_access_mask);
  void add_buffer_read_barriers(VKRenderGraph &render_graph,
                                NodeHandle node_handle,
                                VkPipelineStageFlags node_stages,
                                Barrier &r_barrier);
  void add_buffer_write_barriers(VKRenderGraph &render_graph,
                                 NodeHandle node_handle,
                                 VkPipelineStageFlags node_stages,
                                 Barrier &r_barrier);

  void add_image_barriers(VKRenderGraph &render_graph,
                          NodeHandle node_handle,
                          VkPipelineStageFlags node_stages,
                          ImageTracker &image_tracker,
                          Barrier &r_barrier,
                          bool within_rendering);
  void add_image_barrier(VkImage vk_image,
                         Barrier &r_barrier,
                         VkAccessFlags src_access_mask,
                         VkAccessFlags dst_access_mask,
                         VkImageLayout old_image_layout,
                         VkImageLayout new_image_layout,
                         VkImageAspectFlags aspect_mask,
                         const VKSubImageRange &subimage);
  void add_image_read_barriers(VKRenderGraph &render_graph,
                               NodeHandle node_handle,
                               VkPipelineStageFlags node_stages,
                               ImageTracker &image_tracker,
                               Barrier &r_barrier,
                               bool within_rendering);
  void add_image_write_barriers(VKRenderGraph &render_graph,
                                NodeHandle node_handle,
                                VkPipelineStageFlags node_stages,
                                ImageTracker &image_tracker,
                                Barrier &r_barrier,
                                bool within_rendering);

  struct DebugGroups {
    /**
     * Index of the active debug_group. Points to an element in
     * `VKRenderGraph.debug_.used_groups`.
     */
    int64_t active_debug_group_id = -1;
    /** Current level of debug groups. (number of nested debug groups). */
    int debug_level = 0;
  };

  /**
   * Ensure that the debug group associated with the given node_handle is activated.
   *
   * When activating it determines how to walk from the current debug group to the to be activated
   * debug group by performing end/begin commands on the command buffer.
   *
   * This ensures that when nodes are reordered that they still appear in the right debug group.
   */
  void activate_debug_group(VKRenderGraph &render_graph,
                            VKCommandBufferInterface &command_buffer,
                            DebugGroups &debug_groups,
                            NodeHandle node_handle);

  /**
   * Make sure no debugging groups are active anymore.
   */
  void finish_debug_groups(VKCommandBufferInterface &command_buffer, DebugGroups &debug_groups);

 private:
  /**
   * Update the layered attachments list when beginning a new render scope.
   */
  void layer_tracking_begin(const VKRenderGraph &render_graph, NodeHandle node_handle);

  /**
   * Ensure the layout of a layer.
   *
   * - `old_layout` should be the expected layout of the full image.
   */
  void layer_tracking_update(VkImage vk_image,
                             uint32_t layer,
                             uint32_t layer_count,
                             VkImageLayout old_layout,
                             VkImageLayout new_layout);

  /**
   * End layer tracking.
   *
   * All modified layers (layer_tracking_update) will be changed back to the image layout of
   * the texture (most likely a `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`).
   *
   * Render suspension/resuming will not work after calling this method.
   */
  void layer_tracking_end(VKCommandBufferInterface &command_buffer);

  /**
   * Suspend layer tracking
   *
   * Temporarily suspend layer tracking. This transits all modified layers back to its original
   * layout.
   * NOTE: Only call this method when you the rendering will be resumed, otherwise use
   * `layer_tracking_end`.
   */
  void layer_tracking_suspend(VKCommandBufferInterface &command_buffer);

  /**
   * Resume suspended layer tracking.
   *
   * Resume suspended layer tracking. This transits all registered layers back to its modified
   * state.
   */
  void layer_tracking_resume(VKCommandBufferInterface &command_buffer);

  bool node_has_input_attachments(const VKRenderGraph &render_graph, NodeHandle node);

  std::string to_string_barrier(const Barrier &barrier);
};

}  // namespace blender::gpu::render_graph
