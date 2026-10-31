#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include <QByteArray>
#include <QDataStream>
#include <QString>
#include <QStringList>

#include "ddsrpc/client_base.h"
#include "ddsrpc/error.h"
#include "ddsrpc/wire.h"
#include "task/fibertask.h"
#include "tests/support/echo_service.hpp"
#include "tests/support/fake_host.hpp"
#include "transport/core/Error.hpp"

using namespace std::chrono_literals;

namespace {

constexpr char kEventTopic[] = "ddsrpc.echo.events";

ddsrpc::ClientConfig EchoConfig(quint64 schema = test_support::kEchoSchema) {
  return ddsrpc::ClientConfig{"echo", kEventTopic, schema,
                              transport::RetryPolicy{1000ms, 3}, 100ms};
}

QByteArray EncodeString(const QString& text) {
  QByteArray args;
  QDataStream out(&args, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  out << text;
  return args;
}

QByteArray EncodeInt(int value) {
  QByteArray args;
  QDataStream out(&args, QIODevice::WriteOnly);
  ddsrpc::wire::Begin(out);
  out << value;
  return args;
}

QString DecodeString(const QByteArray& payload) {
  QDataStream in(payload);
  ddsrpc::wire::Begin(in);
  QString text;
  in >> text;
  return text;
}

bool PublishEvent(transport::DdsNode& node, quint64 schema,
                  ddsrpc::wire::EventKind kind, quint32 id, quint64 revision,
                  const QByteArray& payload = {}) {
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
  message.endpoint = transport::Endpoint::Topic(kEventTopic);
  return static_cast<bool>(node.Publish(std::move(message)));
}

class BareClient : public ddsrpc::ClientBase {
 public:
  using ddsrpc::ClientBase::ClientBase;

  Coro::Result<QByteArray> Echo(const QByteArray& args) { return Call(1, args); }
  Coro::Result<QByteArray> UnknownOp(const QByteArray& args) {
    return Call(99, args);
  }
  Coro::Result<void> Sync() { return HandleLinkState(transport::LinkState::kUp); }
  Coro::Result<void> Down() {
    return HandleLinkState(transport::LinkState::kDown);
  }
  Coro::Result<void> StartEventLoop() { return StartEvents(); }
  Coro::Result<void> StopEventLoop() { return StopEvents(); }
  Coro::Result<void> Shutdown() { return StopEndpoint(); }
  Coro::Result<void> Refresh() { return refreshProperties(); }

  int reset_count = 0;
  int signal_count = 0;
  int property_count = 0;
  quint32 last_signal_id = 0;
  quint32 last_property_id = 0;
  quint64 last_property_revision = 0;
  int last_property_value = -1;
  int sync_failed_count = 0;
  QStringList sync_failures;
  std::vector<bool> ready_states;

 protected:
  void ResetPropertiesForResync() override { ++reset_count; }

  Coro::Result<void> ApplyPropertySnapshot(QDataStream& in) override {
    quint32 count = 0;
    in >> count;
    if (in.status() != QDataStream::Ok) {
      return ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode);
    }
    EXPECT_EQ(count, 0u);
    return {};
  }

  void DispatchSignal(quint32 event_id, QDataStream&) override {
    ++signal_count;
    last_signal_id = event_id;
  }

  void DispatchProperty(quint32 event_id, quint64 revision,
                        QDataStream& in) override {
    ++property_count;
    last_property_id = event_id;
    last_property_revision = revision;
    in >> last_property_value;
  }

  void EmitReadyChanged(bool ready) override { ready_states.push_back(ready); }

  void EmitSyncFailed(const QString& message) override {
    ++sync_failed_count;
    sync_failures.push_back(message);
  }
};

struct EchoEnv {
  explicit EchoEnv(quint64 client_schema = test_support::kEchoSchema)
      : bare(client.node(), EchoConfig(client_schema)) {}

  test_support::FakeDdsFixture fixture;
  test_support::Host server{fixture, "server"};
  test_support::Host client{fixture, "client"};
  test_support::EchoService service{server.node()};
  BareClient bare;
};

bool StartEchoEnv(EchoEnv& env) {
  if (!env.service.Prepare()) return false;
  if (!env.bare.Prepare()) return false;
  if (!env.server.start_node()) return false;
  if (!env.client.start_node()) return false;
  if (!env.service.Start()) return false;
  return test_support::WaitLinkUp(env.client.transport());
}

}  // namespace

TEST(ClientBaseTest, RegistersCallsAndHandshakes) {
  EchoEnv env;
  ASSERT_TRUE(StartEchoEnv(env));

  ASSERT_TRUE(env.bare.Sync());
  EXPECT_TRUE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 1);
  EXPECT_EQ(env.bare.sync_failed_count, 0);
  ASSERT_EQ(env.bare.ready_states.size(), 1u);
  EXPECT_TRUE(env.bare.ready_states.back());

  const auto reply = env.bare.Echo(EncodeString(QStringLiteral("hello")));
  ASSERT_TRUE(reply) << reply.error().message();
  EXPECT_EQ(DecodeString(reply.value()), QStringLiteral("hello"));
  EXPECT_EQ(env.bare.reset_count, 1);

  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_FALSE(env.bare.ready());
  ASSERT_GE(env.bare.ready_states.size(), 2u);
  EXPECT_FALSE(env.bare.ready_states.back());

  // stop is idempotent
  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_TRUE(env.service.Stop());
}

