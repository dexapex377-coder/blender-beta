/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"
#include "BLI_time.hh"
#ifdef _WIN32
#  include "BLI_winstuff.hh"
#endif

#include "vk_backend.hh"
#include "vk_device.hh"
#include "vk_shader.hh"
#include "vk_shader_compiler.hh"

#include <cstring>
#include <iostream>
#include <string>

#ifdef __ANDROID__
#  include <android/log.h>
#  include <cstdarg>
#  include <cstdlib>
#  include <sys/system_properties.h>
#endif

/** Per-shader compile tracing, so it is off unless `debug.blender.shaderlog` is set.
 *
 * The question this answers is which shaders come off the disk cache and which have to go
 * through shaderc, and how long each of those takes. On a phone the answer decides whether
 * Eevee is merely slow to start or never starts at all, and the two look identical from the
 * outside: both leave "Compiling EEVEE engine shaders" on screen. */
#ifdef __ANDROID__
static void shader_compile_log(const char *format, ...)
{
  static const bool enabled = []() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.blender.shaderlog", value) > 0 && atoi(value) != 0;
  }();
  if (!enabled) {
    return;
  }
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_INFO, "blender-shader", format, args);
  va_end(args);
}
#  define SHADERLOG(...) shader_compile_log(__VA_ARGS__)
#else
#  define SHADERLOG(...) ((void)0)
#endif

#include "CLG_log.h"

#include "spirv/unified1/spirv.h"

#ifdef WITH_GPU_BACKEND_TESTS
#  if 0
#    include "spirv-tools/libspirv.h"
#  endif
#endif

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

static std::optional<std::string> cache_dir_get()
{
  static std::optional<std::string> result = []() -> std::optional<std::string> {
    static char tmp_dir_buffer[FILE_MAX];
    /* Shader builder doesn't return the correct appdir. */
    BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));

    /* kronos3D tags this directory with the Vulkan/SPIR-V target ("vk-spirv-cache-vk13").
     * Not done here: read_spirv_from_disk() below already compares the SPIR-V version stored
     * in each entry's sidecar against spirv_target_version_get() and recompiles on a
     * mismatch, so a stale module is never reused either way. The tag would only leave a
     * second, equally large cache behind on a device that has little disk to spare. */
    std::string cache_dir = std::string(tmp_dir_buffer) + "vk-spirv-cache" + SEP_STR;
    BLI_dir_create_recursive(cache_dir.c_str());
    return cache_dir;
  }();

  return result;
}

/* -------------------------------------------------------------------- */
/** \name SPIR-V disk cache
 * \{ */

struct SPIRVSidecar {
  /** Size of the SPIRV binary. */
  uint64_t spirv_size;
  /** SPIR-V version. */
  uint32_t spirv_version;
};

/* shaderc_spirv_version, not uint32_t: this is handed straight to SetTargetSpirv(). The sidecar
 * stores the result in a uint32_t, where the enum converts implicitly. */
static shaderc_spirv_version spirv_target_version_get()
{
#ifdef __ANDROID__
  /* Ask for SPIR-V 1.3 on Android whatever the device advertises.
   *
   * Every mobile GPU driver here -- Adreno, Mali and PowerVR -- refuses or mishandles a number
   * of modules emitted as SPIR-V 1.4 or later: modules `spirv-val` accepts and every desktop
   * driver builds, but which come back VK_ERROR_UNKNOWN from vkCreateComputePipelines, or carry
   * an OpCopyLogical for a struct copy that the driver rejects at pipeline creation. Those are
   * the modules Eevee uses for shadows and light culling, so there is nothing to switch off
   * instead. An Adreno driver refusing compute pipelines outright was traced back to exactly
   * this, and the workaround was a --gpu-force-workarounds flag that cost 5-18% of the frame.
   *
   * Blender asks for no SPIR-V 1.4 feature, so the older target costs nothing.
   *
   * Note this is a property of the shader compiler, not of the Vulkan instance version: the
   * instance may declare 1.3 (SPIR-V 1.6) and the generator still has to emit 1.3 here.
   *
   * `debug.blender.spirv` overrides it, so a driver that turns out to want something else can
   * be retried without a rebuild: "10".."16", or "default" to go back to 1.3. The cached
   * sidecar records whichever version was used, so changing it recompiles. */
  {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.blender.spirv", value) > 0 && value[0] != '\0') {
      if (strcmp(value, "default") == 0) {
        return shaderc_spirv_version_1_3;
      }
      const int minor = atoi(value);
      switch (minor) {
        case 0:
          return shaderc_spirv_version_1_0;
        case 1:
          return shaderc_spirv_version_1_1;
        case 2:
          return shaderc_spirv_version_1_2;
        case 3:
          return shaderc_spirv_version_1_3;
        case 4:
          return shaderc_spirv_version_1_4;
        case 5:
          return shaderc_spirv_version_1_5;
        case 6:
          return shaderc_spirv_version_1_6;
        default:
          break;
      }
    }
    return shaderc_spirv_version_1_3;
  }
