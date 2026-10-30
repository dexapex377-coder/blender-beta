// vkbench - synthetic fullscreen-pass benchmark for Android (PowerVR)
// dlopen's libvulkan.so at runtime: no NDK Vulkan link needed at build time.
//
// Usage:
//   vkbench --w 1868 --h 882 --n 5 --color r16f --depth d32s8 \
//           --loadop load --storeop store --draw on --iterations 20
//
// Output:
//   RESULT pass=<i> size=WxH color=FMT depth=DFMT load=LOP store=SOP draw=D ms=<X>
//   TOTAL size=WxH ... n=<N> ms=<total> perpass=<avg>
//
// Timestamps: VK_QUERY_TYPE_TIMESTAMP, converted with device timestampPeriod
// (512 ns on moto g56, matching Blender's scope timestamps).
//
// SPIR-V shaders are read from $VK_BENCH_VERT_SPV / $VK_BENCH_FRAG_SPV.
// Compile them on the host with:
//   glslangValidator -V vert.glsl -o vert.spv
//   glslangValidator -V frag.glsl -o frag.spv

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

#define VK_PROC(name) static PFN_##name g_##name = nullptr;
VK_PROC(vkGetInstanceProcAddr)
VK_PROC(vkCreateInstance)
VK_PROC(vkEnumerateInstanceExtensionProperties)
VK_PROC(vkEnumeratePhysicalDevices)
VK_PROC(vkGetPhysicalDeviceProperties)
VK_PROC(vkGetPhysicalDeviceQueueFamilyProperties)
VK_PROC(vkGetPhysicalDeviceMemoryProperties)
VK_PROC(vkCreateDevice)
VK_PROC(vkGetDeviceQueue)
VK_PROC(vkGetDeviceProcAddr)
VK_PROC(vkCreateImage)
VK_PROC(vkGetImageMemoryRequirements)
VK_PROC(vkAllocateMemory)
VK_PROC(vkBindImageMemory)
VK_PROC(vkCreateImageView)
VK_PROC(vkCreateRenderPass)
VK_PROC(vkCreateFramebuffer)
VK_PROC(vkCreateCommandPool)
VK_PROC(vkAllocateCommandBuffers)
VK_PROC(vkBeginCommandBuffer)
VK_PROC(vkEndCommandBuffer)
VK_PROC(vkCreatePipelineLayout)
VK_PROC(vkCreateShaderModule)
VK_PROC(vkCreateGraphicsPipelines)
VK_PROC(vkCreateQueryPool)
VK_PROC(vkCmdResetQueryPool)
VK_PROC(vkCmdWriteTimestamp)
VK_PROC(vkCmdBeginRenderPass)
VK_PROC(vkCmdBindPipeline)
VK_PROC(vkCmdSetViewport)
VK_PROC(vkCmdSetScissor)
VK_PROC(vkCmdDraw)
VK_PROC(vkCmdEndRenderPass)
VK_PROC(vkQueueSubmit)
VK_PROC(vkQueueWaitIdle)
VK_PROC(vkGetQueryPoolResults)
VK_PROC(vkDestroyQueryPool)
VK_PROC(vkDestroyPipeline)
VK_PROC(vkDestroyPipelineLayout)
VK_PROC(vkDestroyShaderModule)
VK_PROC(vkDestroyFramebuffer)
VK_PROC(vkDestroyRenderPass)
VK_PROC(vkDestroyImageView)
VK_PROC(vkDestroyImage)
VK_PROC(vkFreeMemory)
VK_PROC(vkDestroyCommandPool)
VK_PROC(vkDestroyDevice)
VK_PROC(vkDestroyInstance)

