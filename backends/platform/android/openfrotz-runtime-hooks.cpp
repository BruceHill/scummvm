/*
 * OpenFrotz runtime hooks for ScummVM Glk.
 */

#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <time.h>

#include "engines/glk/openfrotz_runtime_hooks.h"
#include "common/array.h"
#include "common/events.h"
#include "common/keyboard.h"
#include "common/system.h"

namespace Glk {
namespace OpenFrotzRuntimeHooks {
namespace {
constexpr const char *kTag = "OpenFrotzHooks";
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
bool g_active = false;
Common::String g_storyPath;
Common::Array<Common::String> g_commands;
Common::String g_transcript;
uint64 g_revision = 0;
uint64 g_dequeueRevision = 0;
uint64 g_outputRevision = 0;
bool g_charRequestPending = false;
bool g_lineRequestPending = false;
constexpr size_t kMaxTranscriptBytes = 256 * 1024;
constexpr uint32 kPollSleepMs = 10;

void lockState() {
  while (g_lock.test_and_set(std::memory_order_acquire)) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000L * 1000L;
    nanosleep(&ts, nullptr);
  }
}

void unlockState() {
  g_lock.clear(std::memory_order_release);
}

void bumpRevisionLocked() {
  g_revision++;
}

void appendRawLocked(const Common::String &text) {
  if (text.empty()) {
    return;
  }
  g_transcript += text;
  if (g_transcript.size() > kMaxTranscriptBytes) {
    const size_t drop = g_transcript.size() - kMaxTranscriptBytes;
    g_transcript = Common::String(g_transcript.c_str() + drop);
  }
  bumpRevisionLocked();
}

void bumpOutputRevisionLocked() {
  g_outputRevision++;
}
} // namespace

void startSession(const Common::String &storyPath) {
  lockState();
  g_active = true;
  g_storyPath = storyPath;
  g_commands.clear();
  g_transcript.clear();
  appendRawLocked("ScummVM OpenFrotz runtime hooks active.\n");
  appendRawLocked("Story: " + storyPath + "\n");
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "startSession story=%s active=%d",
    storyPath.c_str(),
    g_active ? 1 : 0);
  unlockState();
}

void endSession() {
  lockState();
  g_active = false;
  g_storyPath.clear();
  g_commands.clear();
  g_transcript.clear();
  g_dequeueRevision = 0;
  g_outputRevision = 0;
  g_charRequestPending = false;
  g_lineRequestPending = false;
  bumpRevisionLocked();
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "endSession active=%d",
    g_active ? 1 : 0);
  unlockState();
}

bool isActive() {
  lockState();
  const bool active = g_active;
  unlockState();
  return active;
}

void enqueueCommand(const Common::String &command) {
  lockState();
  if (!g_active) {
    unlockState();
    return;
  }
  g_commands.push_back(command);
  bumpRevisionLocked();
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "enqueue command=%s queue=%zu rev=%llu",
    command.c_str(),
    static_cast<size_t>(g_commands.size()),
    static_cast<unsigned long long>(g_revision));
  unlockState();
}

bool dequeueCommand(Common::String &command) {
  lockState();
  const bool active = g_active;
  const size_t queueSize = static_cast<size_t>(g_commands.size());
  if (!active || queueSize == 0) {
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "dequeue no-command active=%d queue=%zu",
      active ? 1 : 0,
      queueSize);
    unlockState();
    return false;
  }
  command = g_commands[0];
  g_commands.remove_at(0);
  g_dequeueRevision++;
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "dequeue command=%s queue=%zu deqRev=%llu",
    command.c_str(),
    static_cast<size_t>(g_commands.size()),
    static_cast<unsigned long long>(g_dequeueRevision));
  unlockState();
  return true;
}

bool hasQueuedCommand() {
  lockState();
  const bool active = g_active;
  const size_t queueSize = static_cast<size_t>(g_commands.size());
  const bool hasQueued = active && queueSize > 0;
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "hasQueuedCommand active=%d queue=%zu result=%d",
    active ? 1 : 0,
    queueSize,
    hasQueued ? 1 : 0);
  unlockState();
  return hasQueued;
}

void noteLineRequest(const char *kind, uint maxlen, uint initlen) {
  lockState();
  g_lineRequestPending = true;
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "line-request kind=%s active=%d queue=%zu maxlen=%u initlen=%u",
    kind ? kind : "unknown",
    g_active ? 1 : 0,
    static_cast<size_t>(g_commands.size()),
    maxlen,
    initlen);
  unlockState();
}