TEST(ClientBaseTest, LinkLossClearsReadyAndNextUpResyncs) {
  EchoEnv env;
  ASSERT_TRUE(StartEchoEnv(env));

  ASSERT_TRUE(env.bare.Sync());
  ASSERT_TRUE(env.bare.Down());
  EXPECT_FALSE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 1);
  ASSERT_EQ(env.bare.ready_states.size(), 2u);
  EXPECT_TRUE(env.bare.ready_states[0]);
  EXPECT_FALSE(env.bare.ready_states[1]);

  // Staying down is not a state transition.
  ASSERT_TRUE(env.bare.Down());
  ASSERT_EQ(env.bare.ready_states.size(), 2u);

  // Rising edge renews events, resets properties and handshakes again.
  ASSERT_TRUE(env.bare.Sync());
  EXPECT_TRUE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 2);
  ASSERT_EQ(env.bare.ready_states.size(), 3u);
  EXPECT_TRUE(env.bare.ready_states.back());

  const auto reply = env.bare.Echo(EncodeString(QStringLiteral("again")));
  ASSERT_TRUE(reply) << reply.error().message();
  EXPECT_EQ(DecodeString(reply.value()), QStringLiteral("again"));

  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_TRUE(env.service.Stop());
}

TEST(ClientBaseTest, RemoteErrorIsReconstructedAndRecorded) {
  EchoEnv env;
  ASSERT_TRUE(StartEchoEnv(env));
  ASSERT_TRUE(env.bare.Sync());

  const auto failed = env.bare.UnknownOp(EncodeString(QStringLiteral("x")));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidArgument));
  EXPECT_TRUE(env.bare.lastErrorText().contains(QStringLiteral("invalid argument")))
      << env.bare.lastErrorText().toStdString();

  // A successful call does not make the client unready.
  const auto reply = env.bare.Echo(EncodeString(QStringLiteral("ok")));
  ASSERT_TRUE(reply) << reply.error().message();
  EXPECT_TRUE(env.bare.ready());

  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_TRUE(env.service.Stop());
}

TEST(ClientBaseTest, SchemaMismatchBlocksUntilRefreshedOrLinkRises) {
  EchoEnv env(test_support::kEchoSchema ^ 0x5A5Au);
  ASSERT_TRUE(StartEchoEnv(env));

  const auto first = env.bare.Sync();
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kSchemaMismatch));
  EXPECT_FALSE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 1);
  EXPECT_EQ(env.bare.sync_failed_count, 1);
  EXPECT_FALSE(env.bare.lastErrorText().isEmpty());
  EXPECT_TRUE(env.bare.lastErrorText().contains(QStringLiteral("schema")))
      << env.bare.lastErrorText().toStdString();

  // Automatic handshakes are blocked; the link monitor's next poll is a no-op.
  ASSERT_TRUE(env.bare.Sync());
  EXPECT_FALSE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 1);
  EXPECT_EQ(env.bare.sync_failed_count, 1);

  // Explicit refresh clears the block and retries, reporting the failure again.
  const auto refreshed = env.bare.Refresh();
  ASSERT_FALSE(refreshed);
  EXPECT_EQ(refreshed.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kSchemaMismatch));
  EXPECT_FALSE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 2);
  EXPECT_EQ(env.bare.sync_failed_count, 2);

  // The next rising edge clears the block as well.
  ASSERT_TRUE(env.bare.Down());
  const auto after_down = env.bare.Sync();
  ASSERT_FALSE(after_down);
  EXPECT_EQ(after_down.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kSchemaMismatch));
  EXPECT_FALSE(env.bare.ready());
  EXPECT_EQ(env.bare.reset_count, 3);
  EXPECT_EQ(env.bare.sync_failed_count, 3);

  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_TRUE(env.service.Stop());
}

TEST(ClientBaseTest, TransientHandshakeFailureDoesNotBlock) {
  test_support::FakeDdsFixture fixture;
  test_support::Host client(fixture, "client");
  BareClient bare(client.node(), EchoConfig());

  ASSERT_TRUE(bare.Prepare());
  ASSERT_TRUE(client.start_node());

  const auto first = bare.Sync();
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(),
            transport::make_error_code(transport::TransportErrc::kTimeout));
  EXPECT_FALSE(bare.ready());
  EXPECT_EQ(bare.reset_count, 1);
  EXPECT_EQ(bare.sync_failed_count, 0);
  EXPECT_TRUE(bare.lastErrorText().isEmpty());

  // Not blocked: the next poll actually retries and times out again.
  const auto second = bare.Sync();
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error(),
            transport::make_error_code(transport::TransportErrc::kTimeout));
  EXPECT_EQ(bare.sync_failed_count, 0);

  EXPECT_TRUE(bare.Shutdown());
}

