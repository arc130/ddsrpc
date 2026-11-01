#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "calculator.ddsrpc.client.h"
#include "calculator.ddsrpc.service.h"
#include "tests/support/fake_host.hpp"

namespace {

class SignalCalcService final : public testgen::CalculatorService {
 public:
  using CalculatorService::CalculatorService;

 protected:
  Coro::Result<int> add(int a, int b) override { return a + b; }
  Coro::Result<void> reset() override { return {}; }
  Coro::Result<void> report(QString) override { return {}; }
};

}  // namespace

TEST(E2eSignalTest, DeliversIdlSignalToClient) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  auto service = std::make_unique<SignalCalcService>(server.node());
  auto endpoint = std::make_unique<testgen::CalculatorClient>(
      client.node(), client.transport());

  ASSERT_TRUE(service->prepare());
  ASSERT_TRUE(endpoint->prepare());
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service->start());
  ASSERT_TRUE(endpoint->start());
  ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint->ready(); }, 5000));

  std::atomic_int observed{0};
  QObject::connect(endpoint.get(), &testgen::CalculatorClient::calculated,
                   [&observed](int value) { observed.store(value); });
  ASSERT_TRUE(service->publishCalculated(77));
  EXPECT_TRUE(test_support::PumpUntil(
      [&] { return observed.load() == 77; }, 3000));

  EXPECT_TRUE(endpoint->stop());
  EXPECT_TRUE(service->stop());
}
