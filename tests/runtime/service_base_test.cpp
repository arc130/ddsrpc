#include <gtest/gtest.h>

#include <QByteArray>
#include <QDataStream>
#include <QString>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "await/awaitable.hpp"
#include "ddsrpc/wire.h"
#include "task/fibertask.h"
#include "tests/support/echo_service.hpp"
#include "tests/support/fake_host.hpp"

using namespace std::chrono_literals;

namespace {

Coro::Result<QString> EchoOnce(transport::DdsNode& node, const QString& text) {
  QByteArray args;
  QDataStream args_out(&args, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(args_out);
  args_out << text;

  QByteArray payload;
  QDataStream out(&payload, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  ddsrpc::wire::WriteRequestHeader(out, test_support::kEchoSchema, 1);
  if (!args.isEmpty()) {
    out.writeRawData(args.constData(), args.size());
  }
  if (out.status() != QDataStream::Ok) {
    return ddsrpc::make_error_code(ddsrpc::RpcErrc::kEncode);
  }

  transport::Message request;
  request.payload = payload;
  request.endpoint = transport::Endpoint::Service("echo");
  auto reply = node.RequestForResultDirect(std::move(request),
                                           transport::RetryPolicy{1000ms, 3});
  if (!reply) return reply.error();

  QDataStream in(reply.value().payload);
  ddsrpc::wire::Begin(in);
  const auto head = ddsrpc::wire::ReadReplyHeader(in);
  if (!head) return head.error();
  if (!head.value().ok) {
    const auto remote = ddsrpc::wire::ReadError(in);
    if (!remote) return remote.error();
    return remote.value().code;
  }
  QString echoed;
  in >> echoed;
  return echoed;
}

Coro::Result<void> RequestBlockedService(transport::DdsNode& node) {
  QByteArray payload;
  QDataStream out(&payload, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  ddsrpc::wire::WriteRequestHeader(out, test_support::kEchoSchema, 1);
  if (out.status() != QDataStream::Ok) {
    return ddsrpc::make_error_code(ddsrpc::RpcErrc::kEncode);
  }

  transport::Message request;
  request.payload = payload;
  request.endpoint = transport::Endpoint::Service("blocked");
  auto reply = node.RequestForResultDirect(
      std::move(request), transport::RetryPolicy{3000ms, 1});
  if (!reply) return reply.error();
  return {};
}

class BlockingService final : public ddsrpc::ServiceBase {
 public:
  explicit BlockingService(transport::DdsNode& node)
      : ddsrpc::ServiceBase(
            node, ddsrpc::ServiceConfig{"blocked", "blocked.events",
                                        test_support::kEchoSchema}) {}

  std::shared_ptr<Coro::Awaitable<void>> gate =
      std::make_shared<Coro::Awaitable<void>>();
  std::atomic_bool handler_started{false};
  std::atomic_bool interruption_observed{false};
  std::atomic_bool handler_finished{false};

 protected:
  ddsrpc::OperationResult HandleOperation(quint32, QDataStream&) override {
    handler_started.store(true);
    auto waited = gate->await();
    if (!waited) {
      interruption_observed.store(true);
      handler_finished.store(true);
      return ddsrpc::OperationResult::Failure(waited.error());
    }
    handler_finished.store(true);
    return ddsrpc::OperationResult::Success();
  }

  ddsrpc::OperationResult BuildPropertySnapshot() override {
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out << quint32(0);
    return ddsrpc::OperationResult::Success(payload);
  }
};

}  // namespace

TEST(ServiceBaseTest, AnswersRequestsAndStopsCleanly) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  test_support::EchoService service(server.node());

  ASSERT_TRUE(service.Prepare());
  ASSERT_TRUE(client.node().RegisterClients({"echo"}));
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.Start());
  ASSERT_TRUE(test_support::WaitLinkUp(client.transport()));

  QByteArray args;
  QDataStream args_out(&args, QIODevice::WriteOnly);
  args_out << QStringLiteral("hello");

  QByteArray payload;
  QDataStream payload_out(&payload, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(payload_out);
  ddsrpc::wire::WriteRequestHeader(payload_out, test_support::kEchoSchema, 1);
  payload_out.writeRawData(args.constData(), args.size());

  transport::Message req;
  req.payload = payload;
  req.endpoint = transport::Endpoint::Service("echo");
  auto reply = client.node().RequestForResultDirect(
      std::move(req), transport::RetryPolicy{1000ms, 3});
  ASSERT_TRUE(reply) << reply.error().message();

  QDataStream reply_in(reply.value().payload);
  ddsrpc::wire::Begin(reply_in);
  const auto head = ddsrpc::wire::ReadReplyHeader(reply_in);
  ASSERT_TRUE(head);
  EXPECT_TRUE(head.value().ok);
  QString echoed;
  reply_in >> echoed;
  EXPECT_EQ(echoed, QStringLiteral("hello"));

  ASSERT_TRUE(service.Stop());
}

TEST(ServiceBaseTest, ServesConcurrentRequests) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  test_support::EchoService service(server.node());

  ASSERT_TRUE(service.Prepare());
  ASSERT_TRUE(client.node().RegisterClients({"echo"}));
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.Start());
  ASSERT_TRUE(test_support::WaitLinkUp(client.transport()));

  constexpr int kRequests = 8;
  std::vector<Coro::FiberTask<Coro::Result<QString>>> calls;
  calls.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    calls.push_back(Coro::makeTask([&node = client.node(), i] {
      return EchoOnce(node, QStringLiteral("message-%1").arg(i));
    }));
  }

  for (int i = 0; i < kRequests; ++i) {
    auto outer = calls[i].get();
    ASSERT_TRUE(outer) << outer.error().message();
    ASSERT_TRUE(outer.value()) << outer.value().error().message();
    EXPECT_EQ(outer.value().value(), QStringLiteral("message-%1").arg(i));
  }

  EXPECT_TRUE(service.Stop());
}

TEST(ServiceBaseTest, StopCancelsInFlightHandlerAndJoins) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  BlockingService service(server.node());

  ASSERT_TRUE(service.Prepare());
  ASSERT_TRUE(client.node().RegisterClients({"blocked"}));
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.Start());
  ASSERT_TRUE(test_support::WaitLinkUp(client.transport()));

  auto call = Coro::makeTask(
      [&node = client.node()] { return RequestBlockedService(node); });
  ASSERT_TRUE(
      test_support::PumpUntil([&] { return service.handler_started.load(); }));

  const auto stopped_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(service.Stop());
  const auto elapsed = std::chrono::steady_clock::now() - stopped_at;

  EXPECT_LT(elapsed, 1000ms);
  EXPECT_FALSE(service.running());
  EXPECT_TRUE(service.interruption_observed.load());
  EXPECT_TRUE(service.handler_finished.load());
  EXPECT_TRUE(service.Stop());

  (void)call.cancel();
  (void)call.get();
}
