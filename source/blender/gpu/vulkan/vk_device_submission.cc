/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>

#include "BLI_mutex.hh"
#include "BLI_perf_probe.hh"

#include "vk_device.hh"
#include "vk_to_string.hh"

#include "CLG_log.h"

#ifdef __ANDROID__
#  include <android/log.h>
#  include <cstdarg>
#  include <cstdlib>
#  include <sys/system_properties.h>
/* Per-submission tracing, several lines a frame, so off unless `debug.blender.log` is set. */
static void vk_submit_log(const char *format, ...)
{
  static const bool enabled = []() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.blender.log", value) > 0 && atoi(value) != 0;
  }();
  if (!enabled) {
    return;
  }
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_INFO, "blender-vksubmit", format, args);
  va_end(args);
}
#  define VKSUBLOG(...) vk_submit_log(__VA_ARGS__)
#else
#  define VKSUBLOG(...) ((void)0)
#endif

namespace blender {

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace gpu {

/* -------------------------------------------------------------------- */
/** \name Render graph
 * \{ */

struct VKRenderGraphWait {
  Mutex is_submitted_mutex;
  std::condition_variable_any is_submitted_condition;
  bool is_submitted;
};

struct VKRenderGraphSubmitTask {
  render_graph::VKRenderGraph *render_graph;
  uint64_t timeline;
  bool submit_to_device;
  VkPipelineStageFlags wait_dst_stage_mask;
  VkSemaphore wait_semaphore;
  VkSemaphore signal_semaphore;
  VkFence signal_fence;
  VKRenderGraphWait *wait_for_submission;
};

TimelineValue VKDevice::render_graph_submit(render_graph::VKRenderGraph *render_graph,
                                            VKDiscardPool &context_discard_pool,
                                            bool submit_to_device,
                                            bool wait_for_submission,
                                            bool wait_for_completion,
                                            VkPipelineStageFlags wait_dst_stage_mask,
                                            VkSemaphore wait_semaphore,
                                            VkSemaphore signal_semaphore,
                                            VkFence signal_fence)
{
  /* Syncing input flags. */
  /* When we wait for completion/submission we must submit to device. */
  submit_to_device |= wait_for_completion;
  submit_to_device |= wait_for_submission;
  /* We don't need to wait for submission when waiting for completion. */
  wait_for_submission &= !wait_for_completion;

  VKRenderGraphSubmitTask *submit_task = MEM_new<VKRenderGraphSubmitTask>(__func__);
  submit_task->render_graph = render_graph;
  submit_task->submit_to_device = submit_to_device;
  submit_task->wait_dst_stage_mask = wait_dst_stage_mask;
  submit_task->wait_semaphore = wait_semaphore;
  submit_task->signal_semaphore = signal_semaphore;
  submit_task->signal_fence = signal_fence;
  submit_task->wait_for_submission = nullptr;

  VKRenderGraphWait wait_condition{};
  if (wait_for_submission) {
    submit_task->wait_for_submission = &wait_condition;
  }
  TimelineValue timeline = 0;
  {
    std::scoped_lock lock(orphaned_data.mutex_get());
    timeline = submit_task->timeline = submit_to_device ? ++timeline_value_ : timeline_value_ + 1;
    orphaned_data.timeline_ = timeline;
    orphaned_data.move_data(context_discard_pool, timeline);
    BLI_thread_queue_push(
        submitted_render_graphs_, submit_task, BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL);
  }
  submit_task = nullptr;

  if (wait_for_submission) {
    PERF_ZONE(vk_wait_submit);
    std::unique_lock<Mutex> lock(wait_condition.is_submitted_mutex);
    /* Sliced for the same reason wait_for_timeline() is: an unbounded wait turns a missed
     * wakeup into a process that is frozen rather than broken, and nothing is reported. That
     * is what Eevee does on this device -- the main thread sleeps here with no error, no CPU,
     * and no "Compiling EEVEE engine shaders" ever resolving. A missed notify should be
     * visible, not a hang. */
    for (int attempt = 0; !wait_condition.is_submitted; attempt++) {
      const std::cv_status status =
          wait_condition.is_submitted_condition.wait_for(lock, std::chrono::seconds(2));
      if (wait_condition.is_submitted) {
        break;
      }
      VKSUBLOG("submit wait STUCK timeline=%llu attempt=%d res=%d",
               (unsigned long long)timeline,
               attempt,
               int(status));
      if (attempt + 1 >= 5) {
        CLOG_ERROR(&LOG,
                   "Vulkan: submission %llu was never handed to the device (waited 10s).",
                   (unsigned long long)timeline);
        break;
      }
    }
  }

  if (wait_for_completion) {
    PERF_ZONE(vk_wait_complete);
    wait_for_timeline(timeline);
  }
  return timeline;
}

void VKDevice::wait_for_timeline(TimelineValue timeline)
{
  if (timeline == 0) {
    return;
  }
  VkSemaphoreWaitInfo vk_semaphore_wait_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &vk_timeline_semaphore_, &timeline};
  if (is_device_lost()) {
    /* Nothing will ever signal this. Return so the caller unwinds and releases its locks. */
    return;
  }
  /* Wait in slices rather than indefinitely.
   *
   * This used to be a single vkWaitSemaphoresKHR with UINT64_MAX, which turns any GPU that
   * stops signalling into a process that is frozen rather than broken: the main thread sleeps
   * inside the call holding the draw lock, every other thread waits behind it, and nothing
   * reports an error. On a phone that is exactly how Eevee presented -- "Compiling EEVEE engine
   * shaders" stayed on screen indefinitely with every thread asleep and no CPU used at all.
   *
   * A wedged GPU is not always reported as VK_ERROR_DEVICE_LOST either, so the slice loop also
   * catches the case where submission keeps "succeeding" while no work is executed. */
  const int max_attempts = 5;
  VkResult wait_result;
  for (int attempt = 0;; attempt++) {
    wait_result = functions.vkWaitSemaphoresKHR(vk_device_, &vk_semaphore_wait_info, 2000000000ull);
    if (wait_result != VK_TIMEOUT) {
      break;
    }
    if (is_device_lost()) {
      VKSUBLOG("wait ABORTED (device lost) want=%llu signalled=%llu",
               (unsigned long long)timeline,
               (unsigned long long)submission_finished_timeline_get());
      return;
    }
    const TimelineValue signalled = submission_finished_timeline_get();
    VKSUBLOG("wait STUCK want=%llu signalled=%llu allocated=%llu attempt=%d",
             (unsigned long long)timeline,
             (unsigned long long)signalled,
             (unsigned long long)timeline_value_,
             attempt);
    if (attempt + 1 >= max_attempts) {
      device_lost_.store(true, std::memory_order_relaxed);
      CLOG_ERROR(&LOG,
                 "Vulkan: GPU stopped making progress (waiting for %llu, signalled %llu). "
                 "Treating the device as lost.",
                 (unsigned long long)timeline,
                 (unsigned long long)signalled);
      VKSUBLOG("wait GIVING UP want=%llu signalled=%llu -> device marked lost",
               (unsigned long long)timeline,
               (unsigned long long)signalled);
      return;
    }
  }
  if (wait_result != VK_SUCCESS) {
    CLOG_ERROR(
        &LOG, "Vulkan: failed to wait for synchronization timeline [%s]", to_string(wait_result));
  }
}

void VKDevice::wait_queue_idle()
{
  std::scoped_lock lock(*queue_mutex_);
  functions.vkQueueWaitIdle(vk_queue_);
}

render_graph::VKRenderGraph *VKDevice::render_graph_new()
{
  render_graph::VKRenderGraph *render_graph = static_cast<render_graph::VKRenderGraph *>(
      BLI_thread_queue_pop_timeout(unused_render_graphs_, 0));
  if (render_graph) {
    return render_graph;
  }

  std::scoped_lock lock(resources.mutex);
  render_graph = MEM_new<render_graph::VKRenderGraph>(__func__, resources);
  render_graphs_.append(render_graph);
  return render_graph;
}

void VKDevice::submission_runner(VKDevice *device)
{
  CLOG_TRACE(&LOG, "Submission runner has started");

  VkCommandPool vk_command_pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo vk_command_pool_create_info = {
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      nullptr,
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      device->vk_queue_family_};
  device->functions.vkCreateCommandPool(
      device->vk_device_, &vk_command_pool_create_info, nullptr, &vk_command_pool);

  /* GPU scope timing. The command builder writes one timestamp query pair per render scope, this
   * thread reads the pairs back once the submission that wrote them has completed and reports the
   * windowed averages. Android diagnostics, behind the same `debug.blender.perf` switch as the
   * scope accounting. */
#ifdef __ANDROID__
  struct GPUProfileScope {
    uint64_t tag;
    uint32_t width;
    uint32_t height;
    uint32_t offset_x;
    uint32_t offset_y;
    uint64_t count;
    double sum_ms;
    double max_ms;
  };
  struct GPUProfilePending {
    uint64_t timeline;
    /** Whole-submission timestamp pair, or UINT32_MAX when it could not be written. */
    uint32_t global_begin_query = UINT32_MAX;
    uint32_t global_end_query = UINT32_MAX;
    Vector<render_graph::VKScopeTimestamp> scopes;
  };
  VkQueryPool timestamp_pool = VK_NULL_HANDLE;
  uint32_t timestamp_pool_size = 0;
  uint32_t timestamp_cursor = 0;
  double timestamp_period_ms = 0.0;
  uint32_t timestamp_valid_bits = 0;
  bool timestamps_enabled = false;
  Vector<GPUProfilePending> timestamp_pending;
  Vector<GPUProfileScope> timestamp_stats;
  uint64_t timestamp_samples = 0;
  uint64_t timestamp_submits = 0;
  double timestamp_submit_sum_ms = 0.0;
  double timestamp_submit_max_ms = 0.0;
  /* Scope sums run alongside the whole-submission pair: the difference between the two is how
   * much the scope boundaries overlap (see retire_timestamps). */
  double timestamp_scope_sum_ms = 0.0;
  double timestamp_scope_sum_max_ms = 0.0;
  {
    static const bool perf_enabled = []() {
      char value[PROP_VALUE_MAX] = {};
      return __system_property_get("debug.blender.perf", value) > 0 && atoi(value) != 0;
    }();
    const VkPhysicalDeviceProperties &properties = device->physical_device_properties_get();
    timestamp_period_ms = double(properties.limits.timestampPeriod) * 1e-6;
    /* Timestamps are a per queue family capability, not a device limit. */
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        device->vk_physical_device_, &queue_family_count, nullptr);
    if (queue_family_count > 0 && queue_family_count <= 8) {
      VkQueueFamilyProperties queue_families[8];
      vkGetPhysicalDeviceQueueFamilyProperties(
          device->vk_physical_device_, &queue_family_count, queue_families);
      if (device->vk_queue_family_ < queue_family_count) {
        timestamp_valid_bits = queue_families[device->vk_queue_family_].timestampValidBits;
      }
    }
    if (perf_enabled && timestamp_valid_bits > 0 && properties.limits.timestampPeriod > 0.0f) {
      timestamp_pool_size = 2048;
      VkQueryPoolCreateInfo timestamp_pool_create_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      timestamp_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      timestamp_pool_create_info.queryCount = timestamp_pool_size;
      const VkResult create_result = device->functions.vkCreateQueryPool(
          device->vk_device_, &timestamp_pool_create_info, nullptr, &timestamp_pool);
      timestamps_enabled = create_result == VK_SUCCESS && timestamp_pool != VK_NULL_HANDLE;
    }
    __android_log_print(ANDROID_LOG_WARN,
                        "blender-gputime",
                        "gpu timestamps %s: pool=%u valid_bits=%u period=%.2fns",
                        timestamps_enabled ? "on" : "off",
                        timestamp_pool_size,
                        timestamp_valid_bits,
                        double(properties.limits.timestampPeriod));
  }
#endif

