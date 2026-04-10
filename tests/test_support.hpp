#pragma once
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

inline bool Check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}
#define CHECK(expression) \
  do { \
    if (!Check(static_cast<bool>(expression), #expression, __LINE__)) return 1; \
  } while (false)

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::invoke(predicate)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::invoke(predicate);
}
