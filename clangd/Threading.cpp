#include "Threading.h"
#include "Trace.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Threading.h"
#include <atomic>
#include <thread>
#ifdef __USE_POSIX
#include <pthread.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace clang {
namespace clangd {

void Notification::notify() {
  {
    std::lock_guard<std::mutex> Lock(Mu);
    Notified = true;
  }
  CV.notify_all();
}

void Notification::wait() const {
  std::unique_lock<std::mutex> Lock(Mu);
  CV.wait(Lock, [this] { return Notified; });
}

Semaphore::Semaphore(std::size_t MaxLocks) : FreeSlots(MaxLocks) {}

bool Semaphore::try_lock() {
  std::unique_lock<std::mutex> Lock(Mutex);
  if (FreeSlots > 0) {
    --FreeSlots;
    return true;
  }
  return false;
}

void Semaphore::lock() {
  trace::Span Span("WaitForFreeSemaphoreSlot");
  // trace::Span can also acquire locks in ctor and dtor, we make sure it
  // happens when Semaphore's own lock is not held.
  {
    std::unique_lock<std::mutex> Lock(Mutex);
    SlotsChanged.wait(Lock, [&]() { return FreeSlots > 0; });
    --FreeSlots;
  }
}

void Semaphore::unlock() {
  std::unique_lock<std::mutex> Lock(Mutex);
  ++FreeSlots;
  Lock.unlock();

  SlotsChanged.notify_one();
}

AsyncTaskRunner::~AsyncTaskRunner() { wait(); }

bool AsyncTaskRunner::wait(Deadline D) const {
  std::unique_lock<std::mutex> Lock(Mutex);
  return clangd::wait(Lock, TasksReachedZero, D,
                      [&] { return InFlightTasks == 0; });
}

void AsyncTaskRunner::runAsync(const llvm::Twine &Name,
                               llvm::unique_function<void()> Action) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    ++InFlightTasks;
  }

  auto CleanupTask = llvm::make_scope_exit([this]() {
    std::lock_guard<std::mutex> Lock(Mutex);
    int NewTasksCnt = --InFlightTasks;
    if (NewTasksCnt == 0) {
      // Note: we can't unlock here because we don't want the object to be
      // destroyed before we notify.
      TasksReachedZero.notify_one();
    }
  });

  std::thread(
      [](std::string Name, decltype(Action) Action, decltype(CleanupTask)) {
        llvm::set_thread_name(Name);
        Action();
        // Make sure function stored by Action is destroyed before CleanupTask
        // is run.
        Action = nullptr;
      },
      Name.str(), std::move(Action), std::move(CleanupTask))
      .detach();
}

Deadline timeoutSeconds(llvm::Optional<double> Seconds) {
  using namespace std::chrono;
  if (!Seconds)
    return Deadline::infinity();
  return steady_clock::now() +
         duration_cast<steady_clock::duration>(duration<double>(*Seconds));
}

void wait(std::unique_lock<std::mutex> &Lock, std::condition_variable &CV,
          Deadline D) {
  if (D == Deadline::zero())
    return;
  if (D == Deadline::infinity())
    return CV.wait(Lock);
  CV.wait_until(Lock, D.time());
}

static std::atomic<bool> AvoidThreadStarvation = {false};

void setCurrentThreadPriority(ThreadPriority Priority) {
  // Some *really* old glibcs are missing SCHED_IDLE.
#if defined(__linux__) && defined(SCHED_IDLE)
  sched_param priority;
  priority.sched_priority = 0;
  pthread_setschedparam(
      pthread_self(),
      Priority == ThreadPriority::Low && !AvoidThreadStarvation ? SCHED_IDLE
                                                                : SCHED_OTHER,
      &priority);
#elif defined(__APPLE__)
  // https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getpriority.2.html
  setpriority(PRIO_DARWIN_THREAD, 0,
              Priority == ThreadPriority::Low && !AvoidThreadStarvation
                  ? PRIO_DARWIN_BG
                  : 0);
#endif
}

void preventThreadStarvationInTests() { AvoidThreadStarvation = true; }

} // namespace clangd
} // namespace clang
