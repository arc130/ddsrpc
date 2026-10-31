#pragma once
#include <cstdint>
#include <functional>
#include <memory>

#include "await/awaitable.hpp"
#include "task/fibertask.h"
#include "detail/result.hpp"

namespace ddsrpc {

class TaskGroup {
 public:
  TaskGroup() = default;
  ~TaskGroup();
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;

  void Start();
  bool Add(std::function<void()> fn);
  [[nodiscard]] Coro::Result<void> Stop();

 private:
  struct State;
  void ReapLoop();

  std::shared_ptr<State> state_;
  std::shared_ptr<Coro::FiberTask<void>> reaper_;
  std::uint64_t next_id_ = 1;
  bool started_ = false;
};

}  // namespace ddsrpc
