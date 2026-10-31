#include "task_group.h"

#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace ddsrpc {

struct TaskGroup::State {
  std::map<std::uint64_t, std::shared_ptr<Coro::FiberTask<void>>> tasks;
  std::shared_ptr<Coro::Awaitable<std::uint64_t>> completed;
  bool stopping = false;
};

TaskGroup::~TaskGroup() {
  if (state_) {
    (void)Stop();
  }
}

void TaskGroup::Start() {
  if (started_) return;
  state_ = std::make_shared<State>();
  state_->completed = std::make_shared<Coro::Awaitable<std::uint64_t>>();
  // 服务端请求并发没有框架上限：完成通知队列必须无界，否则满时丢最旧会让
  // 已结束任务的句柄永远留在 map 里。
  state_->completed->setCapacity(0);
  started_ = true;
  reaper_ = std::make_shared<Coro::FiberTask<void>>(
      Coro::makeTask([this] { ReapLoop(); }, Coro::Priority::Normal,
                     Coro::Affinity::fixed(std::this_thread::get_id())));
}

bool TaskGroup::Add(std::function<void()> fn) {
  if (!started_ || !state_ || state_->stopping) return false;
  auto state = state_;
  const std::uint64_t id = next_id_++;
  auto channel = state->completed->channel();
  auto task = std::make_shared<Coro::FiberTask<void>>(
      Coro::makeTask([fn = std::move(fn), channel = std::move(channel), id] {
        try {
          fn();
        } catch (...) {
          // 任务函数不得把异常抛到 FiberTask 边界之外；取消与异常都只以
          // 完成通知收敛，归属由 Stop 的统一 join 负责。
        }
        (void)channel->push(id);
      }));
  state->tasks.emplace(id, std::move(task));
  return true;
}

void TaskGroup::ReapLoop() {
  auto state = state_;
  while (auto id = Coro::await(state->completed)) {
    state->tasks.erase(id.value());
  }
}

Coro::Result<void> TaskGroup::Stop() {
  auto state = state_;
  started_ = false;
  if (!state) return {};
  state->stopping = true;

  std::vector<std::shared_ptr<Coro::FiberTask<void>>> pending;
  pending.reserve(state->tasks.size());
  for (auto& entry : state->tasks) {
    pending.push_back(std::move(entry.second));
  }
  state->tasks.clear();

  for (auto& task : pending) {
    if (!task) continue;
    task->cancel();
    (void)task->get();
  }

  if (state->completed) state->completed->close();
  if (reaper_) {
    (void)reaper_->get();
    reaper_.reset();
  }
  state->completed.reset();
  state_.reset();
  return {};
}

}  // namespace ddsrpc