TEST(ClientBaseTest, PrepareRollsBackRegistrationWhenSubscriberFails) {
  test_support::FakeDdsFixture fixture;
  test_support::Host client(fixture, "client");
  BareClient bad(client.node(),
                 ddsrpc::ClientConfig{"echo", "cfg.events",
                                      test_support::kEchoSchema,
                                      transport::RetryPolicy{1000ms, 3}, 100ms});

  EXPECT_FALSE(bad.Prepare());

  // The client registration must be gone; otherwise this same-node service
  // registration would be rejected as a self-request.
  EXPECT_TRUE(client.node().RegisterServices({"echo"}));
}

TEST(ClientBaseTest, PrepareAndEventLifecycleIsIdempotent) {
  test_support::FakeDdsFixture fixture;
  test_support::Host client(fixture, "client");
  BareClient bare(client.node(), EchoConfig());

  // Starting events requires a successful prepare.
  const auto unprepared = bare.StartEventLoop();
  EXPECT_FALSE(unprepared);
  EXPECT_EQ(unprepared.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidState));

  ASSERT_TRUE(bare.Prepare());
  ASSERT_TRUE(bare.Prepare());

  ASSERT_TRUE(client.start_node());
  ASSERT_TRUE(bare.StartEventLoop());
  const auto duplicate = bare.StartEventLoop();
  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidState));

  const auto running_prepare = bare.Prepare();
  EXPECT_FALSE(running_prepare);
  EXPECT_EQ(running_prepare.error(),
            ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidState));

  EXPECT_TRUE(bare.StopEventLoop());
  EXPECT_TRUE(bare.StopEventLoop());
  EXPECT_TRUE(bare.Shutdown());
  EXPECT_TRUE(bare.Shutdown());
  EXPECT_FALSE(bare.ready());
}

TEST(ClientBaseTest, DispatchesEventsAndStopsEventLoop) {
  EchoEnv env;
  ASSERT_TRUE(StartEchoEnv(env));
  ASSERT_TRUE(env.bare.StartEventLoop());

  ASSERT_TRUE(PublishEvent(env.server.node(), test_support::kEchoSchema,
                           ddsrpc::wire::EventKind::kSignal, 7, 3));
  ASSERT_TRUE(test_support::PumpUntil([&] { return env.bare.signal_count == 1; }));
  EXPECT_EQ(env.bare.last_signal_id, 7u);

  ASSERT_TRUE(PublishEvent(env.server.node(), test_support::kEchoSchema,
                           ddsrpc::wire::EventKind::kProperty, 9, 5,
                           EncodeInt(42)));
  ASSERT_TRUE(
      test_support::PumpUntil([&] { return env.bare.property_count == 1; }));
  EXPECT_EQ(env.bare.last_property_id, 9u);
  EXPECT_EQ(env.bare.last_property_revision, 5u);
  EXPECT_EQ(env.bare.last_property_value, 42);

  // After StopEvents the subscription no longer dispatches.
  ASSERT_TRUE(env.bare.StopEventLoop());
  ASSERT_TRUE(PublishEvent(env.server.node(), test_support::kEchoSchema,
                           ddsrpc::wire::EventKind::kSignal, 8, 6));
  Coro::msleep(50);
  EXPECT_EQ(env.bare.signal_count, 1);

  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_TRUE(env.service.Stop());
}

TEST(ClientBaseTest, RejectsEventSchemaMismatch) {
  EchoEnv env;
  ASSERT_TRUE(StartEchoEnv(env));
  ASSERT_TRUE(env.bare.StartEventLoop());

  ASSERT_TRUE(PublishEvent(env.server.node(),
                           test_support::kEchoSchema ^ 0x5A5Au,
                           ddsrpc::wire::EventKind::kSignal, 1, 1));
  ASSERT_TRUE(
      test_support::PumpUntil([&] { return env.bare.sync_failed_count == 1; }));
  EXPECT_EQ(env.bare.signal_count, 0);
  EXPECT_FALSE(env.bare.lastErrorText().isEmpty());
  EXPECT_TRUE(env.bare.lastErrorText().contains(QStringLiteral("schema")))
      << env.bare.lastErrorText().toStdString();

  // The loop stopped: a subsequent well-formed event is not dispatched.
  ASSERT_TRUE(PublishEvent(env.server.node(), test_support::kEchoSchema,
                           ddsrpc::wire::EventKind::kSignal, 2, 2));
  Coro::msleep(50);
  EXPECT_EQ(env.bare.signal_count, 0);

  EXPECT_TRUE(env.bare.StopEventLoop());
  EXPECT_TRUE(env.bare.Shutdown());
  EXPECT_TRUE(env.service.Stop());
}