void noteInputEvent(const char *event, uint32 a0, uint32 a1, uint32 a2) {
  lockState();
  if (event) {
    if (std::strcmp(event, "requestCharEvent") == 0 || std::strcmp(event, "requestCharEventUni") == 0 ||
        std::strcmp(event, "requestCharEventAutoAdvance") == 0 ||
        std::strcmp(event, "requestCharEventUniAutoAdvance") == 0) {
      g_charRequestPending = true;
    } else if (std::strcmp(event, "acceptReadChar") == 0) {
      g_charRequestPending = false;
    } else if (std::strcmp(event, "acceptReadLine") == 0 || std::strcmp(event, "acceptLine") == 0 ||
               std::strcmp(event, "cancelLineEvent") == 0) {
      g_lineRequestPending = false;
    }
  }
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "input-event event=%s active=%d queue=%zu a0=%u a1=%u a2=%u",
    event ? event : "unknown",
    g_active ? 1 : 0,
    static_cast<size_t>(g_commands.size()),
    static_cast<unsigned int>(a0),
    static_cast<unsigned int>(a1),
    static_cast<unsigned int>(a2));
  unlockState();
}

bool nudgeInputPump() {
  lockState();
  const bool active = g_active;
  const size_t queueSize = static_cast<size_t>(g_commands.size());
  const bool charPending = g_charRequestPending;
  const bool linePending = g_lineRequestPending;
  unlockState();
  if (!active || queueSize == 0 || !g_system || !g_system->getEventManager()) {
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "nudgeInputPump skipped active=%d queue=%zu eventManager=%d",
      active ? 1 : 0,
      queueSize,
      (g_system && g_system->getEventManager()) ? 1 : 0);
    return false;
  }

  if (charPending || linePending) {
    Common::Event down;
    down.type = Common::EVENT_KEYDOWN;
    down.kbd.keycode = Common::KEYCODE_RETURN;
    down.kbd.ascii = '\r';
    down.kbd.flags = 0;

    Common::Event up;
    up.type = Common::EVENT_KEYUP;
    up.kbd.keycode = Common::KEYCODE_RETURN;
    up.kbd.ascii = '\r';
    up.kbd.flags = 0;

    g_system->getEventManager()->pushEvent(down);
    g_system->getEventManager()->pushEvent(up);
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "nudgeInputPump posted return key events queue=%zu charPending=%d linePending=%d",
      queueSize,
      charPending ? 1 : 0,
      linePending ? 1 : 0);
  } else {
    Common::Event wake;
    wake.type = Common::EVENT_MOUSEMOVE;
    wake.mouse.x = 0;
    wake.mouse.y = 0;
    g_system->getEventManager()->pushEvent(wake);
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "nudgeInputPump posted wake event queue=%zu charPending=%d linePending=%d",
      queueSize,
      charPending ? 1 : 0,
      linePending ? 1 : 0);
  }
  return true;
}

void appendOutput(const char *buf, uint len) {
  if (!buf || len == 0) {
    return;
  }
  lockState();
  if (!g_active) {
    unlockState();
    return;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "appendOutput len=%u", len);
  appendRawLocked(Common::String(buf, buf + len));
  bumpOutputRevisionLocked();
  unlockState();
}

void appendOutputUni(const uint32 *buf, uint len) {
  if (!buf || len == 0) {
    return;
  }
  Common::String ascii;
  for (uint i = 0; i < len; i++) {
    const uint32 ch = buf[i];
    if (ch >= 32 && ch <= 126) {
      ascii += static_cast<char>(ch);
    } else if (ch == '\n' || ch == '\r' || ch == '\t') {
      ascii += static_cast<char>(ch);
    } else {
      ascii += '?';
    }
  }
  lockState();
  if (!g_active) {
    unlockState();
    return;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "appendOutputUni len=%u", len);
  appendRawLocked(ascii);
  bumpOutputRevisionLocked();
  unlockState();
}

void appendInputEcho(const Common::String &line) {
  lockState();
  if (!g_active) {
    unlockState();
    return;
  }
  appendRawLocked("> " + line + "\n");
  unlockState();
}

void appendSystemMessage(const Common::String &line) {
  lockState();
  if (!g_active) {
    unlockState();
    return;
  }
  appendRawLocked(line + "\n");
  unlockState();
}

