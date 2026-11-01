#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDataStream>
#include <QObject>
#include <QString>

#include "calculator.ddsrpc.client.h"
#include "calculator.ddsrpc.service.h"
#include "ddsrpc/client_base.h"
#include "ddsrpc/error.h"
#include "ddsrpc/wire.h"
#include "echo.ddsrpc.client.h"
#include "echo.ddsrpc.service.h"
#include "task/fibertask.h"
#include "tests/support/echo_service.hpp"
#include "tests/support/fake_host.hpp"
#include "transport/core/Message.hpp"

using namespace std::chrono_literals;

namespace {

constexpr char kEchoTopic[] = "ddsrpc.echo.events";
constexpr char kCalculatorTopic[] = "ddsrpc.Calculator.events";

transport::RetryPolicy StressRetry() { return {5000ms, 5}; }

QByteArray EncodeString(const QString& text) {
  QByteArray payload;
  QDataStream out(&payload, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  out << text;
  return payload;
}

QString DecodeString(const QByteArray& payload) {
  QDataStream in(payload);
  ddsrpc::wire::Begin(in);
  QString text;
  in >> text;
  return text;
}

QByteArray RequestPayload(quint64 schema, quint32 op,
                          const QByteArray& args = {}) {
  QByteArray payload;
  QDataStream out(&payload, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  ddsrpc::wire::WriteRequestHeader(out, schema, op);
  if (!args.isEmpty()) {
    out.writeRawData(args.constData(), args.size());
  }
  return payload;
}

struct CallOutcome {
  bool ok = false;
  std::error_code error;
  QByteArray payload;
};

CallOutcome SendRaw(transport::DdsNode& node, const std::string& service,
                    quint64 schema, const QByteArray& payload,
                    transport::RetryPolicy retry = StressRetry()) {
  transport::Message request;
  request.payload = payload;
  request.endpoint = transport::Endpoint::Service(service);
  auto reply = node.RequestForResultDirect(std::move(request), retry);
  if (!reply) return {false, reply.error(), {}};

  QDataStream in(reply.value().payload);
  ddsrpc::wire::Begin(in);
  const auto header = ddsrpc::wire::ReadReplyHeader(in);
  if (!header) return {false, header.error(), {}};
  if (header.value().schema != schema) {
    return {false, ddsrpc::make_error_code(ddsrpc::RpcErrc::kSchemaMismatch),
            {}};
  }
  if (!header.value().ok) {
    const auto remote = ddsrpc::wire::ReadError(in);
    if (!remote) return {false, remote.error(), {}};
    return {false, remote.value().code, {}};
  }

  const qint64 position = in.device() ? in.device()->pos() : 0;
  if (position < 0 || position > reply.value().payload.size()) {
    return {false, ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode), {}};
  }
  return {true, {},
          reply.value().payload.mid(static_cast<int>(position))};
}

bool PublishEvent(transport::DdsNode& node, const std::string& topic,
                  quint64 schema, ddsrpc::wire::EventKind kind, quint32 id,
                  quint64 revision, const QByteArray& payload = {}) {
  QByteArray bytes;
  QDataStream out(&bytes, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  ddsrpc::wire::WriteEventHeader(out, schema, kind, id, revision);
  if (!payload.isEmpty()) {
    out.writeRawData(payload.constData(), payload.size());
  }
  if (out.status() != QDataStream::Ok) return false;

  transport::Message message;
  message.payload = bytes;
  message.endpoint = transport::Endpoint::Topic(topic);
  return static_cast<bool>(node.Publish(std::move(message)));
}

struct EncodedRequest {
  QByteArray payload;
  QString expected_text;
  std::error_code expected_error;
};

EncodedRequest MakeMixedRequest(int kind, int index, const QString& large) {
  EncodedRequest request;
  switch (kind) {
    case 0:
      request.expected_text = QStringLiteral("valid-%1").arg(index);
      request.payload =
          RequestPayload(test_support::kEchoSchema, 1,
                         EncodeString(request.expected_text));
      break;
    case 1:
      request.expected_text = large;
      request.payload =
          RequestPayload(test_support::kEchoSchema, 1,
                         EncodeString(request.expected_text));
      break;
    case 2:
      request.payload = QByteArray("bad!");
      request.expected_error =
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol);
      break;
    case 3: {
      QDataStream out(&request.payload, QIODevice::WriteOnly);
      ddsrpc::wire::Begin(out);
      out << ddsrpc::wire::kMagic
          << static_cast<quint16>(ddsrpc::wire::kVersion + 1)
          << test_support::kEchoSchema << quint32(1);
      request.expected_error =
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol);
      break;
    }
    case 4: {
      QDataStream out(&request.payload, QIODevice::WriteOnly);
      ddsrpc::wire::Begin(out);
      out << ddsrpc::wire::kMagic << ddsrpc::wire::kVersion
          << test_support::kEchoSchema;  // op missing
      request.expected_error =
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol);
      break;
    }
    case 5:
      request.payload = RequestPayload(
          test_support::kEchoSchema ^ 0x1u, 1,
          EncodeString(QStringLiteral("wrong-schema")));
      request.expected_error =
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kSchemaMismatch);
      break;
    case 6:
      request.payload = RequestPayload(test_support::kEchoSchema, 999);
      request.expected_error =
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidArgument);
      break;
    default:
      request.payload =
          RequestPayload(test_support::kEchoSchema, 1, QByteArray("\x01", 1));
      request.expected_error =
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode);
      break;
  }
  return request;
}

