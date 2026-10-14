/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup BLI
 *
 * Runtime switches for the Android port.
 *
 * A hypothesis about behaviour on a phone costs a build to test, and a build
 * is twenty minutes. Six were spent that way on the touch work -- a one finger
 * pan reported from GHOST, a keymap item declared from a script, an ordering
 * problem, a poll that was never consulted -- and the cheap ones were the ones
 * that worked: the scroll fix was proved from a Python script with no rebuild at
 * all. The difference is whether the thing being changed can be reached without
 * recompiling.
 *
 * So any behaviour that might need tuning on real hardware reads its switch
 * from here, and after this a hypothesis is a line in a file plus a restart
 * rather than a build.
 *
 * The file is one `name = value` per line, and can be rewritten while Blender
 * is open:
 *
 *     clickdrag.viewport = 1
 *     log.all = 1
 *
 * Re-read it with the operator `wm.android_knobs_reload`, so no restart is
 * needed either. Values absent take their default.
 *
 * Logging goes through CLOG rather than logcat directly, and it is now wired up
 * at all: CLG_init() is what installs the handler, creator.cc calls it for a
 * desktop build and skips it on Android, so until now every CLOG_DEBUG in the
 * tree had been discarded silently. The categories already exist -- `--debug`
 * and `--debug-<name>` turn them on from the command line, and the click-drag
 * trace in wm_event_system.cc is one of them.
 *
 * It lives in blenlib rather than in GHOST because the window manager cannot
 * include GHOST's private headers, and this has to be reachable from both the
 * window manager and the draw engines.
 */

#pragma once

#ifdef __ANDROID__

#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <map>
#  include <string>

/** Where the switch file lives. The app's own external files directory: writable
 * without a rebuild, and it survives between launches. */
#  define BLI_ANDROID_KNOBS_FILE \
    "/sdcard/Android/data/com.kronos3d.app/files/knobs.txt"

namespace blender_android_knobs {

/** Trim trailing whitespace and the newline a shell-written file arrives with.
 * "1\n" would read as 1 only by luck. */
inline void trim(char *s)
{
  size_t l = strlen(s);
  while (l > 0 && strchr(" \t\r\n", s[l - 1]) != nullptr) {
    s[--l] = '\0';
  }
}

inline void load()
{
  std::map<std::string, std::string> parsed;
  FILE *f = fopen(BLI_ANDROID_KNOBS_FILE, "r");
  if (f == nullptr) {
    table() = parsed;
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), f) != nullptr) {
    char *eq = strchr(line, '=');
    if (eq == nullptr) {
      continue;
    }
    *eq = '\0';
    char *k = line;
    char *v = eq + 1;
    while (*k == ' ' || *k == '\t') {
      k++;
    }
    while (*v == ' ' || *v == '\t') {
      v++;
    }
    trim(k);
    trim(v);
    if (strlen(k) > 0 && strlen(v) > 0) {
      parsed[std::string(k)] = std::string(v);
    }
  }
  fclose(f);
  table() = parsed;
}

}  // namespace blender_android_knobs

/** The switch table. A function-local static so the accessors below need no
 * state and a later include cannot disturb it. */
inline std::map<std::string, std::string> &table()
{
  static std::map<std::string, std::string> t;
  return t;
}

/** Re-read the switch file. Call after editing it; the operator
 * `wm.android_knobs_reload` does. */
inline void BLI_android_knobs_reload()
{
  blender_android_knobs::load();
}

/** Integer switch. */
inline int32_t BLI_android_knob(const char *name, int32_t default_value)
{
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    blender_android_knobs::load();
  }
  auto it = blender_android_knobs::table().find(std::string(name));
  if (it != blender_android_knobs::table().end()) {
    return int32_t(atoi(it->second.c_str()));
  }
  return default_value;
}

/** Boolean switch.
 *
 * Only "0" means off. A stray value -- a trailing space, a word, an edit that
 * did not land -- then leaves the switch on, which is the right way to fail:
 * watching something that was asked for beats a switch that silently turned
 * itself off and left a run with no evidence in it. */
inline bool BLI_android_knob_bool(const char *name, bool default_value)
{
  return BLI_android_knob(name, default_value ? 1 : 0) != 0;
}

/** Every parsed switch, for the reload operator to print. */
inline std::string BLI_android_knobs_dump()
{
  std::string out;
  for (const auto &pair : blender_android_knobs::table()) {
    out += pair.first + "=" + pair.second + "\n";
  }
  return out;
}

#endif /* __ANDROID__ */