/*
 * OpenFrotz runtime bridge shim for Android ScummVM builds.
 */

#include <cstdlib>
#include <cstring>
#include <atomic>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <time.h>
#include <unordered_map>
#include <android/log.h>

#include "base/main.h"
#include "backends/platform/android/jni-android.h"
#include "common/system.h"
#include "common/str.h"
#include "engines/glk/openfrotz_runtime_hooks.h"

namespace {
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
Common::String g_story;
std::unordered_map<std::string, Common::String> g_slotSnapshots;
uint64 g_lastRevision = 0;
std::thread g_runnerThread;
bool g_runnerStarted = false;
constexpr const char *kTag = "OpenFrotzRuntime";
constexpr const char *kRuntimeBuildMarker = "openfrotz-runtime-2026-02-14-v53-export-embedded-runner";
constexpr bool kAllowScummVmMainFallback = false;
constexpr bool kEnableDetachedEmbeddedRunner = false;
using EmbeddedRunnerMainFn = int (*)(int, const char *[]);
EmbeddedRunnerMainFn g_embeddedRunnerMain = nullptr;
std::string g_embeddedRunnerSymbol;

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

struct StateLock {
  StateLock() { lockState(); }
  ~StateLock() { unlockState(); }
};

const char *dupCString(const std::string &value) {
  char *ptr = static_cast<char *>(std::malloc(value.size() + 1));
  if (!ptr) {
    return nullptr;
  }
  std::memcpy(ptr, value.c_str(), value.size() + 1);
  return ptr;
}

std::string trim(const std::string &value) {
  size_t start = 0;
  while (start < value.size()) {
    const unsigned char ch = static_cast<unsigned char>(value[start]);
    if (!(ch == 32 || ch == 9 || ch == 10 || ch == 13)) break;
    ++start;
  }
  size_t end = value.size();
  while (end > start) {
    const unsigned char ch = static_cast<unsigned char>(value[end - 1]);
    if (!(ch == 32 || ch == 9 || ch == 10 || ch == 13)) break;
    --end;
  }
  return value.substr(start, end - start);
}

std::string normalizeSlot(const char *slot) {
  std::string out = trim(slot ? slot : "");
  if (out.empty()) {
    out = "quick";
  }
  for (char &c : out) {
    const unsigned char uc = static_cast<unsigned char>(c);
    const bool ok = ((uc >= '0' && uc <= '9') || (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || c == '.' || c == '_' || c == '-');
    if (!ok) {
      c = '_';
    }
  }
  return out;
}

void splitStoryPath(const std::string &path, std::string &dirOut, std::string &fileOut) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    dirOut = ".";
    fileOut = path;
    return;
  }
  dirOut = (pos == 0) ? "/" : path.substr(0, pos);
  fileOut = path.substr(pos + 1);
}

bool resolveEmbeddedRunnerMain() {
  if (g_embeddedRunnerMain) {
    return true;
  }
  const char *candidateSymbols[] = {
      "openfrotz_embedded_if_main",
      "openfrotz_if_runner_main",
      "openfrotz_embedded_runner_main"};
  for (const char *symbol : candidateSymbols) {
    void *raw = dlsym(RTLD_DEFAULT, symbol);
    if (!raw) {
      continue;
    }
    g_embeddedRunnerMain = reinterpret_cast<EmbeddedRunnerMainFn>(raw);
    g_embeddedRunnerSymbol = symbol;
    __android_log_print(ANDROID_LOG_INFO, kTag, "Resolved embedded runner symbol=%s", symbol);
    return true;
  }
  return false;
}