class StressEchoService final : public testgen::EchoService {
 public:
  using EchoService::EchoService;

  quint64 ddsrpc_schema() const { return this->schema_hash(); }

 protected:
  Coro::Result<QString> echo(QString text) override { return text; }
};

class ThrowingService final : public ddsrpc::ServiceBase {
 public:
  explicit ThrowingService(transport::DdsNode& node)
      : ddsrpc::ServiceBase(
            node, ddsrpc::ServiceConfig{"throwing", "ddsrpc.throwing.events",
                                        test_support::kEchoSchema}) {}

 protected:
  ddsrpc::OperationResult HandleOperation(quint32 op, QDataStream&) override {
    if (op == 1) throw std::runtime_error("handler boom");
    if (op == 2) return ddsrpc::OperationResult::Success();
    return ddsrpc::OperationResult::Failure(
        ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidArgument));
  }

  ddsrpc::OperationResult BuildPropertySnapshot() override {
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out << quint32(0);
    return ddsrpc::OperationResult::Success(payload);
  }
};

class StressCalcService final : public testgen::CalculatorService {
 public:
  using CalculatorService::CalculatorService;

  transport::DdsNode& dds_node() { return this->node(); }
  quint64 ddsrpc_schema() const { return this->schema_hash(); }

 protected:
  Coro::Result<int> add(int a, int b) override { return a + b; }
  Coro::Result<void> reset() override { return {}; }
  Coro::Result<void> report(QString) override { return {}; }
};

class RawClient : public ddsrpc::ClientBase {
 public:
  using ddsrpc::ClientBase::ClientBase;

  Coro::Result<QByteArray> Echo(const QByteArray& args) { return Call(1, args); }
  Coro::Result<void> Shutdown() { return StopEndpoint(); }

 protected:
  void ResetPropertiesForResync() override {}
  Coro::Result<void> ApplyPropertySnapshot(QDataStream& in) override {
    quint32 count = 0;
    in >> count;
    if (in.status() != QDataStream::Ok) {
      return ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode);
    }
    return {};
  }
  void DispatchSignal(quint32, QDataStream&) override {}
  void DispatchProperty(quint32, quint64, QDataStream&) override {}
  void EmitReadyChanged(bool) override {}
  void EmitSyncFailed(const QString&) override {}
  void EmitPendingPropertyChanges() override {}
};

}  // namespace

