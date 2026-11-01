#include <gtest/gtest.h>

#include <memory>

#include "echo.ddsrpc.client.h"
#include "echo.ddsrpc.service.h"
#include "tests/support/fake_host.hpp"

namespace {

class GeneratedEchoService final : public testgen::EchoService {
 public:
  using EchoService::EchoService;

 protected:
  Coro::Result<QString> echo(QString text) override { return text; }
};

}  // namespace

TEST(E2eRpcTest, CallsGeneratedSlotThroughDds) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  auto service = std::make_unique<GeneratedEchoService>(server.node());
  auto endpoint = std::make_unique<testgen::EchoClient>(
      client.node(), client.transport());

  ASSERT_TRUE(service->prepare());
  ASSERT_TRUE(endpoint->prepare());
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service->start());
  ASSERT_TRUE(endpoint->start());
  ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint->ready(); }, 5000));

  auto answer = endpoint->echo(QStringLiteral("hello"));
  ASSERT_TRUE(answer) << answer.error().message();
  EXPECT_EQ(answer.value(), QStringLiteral("hello"));

  EXPECT_TRUE(endpoint->stop());
  EXPECT_TRUE(service->stop());
}
