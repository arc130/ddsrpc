#include <QCoreApplication>
#include <gtest/gtest.h>

#include "task/fiberapplication.h"
#include "task/fibertask.h"

// 运行时测试须在 fiber 调度器内跑：主线程安装本地调度器，断言体在
// makeTask 的 fiber 中执行，全部跑完后 quit() 让 Coro::exec() 返回。
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  Coro::installFiberApplication();
  int rc = 0;
  auto task = Coro::makeTask([&] {
    rc = RUN_ALL_TESTS();
    Coro::quit();
  });
  Coro::exec();
  (void)task;
  return rc;
}