TEST(StabilityTest, ConcurrentGeneratedRpcCallsRemainCorrelated) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  StressEchoService service(server.node());
  testgen::EchoClient::Options options;
  options.request_retry = StressRetry();
  options.handshake_interval = 50ms;
  options.link_poll_interval = 20ms;
  testgen::EchoClient endpoint(client.node(), client.transport(), options);

  ASSERT_TRUE(service.prepare());
  ASSERT_TRUE(endpoint.prepare());
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.start());
  ASSERT_TRUE(endpoint.start());
  ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint.ready(); }, 5000));

  constexpr int kRequests = 512;
  std::vector<QString> expected;
  expected.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    expected.push_back(QStringLiteral("req-%1:%2")
                           .arg(i)
                           .arg(QString(1024, QChar('x'))));
  }

  std::vector<Coro::FiberTask<Coro::Result<QString>>> calls;
  calls.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    calls.push_back(Coro::makeTask(
        [&endpoint, text = expected[static_cast<size_t>(i)]] {
          return endpoint.echo(text);
        }));
  }

  for (int i = 0; i < kRequests; ++i) {
    auto outer = calls[static_cast<size_t>(i)].get();
    ASSERT_TRUE(outer) << outer.error().message();
    ASSERT_TRUE(outer.value()) << outer.value().error().message();
    EXPECT_EQ(outer.value().value(), expected[static_cast<size_t>(i)]);
  }

  EXPECT_TRUE(endpoint.ready());
  auto tail = endpoint.echo(QStringLiteral("tail"));
  ASSERT_TRUE(tail) << tail.error().message();
  EXPECT_EQ(tail.value(), QStringLiteral("tail"));

  EXPECT_TRUE(endpoint.stop());
  EXPECT_TRUE(service.stop());
}

TEST(StabilityTest, MixedMalformedRequestsDoNotPoisonService) {
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

  const QString large = QString(16 * 1024, QChar('z'));
  constexpr int kKinds = 8;
  constexpr int kRequests = 320;
  std::vector<EncodedRequest> requests;
  requests.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    requests.push_back(MakeMixedRequest(i % kKinds, i, large));
  }

  std::vector<Coro::FiberTask<CallOutcome>> calls;
  calls.reserve(kRequests);
  for (const EncodedRequest& request : requests) {
    calls.push_back(Coro::makeTask(
        [&node = client.node(), payload = request.payload] {
          return SendRaw(node, "echo", test_support::kEchoSchema, payload,
                         StressRetry());
        }));
  }

  for (int i = 0; i < kRequests; ++i) {
    auto outer = calls[static_cast<size_t>(i)].get();
    ASSERT_TRUE(outer) << outer.error().message();
    const CallOutcome& outcome = outer.value();
    const EncodedRequest& request = requests[static_cast<size_t>(i)];
    if (request.expected_error) {
      EXPECT_FALSE(outcome.ok) << "index " << i;
      EXPECT_EQ(outcome.error, request.expected_error) << "index " << i;
    } else {
      ASSERT_TRUE(outcome.ok) << "index " << i;
      EXPECT_EQ(DecodeString(outcome.payload), request.expected_text)
          << "index " << i;
    }
  }

  EXPECT_TRUE(service.running());
  auto tail = SendRaw(client.node(), "echo", test_support::kEchoSchema,
                      RequestPayload(test_support::kEchoSchema, 1,
                                     EncodeString(QStringLiteral("after"))));
  ASSERT_TRUE(tail.ok) << tail.error.message();
  EXPECT_EQ(DecodeString(tail.payload), QStringLiteral("after"));

  ASSERT_TRUE(service.Stop());
}

TEST(StabilityTest, RandomMalformedRequestBurstIsContained) {
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

  constexpr int kRequests = 256;
  std::mt19937 rng(0xD05C0D3u);
  std::uniform_int_distribution<int> length_dist(0, 255);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::vector<QByteArray> payloads;
  payloads.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    QByteArray payload;
    const int length = length_dist(rng);
    payload.reserve(length);
    for (int j = 0; j < length; ++j) {
      payload.append(static_cast<char>(byte_dist(rng)));
    }
    payloads.push_back(std::move(payload));
  }

  std::vector<Coro::FiberTask<CallOutcome>> calls;
  calls.reserve(kRequests);
  for (const QByteArray& payload : payloads) {
    calls.push_back(Coro::makeTask(
        [&node = client.node(), payload] {
          return SendRaw(node, "echo", test_support::kEchoSchema, payload,
                         StressRetry());
        }));
  }

  for (int i = 0; i < kRequests; ++i) {
    auto outer = calls[static_cast<size_t>(i)].get();
    ASSERT_TRUE(outer) << outer.error().message();
    EXPECT_FALSE(outer.value().ok) << "index " << i;
    EXPECT_EQ(outer.value().error,
              ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol))
        << "index " << i;
  }

  auto tail = SendRaw(client.node(), "echo", test_support::kEchoSchema,
                      RequestPayload(test_support::kEchoSchema, 1,
                                     EncodeString(QStringLiteral("after"))));
  ASSERT_TRUE(tail.ok) << tail.error.message();
  EXPECT_EQ(DecodeString(tail.payload), QStringLiteral("after"));
  EXPECT_TRUE(service.running());

  ASSERT_TRUE(service.Stop());
}