void runnerMain(std::string storyPath) {
  std::string dir;
  std::string file;
  splitStoryPath(storyPath, dir, file);
  if (file.empty()) {
    Glk::OpenFrotzRuntimeHooks::appendSystemMessage("ERROR:Invalid story filename for runner.");
    return;
  }

  Glk::OpenFrotzRuntimeHooks::appendSystemMessage("Embedded runner thread starting.");
  __android_log_print(ANDROID_LOG_INFO, kTag, "runnerMain marker=%s story=%s", kRuntimeBuildMarker, storyPath.c_str());
  const std::string pathArg = std::string("--path=") + dir;
  const char *argv[3];
  argv[0] = "scummvm";
  argv[1] = pathArg.c_str();
  argv[2] = "zcode";

  int rc = -1;
  if (resolveEmbeddedRunnerMain()) {
    Glk::OpenFrotzRuntimeHooks::appendSystemMessage(
        Common::String(("Embedded IF runner symbol: " + g_embeddedRunnerSymbol).c_str()));
    __android_log_print(ANDROID_LOG_INFO, kTag, "runner invoking embedded symbol=%s", g_embeddedRunnerSymbol.c_str());
    rc = g_embeddedRunnerMain(3, argv);
  } else if (kAllowScummVmMainFallback) {
    Glk::OpenFrotzRuntimeHooks::appendSystemMessage(
        "WARNING:Embedded IF runner symbol missing; falling back to scummvm_main.");
    __android_log_print(ANDROID_LOG_INFO, kTag, "runner invoking scummvm_main fallback");
    rc = scummvm_main(3, argv);
  } else {
    Glk::OpenFrotzRuntimeHooks::appendSystemMessage(
        "ERROR:Embedded IF runner symbol missing; scummvm_main fallback disabled.");
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "runner returned rc=%d", rc);
  std::string rcMsg = "Embedded runner exited rc=" + std::to_string(rc);
  Glk::OpenFrotzRuntimeHooks::appendSystemMessage(Common::String(rcMsg.c_str()));
  StateLock lock;
  g_runnerStarted = false;
}
} // namespace

extern "C" __attribute__((visibility("default"))) int openfrotz_embedded_if_main(int argc, const char *argv[]) {
  __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "openfrotz_embedded_if_main entering scummvm_main argc=%d",
      argc);
  return scummvm_main(argc, argv);
}

extern "C" __attribute__((visibility("default"))) const char *openfrotz_runtime_start(const char *storyPath) {
  const std::string normalizedPath = trim(storyPath ? storyPath : "");
  if (normalizedPath.empty()) {
    return dupCString("ERROR:Story path is required.");
  }
  StateLock lock;
  g_story = Common::String(normalizedPath.c_str());
  __android_log_print(ANDROID_LOG_INFO, kTag, "openfrotz_runtime_start marker=%s story=%s", kRuntimeBuildMarker, normalizedPath.c_str());
  Glk::OpenFrotzRuntimeHooks::startSession(g_story);
  g_slotSnapshots.clear();
  g_lastRevision = 0;
  if (!resolveEmbeddedRunnerMain() && !kAllowScummVmMainFallback) {
    Glk::OpenFrotzRuntimeHooks::appendSystemMessage(
        "Embedded IF runner symbol missing; using JNI main-loop command path.");
    __android_log_print(
      ANDROID_LOG_WARN,
      kTag,
      "runtime_start runner symbol missing; session active without runtime-owned runner");
    return dupCString("");
  }
  if (!kEnableDetachedEmbeddedRunner) {
    Glk::OpenFrotzRuntimeHooks::appendSystemMessage(
        "Detached embedded runner disabled; using JNI main-loop ownership.");
    __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "runtime_start detached runner disabled; JNI main-loop mode enforced");
    return dupCString("");
  }
  if (!g_runnerStarted) {
    g_runnerStarted = true;
    g_runnerThread = std::thread(runnerMain, normalizedPath);
    g_runnerThread.detach();
  }
  return dupCString("");
}