  render_graph::VKScheduler scheduler;
  render_graph::VKCommandBuilder command_builder;
  Vector<VkCommandBuffer> command_buffers_unused;
  TimelineResources<VkCommandBuffer> command_buffers_in_use;
  VkCommandBuffer vk_command_buffer = VK_NULL_HANDLE;
  std::optional<render_graph::VKCommandBufferWrapper> command_buffer;
  uint64_t previous_gc_timeline = 0;
  uint64_t num_nodes = 0;

#ifdef __ANDROID__
  /* Pairs are allocated from the same pool as the scope pairs; the scopes advance the cursor
   * themselves, so this only picks the two slots the whole-submission pair uses. */
  render_graph::VKScopeTimestampConfig timestamp_config;
  timestamp_config.pool = timestamp_pool;
  timestamp_config.pool_size = timestamp_pool_size;
  timestamp_config.cursor = &timestamp_cursor;
  timestamp_config.enabled = timestamps_enabled;
  command_builder.timestamps_configure(timestamp_config);

  /** Queries of the whole-submission pair of the command buffer being recorded. */
  uint32_t timestamp_global_begin = UINT32_MAX;

  /* Read back the timestamp pairs of completed submissions and fold them into the window report.
   * A submission's queries are only read once its timeline value has passed, so the results are
   * available without waiting. */
  auto retire_timestamps = [&](uint64_t finished_timeline) {
    for (int64_t i = int64_t(timestamp_pending.size()) - 1; i >= 0; i--) {
      GPUProfilePending &pending = timestamp_pending[i];
      if (pending.timeline > finished_timeline) {
        continue;
      }
      /* Reads one timestamp pair and masks the ticks to the queue family's valid bits. */
      auto read_pair = [&](const uint32_t begin_query,
                           uint64_t &r_begin_ticks,
                           uint64_t &r_end_ticks) {
        uint64_t results[2] = {0, 0};
        const VkResult result = device->functions.vkGetQueryPoolResults(device->vk_device_,
                                                                        timestamp_pool,
                                                                        begin_query,
                                                                        2,
                                                                        sizeof(results),
                                                                        results,
                                                                        sizeof(uint64_t),
                                                                        VK_QUERY_RESULT_WAIT_BIT |
                                                                            VK_QUERY_RESULT_64_BIT);
        if (result != VK_SUCCESS) {
          return false;
        }
        r_begin_ticks = results[0];
        r_end_ticks = results[1];
        if (timestamp_valid_bits < 64) {
          const uint64_t mask = (uint64_t(1) << timestamp_valid_bits) - 1;
          r_begin_ticks &= mask;
          r_end_ticks &= mask;
          if (r_end_ticks < r_begin_ticks) {
            r_end_ticks += mask + 1;
          }
        }
        return true;
      };

      /* Whole-submission pair: every command in the buffer sits between its two writes, so this
       * is the submission's real busy time, the number comparable with the frame period. The
       * scope times cannot be added up to get it: a scope end waits with BOTTOM_OF_PIPE for all
       * prior work to drain, while the next scope begins with TOP_OF_PIPE without waiting, so
       * consecutive scope intervals overlap by the depth of the pipeline. That is why the raw
       * scope sum used to come out well above the frame period. */
      double global_ms = -1.0;
      if (pending.global_begin_query != UINT32_MAX) {
        uint64_t begin_ticks = 0, end_ticks = 0;
        if (read_pair(pending.global_begin_query, begin_ticks, end_ticks)) {
          global_ms = double(end_ticks - begin_ticks) * timestamp_period_ms;
        }
      }

      /* Scope times, attributed so the intervals tile the submission instead of overlapping: a
       * scope starts where the previous one stopped draining. The raw pairs in the pool are
       * untouched; only the attribution changes. With that, the scope sum matches the
       * whole-submission pair and each scope keeps a usable individual cost. */
      double scope_sum_ms = 0.0;
      uint64_t prev_end_ticks = 0;
      bool has_prev_end = false;
      for (const render_graph::VKScopeTimestamp &scope : pending.scopes) {
        uint64_t begin_ticks = 0, end_ticks = 0;
        if (!read_pair(scope.begin_query, begin_ticks, end_ticks)) {
          continue;
        }
        uint64_t effective_begin = begin_ticks;
        if (has_prev_end && prev_end_ticks > effective_begin) {
          effective_begin = prev_end_ticks;
        }
        if (end_ticks < effective_begin) {
          /* Never report a negative time if a counter wrapped or the driver returned the pair
           * out of order. */
          end_ticks = effective_begin;
        }
        prev_end_ticks = end_ticks;
        has_prev_end = true;
        const double scope_ms = double(end_ticks - effective_begin) * timestamp_period_ms;
        scope_sum_ms += scope_ms;
        GPUProfileScope *accum = nullptr;
        for (GPUProfileScope &candidate : timestamp_stats) {
          if (candidate.tag == scope.tag && candidate.width == scope.width &&
              candidate.height == scope.height && candidate.offset_x == scope.offset_x &&
              candidate.offset_y == scope.offset_y)
          {
            accum = &candidate;
            break;
          }
        }
        if (accum == nullptr) {
          timestamp_stats.append(GPUProfileScope{scope.tag,
                                                 scope.width,
                                                 scope.height,
                                                 scope.offset_x,
                                                 scope.offset_y,
                                                 0,
                                                 0.0,
                                                 0.0});
          accum = &timestamp_stats.last();
        }
        accum->count++;
        accum->sum_ms += scope_ms;
        if (scope_ms > accum->max_ms) {
          accum->max_ms = scope_ms;
        }
        timestamp_samples++;
      }
      if (global_ms < 0.0 && !pending.scopes.is_empty()) {
        /* No whole-submission pair for this one: the scope sum is the best available number. */
        global_ms = scope_sum_ms;
      }
      if (global_ms >= 0.0) {
        timestamp_submits++;
        timestamp_submit_sum_ms += global_ms;
        if (global_ms > timestamp_submit_max_ms) {
          timestamp_submit_max_ms = global_ms;
        }
        timestamp_scope_sum_ms += scope_sum_ms;
        if (scope_sum_ms > timestamp_scope_sum_max_ms) {
          timestamp_scope_sum_max_ms = scope_sum_ms;
        }
      }
      timestamp_pending.remove(i);
    }

    if (timestamp_samples >= 60) {
      std::sort(timestamp_stats.begin(),
                timestamp_stats.end(),
                [](const GPUProfileScope &a, const GPUProfileScope &b) {
                  return (a.sum_ms / double(a.count)) > (b.sum_ms / double(b.count));
                });
      std::string report;
      char chunk[224];
      snprintf(chunk,
               sizeof(chunk),
               "window: scopes=%llu submits=%llu gpu/submit avg=%.2fms max=%.2fms "
               "scopesum avg=%.2fms max=%.2fms",
               (unsigned long long)timestamp_samples,
               (unsigned long long)timestamp_submits,
               timestamp_submits ? timestamp_submit_sum_ms / double(timestamp_submits) : 0.0,
               timestamp_submit_max_ms,
               timestamp_submits ? timestamp_scope_sum_ms / double(timestamp_submits) : 0.0,
               timestamp_scope_sum_max_ms);
      report = chunk;
      for (const GPUProfileScope &scope : timestamp_stats) {
        if (report.size() > 1400) {
          break;
        }
        snprintf(chunk,
                 sizeof(chunk),
                 " | %016llx %ux%u+%d+%d n=%llu avg=%.2f max=%.2f",
                 (unsigned long long)scope.tag,
                 scope.width,
                 scope.height,
                 int(scope.offset_x),
                 int(scope.offset_y),
                 (unsigned long long)scope.count,
                 scope.sum_ms / double(scope.count),
                 scope.max_ms);
        report += chunk;
      }
      __android_log_print(ANDROID_LOG_WARN, "blender-gputime", "%s", report.c_str());
      timestamp_stats.clear();
      timestamp_samples = 0;
      timestamp_submits = 0;
      timestamp_submit_sum_ms = 0.0;
      timestamp_submit_max_ms = 0.0;
      timestamp_scope_sum_ms = 0.0;
      timestamp_scope_sum_max_ms = 0.0;
    }
  };
#endif

