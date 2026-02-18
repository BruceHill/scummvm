/*
 * OpenFrotz mutex audit interposer for Android ScummVM builds.
 *
 * This file overrides pthread mutex entry points inside libscummvm.so so we
 * can track init/destroy/lock ordering for raw mutex addresses observed in
 * Android FORTIFY crashes.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <unordered_map>

namespace {
constexpr const char *kTag = "OpenFrotzMutexAudit";
constexpr bool kSoftFailDestroyedLock = true;

struct MutexMeta {
  bool seen = false;
  bool destroyed = false;
  uintptr_t firstSeenPc = 0;
  uintptr_t initPc = 0;
  uintptr_t destroyPc = 0;
  int firstSeenTid = 0;
  int initTid = 0;
  int destroyTid = 0;
  uint64_t initSeq = 0;
  uint64_t destroySeq = 0;
};

using MutexInitFn = int (*)(pthread_mutex_t *, const pthread_mutexattr_t *);
using MutexDestroyFn = int (*)(pthread_mutex_t *);
using MutexLockFn = int (*)(pthread_mutex_t *);
using MutexTryLockFn = int (*)(pthread_mutex_t *);
using MutexUnlockFn = int (*)(pthread_mutex_t *);

MutexInitFn g_realInit = nullptr;
MutexDestroyFn g_realDestroy = nullptr;
MutexLockFn g_realLock = nullptr;
MutexTryLockFn g_realTryLock = nullptr;
MutexUnlockFn g_realUnlock = nullptr;

std::atomic<bool> g_resolved{false};
std::atomic_flag g_mapGuard = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_seq{0};
std::unordered_map<const void *, MutexMeta> g_metaByPtr;
thread_local bool g_inHook = false;

inline int currentTid() {
  return static_cast<int>(::syscall(SYS_gettid));
}

inline uintptr_t callerPc() {
  return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
}

inline void lockMap() {
  while (g_mapGuard.test_and_set(std::memory_order_acquire)) {
  }
}

inline void unlockMap() {
  g_mapGuard.clear(std::memory_order_release);
}

void resolveSymbols() {
  if (g_resolved.load(std::memory_order_acquire)) {
    return;
  }

  g_inHook = true;
  g_realInit = reinterpret_cast<MutexInitFn>(dlsym(RTLD_NEXT, "pthread_mutex_init"));
  g_realDestroy = reinterpret_cast<MutexDestroyFn>(dlsym(RTLD_NEXT, "pthread_mutex_destroy"));
  g_realLock = reinterpret_cast<MutexLockFn>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
  g_realTryLock = reinterpret_cast<MutexTryLockFn>(dlsym(RTLD_NEXT, "pthread_mutex_trylock"));
  g_realUnlock = reinterpret_cast<MutexUnlockFn>(dlsym(RTLD_NEXT, "pthread_mutex_unlock"));
  g_inHook = false;

  g_resolved.store(true, std::memory_order_release);
  __android_log_print(
      ANDROID_LOG_INFO,
      kTag,
      "resolved init=%p destroy=%p lock=%p trylock=%p unlock=%p",
      reinterpret_cast<void *>(g_realInit),
      reinterpret_cast<void *>(g_realDestroy),
      reinterpret_cast<void *>(g_realLock),
      reinterpret_cast<void *>(g_realTryLock),
      reinterpret_cast<void *>(g_realUnlock));
}

inline MutexMeta &ensureMeta(const void *ptr, uintptr_t pc, int tid) {
  MutexMeta &meta = g_metaByPtr[ptr];
  if (!meta.seen) {
    meta.seen = true;
    meta.firstSeenPc = pc;
    meta.firstSeenTid = tid;
  }
  return meta;
}
} // namespace

extern "C" __attribute__((constructor)) void openfrotz_mutex_audit_init() {
  __android_log_print(ANDROID_LOG_WARN, kTag, "mutex audit loaded");
}

extern "C" int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
  resolveSymbols();
  if (!g_realInit) {
    return EINVAL;
  }
  if (g_inHook) {
    return g_realInit(mutex, attr);
  }

  g_inHook = true;
  const int rc = g_realInit(mutex, attr);
  const uintptr_t pc = callerPc();
  const int tid = currentTid();
  if (rc == 0) {
    lockMap();
    MutexMeta &meta = ensureMeta(mutex, pc, tid);
    meta.destroyed = false;
    meta.initPc = pc;
    meta.initTid = tid;
    meta.initSeq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    unlockMap();
    __android_log_print(
        ANDROID_LOG_INFO,
        kTag,
        "init ptr=%p tid=%d pc=0x%" PRIxPTR " seq=%" PRIu64,
        static_cast<void *>(mutex),
        tid,
        pc,
        meta.initSeq);
  } else {
    __android_log_print(
        ANDROID_LOG_WARN,
        kTag,
        "init failed ptr=%p tid=%d pc=0x%" PRIxPTR " rc=%d",
        static_cast<void *>(mutex),
        tid,
        pc,
        rc);
  }
  g_inHook = false;
  return rc;
}

extern "C" int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  resolveSymbols();
  if (!g_realDestroy) {
    return EINVAL;
  }
  if (g_inHook) {
    return g_realDestroy(mutex);
  }

  g_inHook = true;
  const uintptr_t pc = callerPc();
  const int tid = currentTid();
  const int rc = g_realDestroy(mutex);
  lockMap();
  MutexMeta &meta = ensureMeta(mutex, pc, tid);
  if (rc == 0) {
    meta.destroyed = true;
    meta.destroyPc = pc;
    meta.destroyTid = tid;
    meta.destroySeq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    unlockMap();
    __android_log_print(
        ANDROID_LOG_WARN,
        kTag,
        "destroy ptr=%p tid=%d pc=0x%" PRIxPTR " seq=%" PRIu64
        " initTid=%d initPc=0x%" PRIxPTR " initSeq=%" PRIu64,
        static_cast<void *>(mutex),
        tid,
        pc,
        meta.destroySeq,
        meta.initTid,
        meta.initPc,
        meta.initSeq);
  } else {
    unlockMap();
    __android_log_print(
        ANDROID_LOG_WARN,
        kTag,
        "destroy failed ptr=%p tid=%d pc=0x%" PRIxPTR " rc=%d",
        static_cast<void *>(mutex),
        tid,
        pc,
        rc);
  }
  g_inHook = false;
  return rc;
}

extern "C" int pthread_mutex_lock(pthread_mutex_t *mutex) {
  resolveSymbols();
  if (!g_realLock) {
    return EINVAL;
  }
  if (g_inHook) {
    return g_realLock(mutex);
  }

  g_inHook = true;
  const uintptr_t pc = callerPc();
  const int tid = currentTid();

  bool destroyed = false;
  MutexMeta metaCopy;
  lockMap();
  MutexMeta &meta = ensureMeta(mutex, pc, tid);
  destroyed = meta.destroyed;
  metaCopy = meta;
  unlockMap();

  if (destroyed) {
    __android_log_print(
        ANDROID_LOG_ERROR,
        kTag,
        "lock-destroyed ptr=%p tid=%d pc=0x%" PRIxPTR
        " destroyTid=%d destroyPc=0x%" PRIxPTR " destroySeq=%" PRIu64
        " initTid=%d initPc=0x%" PRIxPTR " initSeq=%" PRIu64,
        static_cast<void *>(mutex),
        tid,
        pc,
        metaCopy.destroyTid,
        metaCopy.destroyPc,
        metaCopy.destroySeq,
        metaCopy.initTid,
        metaCopy.initPc,
        metaCopy.initSeq);
    g_inHook = false;
    if (kSoftFailDestroyedLock) {
      return EINVAL;
    }
    g_inHook = true;
  }

  const int rc = g_realLock(mutex);
  g_inHook = false;
  return rc;
}

extern "C" int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  resolveSymbols();
  if (!g_realTryLock) {
    return EINVAL;
  }
  if (g_inHook) {
    return g_realTryLock(mutex);
  }

  g_inHook = true;
  const uintptr_t pc = callerPc();
  const int tid = currentTid();
  bool destroyed = false;
  MutexMeta metaCopy;
  lockMap();
  MutexMeta &meta = ensureMeta(mutex, pc, tid);
  destroyed = meta.destroyed;
  metaCopy = meta;
  unlockMap();
  if (destroyed) {
    __android_log_print(
        ANDROID_LOG_ERROR,
        kTag,
        "trylock-destroyed ptr=%p tid=%d pc=0x%" PRIxPTR
        " destroyTid=%d destroyPc=0x%" PRIxPTR " destroySeq=%" PRIu64,
        static_cast<void *>(mutex),
        tid,
        pc,
        metaCopy.destroyTid,
        metaCopy.destroyPc,
        metaCopy.destroySeq);
    g_inHook = false;
    return EINVAL;
  }
  const int rc = g_realTryLock(mutex);
  g_inHook = false;
  return rc;
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  resolveSymbols();
  if (!g_realUnlock) {
    return EINVAL;
  }
  if (g_inHook) {
    return g_realUnlock(mutex);
  }
  g_inHook = true;
  const int rc = g_realUnlock(mutex);
  g_inHook = false;
  return rc;
}

// Bionic may call internal symbols directly; alias them to our wrappers.
extern "C" int __pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *)
    __attribute__((alias("pthread_mutex_init")));
extern "C" int __pthread_mutex_destroy(pthread_mutex_t *)
    __attribute__((alias("pthread_mutex_destroy")));
extern "C" int __pthread_mutex_lock(pthread_mutex_t *)
    __attribute__((alias("pthread_mutex_lock")));
extern "C" int __pthread_mutex_trylock(pthread_mutex_t *)
    __attribute__((alias("pthread_mutex_trylock")));
extern "C" int __pthread_mutex_unlock(pthread_mutex_t *)
    __attribute__((alias("pthread_mutex_unlock")));