TEST(StabilityTest, GeneratedServiceRejectsMalformedArguments) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  StressEchoService service(server.node());

  ASSERT_TRUE(service.prepare());
  ASSERT_TRUE(client.node().RegisterClients({"Echo"}));
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.start());
  ASSERT_TRUE(test_support::WaitLinkUp(client.transport()));

  const quint64 schema = service.ddsrpc_schema();
  auto empty = SendRaw(client.node(), "Echo", schema, QByteArray());
  EXPECT_FALSE(empty.ok);
  EXPECT_EQ(empty.error, ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol));

  auto truncated_arg =
      SendRaw(client.node(), "Echo", schema,
              RequestPayload(schema, 1, QByteArray("\x01", 1)));
  EXPECT_FALSE(truncated_arg.ok);
  EXPECT_EQ(truncated_arg.error,
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode));

  auto valid = SendRaw(client.node(), "Echo", schema,
                       RequestPayload(schema, 1,
                                      EncodeString(QStringLiteral("ok"))));
  ASSERT_TRUE(valid.ok) << valid.error.message();
  EXPECT_EQ(DecodeString(valid.payload), QStringLiteral("ok"));
  EXPECT_TRUE(service.running());

  ASSERT_TRUE(service.Stop());
}

TEST(StabilityTest, HandlerExceptionsAreIsolatedUnderLoad) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  ThrowingService service(server.node());

  ASSERT_TRUE(service.Prepare());
  ASSERT_TRUE(client.node().RegisterClients({"throwing"}));
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.Start());
  ASSERT_TRUE(test_support::WaitLinkUp(client.transport()));

  constexpr int kRequests = 64;
  std::vector<Coro::FiberTask<CallOutcome>> calls;
  calls.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    const quint32 op = i % 2 == 0 ? 1 : 2;
    const QByteArray payload = RequestPayload(test_support::kEchoSchema, op);
    calls.push_back(Coro::makeTask(
        [&node = client.node(), payload] {
          return SendRaw(node, "throwing", test_support::kEchoSchema, payload,
                         StressRetry());
        }));
  }

  for (int i = 0; i < kRequests; ++i) {
    auto outer = calls[static_cast<size_t>(i)].get();
    ASSERT_TRUE(outer) << outer.error().message();
    const CallOutcome& outcome = outer.value();
    if (i % 2 == 0) {
      EXPECT_FALSE(outcome.ok) << "index " << i;
      EXPECT_EQ(outcome.error,
                ddsrpc::make_error_code(ddsrpc::RpcErrc::kRemoteError))
          << "index " << i;
    } else {
      EXPECT_TRUE(outcome.ok) << "index " << i;
    }
  }

  EXPECT_TRUE(service.running());
  auto tail = SendRaw(client.node(), "throwing", test_support::kEchoSchema,
                      RequestPayload(test_support::kEchoSchema, 2));
  EXPECT_TRUE(tail.ok) << tail.error.message();

  ASSERT_TRUE(service.Stop());
}

