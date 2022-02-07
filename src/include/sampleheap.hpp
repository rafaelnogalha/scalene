#pragma once

#ifndef SAMPLEHEAP_H
#define SAMPLEHEAP_H

#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>  // for getpid()
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#include <atomic>
#include <random>

// We're unable to use the limited API because, for example,
// there doesn't seem to be a function returning co_filename
//#define Py_LIMITED_API 0x03070000

#include "common.hpp"
#include "mallocrecursionguard.hpp"
#include "printf.h"
#include "pywhere.hpp"
#include "samplefile.hpp"
#include "sampleinterval.hpp"
#include "scaleneheader.hpp"

static SampleFile& getSampleFile() {
  static SampleFile mallocSampleFile("/tmp/scalene-malloc-signal%d",
                                     "/tmp/scalene-malloc-lock%d",
                                     "/tmp/scalene-malloc-init%d");

  return mallocSampleFile;
}

#define USE_ATOMICS 0

#if USE_ATOMICS
typedef std::atomic<uint64_t> counterType;
#else
typedef uint64_t counterType;
#endif

template <uint64_t DefaultAllocationSamplingRateBytes, class SuperHeap>
class SampleHeap : public SuperHeap {
  constexpr static auto sampling_window_envname =
      "SCALENE_ALLOCATION_SAMPLING_WINDOW";

 public:
  enum { Alignment = SuperHeap::Alignment };
  enum AllocSignal { MallocSignal = SIGXCPU, FreeSignal = SIGXFSZ };

  static constexpr uint64_t NEWLINE =
      98821;  // Sentinel value denoting a new line has executed

  SampleHeap()
      : _lastMallocTrigger(nullptr),
        _freedLastMallocTrigger(false),
        _allocationSampler(getenv(sampling_window_envname)
                               ? atol(getenv(sampling_window_envname))
                               : DefaultAllocationSamplingRateBytes) {
    getSampleFile();  // invoked here so the file gets initialized before python
                      // attempts to read from it

    get_signal_init_lock().lock();
    auto old_malloc = signal(MallocSignal, SIG_IGN);
    if (old_malloc != SIG_DFL) {
      signal(MallocSignal, old_malloc);
    }
    auto old_free = signal(FreeSignal, SIG_IGN);
    if (old_free != SIG_DFL) {
      signal(FreeSignal, old_free);
    }
    get_signal_init_lock().unlock();
  }

  ATTRIBUTE_ALWAYS_INLINE inline void* malloc(size_t sz) {
    MallocRecursionGuard g;
    auto ptr = SuperHeap::malloc(sz);
    if (unlikely(ptr == nullptr)) {
      return nullptr;
    }
    if (pythonDetected() && !g.wasInMalloc()) {
      auto realSize = SuperHeap::getSize(ptr);
      if (realSize > 0) {
        if (sz == NEWLINE + sizeof(ScaleneHeader)) {
          // Don't count these allocations
          return ptr;
        }
        register_malloc(realSize, ptr, false);  // false -> invoked from C/C++
      }
    }
    return ptr;
  }

  inline void register_malloc(size_t realSize, void* ptr,
                              bool inPythonAllocator = true) {
    assert(realSize);
    // If this is the special NEWLINE value, trigger an update.
    if (unlikely(realSize == NEWLINE)) {
      std::string filename;
      int lineno;
      int bytei;
      decltype(whereInPython)* where = p_whereInPython;
      if (where != nullptr && where(filename, lineno, bytei)) {
        writeCount(MallocSignal, realSize, ptr, filename, lineno, bytei);
      }
      mallocTriggered()++;
      return;
    }
    auto sampleMalloc = _allocationSampler.increment(realSize);
    if (inPythonAllocator) {
      _pythonCount += realSize;
    } else {
      _cCount += realSize;
    }
    if (unlikely(sampleMalloc)) {
      process_malloc(sampleMalloc, ptr);
    }
  }

 private:
  void process_malloc(size_t sampleMalloc, void* ptr) {
    std::string filename;
    int lineno;
    int bytei;

    decltype(whereInPython)* where = p_whereInPython;
    if (where != nullptr && where(filename, lineno, bytei)) {
      // printf_("MALLOC HANDLED (SAMPLEHEAP): %p -> %lu (%s, %d)\n", ptr,
      // sampleMalloc, filename.c_str(), lineno);
      writeCount(MallocSignal, sampleMalloc, ptr, filename, lineno, bytei);
#if !SCALENE_DISABLE_SIGNALS
      raise(MallocSignal);
#endif
      _lastMallocTrigger = ptr;
      _freedLastMallocTrigger = false;
      _pythonCount = 0;
      _cCount = 0;
      mallocTriggered()++;
    }
  }

