#include <gtest/gtest.h>

#include <memory>

#include "calculator.ddsrpc.client.h"
#include "calculator.ddsrpc.service.h"
#include "tests/support/fake_host.hpp"

namespace {

class PropertyCalcService final : public testgen::CalculatorService {
 public:
  using CalculatorService::CalculatorService;

 protected:
  Coro::Result<int> add(int a, int b) override { return a + b; }
  Coro::Result<void> reset() override { return {}; }
  Coro::Result<void> report(QString) override { return {}; }
};

}  // namespace

TEST(E2ePropertyTest, SnapshotsPushesAndRemoteWrites) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  auto service = std::make_unique<PropertyCalcService>(server.node());
  auto endpoint = std::make_unique<testgen::CalculatorClient>(
      client.node(), client.transport());

  ASSERT_TRUE(service->prepare());
  ASSERT_TRUE(endpoint->prepare());
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service->start());
  ASSERT_TRUE(endpoint->start());
  ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint->ready(); }, 5000));

  EXPECT_EQ(endpoint->total(), 0);
  EXPECT_EQ(endpoint->name(), QString());
  EXPECT_EQ(endpoint->mode(), testgen::Mode::Idle);

  ASSERT_TRUE(endpoint->pushTotal(10));
  ASSERT_TRUE(test_support::PumpUntil(
      [&] { return endpoint->total() == 10; }, 3000));
  ASSERT_TRUE(service->setName(QStringLiteral("demo")));
  ASSERT_TRUE(test_support::PumpUntil(
      [&] { return endpoint->name() == QStringLiteral("demo"); }, 3000));
  ASSERT_TRUE(endpoint->setMode(testgen::Mode::Running));
  ASSERT_TRUE(test_support::PumpUntil(
      [&] { return endpoint->mode() == testgen::Mode::Running; }, 3000));

  EXPECT_TRUE(endpoint->stop());
  EXPECT_TRUE(service->stop());
}