#else
  const VKDevice &device = VKBackend::get().device;
  return device.extensions_get().spirv_1_4 ? shaderc_spirv_version_1_4 : shaderc_spirv_version_1_3;
#endif
}

static bool read_spirv_from_disk(VKShaderModule &shader_module, StringRef hash_extra)
{
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    /* Debug information is not part of the cached SPIR-V, so don't use the cache in this case. */
    return false;
  }
  if (!cache_dir_get().has_value()) {
    return false;
  }
  shader_module.build_sources_hash(hash_extra);
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";

  if (!BLI_exists(spirv_path.c_str()) || !BLI_exists(sidecar_path.c_str())) {
    return false;
  }

  BLI_file_touch(spirv_path.c_str());
  BLI_file_touch(sidecar_path.c_str());

  /* Read sidecar. */
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize sidecar_size_on_disk = sidecar_file.tellg();
  SPIRVSidecar sidecar = {};
  if (sidecar_size_on_disk != sizeof(sidecar)) {
    return false;
  }
  sidecar_file.seekg(0, std::ios::beg);
  sidecar_file.read(reinterpret_cast<char *>(&sidecar), sizeof(sidecar));

  if (sidecar.spirv_version != spirv_target_version_get()) {
    CLOG_TRACE(
        &LOG,
        "Recompiling SPIR-V cache entry for %s. Cached SPIR-V version %08x, but %08x requested.",
        spirv_path.c_str(),
        sidecar.spirv_version,
        spirv_target_version_get());
    return false;
  }

  /* Read spirv binary. */
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize size = spirv_file.tellg();
  if (size != sidecar.spirv_size) {
    return false;
  }
  spirv_file.seekg(0, std::ios::beg);
  shader_module.spirv_binary.resize(size / 4);
  spirv_file.read(reinterpret_cast<char *>(shader_module.spirv_binary.data()), size);

  CLOG_TRACE(&LOG, "reading SpirV from disk %s", spirv_path.c_str());
  return true;
}

static void write_spirv_to_disk(VKShaderModule &shader_module)
{
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    /* Debug information is not part of the cached SPIR-V, so don't use the cache in this case. */
    return;
  }
  if (!cache_dir_get().has_value()) {
    return;
  }

  /* Write the SPIR-V binary. Note that spirv_binary is used instead of compilation_result so
   * that any post-processing (e.g. the injected OpString shader name) is stored in the cache and
   * does not have to be redone every time the cache entry is read back. */
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  CLOG_TRACE(&LOG, "write SpirV to disk %s", spirv_path.c_str());
  size_t size = shader_module.spirv_binary.size() * sizeof(uint32_t);
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::out);
  spirv_file.write(reinterpret_cast<const char *>(shader_module.spirv_binary.data()), size);

  /* Write the sidecar */
  SPIRVSidecar sidecar;
  sidecar.spirv_size = size;
  sidecar.spirv_version = spirv_target_version_get();
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::out);
  sidecar_file.write(reinterpret_cast<const char *>(&sidecar), sizeof(SPIRVSidecar));
}

