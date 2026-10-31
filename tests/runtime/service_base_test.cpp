#include <gtest/gtest.h>

#include <QByteArray>
#include <QDataStream>
#include <QString>
#include <chrono>
#include <utility>
#include <vector>

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
