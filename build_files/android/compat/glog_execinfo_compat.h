/* Android shim for glog's stack trace.
 *
 * glog picks a stack trace implementation in extern/glog/src/utilities.h. On x86 it lands
 * on stacktrace_x86_64-inl.h, which uses _Unwind_Backtrace. On ARM none of the branches
 * match, so it falls through to stacktrace_generic-inl.h, which calls backtrace() and
 * backtrace_symbols() from glibc's <execinfo.h>. Bionic has neither, and glog ships no
 * config_android.h, so extern/glog falls back to config_linux.h and the build stops with
 * "use of undeclared identifier 'backtrace'".
 *
 * These stubs are the minimum needed to compile. The trace comes back empty, which is a
 * downgrade from desktop but not a crash, and it is the same behaviour glog already has
 * wherever stack traces are unsupported. Making it real would mean routing
 * _Unwind_Backtrace through a non-lambda callback, since this header is force-included
 * into C translation units as well.
 *
 * Force-included the same way as atomic_ref_compat.hpp, from platform_android.cmake.
 */

#if defined(__ANDROID__)

/* glog includes <execinfo.h> in several places. Bionic ships the header without the
 * declarations, so these two keep the includes valid without pulling in glibc's. */
static inline int backtrace(void **buffer, int size)
{
  if (buffer != nullptr && size > 0) {
    buffer[0] = nullptr;
  }
  return 0;
}

static inline char **backtrace_symbols(void *const *buffer, int size)
{
  (void)buffer;
  (void)size;
  return nullptr;
}

static inline void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
  (void)buffer;
  (void)size;
  (void)fd;
}

#endif /* __ANDROID__ */