Common::String waitForTranscriptSince(uint64 afterRevision, uint32 timeoutMs, uint64 *outRevision) {
  lockState();
  if (!g_active) {
    if (outRevision) {
      *outRevision = g_revision;
    }
    unlockState();
    return "";
  }
  uint32 waitedMs = 0;
  while (g_active && g_revision <= afterRevision && waitedMs < timeoutMs) {
    unlockState();
    // Avoid calling into g_system from binder/runtime bridge threads.
    // g_system lifecycle can race with runtime bootstrap/teardown and has
    // previously triggered lock-on-destroyed-mutex aborts.
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<long>(kPollSleepMs) * 1000L * 1000L;
    nanosleep(&ts, nullptr);
    waitedMs += kPollSleepMs;
    lockState();
  }
  if (outRevision) {
    *outRevision = g_revision;
  }
  if (g_revision > afterRevision) {
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "waitForTranscriptSince hit rev=%llu after=%llu bytes=%zu",
      static_cast<unsigned long long>(g_revision),
      static_cast<unsigned long long>(afterRevision),
      static_cast<size_t>(g_transcript.size()));
  } else {
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "waitForTranscriptSince timeout after=%llu",
      static_cast<unsigned long long>(afterRevision));
  }
  const Common::String snapshot = g_transcript;
  unlockState();
  return snapshot;
}

bool waitForDequeueSince(uint64 afterDequeueRevision, uint32 timeoutMs, uint64 *outDequeueRevision) {
  lockState();
  if (!g_active) {
    if (outDequeueRevision) {
      *outDequeueRevision = g_dequeueRevision;
    }
    unlockState();
    return false;
  }
  uint32 waitedMs = 0;
  while (g_active && g_dequeueRevision <= afterDequeueRevision && waitedMs < timeoutMs) {
    unlockState();
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<long>(kPollSleepMs) * 1000L * 1000L;
    nanosleep(&ts, nullptr);
    waitedMs += kPollSleepMs;
    lockState();
  }
  if (outDequeueRevision) {
    *outDequeueRevision = g_dequeueRevision;
  }
  if (g_dequeueRevision > afterDequeueRevision) {
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "waitForDequeueSince hit deqRev=%llu after=%llu queue=%zu",
      static_cast<unsigned long long>(g_dequeueRevision),
      static_cast<unsigned long long>(afterDequeueRevision),
      static_cast<size_t>(g_commands.size()));
    unlockState();
    return true;
  }
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "waitForDequeueSince timeout after=%llu",
    static_cast<unsigned long long>(afterDequeueRevision));
  unlockState();
  return false;
}

Common::String waitForOutputSince(uint64 afterOutputRevision, uint32 timeoutMs, uint64 *outOutputRevision) {
  lockState();
  if (!g_active) {
    if (outOutputRevision) {
      *outOutputRevision = g_outputRevision;
    }
    unlockState();
    return "";
  }
  uint32 waitedMs = 0;
  while (g_active && g_outputRevision <= afterOutputRevision && waitedMs < timeoutMs) {
    unlockState();
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<long>(kPollSleepMs) * 1000L * 1000L;
    nanosleep(&ts, nullptr);
    waitedMs += kPollSleepMs;
    lockState();
  }
  if (outOutputRevision) {
    *outOutputRevision = g_outputRevision;
  }
  if (g_outputRevision > afterOutputRevision) {
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "waitForOutputSince hit outRev=%llu after=%llu bytes=%zu",
      static_cast<unsigned long long>(g_outputRevision),
      static_cast<unsigned long long>(afterOutputRevision),
      static_cast<size_t>(g_transcript.size()));
    Common::String out = g_transcript;
    unlockState();
    return out;
  }
  __android_log_print(
    ANDROID_LOG_INFO,
    kTag,
    "waitForOutputSince timeout after=%llu",
    static_cast<unsigned long long>(afterOutputRevision));
  unlockState();
  return "";
}

Common::String getTranscriptSnapshot(uint64 *outRevision) {
  lockState();
  if (outRevision) {
    *outRevision = g_revision;
  }
  const Common::String snapshot = g_transcript;
  unlockState();
  return snapshot;
}

uint64 getDequeueRevision() {
  lockState();
  const uint64 rev = g_dequeueRevision;
  unlockState();
  return rev;
}

uint64 getOutputRevision() {
  lockState();
  const uint64 rev = g_outputRevision;
  unlockState();
  return rev;
}

} // namespace OpenFrotzRuntimeHooks
} // namespace Glk