TEST(StabilityTest, BurstsOfPropertiesAndSignalsWithConcurrentRpc) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  StressCalcService service(server.node());
  testgen::CalculatorClient::Options options;
  options.request_retry = StressRetry();
  options.handshake_interval = 50ms;
  options.link_poll_interval = 20ms;
  testgen::CalculatorClient endpoint(client.node(), client.transport(),
                                     options);

  ASSERT_TRUE(service.prepare());
  ASSERT_TRUE(endpoint.prepare());
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.start());
  ASSERT_TRUE(endpoint.start());
  ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint.ready(); }, 5000));

  std::atomic_int calculated{0};
  QObject::connect(&endpoint, &testgen::CalculatorClient::calculated,
                   [&calculated](int) { calculated.fetch_add(1); });

  constexpr int kAdds = 96;
  constexpr int kEvents = 256;
  std::atomic_bool flood_ok{true};
  auto flood = Coro::makeTask([&] {
    for (int i = 0; i < kEvents; ++i) {
      auto total = service.setTotal(i);
      if (!total) {
        flood_ok.store(false);
        return;
      }
      auto signal = service.publishCalculated(i);
      if (!signal) {
        flood_ok.store(false);
        return;
      }
      Coro::msleep(1);
    }
  });

  std::vector<Coro::FiberTask<Coro::Result<int>>> adds;
  adds.reserve(kAdds);
  for (int i = 0; i < kAdds; ++i) {
    adds.push_back(Coro::makeTask(
        [&endpoint, i] { return endpoint.add(i, i + 1); }));
  }

  auto flood_result = flood.get();
  ASSERT_TRUE(flood_result) << flood_result.error().message();
  EXPECT_TRUE(flood_ok.load());

  for (int i = 0; i < kAdds; ++i) {
    auto outer = adds[static_cast<size_t>(i)].get();
    ASSERT_TRUE(outer) << outer.error().message();
    ASSERT_TRUE(outer.value()) << outer.value().error().message();
    EXPECT_EQ(outer.value().value(), 2 * i + 1);
  }

  EXPECT_TRUE(test_support::PumpUntil(
      [&] {
        return endpoint.total() == kEvents - 1 &&
               calculated.load() == kEvents;
      },
      5000));
  EXPECT_EQ(endpoint.total(), kEvents - 1);
  EXPECT_EQ(calculated.load(), kEvents);
  EXPECT_TRUE(endpoint.ready());

  EXPECT_TRUE(endpoint.stop());
  EXPECT_TRUE(service.stop());
}

TEST(StabilityTest, MalformedEventPayloadsDoNotStopDispatch) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  StressCalcService service(server.node());
  testgen::CalculatorClient endpoint(client.node(), client.transport());

  ASSERT_TRUE(service.prepare());
  ASSERT_TRUE(endpoint.prepare());
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(service.start());
  ASSERT_TRUE(endpoint.start());
  ASSERT_TRUE(test_support::PumpUntil([&] { return endpoint.ready(); }, 5000));

  std::atomic_int calculated{0};
  std::atomic_int sync_failures{0};
  QObject::connect(&endpoint, &testgen::CalculatorClient::calculated,
                   [&calculated](int) { calculated.fetch_add(1); });
  QObject::connect(&endpoint, &testgen::CalculatorClient::syncFailed,
                   [&sync_failures](const QString&) {
                     sync_failures.fetch_add(1);
                   });

  const quint64 schema = service.ddsrpc_schema();
  constexpr int kMalformed = 128;
  for (int i = 0; i < kMalformed; ++i) {
    ASSERT_TRUE(PublishEvent(service.dds_node(), kCalculatorTopic, schema,
                             ddsrpc::wire::EventKind::kProperty, 1,
                             static_cast<quint64>(i + 1)));
    ASSERT_TRUE(PublishEvent(service.dds_node(), kCalculatorTopic, schema,
                             ddsrpc::wire::EventKind::kSignal, 1,
                             static_cast<quint64>(i + 1)));
  }

  ASSERT_TRUE(PublishEvent(service.dds_node(), kCalculatorTopic, schema,
                           ddsrpc::wire::EventKind::kProperty, 1, 1000,
                           [] {
                             QByteArray payload;
                             QDataStream out(&payload, QIODevice::WriteOnly);
                             ddsrpc::wire::Begin(out);
                             out << 123;
                             return payload;
                           }()));
  ASSERT_TRUE(PublishEvent(service.dds_node(), kCalculatorTopic, schema,
                           ddsrpc::wire::EventKind::kSignal, 1, 1000,
                           [] {
                             QByteArray payload;
                             QDataStream out(&payload, QIODevice::WriteOnly);
                             ddsrpc::wire::Begin(out);
                             out << 77;
                             return payload;
                           }()));

  ASSERT_TRUE(test_support::PumpUntil(
      [&] { return endpoint.total() == 123 && calculated.load() == 1; },
      5000));
  EXPECT_TRUE(endpoint.ready());
  EXPECT_EQ(sync_failures.load(), 0);

  EXPECT_TRUE(endpoint.stop());
  EXPECT_TRUE(service.stop());
}

