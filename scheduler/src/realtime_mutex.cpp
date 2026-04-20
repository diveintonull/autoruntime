#include <autoruntime/realtime_mutex.hpp>

#include <cerrno>
#include <exception>
#include <mutex>
#include <system_error>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace autoruntime {

struct RealtimeMutex::Impl {
  explicit Impl(MutexProtocol protocol)
      : priority_inheritance_requested(
            protocol == MutexProtocol::PriorityInheritance) {
#if defined(__linux__)
    pthread_mutexattr_t attributes;
    int error = ::pthread_mutexattr_init(&attributes);
    if (error != 0) {
      throw std::system_error(error, std::generic_category(),
                              "pthread_mutexattr_init");
    }

    if (priority_inheritance_requested) {
      error = ::pthread_mutexattr_setprotocol(
          &attributes, PTHREAD_PRIO_INHERIT);
      if (error == 0) {
        priority_inheritance_active = true;
      } else {
        priority_inheritance_error = error;
      }
    }

    error = ::pthread_mutex_init(&mutex, &attributes);
    static_cast<void>(::pthread_mutexattr_destroy(&attributes));
    if (error != 0 && priority_inheritance_active) {
      priority_inheritance_active = false;
      priority_inheritance_error = error;
      error = ::pthread_mutex_init(&mutex, nullptr);
    }
    if (error != 0) {
      throw std::system_error(error, std::generic_category(),
                              "pthread_mutex_init");
    }
    initialized = true;
#else
    if (priority_inheritance_requested) {
      priority_inheritance_error = ENOTSUP;
    }
#endif
  }

  ~Impl() {
#if defined(__linux__)
    if (initialized) {
      static_cast<void>(::pthread_mutex_destroy(&mutex));
    }
#endif
  }

#if defined(__linux__)
  pthread_mutex_t mutex{};
  bool initialized{false};
#else
  std::mutex mutex;
#endif
  bool priority_inheritance_requested{false};
  bool priority_inheritance_active{false};
  int priority_inheritance_error{0};
};

RealtimeMutex::RealtimeMutex(MutexProtocol protocol)
    : impl_(std::make_unique<Impl>(protocol)) {}

RealtimeMutex::~RealtimeMutex() = default;

void RealtimeMutex::lock() {
#if defined(__linux__)
  const int error = ::pthread_mutex_lock(&impl_->mutex);
  if (error != 0) {
    throw std::system_error(error, std::generic_category(),
                            "pthread_mutex_lock");
  }
#else
  impl_->mutex.lock();
#endif
}

bool RealtimeMutex::try_lock() {
#if defined(__linux__)
  const int error = ::pthread_mutex_trylock(&impl_->mutex);
  if (error == 0) {
    return true;
  }
  if (error == EBUSY) {
    return false;
  }
  throw std::system_error(error, std::generic_category(),
                          "pthread_mutex_trylock");
#else
  return impl_->mutex.try_lock();
#endif
}

void RealtimeMutex::unlock() noexcept {
#if defined(__linux__)
  if (::pthread_mutex_unlock(&impl_->mutex) != 0) {
    std::terminate();
  }
#else
  impl_->mutex.unlock();
#endif
}

bool RealtimeMutex::priority_inheritance_requested() const noexcept {
  return impl_->priority_inheritance_requested;
}

bool RealtimeMutex::priority_inheritance_active() const noexcept {
  return impl_->priority_inheritance_active;
}

int RealtimeMutex::priority_inheritance_error() const noexcept {
  return impl_->priority_inheritance_error;
}

}  // namespace autoruntime
