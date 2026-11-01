#include <gtest/gtest.h>

#include <memory>

#include "calculator.ddsrpc.client.h"
#include "calculator.ddsrpc.service.h"
#include "tests/support/fake_host.hpp"

namespace {

class ReconnectCalcService final : public testgen::CalculatorService {
 public:
  using CalculatorService::CalculatorService;

 protected:
  Coro::Result<int> add(int a, int b) override { return a + b; }
  Coro::Result<void> reset() override { return {}; }
  Coro::Result<void> report(QString) override { return {}; }
};

}  // namespace

TEST(E2eReconnectTest, RefreshesPropertiesAfterServiceRestart) {
  test_support::FakeDdsFixture fixture;
  test_support::Host client(fixture, "client");
  auto endpoint = std::make_unique<testgen::CalculatorClient>(
      client.node(), client.transport());
  ASSERT_TRUE(endpoint->prepare());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(endpoint->start());
  EXPECT_FALSE(endpoint->ready());

  {
    test_support::Host server(fixture, "server1");
    auto service = std::make_unique<ReconnectCalcService>(server.node());
    ASSERT_TRUE(service->prepare());
    ASSERT_TRUE(server.start_node());
    ASSERT_TRUE(service->start());
    ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint->ready(); }, 5000));
    ASSERT_TRUE(service->setName(QStringLiteral("before")));
    ASSERT_TRUE(test_support::PumpUntil(
        [&] { return endpoint->name() == QStringLiteral("before"); }, 3000));
    EXPECT_TRUE(service->stop());
  }

  EXPECT_TRUE(test_support::PumpUntil([&] { return !endpoint->ready(); }, 5000));

  {
    test_support::Host server(fixture, "server2");
    auto service = std::make_unique<ReconnectCalcService>(server.node());
    ASSERT_TRUE(service->prepare());
    ASSERT_TRUE(server.start_node());
    ASSERT_TRUE(service->start());
    ASSERT_TRUE(service->setName(QStringLiteral("after")));
    ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint->ready(); }, 5000));
    ASSERT_TRUE(test_support::PumpUntil(
        [&] { return endpoint->name() == QStringLiteral("after"); }, 5000));
    EXPECT_TRUE(service->stop());
  }

  EXPECT_TRUE(endpoint->stop());
}