TEST(StabilityTest, MalformedRepliesDoNotPoisonClient) {
  test_support::FakeDdsFixture fixture;
  test_support::Host server(fixture, "server");
  test_support::Host client(fixture, "client");
  const ddsrpc::ClientConfig config{
      "echo", kEchoTopic, test_support::kEchoSchema, StressRetry(), 50ms};
  RawClient endpoint(client.node(), config);

  ASSERT_TRUE(endpoint.Prepare());
  ASSERT_TRUE(server.node().RegisterServices({"echo"}));
  ASSERT_TRUE(server.start_node());
  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(test_support::WaitLinkUp(client.transport()));

  auto served = server.node().ServeRequests("echo");
  ASSERT_TRUE(served) << served.error().message();
  auto ticket = std::make_shared<transport::DdsNode::Ticket>(
      std::move(served).value());

  constexpr int kReplies = 120;
  auto responder = Coro::makeTask([ticket, &server] {
    for (int i = 0; i < kReplies; ++i) {
      auto request = ticket->Wait();
      if (!request) return;
      QDataStream in(request.value().payload);
      ddsrpc::wire::Begin(in);
      const auto header = ddsrpc::wire::ReadRequestHeader(in);
      if (!header) return;

      QByteArray bytes;
      QDataStream out(&bytes, QIODevice::WriteOnly);
      ddsrpc::wire::Begin(out);
      switch (i % 6) {
        case 0:
        case 5:
          ddsrpc::wire::WriteReplyHeader(out, test_support::kEchoSchema,
                                         header.value().op, true);
          out << QStringLiteral("reply-%1").arg(i);
          break;
        case 1:
          bytes = QByteArray("bad-reply");
          break;
        case 2:
          ddsrpc::wire::WriteReplyHeader(out,
                                         test_support::kEchoSchema ^ 0x1u,
                                         header.value().op, true);
          out << QStringLiteral("wrong-schema");
          break;
        case 3:
          ddsrpc::wire::WriteReplyHeader(out, test_support::kEchoSchema,
                                         header.value().op + 1, true);
          out << QStringLiteral("wrong-op");
          break;
        default:
          ddsrpc::wire::WriteReplyHeader(out, test_support::kEchoSchema,
                                         header.value().op, false);
          out << quint32(1);  // truncated error body
          break;
      }

      transport::Message reply;
      reply.payload = bytes;
      (void)server.node().Reply(request.value(), std::move(reply));
    }
  });

  for (int i = 0; i < kReplies; ++i) {
    const auto reply = endpoint.Echo(
        EncodeString(QStringLiteral("request-%1").arg(i)));
    switch (i % 6) {
      case 0:
      case 5:
        ASSERT_TRUE(reply) << "index " << i << ": "
                           << reply.error().message();
        EXPECT_EQ(DecodeString(reply.value()),
                  QStringLiteral("reply-%1").arg(i));
        break;
      case 1:
        ASSERT_FALSE(reply);
        EXPECT_EQ(reply.error(),
                  ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol));
        break;
      case 2:
        ASSERT_FALSE(reply);
        EXPECT_EQ(reply.error(),
                  ddsrpc::make_error_code(ddsrpc::RpcErrc::kSchemaMismatch));
        break;
      case 3:
        ASSERT_FALSE(reply);
        EXPECT_EQ(reply.error(),
                  ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol));
        break;
      default:
        ASSERT_FALSE(reply);
        EXPECT_EQ(reply.error(),
                  ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode));
        break;
    }
  }

  auto responder_result = responder.get();
  ASSERT_TRUE(responder_result) << responder_result.error().message();

  EXPECT_TRUE(endpoint.Shutdown());
}
