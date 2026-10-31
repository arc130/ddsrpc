#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

#include <boost/fiber/operations.hpp>

#include "detail/asyncdefine.h"
#include "task_group.h"

TEST(TaskGroupTest, RunsAndStopsAllTasks) {
  ddsrpc::TaskGroup group;
  group.Start();
  std::atomic_int completed{0};
  for (int i = 0; i < 3; ++i) {
    group.Add([&completed] {
      boost::this_fiber::sleep_for(std::chrono::milliseconds(5));
      completed.fetch_add(1);
    });
  }
  Coro::msleep(30);
  EXPECT_EQ(completed.load(), 3);
  EXPECT_TRUE(group.Stop());
}

TEST(TaskGroupTest, StopCancelsAwaitingTasks) {
  ddsrpc::TaskGroup group;
  group.Start();
  Coro::Awaitable<int> gate;
  std::atomic_bool cancelled{false};
  group.Add([&cancelled, &gate] {
    auto r = Coro::await_for(gate, std::chrono::seconds(10));
    cancelled = !r && r.error() == std::make_error_code(std::errc::interrupted);
  });
  Coro::msleep(10);
  EXPECT_TRUE(group.Stop());
  EXPECT_TRUE(cancelled.load());
}