void VKShaderCompiler::cache_dir_clear_old()
{
  if (!cache_dir_get().has_value()) {
    return;
  }

  direntry *entries = nullptr;
  uint32_t dir_len = BLI_filelist_dir_contents(cache_dir_get()->c_str(), &entries);
  for (int i : IndexRange(dir_len)) {
    direntry entry = entries[i];
    if (S_ISDIR(entry.s.st_mode)) {
      continue;
    }
    const time_t ts_now = time(nullptr);
    const time_t delete_threshold = 60 /*seconds*/ * 60 /*minutes*/ * 24 /*hours*/ * 30 /*days*/;
    if (entry.s.st_mtime + delete_threshold < ts_now) {
      BLI_delete(entry.path, false, false);
    }
  }
  BLI_filelist_free(entries, dir_len);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compilation
 * \{ */

static StringRef to_stage_name(shaderc_shader_kind stage)
{
  switch (stage) {
    case shaderc_vertex_shader:
      return "vertex";
    case shaderc_geometry_shader:
      return "geometry";
    case shaderc_fragment_shader:
      return "fragment";
    case shaderc_compute_shader:
      return "compute";

    default:
      BLI_assert_msg(false, "Do not know how to convert shaderc_shader_kind to stage name.");
      break;
  }
  return "unknown stage";
}

/**
 * Injects an "OpString" holding the shader name into the SPIR-V binary, and references it via an
 * "OpSource" instruction. This is not necessary when G_DEBUG_GPU_SHADER_DEBUG_INFO is used, as
 * then shaderc/glslang will generate this OpString together with an OpSource instruction that
 * contains the full GLSL source code.
 *
 * This way, the human-readable shader name can be found/referenced when inspecting the binary or
 * debugging even when not using the flag G_DEBUG_GPU_SHADER_DEBUG_INFO.
 */
static void spirv_inject_op_string(const Span<uint32_t> &spirv_in,
                                   Vector<uint32_t> &spirv_out,
                                   StringRef name,
                                   uint32_t glsl_version)
{
  struct SpirvHeader {
    uint32_t magic_number;
    uint32_t version;
    uint32_t generator;
    uint32_t bound; /* Indicates the next available result ID. */
    uint32_t schema;
  };
  constexpr uint32_t SPIRV_HEADER_WORD_COUNT = sizeof(SpirvHeader) / sizeof(uint32_t);
  const SpirvHeader *header_in = reinterpret_cast<const SpirvHeader *>(spirv_in.data());
  if (spirv_in.size() < SPIRV_HEADER_WORD_COUNT || header_in->magic_number != SpvMagicNumber) {
    spirv_out.extend(spirv_in); /* pass through unmodified */
    BLI_assert_msg(false, "Invalid SPIR-V header");
    return;
  }

  /* Insertion point = start of the debug section: After all OpCapability / OpExtension /
   * OpExtInstImport / OpMemoryModel / OpEntryPoint / OpExecutionMode instructions. */
  int64_t pos_inject = SPIRV_HEADER_WORD_COUNT;
  while (pos_inject < spirv_in.size()) {
    const uint32_t opcode = spirv_in[pos_inject] & SpvOpCodeMask;
    const uint32_t length = spirv_in[pos_inject] >> SpvWordCountShift;
    if (length == 0) {
      /* Invalid SPIR-V binary. */
      spirv_out.extend(spirv_in); /* pass through unmodified */
      BLI_assert_msg(false, "Invalid SPIR-V code word length");
      return;
    }
    const bool pre_debug = ELEM(opcode,
                                SpvOpCapability,
                                SpvOpExtension,
                                SpvOpExtInstImport,
                                SpvOpMemoryModel,
                                SpvOpEntryPoint,
                                SpvOpExecutionMode,
                                SpvOpExecutionModeId);
    if (!pre_debug) {
      break;
    }
    pos_inject += length;
  }

  /* Literal string: UTF-8, null-terminated, packed 4 bytes/word, zero-padded. */
  const size_t byte_len = name.size() + 1; /* +1 for null terminator. */
  const uint32_t num_string_words = divide_ceil_u(byte_len, 4);

  /* Compute the number of words to inject; OpString (2 + string) + OpSource (4). */
  const uint32_t num_words_inject = 2u + num_string_words + 4u;

  /* Compute the size of the SPIR-V output binary and add the part until the injection point. */
  spirv_out.reserve(spirv_in.size() + num_words_inject);
  spirv_out.extend(spirv_in.take_front(pos_inject));

  /* Inject OpString holding the shader name. */
  const uint32_t op_string_word_count = 2u + num_string_words;
  spirv_out.append((op_string_word_count << SpvWordCountShift) | SpvOpString);
  spirv_out.append(header_in->bound);
  const int64_t string_offset = spirv_out.size();
  spirv_out.resize(spirv_out.size() + num_string_words, 0u);
  memcpy(spirv_out.data() + string_offset, name.data(), name.size());

  /* Inject OpSource referencing the OpString above as the source file so tools can associate the
   * shader name with this module. */
  constexpr uint32_t op_source_word_count = 4u;
  spirv_out.append((op_source_word_count << SpvWordCountShift) | SpvOpSource);
  spirv_out.append(SpvSourceLanguageGLSL);
  spirv_out.append(glsl_version);
  spirv_out.append(header_in->bound);

  /* Add the part from the source binary after the injection position to the output. */
  spirv_out.extend(spirv_in.drop_front(pos_inject));

  /* Raise the result ID bound in the output binary by one due to adding OpString. */
  SpirvHeader *header_out = reinterpret_cast<SpirvHeader *>(spirv_out.data());
  header_out->bound = header_in->bound + 1u;
}

#ifdef WITH_GPU_BACKEND_TESTS
/* Validate the SPIR-V binary using SPIRV-Tools. Only compiled into test builds since validation
 * is expensive. Used to catch breakage of spirv_inject_op_string with future SPIR-V versions.
 * \todo Enable the code below once SPIRV-Tools static library is included in the dependencies. */
#  if 0
static bool spirv_validate(const Span<uint32_t> &spirv, StringRef name)
{
  spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_2);
  spv_diagnostic diagnostic = nullptr;
  const spv_result_t result = spvValidateBinary(context, spirv.data(), spirv.size(), &diagnostic);
  const bool valid = result == SPV_SUCCESS;
  if (!valid) {
    CLOG_ERROR(&LOG,
               "SPIR-V validation failed for %s: %s",
               std::string(name).c_str(),
               (diagnostic && diagnostic->error) ? diagnostic->error : "unknown error");
  }
  spvDiagnosticDestroy(diagnostic);
  spvContextDestroy(context);
  return valid;
}
#  endif
#endif

static std::string patch_line_directives(std::string source)
{
  /* Patch line directives so that we can make error reporting consistent. */
  size_t start_pos = 0;
  while ((start_pos = source.find("#line ", start_pos)) != std::string::npos) {
    source[start_pos] = '/';
    source[start_pos + 1] = '/';
  }
  return source;
}

static bool compile_ex(shaderc::Compiler &compiler,
                       VKShader &shader,
                       shaderc_shader_kind stage,
                       VKShaderModule &shader_module)
{
  std::string full_name = shader.name_get() + "_" + to_stage_name(stage);

  shader_module.original_sources = std::move(shader_module.combined_sources);

  Shader::dump_source_to_disk(
      shader.name_get(), full_name, ".glsl", shader_module.original_sources);

  if (!shader.skip_preprocessor) {
    shader_module.combined_sources = Shader::run_preprocessor(shader_module.original_sources,
                                                              G.debug & G_DEBUG_GPU_SHADER_NO_DCE);

    Shader::dump_source_to_disk(
        shader.name_get(), full_name + ".expanded", ".glsl", shader_module.combined_sources);
  }
  else {
    shader_module.combined_sources = shader_module.original_sources;
  }

  /* The cached SPIR-V binary contains an injected OpString, which depends on the shader name.
   * Include the name in the cache key so an entry is only reused for an identical result. This
   * also invalidates older caches that stored the SPIR-V without the injected OpString. */
  const std::string hash_extra = std::string("op_string:") + full_name;

  if (read_spirv_from_disk(shader_module, hash_extra)) {
    SHADERLOG("CACHE  %-44s spirv=%u", full_name.c_str(), unsigned(spirv_target_version_get()));
    return true;
  }

  const double compile_t0 = BLI_time_now_seconds();

  shaderc::CompileOptions options;
  bool do_optimize = true;
  if (VKBackend::get().device.extensions_get().spirv_1_4) {
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    /* Must come from spirv_target_version_get(), which is where the Android pin to 1.3
     * lives. This used to be a literal shaderc_spirv_version_1_4, so the pin was dead code:
     * it only ever reached the sidecar tag written next to the module, and every device that
     * advertises spirv_1_4 -- which the PowerVR here does -- still got 1.4 modules generated.
     * That is the version Eevee's shadow and light-culling pipelines crash on, and it is why
     * the sidecar and the binary used to disagree about what was in the file. */
    options.SetTargetSpirv(spirv_target_version_get());
  }
  else {
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
  }
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    do_optimize = false;
  }
  /* WORKAROUND: Qualcomm driver can crash when handling optimized SPIR-V. */
  if (GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_ANY, GPU_DRIVER_ANY)) {
    do_optimize = false;
  }
  options.SetOptimizationLevel(do_optimize ? shaderc_optimization_level_performance :
                                             shaderc_optimization_level_zero);

  /* Increase the max id bound.
   *
   * SPIR-V has a default max id bound set to 0x3fffff which is the minimum amount of ids that
   * needs to be supported by any platform. However during optimization the max id bound can
   * increase very fast and lowered at the end. As glslang uses max id bound in their internal
   * structures to allocate arrays out of bound errors can occur.
   *
   * Increasing the max id bound to a larger number to increase the internal arrays of the
   * compiler to work around the compiler crash.
   *
   * NOTE: Test-files in #144614 and #143516 would surpass the default limit during compilation.
   * The final optimized SPIR-V is far less than the default so be fine to be used on platforms
   * with minimum spec.
   *
   * https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.html#_a_id_limits_a_universal_limits
   */
  options.SetMaxIdBound(0xffffff);

  /* Should always be called after setting the optimization level. Setting optimization level
   * resets all previous passes. */
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    options.SetGenerateDebugInfo();
  }

  /* Removes line directive. */
  std::string sources = patch_line_directives(shader_module.combined_sources);

  shader_module.compilation_result = compiler.CompileGlslToSpv(
      sources, stage, full_name.c_str(), options);
  bool compilation_succeeded = shader_module.compilation_result.GetCompilationStatus() ==
                               shaderc_compilation_status_success;
  SHADERLOG("%s %-44s spirv=%u opt=%d %.0f ms",
            compilation_succeeded ? "BUILD " : "FAIL  ",
            full_name.c_str(),
            unsigned(spirv_target_version_get()),
            int(do_optimize),
            (BLI_time_now_seconds() - compile_t0) * 1000.0f);
  if (!compilation_succeeded) {
    /* This is the only place a shader failure is visible without a debugger: Blender reports
     * "Could not compile shader" to the UI and otherwise says nothing about why. */
    const std::string error_message = shader_module.compilation_result.GetErrorMessage();
    if (!error_message.empty()) {
      std::string one_line = error_message;
      for (char &c : one_line) {
        if (c == '\n' || c == '\r') {
          c = ' ';
        }
      }
      SHADERLOG("      error: %s", one_line.c_str());
    }
  }
  if (compilation_succeeded) {
    /* Copy the compiled SPIR-V code into spirv_binary so it can be post-processed.
     * The function VKShaderModule::finalize prefers spirv_binary when it is not empty. */
    const uint32_t *begin = shader_module.compilation_result.begin();
    const uint32_t *end = shader_module.compilation_result.end();
    Span<uint32_t> compilation_result_span(begin, end - begin);
    if ((G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) == 0) {
      const VKDevice &device = VKBackend::get().device;
      const bool stage_use_ray_query = stage != shaderc_geometry_shader &&
                                       shader.use_ray_query_get();
      const uint32_t glsl_version = device.glsl_patch_version_get(stage_use_ray_query);
      spirv_inject_op_string(
          compilation_result_span, shader_module.spirv_binary, full_name, glsl_version);
    }
    else {
      shader_module.spirv_binary = Vector<uint32_t>(compilation_result_span);
    }

#ifdef WITH_GPU_BACKEND_TESTS
    /* TODO(@chrismile): enable the code below once SPIRV-Tools library is available. */
#  if 0
    if (!spirv_validate(shader_module.spirv_binary, full_name)) {
      return false;
    }
#  endif
#endif

    write_spirv_to_disk(shader_module);
  }
  return compilation_succeeded;
}

bool VKShaderCompiler::compile_module(VKShader &shader,
                                      shaderc_shader_kind stage,
                                      VKShaderModule &shader_module)
{
  shaderc::Compiler compiler;
  return compile_ex(compiler, shader, stage, shader_module);
}

/** \} */

}  // namespace blender::gpu