static void *libvk = nullptr;
#define LOAD_LIB(sym) \
  do { g_##sym = (PFN_##sym)dlsym(libvk, #sym); if (!g_##sym) { fprintf(stderr, "missing symbol %s\n", #sym); return 1; } } while (0)
#define LOAD_DEV(sym, dev) \
  do { g_##sym = (PFN_##sym)g_vkGetDeviceProcAddr(dev, #sym); if (!g_##sym) { fprintf(stderr, "missing device symbol %s\n", #sym); return 1; } } while (0)

static VkFormat fmt_color(const char *s) {
  if (!strcmp(s, "r16f")) return VK_FORMAT_R16G16B16A16_SFLOAT;
  if (!strcmp(s, "r8"))   return VK_FORMAT_R8G8B8A8_UNORM;
  if (!strcmp(s, "r8s"))  return VK_FORMAT_R8G8B8A8_SRGB;
  fprintf(stderr, "unknown color format %s\n", s); exit(2);
}
static VkFormat fmt_depth(const char *s) {
  if (!strcmp(s, "d32s8")) return VK_FORMAT_D32_SFLOAT_S8_UINT;
  if (!strcmp(s, "d24s8")) return VK_FORMAT_D24_UNORM_S8_UINT;
  if (!strcmp(s, "d16"))   return VK_FORMAT_D16_UNORM;
  fprintf(stderr, "unknown depth format %s\n", s); exit(2);
}
static VkAttachmentLoadOp loadop(const char *s) {
  if (!strcmp(s, "load")) return VK_ATTACHMENT_LOAD_OP_LOAD;
  if (!strcmp(s, "clear")) return VK_ATTACHMENT_LOAD_OP_CLEAR;
  if (!strcmp(s, "dontcare")) return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  fprintf(stderr, "unknown loadop %s\n", s); exit(2);
}
static VkAttachmentStoreOp storeop(const char *s) {
  if (!strcmp(s, "store")) return VK_ATTACHMENT_STORE_OP_STORE;
  if (!strcmp(s, "dontcare")) return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  fprintf(stderr, "unknown storeop %s\n", s); exit(2);
}

static std::vector<uint32_t> read_spv(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<uint32_t> out(sz / 4);
  fread(out.data(), 4, out.size(), f);
  fclose(f);
  return out;
}

int main(int argc, char **argv) {
  int w = 1868, h = 882, n = 5, iterations = 1;
  const char *color_s = "r16f", *depth_s = "d32s8";
  const char *load_s = "load", *store_s = "store", *draw_s = "on";

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--w")) w = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--h")) h = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--n")) n = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--color")) color_s = argv[++i];
    else if (!strcmp(argv[i], "--depth")) depth_s = argv[++i];
    else if (!strcmp(argv[i], "--loadop")) load_s = argv[++i];
    else if (!strcmp(argv[i], "--storeop")) store_s = argv[++i];
    else if (!strcmp(argv[i], "--draw")) draw_s = argv[++i];
    else if (!strcmp(argv[i], "--iterations")) iterations = atoi(argv[++i]);
    else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
  }

  const char *vertPath = getenv("VK_BENCH_VERT_SPV");
  const char *fragPath = getenv("VK_BENCH_FRAG_SPV");
  if (!vertPath || !fragPath) {
    fprintf(stderr, "set VK_BENCH_VERT_SPV and VK_BENCH_FRAG_SPV to compiled SPIR-V files\n");
    return 2;
  }

  libvk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!libvk) {
    const char *paths[] = {
      "/vendor/lib64/libvulkan.so", "/system/lib64/libvulkan.so",
      "/system/lib/vulkan/libvulkan.so", "/system/lib64/vulkan/libvulkan.so", nullptr};
    for (int k = 0; paths[k]; k++) {
      libvk = dlopen(paths[k], RTLD_NOW | RTLD_LOCAL);
      if (libvk) break;
    }
  }
  if (!libvk) { fprintf(stderr, "cannot dlopen libvulkan.so\n"); return 1; }

  LOAD_LIB(vkGetInstanceProcAddr);
  g_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(libvk, "vkGetInstanceProcAddr");
  g_vkCreateInstance = (PFN_vkCreateInstance)g_vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
  if (!g_vkCreateInstance) { fprintf(stderr, "no vkCreateInstance\n"); return 1; }
  fprintf(stderr, "step: loader ok\n");

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "vkbench";
  appInfo.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &appInfo;
  VkInstance instance = VK_NULL_HANDLE;
  if (g_vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance failed\n"); return 1; }
  fprintf(stderr, "step: instance ok\n");

  /* Instance-level entry points must be fetched with the instance, not NULL. */
  g_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)g_vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
  g_vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)g_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
  g_vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  g_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)g_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");
  g_vkCreateDevice = (PFN_vkCreateDevice)g_vkGetInstanceProcAddr(instance, "vkCreateDevice");
  g_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)g_vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
  if (!g_vkEnumeratePhysicalDevices || !g_vkGetPhysicalDeviceProperties ||
      !g_vkGetPhysicalDeviceQueueFamilyProperties || !g_vkGetPhysicalDeviceMemoryProperties ||
      !g_vkCreateDevice || !g_vkGetDeviceProcAddr) {
    fprintf(stderr, "missing instance entry point\n"); return 1;
  }
  fprintf(stderr, "step: instance procs ok\n");

  uint32_t gpuCount = 0;
  g_vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
  std::vector<VkPhysicalDevice> gpus(gpuCount);
  g_vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
  VkPhysicalDevice chosen = gpus[0];
  VkPhysicalDeviceProperties props{};
  g_vkGetPhysicalDeviceProperties(chosen, &props);
  for (uint32_t i = 0; i < gpuCount; i++) {
    VkPhysicalDeviceProperties p{}; g_vkGetPhysicalDeviceProperties(gpus[i], &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { chosen = gpus[i]; props = p; break; }
  }
  fprintf(stderr, "GPU: %s type=%d timestampPeriod=%.1f\n", props.deviceName, (int)props.deviceType, (double)props.limits.timestampPeriod);
  double tsPeriodMs = props.limits.timestampPeriod / 1e6;

  uint32_t qfCount = 0;
  g_vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfCount);
  g_vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qfCount, qfs.data());
  uint32_t qf = VK_QUEUE_FAMILY_IGNORED;
  for (uint32_t i = 0; i < qfCount; i++)
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
  if (qf == VK_QUEUE_FAMILY_IGNORED) { fprintf(stderr, "no graphics queue\n"); return 1; }

  float qPriority = 1.0f;
  VkDeviceQueueCreateInfo dqci{}; dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  dqci.queueFamilyIndex = qf; dqci.queueCount = 1; dqci.pQueuePriorities = &qPriority;
  VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dqci;
  VkDevice device = VK_NULL_HANDLE;
  if (g_vkCreateDevice(chosen, &dci, nullptr, &device) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); return 1; }
  fprintf(stderr, "step: device ok\n");

  LOAD_DEV(vkGetDeviceQueue, device);
  LOAD_DEV(vkCreateImage, device);
  LOAD_DEV(vkGetImageMemoryRequirements, device);
  LOAD_DEV(vkAllocateMemory, device);
  LOAD_DEV(vkBindImageMemory, device);
  LOAD_DEV(vkCreateImageView, device);
  LOAD_DEV(vkCreateRenderPass, device);
  LOAD_DEV(vkCreateFramebuffer, device);
  LOAD_DEV(vkCreateCommandPool, device);
  LOAD_DEV(vkAllocateCommandBuffers, device);
  LOAD_DEV(vkBeginCommandBuffer, device);
  LOAD_DEV(vkEndCommandBuffer, device);
  LOAD_DEV(vkCreatePipelineLayout, device);
  LOAD_DEV(vkCreateShaderModule, device);
  LOAD_DEV(vkCreateGraphicsPipelines, device);
  LOAD_DEV(vkCreateQueryPool, device);
  LOAD_DEV(vkCmdResetQueryPool, device);
  LOAD_DEV(vkCmdWriteTimestamp, device);
  LOAD_DEV(vkCmdBeginRenderPass, device);
  LOAD_DEV(vkCmdBindPipeline, device);
  LOAD_DEV(vkCmdSetViewport, device);
  LOAD_DEV(vkCmdSetScissor, device);
  LOAD_DEV(vkCmdDraw, device);
  LOAD_DEV(vkCmdEndRenderPass, device);
  LOAD_DEV(vkQueueSubmit, device);
  LOAD_DEV(vkQueueWaitIdle, device);
  LOAD_DEV(vkGetQueryPoolResults, device);
  LOAD_DEV(vkDestroyQueryPool, device);
  LOAD_DEV(vkDestroyPipeline, device);
  LOAD_DEV(vkDestroyPipelineLayout, device);
  LOAD_DEV(vkDestroyShaderModule, device);
  LOAD_DEV(vkDestroyFramebuffer, device);
  LOAD_DEV(vkDestroyRenderPass, device);
  LOAD_DEV(vkDestroyImageView, device);
  LOAD_DEV(vkDestroyImage, device);
  LOAD_DEV(vkFreeMemory, device);
  LOAD_DEV(vkDestroyCommandPool, device);
  LOAD_DEV(vkDestroyDevice, device);

  VkQueue queue;
  g_vkGetDeviceQueue(device, qf, 0, &queue);
  fprintf(stderr, "step: device procs ok\n");

  VkFormat colorFmt = fmt_color(color_s);
  VkFormat depthFmt = fmt_depth(depth_s);
  VkAttachmentLoadOp lo = loadop(load_s);
  VkAttachmentStoreOp so = storeop(store_s);
  bool doDraw = !strcmp(draw_s, "on");

  auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage &img, VkDeviceMemory &mem) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = usage;
    if (g_vkCreateImage(device, &ici, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; g_vkGetImageMemoryRequirements(device, img, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    g_vkGetPhysicalDeviceMemoryProperties(chosen, &mp);
    uint32_t memType = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((mr.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { memType = i; break; }
    }
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = memType;
    if (g_vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    g_vkBindImageMemory(device, img, mem, 0);
    return true;
  };
  auto make_view = [&](VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView &view) {
    VkImageViewCreateInfo ivci{}; ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = img; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = fmt;
    ivci.subresourceRange = { aspect, 0, 1, 0, 1 };
    return g_vkCreateImageView(device, &ivci, nullptr, &view) == VK_SUCCESS;
  };

  VkImage colorImg, depthImg; VkDeviceMemory colorMem, depthMem;
  VkImageView colorView, depthView;
  if (!make_image(colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, colorImg, colorMem)) { fprintf(stderr, "color image fail\n"); return 1; }
  if (!make_image(depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImg, depthMem)) { fprintf(stderr, "depth image fail\n"); return 1; }
  if (!make_view(colorImg, colorFmt, VK_IMAGE_ASPECT_COLOR_BIT, colorView)) { fprintf(stderr, "color view fail\n"); return 1; }
  if (!make_view(depthImg, depthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, depthView)) { fprintf(stderr, "depth view fail\n"); return 1; }

  VkAttachmentDescription atts[2] = {};
  atts[0].format = colorFmt; atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[0].loadOp = lo; atts[0].storeOp = so;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  atts[1].format = depthFmt; atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{}; colorRef.attachment = 0; colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference depthRef{}; depthRef.attachment = 1; depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;
  VkRenderPassCreateInfo rpci{}; rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 2; rpci.pAttachments = atts;
  rpci.subpassCount = 1; rpci.pSubpasses = &subpass;
  VkRenderPass renderPass;
  if (g_vkCreateRenderPass(device, &rpci, nullptr, &renderPass) != VK_SUCCESS) { fprintf(stderr, "renderpass fail\n"); return 1; }

  VkImageView attachments[2] = { colorView, depthView };
  VkFramebufferCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = renderPass; fci.attachmentCount = 2; fci.pAttachments = attachments;
  fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
  VkFramebuffer fb;
  if (g_vkCreateFramebuffer(device, &fci, nullptr, &fb) != VK_SUCCESS) { fprintf(stderr, "framebuffer fail\n"); return 1; }

  // shaders
  auto vertSpv = read_spv(vertPath);
  auto fragSpv = read_spv(fragPath);
  auto makeSM = [&](const std::vector<uint32_t> &spv, VkShaderStageFlagBits stage) {
    VkShaderModuleCreateInfo smci{}; smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.size() * 4; smci.pCode = spv.data();
    VkShaderModule sm; g_vkCreateShaderModule(device, &smci, nullptr, &sm); return sm;
  };
  VkShaderModule vertSM = makeSM(vertSpv, VK_SHADER_STAGE_VERTEX_BIT);
  VkShaderModule fragSM = makeSM(fragSpv, VK_SHADER_STAGE_FRAGMENT_BIT);
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertSM; stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragSM; stages[1].pName = "main";
  VkPipelineLayoutCreateInfo plci{}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout; g_vkCreatePipelineLayout(device, &plci, nullptr, &layout);

  VkViewport viewport{}; viewport.width = (float)w; viewport.height = (float)h; viewport.maxDepth = 1.0f;
  VkRect2D scissor{}; scissor.extent = { (uint32_t)w, (uint32_t)h };
  VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.pViewports = &viewport; vp.scissorCount = 1; vp.pScissors = &scissor;
  VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
  VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{}; blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; blend.attachmentCount = 1; blend.pAttachments = &cba;
  VkGraphicsPipelineCreateInfo gpci{}; gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2; gpci.pStages = stages; gpci.pViewportState = &vp; gpci.pMultisampleState = &ms;
  gpci.pDepthStencilState = &ds; gpci.pRasterizationState = &rs; gpci.pColorBlendState = &blend;
  gpci.layout = layout; gpci.renderPass = renderPass; gpci.subpass = 0;
  VkPipeline pipeline;
  if (g_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS) { fprintf(stderr, "pipeline fail\n"); return 1; }
  fprintf(stderr, "step: pipeline ok\n");

  VkCommandPoolCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpci.queueFamilyIndex = qf;
  VkCommandPool pool; g_vkCreateCommandPool(device, &cpci, nullptr, &pool);
  VkCommandBufferAllocateInfo cbai{}; cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
  VkCommandBuffer cb; g_vkAllocateCommandBuffers(device, &cbai, &cb);

  int nTs = n * 2 + 2;
  VkQueryPoolCreateInfo qpci{}; qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = nTs;
  VkQueryPool qp; g_vkCreateQueryPool(device, &qpci, nullptr, &qp);

  std::vector<double> passMs(n);
  auto run_once = [&]() {
    g_vkCmdResetQueryPool(cb, qp, 0, nTs);
    VkCommandBufferBeginInfo cbbi{}; cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vkBeginCommandBuffer(cb, &cbbi);
    g_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
    int idx = 1;
    for (int i = 0; i < n; i++) {
      g_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, idx++);
      VkRenderPassBeginInfo rpbi{}; rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rpbi.renderPass = renderPass; rpbi.framebuffer = fb;
      rpbi.renderArea = { {0, 0}, {(uint32_t)w, (uint32_t)h} };
      VkClearValue clears[2] = {};
      clears[0].color = { {0.0f, 0.0f, 0.0f, 0.0f} }; clears[1].depthStencil = { 1.0f, 0 };
      rpbi.clearValueCount = 2; rpbi.pClearValues = clears;
      g_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      if (doDraw) {
        g_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_vkCmdSetViewport(cb, 0, 1, &viewport);
        g_vkCmdSetScissor(cb, 0, 1, &scissor);
        g_vkCmdDraw(cb, 3, 1, 0, 0);
      }
      g_vkCmdEndRenderPass(cb);
      g_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, idx++);
    }
    g_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, idx);
    g_vkEndCommandBuffer(cb);

    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    g_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    g_vkQueueWaitIdle(queue);

    std::vector<uint64_t> ts(nTs);
    g_vkGetQueryPoolResults(device, qp, 0, nTs, nTs * sizeof(uint64_t), ts.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    double total = (ts[idx] - ts[0]) * tsPeriodMs;
    for (int i = 0; i < n; i++) passMs[i] = (ts[2 * i + 2] - ts[2 * i + 1]) * tsPeriodMs;
    return total;
  };

  double bestTotal = 1e18;
  for (int it = 0; it < iterations; it++) {
    double total = run_once();
    if (total < bestTotal) bestTotal = total;
  }

  printf("RESULT size=%dx%d color=%s depth=%s load=%s store=%s draw=%s n=%d\n",
         w, h, color_s, depth_s, load_s, store_s, draw_s, n);
  for (int i = 0; i < n; i++) {
    printf("RESULT pass=%d size=%dx%d color=%s depth=%s load=%s store=%s draw=%s ms=%.3f\n",
           i, w, h, color_s, depth_s, load_s, store_s, draw_s, passMs[i]);
  }
  printf("TOTAL size=%dx%d color=%s depth=%s load=%s store=%s draw=%s n=%d mpix=%.3f ms=%.3f perpass=%.3f ms_per_mpix=%.3f\n",
         w, h, color_s, depth_s, load_s, store_s, draw_s, n, w * h / 1e6, bestTotal, bestTotal / n,
         bestTotal / (n * w * h / 1e6));

  g_vkDestroyPipeline(device, pipeline, nullptr);
  g_vkDestroyPipelineLayout(device, layout, nullptr);
  g_vkDestroyShaderModule(device, fragSM, nullptr);
  g_vkDestroyShaderModule(device, vertSM, nullptr);
  g_vkDestroyFramebuffer(device, fb, nullptr);
  g_vkDestroyRenderPass(device, renderPass, nullptr);
  g_vkDestroyImageView(device, depthView, nullptr);
  g_vkDestroyImageView(device, colorView, nullptr);
  g_vkDestroyImage(device, depthImg, nullptr);
  g_vkFreeMemory(device, depthMem, nullptr);
  g_vkDestroyImage(device, colorImg, nullptr);
  g_vkFreeMemory(device, colorMem, nullptr);
  g_vkDestroyQueryPool(device, qp, nullptr);
  g_vkDestroyCommandPool(device, pool, nullptr);
  g_vkDestroyDevice(device, nullptr);
  g_vkDestroyInstance(instance, nullptr);
  dlclose(libvk);
  return 0;
}
