#pragma once

#include <memory>

namespace autoruntime {

enum class MutexProtocol {
  Default,
  PriorityInheritance,
};

class RealtimeMutex {
 public:
  explicit RealtimeMutex(
      MutexProtocol protocol = MutexProtocol::PriorityInheritance);
  ~RealtimeMutex();

  RealtimeMutex(const RealtimeMutex&) = delete;
  RealtimeMutex& operator=(const RealtimeMutex&) = delete;
  RealtimeMutex(RealtimeMutex&&) = delete;
  RealtimeMutex& operator=(RealtimeMutex&&) = delete;

  void lock();
  [[nodiscard]] bool try_lock();
  void unlock() noexcept;

  [[nodiscard]] bool priority_inheritance_requested() const noexcept;
  [[nodiscard]] bool priority_inheritance_active() const noexcept;
  [[nodiscard]] int priority_inheritance_error() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
