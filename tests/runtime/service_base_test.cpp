#include <gtest/gtest.h>

#include <QByteArray>
#include <QDataStream>
#include <chrono>

#include "ddsrpc/wire.h"
#include "tests/support/echo_service.hpp"
#include "tests/support/fake_host.hpp"

using namespace std::chrono_literals;

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