extern "C" __attribute__((visibility("default"))) const char *openfrotz_runtime_send_command(const char *command) {
  {
    StateLock lock;
    if (g_story.empty()) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "send_command rejected: no active story");
      return dupCString("ERROR:No active story.");
    }
  }
  const std::string cmd = trim(command ? command : "");
  if (cmd.empty()) {
    uint64 rev = 0;
    const Common::String snapshot = Glk::OpenFrotzRuntimeHooks::getTranscriptSnapshot(&rev);
    {
      StateLock lock;
      g_lastRevision = rev;
    }
    return dupCString(snapshot.c_str());
  }

  uint64 beforeRevision = 0;
  uint64 beforeDequeueRevision = Glk::OpenFrotzRuntimeHooks::getDequeueRevision();
  Glk::OpenFrotzRuntimeHooks::getTranscriptSnapshot(&beforeRevision);
  Glk::OpenFrotzRuntimeHooks::enqueueCommand(Common::String(cmd.c_str()));
  uint64 waitFromRevision = 0;
  Glk::OpenFrotzRuntimeHooks::getTranscriptSnapshot(&waitFromRevision);
  uint64 rev = waitFromRevision;
  __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "send_command cmd=%s before=%llu afterEnqueue=%llu",
      cmd.c_str(),
      static_cast<unsigned long long>(beforeRevision),
      static_cast<unsigned long long>(waitFromRevision));
  // Avoid injecting synthetic RETURN unless we're actually stalled; eager nudges
  // can produce a blank command ("I beg your pardon?") before real command output.
  bool dequeued =
      Glk::OpenFrotzRuntimeHooks::waitForDequeueSince(beforeDequeueRevision, 150, nullptr);
  if (!dequeued) {
    Glk::OpenFrotzRuntimeHooks::nudgeInputPump();
    dequeued =
        Glk::OpenFrotzRuntimeHooks::waitForDequeueSince(beforeDequeueRevision, 1050, nullptr);
  }
  const uint64 outputBaselineRevision = Glk::OpenFrotzRuntimeHooks::getOutputRevision();
  if (!dequeued) {
    __android_log_print(
        ANDROID_LOG_WARN,
        kTag,
        "send_command cmd=%s dequeue wait timed out before output wait",
        cmd.c_str());
  }
  Common::String transcript =
      Glk::OpenFrotzRuntimeHooks::waitForOutputSince(outputBaselineRevision, 1200, nullptr);
  if (transcript.empty()) {
    transcript = Glk::OpenFrotzRuntimeHooks::waitForTranscriptSince(waitFromRevision, 400, &rev);
  } else {
    Glk::OpenFrotzRuntimeHooks::getTranscriptSnapshot(&rev);
  }
  {
    StateLock lock;
    g_lastRevision = rev;
  }
  if (!transcript.empty()) {
    return dupCString(transcript.c_str());
  }
  uint64 snapshotRevision = 0;
  const Common::String snapshot = Glk::OpenFrotzRuntimeHooks::getTranscriptSnapshot(&snapshotRevision);
  {
    StateLock lock;
    g_lastRevision = snapshotRevision;
  }
  __android_log_print(
      ANDROID_LOG_WARN,
      kTag,
      "send_command timeout cmd=%s returning snapshot rev=%llu bytes=%zu",
      cmd.c_str(),
      static_cast<unsigned long long>(snapshotRevision),
      static_cast<size_t>(snapshot.size()));
  return dupCString(snapshot.c_str());
}

extern "C" __attribute__((visibility("default"))) const char *openfrotz_runtime_save(const char *slot) {
  StateLock lock;
  if (g_story.empty()) {
    return dupCString("ERROR:No active story.");
  }
  const std::string slotName = normalizeSlot(slot);
  uint64 rev = 0;
  const Common::String snapshot = Glk::OpenFrotzRuntimeHooks::getTranscriptSnapshot(&rev);
  g_slotSnapshots[slotName] = snapshot;
  g_lastRevision = rev;
  return dupCString("Saved slot in runtime transcript bridge.");
}

extern "C" __attribute__((visibility("default"))) const char *openfrotz_runtime_restore(const char *slot) {
  StateLock lock;
  if (g_story.empty()) {
    return dupCString("ERROR:No active story.");
  }
  const std::string slotName = normalizeSlot(slot);
  auto it = g_slotSnapshots.find(slotName);
  if (it == g_slotSnapshots.end()) {
    return dupCString("ERROR:No save found for slot.");
  }
  return dupCString((std::string("Restored slot transcript snapshot (state restore pending): ") + slotName).c_str());
}

extern "C" __attribute__((visibility("default"))) void openfrotz_runtime_close() {
  StateLock lock;
  __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "runtime_close storyWasSet=%d runnerStarted=%d lastRevision=%llu",
      g_story.empty() ? 0 : 1,
      g_runnerStarted ? 1 : 0,
      static_cast<unsigned long long>(g_lastRevision));
  g_story.clear();
  g_slotSnapshots.clear();
  g_lastRevision = 0;
  Glk::OpenFrotzRuntimeHooks::endSession();
}

extern "C" __attribute__((visibility("default"))) void openfrotz_runtime_free(const char *value) {
  std::free(const_cast<char *>(value));
}

extern "C" __attribute__((visibility("default"))) const char *openfrotz_runtime_engine_mode() {
  if (!kEnableDetachedEmbeddedRunner) {
    return dupCString("jni-main-loop-only");
  }
  if (resolveEmbeddedRunnerMain()) {
    return dupCString((std::string("embedded-if-runner:") + g_embeddedRunnerSymbol).c_str());
  }
  if (kAllowScummVmMainFallback) {
    return dupCString("scummvm-main-fallback");
  }
  return dupCString("embedded-if-runner-missing");
}