 public:
  ATTRIBUTE_ALWAYS_INLINE inline void free(void* ptr) {
    MallocRecursionGuard g;

    if (unlikely(ptr == nullptr)) {
      return;
    }
    if (pythonDetected() && !g.wasInMalloc()) {
      auto realSize = SuperHeap::getSize(ptr);
      register_free(realSize, ptr);
    }
    SuperHeap::free(ptr);
  }

  inline void register_free(size_t realSize, void* ptr) {
    auto sampleFree = _allocationSampler.decrement(realSize);
    if (unlikely(ptr && (ptr == _lastMallocTrigger))) {
      _freedLastMallocTrigger = true;
    }
    if (unlikely(sampleFree)) {
      process_free(sampleFree, ptr);
    }
  }

 private:
  void process_free(size_t sampleFree, void* ptr) {
    std::string filename;
    int lineno = 1;
    int bytei = 0;

#if 1
    decltype(whereInPython)* where = p_whereInPython;
    if (where != nullptr && where(filename, lineno, bytei)) {
      ;
    }
#endif

    writeCount(FreeSignal, sampleFree, ptr, filename, lineno, bytei);
#if !SCALENE_DISABLE_SIGNALS
    raise(FreeSignal);  // was FreeSignal
#endif
    freeTriggered()++;
  }

 public:
  void* memalign(size_t alignment, size_t sz) {
    MallocRecursionGuard g;
    auto ptr = SuperHeap::memalign(alignment, sz);
    if (unlikely(ptr == nullptr)) {
      return nullptr;
    }
    if (pythonDetected() && !g.wasInMalloc()) {
      auto realSize = SuperHeap::getSize(ptr);
      assert(realSize >= sz);
      assert((sz < 16) || (realSize <= 2 * sz));
      register_malloc(realSize, ptr);
    }
    return ptr;
  }

 private:
  // Prevent copying and assignment.
  SampleHeap(const SampleHeap&) = delete;
  SampleHeap& operator=(const SampleHeap&) = delete;

  static auto& mallocTriggered() {
    static std::atomic<uint64_t> _mallocTriggered{0};
    return _mallocTriggered;
  }
  static auto& freeTriggered() {
    static std::atomic<uint64_t> _freeTriggered{0};
    return _freeTriggered;
  }

  counterType _pythonCount{0};
  counterType _cCount{0};

  void* _lastMallocTrigger;
  bool _freedLastMallocTrigger;
  SampleInterval _allocationSampler;

  static constexpr auto flags = O_RDWR | O_CREAT;
  static constexpr auto perms = S_IRUSR | S_IWUSR;

  void writeCount(AllocSignal sig, uint64_t count, void* ptr,
                  const std::string& filename, int lineno, int bytei) {
    char buf[SampleFile::MAX_BUFSIZE];
    if (_pythonCount == 0) {
      _pythonCount = 1;  // prevent 0/0
    }
    #if defined(__APPLE__)
    uint64_t nanos = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    #else
    struct timespec ts;
    auto res = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (res != 0) {
      printf("ERRNO %d", errno);
    }
    auto nanos = ts.tv_sec * 1000000000 + ts.tv_nsec;
    #endif
    
    snprintf_(
        buf, SampleFile::MAX_BUFSIZE,
#if defined(__APPLE__)
        "%c,%llu,%llu,%f,%d,%p,%s,%d,%d,%llu\n\n",
#else
        "%c,%lu,%lu,%f,%d,%p,%s,%d,%d,%ld\n\n",
#endif
        ((sig == MallocSignal) ? 'M' : ((_freedLastMallocTrigger) ? 'f' : 'F')),
        mallocTriggered() + freeTriggered(), count,
        (float)_pythonCount / (_pythonCount + _cCount), getpid(),
        _freedLastMallocTrigger ? _lastMallocTrigger : ptr, filename.c_str(),
        lineno, bytei, nanos);
    // Ensure we don't report last-malloc-freed multiple times.
    _freedLastMallocTrigger = false;
    getSampleFile().writeToFile(buf);
  }

  static HL::PosixLock& get_signal_init_lock() {
    static HL::PosixLock signal_init_lock;
    return signal_init_lock;
  }
};

#endif