  CLOG_TRACE(&LOG, "Submission runner initialized");
  while (!device->submission_thread_should_exit_) {
    VKRenderGraphSubmitTask *submit_task = static_cast<VKRenderGraphSubmitTask *>(
        BLI_thread_queue_pop_timeout(device->submitted_render_graphs_, 1));
    if (submit_task == nullptr) {
      continue;
    }
    uint64_t current_timeline = device->submission_finished_timeline_get();
    if (assign_if_different(previous_gc_timeline, current_timeline)) {
      device->orphaned_data.destroy_discarded_resources(*device, current_timeline);
    }
#ifdef __ANDROID__
    if (timestamps_enabled) {
      retire_timestamps(current_timeline);
    }
#endif

    if (!command_buffer.has_value()) {
      /* Check for completed command buffers that can be reused. */
      if (command_buffers_unused.is_empty()) {
        command_buffers_in_use.remove_old(current_timeline,
                                          [&](VkCommandBuffer vk_command_buffer) {
                                            command_buffers_unused.append(vk_command_buffer);
                                          });
      }

      /* Create new command buffers when there are no left to be reused. */
      if (command_buffers_unused.is_empty()) {
        command_buffers_unused.resize(10, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo vk_command_buffer_allocate_info = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            nullptr,
            vk_command_pool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            10};
        device->functions.vkAllocateCommandBuffers(
            device->vk_device_, &vk_command_buffer_allocate_info, command_buffers_unused.data());
      };

      vk_command_buffer = command_buffers_unused.pop_last();
      command_buffer = std::make_optional<render_graph::VKCommandBufferWrapper>(
          vk_command_buffer, device->functions, device->extensions_, device->workarounds_);
      command_buffer->begin_recording();
#ifdef __ANDROID__
      if (timestamps_enabled) {
        /* Whole-submission pair: opened here, at the first command of the buffer, closed right
         * before the buffer is submitted. */
        if (timestamp_cursor + 2 > timestamp_pool_size) {
          timestamp_cursor = 0;
        }
        timestamp_global_begin = timestamp_cursor;
        timestamp_cursor += 2;
        command_buffer->reset_query_pool(timestamp_pool, timestamp_global_begin, 2);
        command_buffer->write_timestamp(
            timestamp_pool, timestamp_global_begin, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
      }
#endif
    }

    BLI_assert(vk_command_buffer != VK_NULL_HANDLE);

    render_graph::VKRenderGraph &render_graph = *submit_task->render_graph;
    Span<render_graph::NodeHandle> node_handles = scheduler.select_nodes(render_graph);
    {
      std::scoped_lock lock_resources(device->resources.mutex);
      PERF_ZONE(vk_build_nodes);
      command_builder.build_nodes(render_graph, *command_buffer, node_handles);
    }
    {
      PERF_ZONE(vk_record_commands);
      command_builder.record_commands(render_graph, *command_buffer, node_handles);
    }
    num_nodes += node_handles.size();

    if (submit_task->submit_to_device) {
#ifdef __ANDROID__
      if (timestamps_enabled && timestamp_global_begin != UINT32_MAX) {
        command_buffer->write_timestamp(timestamp_pool,
                                        timestamp_global_begin + 1,
                                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      }
#endif
      /* Finalize current command buffer. */
      command_buffer->end_recording();

      uint32_t wait_semaphore_len = submit_task->wait_semaphore == VK_NULL_HANDLE ? 1 : 2;
      VkSemaphore wait_semaphores[2] = {device->vk_timeline_semaphore_,
                                        submit_task->wait_semaphore};
      uint64_t wait_semaphore_values[2] = {submit_task->timeline - 1, 0};

      uint32_t signal_semaphore_len = submit_task->signal_semaphore == VK_NULL_HANDLE ? 1 : 2;
      VkSemaphore signal_semaphores[2] = {device->vk_timeline_semaphore_,
                                          submit_task->signal_semaphore};
      uint64_t signal_semaphore_values[2] = {submit_task->timeline, 0};
      VkPipelineStageFlags pipeline_stage_flags[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                      submit_task->wait_dst_stage_mask};

      VkTimelineSemaphoreSubmitInfo vk_timeline_semaphore_submit_info = {
          VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
          nullptr,
          wait_semaphore_len,
          wait_semaphore_values,
          signal_semaphore_len,
          signal_semaphore_values};
      VkSubmitInfo vk_submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                     &vk_timeline_semaphore_submit_info,
                                     wait_semaphore_len,
                                     wait_semaphores,
                                     pipeline_stage_flags,
                                     1,
                                     &vk_command_buffer,
                                     signal_semaphore_len,
                                     signal_semaphores};

      CLOG_TRACE(&LOG, "Submitting %u render graph nodes to device.", uint32_t(num_nodes));
      const uint64_t submitted_nodes = num_nodes;
      num_nodes = 0;

      VkResult submit_result = VK_SUCCESS;
      if (device->is_device_lost()) {
        /* Queue is dead; every further submit would fail. Skip it, but keep draining the task
         * queue so waiters are released and command buffers are recycled. */
        VKSUBLOG("submit timeline=%llu SKIPPED (device lost)",
                 (unsigned long long)submit_task->timeline);
      }
      else {
        std::scoped_lock lock_queue(*device->queue_mutex_);
        {
          PERF_ZONE(vk_queue_submit);
          submit_result = device->functions.vkQueueSubmit(
              device->vk_queue_, 1, &vk_submit_info, submit_task->signal_fence);
        }
        VKSUBLOG(
            "submit timeline=%llu nodes=%u waits_for=%llu signalled=%llu wait_sem=%p "
            "signal_sem=%p res=%d",
            (unsigned long long)submit_task->timeline,
            uint32_t(submitted_nodes),
            (unsigned long long)(submit_task->timeline - 1),
            (unsigned long long)device->submission_finished_timeline_get(),
            (void *)submit_task->wait_semaphore,
            (void *)submit_task->signal_semaphore,
            int(submit_result));
        if (submit_result == VK_ERROR_DEVICE_LOST) {
          /* The timeline semaphore will never advance again. Latch this so waiters give up
           * instead of blocking forever while holding higher level locks. */
          if (!device->device_lost_.exchange(true, std::memory_order_relaxed)) {
            CLOG_ERROR(&LOG,
                       "Vulkan: device lost on submission %llu; GPU work is no longer being "
                       "executed.",
                       (unsigned long long)submit_task->timeline);
          }
        }
      }
#ifdef __ANDROID__
      /* Hand the timestamps over for readback. The pairs are only read when the commands were
       * really submitted, otherwise no query is ever written for them. */
      if (timestamps_enabled) {
        const Span<render_graph::VKScopeTimestamp> scopes =
            command_builder.scope_timestamps_get();
        const bool global_valid = timestamp_global_begin != UINT32_MAX;
        if (submit_result == VK_SUCCESS && !device->is_device_lost() &&
            (!scopes.is_empty() || global_valid))
        {
          timestamp_pending.append(GPUProfilePending{});
          GPUProfilePending &pending = timestamp_pending.last();
          pending.timeline = submit_task->timeline;
          if (global_valid) {
            pending.global_begin_query = timestamp_global_begin;
            pending.global_end_query = timestamp_global_begin + 1;
          }
          for (const render_graph::VKScopeTimestamp &scope : scopes) {
            pending.scopes.append(scope);
          }
        }
        command_builder.scope_timestamps_clear();
        timestamp_global_begin = UINT32_MAX;
      }
#endif
      if (submit_task->wait_for_submission != nullptr) {
        std::unique_lock<Mutex> lock(submit_task->wait_for_submission->is_submitted_mutex);
        submit_task->wait_for_submission->is_submitted = true;
        submit_task->wait_for_submission->is_submitted_condition.notify_one();
      }
      command_buffers_in_use.append_timeline(submit_task->timeline, vk_command_buffer);
      vk_command_buffer = VK_NULL_HANDLE;
      command_buffer.reset();
    }

    render_graph.reset();
    BLI_thread_queue_push(device->unused_render_graphs_,
                          std::move(submit_task->render_graph),
                          BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL);
    MEM_delete<VKRenderGraphSubmitTask>(submit_task);
  }
  CLOG_TRACE(&LOG, "Submission runner is being canceled");

  /* Clear command buffers and pool */
  {
    std::scoped_lock lock(*device->queue_mutex_);
    device->functions.vkDeviceWaitIdle(device->vk_device_);
  }
  command_buffers_in_use.remove_old(UINT64_MAX, [&](VkCommandBuffer vk_command_buffer) {
    command_buffers_unused.append(vk_command_buffer);
  });
  device->functions.vkFreeCommandBuffers(device->vk_device_,
                                         vk_command_pool,
                                         command_buffers_unused.size(),
                                         command_buffers_unused.data());
#ifdef __ANDROID__
  if (timestamp_pool != VK_NULL_HANDLE) {
    /* The device is idle here, so every outstanding pair can be read back before teardown. */
    if (!device->is_device_lost()) {
      retire_timestamps(UINT64_MAX);
    }
    device->functions.vkDestroyQueryPool(device->vk_device_, timestamp_pool, nullptr);
  }
#endif
  device->functions.vkDestroyCommandPool(device->vk_device_, vk_command_pool, nullptr);
  CLOG_TRACE(&LOG, "Submission runner finished");
}

void VKDevice::init_submission_thread()
{
  CLOG_TRACE(&LOG, "Create submission thread");
  submission_thread_should_exit_ = false;
  submitted_render_graphs_ = BLI_thread_queue_init();
  unused_render_graphs_ = BLI_thread_queue_init();

  VkSemaphoreTypeCreateInfo vk_semaphore_type_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
  VkSemaphoreCreateInfo vk_semaphore_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &vk_semaphore_type_create_info, 0};
  functions.vkCreateSemaphore(
      vk_device_, &vk_semaphore_create_info, nullptr, &vk_timeline_semaphore_);

  submission_thread_ = std::thread(VKDevice::submission_runner, this);
}

void VKDevice::deinit_submission_thread()
{
  CLOG_TRACE(&LOG, "Stopping submission thread");
  submission_thread_should_exit_ = true;
  CLOG_TRACE(&LOG, "Waiting for completion");
  submission_thread_.join();

  while (!BLI_thread_queue_is_empty(submitted_render_graphs_)) {
    VKRenderGraphSubmitTask *submit_task = static_cast<VKRenderGraphSubmitTask *>(
        BLI_thread_queue_pop(submitted_render_graphs_));
    MEM_delete<VKRenderGraphSubmitTask>(submit_task);
  }
  BLI_thread_queue_free(submitted_render_graphs_);
  submitted_render_graphs_ = nullptr;
  BLI_thread_queue_free(unused_render_graphs_);
  unused_render_graphs_ = nullptr;

  functions.vkDestroySemaphore(vk_device_, vk_timeline_semaphore_, nullptr);
  vk_timeline_semaphore_ = VK_NULL_HANDLE;
}

/** \} */

}  // namespace gpu
}  // namespace blender
