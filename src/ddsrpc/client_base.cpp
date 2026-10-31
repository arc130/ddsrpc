#include "ddsrpc/client_base.h"

#include <exception>
#include <system_error>
#include <utility>

#include <QIODevice>

namespace ddsrpc {
namespace {

std::error_code RpcError(RpcErrc error) { return make_error_code(error); }

bool IsSyncBlockingError(const std::error_code& error) {
  return error == make_error_code(RpcErrc::kSchemaMismatch) ||
         error == make_error_code(RpcErrc::kProtocol);
}

QString ErrorText(const QString& context, const std::error_code& error) {
  return QStringLiteral("%1: %2")
      .arg(context, QString::fromStdString(error.message()));
}

}  // namespace

ClientBase::ClientBase(transport::DdsNode& node, ClientConfig config)
    : node_(node), config_(std::move(config)) {}

ClientBase::~ClientBase() {
  if (running_) {
    (void)StopEndpoint();
  }
}

Coro::Result<void> ClientBase::Prepare() {
  if (running_) return RpcError(RpcErrc::kInvalidState);
  if (prepared_) return {};

  auto clients = node_.RegisterClients({config_.service_name});
  if (!clients) return clients.error();

  auto subscribers = node_.RegisterSubscribers({config_.event_topic});
  if (!subscribers) {
    (void)node_.UnregisterClients({config_.service_name});
    return subscribers.error();
  }

  prepared_ = true;
  return {};
}

bool ClientBase::ready() const { return ready_.load(); }

QString ClientBase::lastErrorText() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_text_;
}

Coro::Result<void> ClientBase::StartEvents() {
  if (!prepared_ || running_) return RpcError(RpcErrc::kInvalidState);

  auto events =
      node_.Subscribe({config_.event_topic}, transport::MessageKind::kNotify);
  if (!events) return events.error();
  events_.emplace(std::move(events).value());

  event_task_ = std::make_shared<Coro::FiberTask<void>>(
      Coro::makeTask([this] { EventLoop(); }));
  running_ = true;
  return {};
}

Coro::Result<void> ClientBase::StopEvents() {
  if (!running_ && !events_ && !event_task_) return {};
  running_ = false;

  if (events_ && events_->mailbox()) {
    events_->mailbox()->close(std::make_error_code(std::errc::interrupted));
  }
  if (event_task_) {
    event_task_->cancel();
    (void)event_task_->get();
    event_task_.reset();
  }
  events_.reset();
  return {};
}

Coro::Result<void> ClientBase::HandleLinkState(transport::LinkState state) {
  if (state != transport::LinkState::kUp) {
    if (link_up_) {
      link_up_ = false;
      sync_blocked_ = false;
      SetReady(false);
    }
    return {};
  }

  if (!link_up_) {
    sync_blocked_ = false;
    auto renewed = RenewEvents();
    if (!renewed) return renewed.error();
    link_up_ = true;
    SetReady(false);
    ResetPropertiesForResync();
  }

  if (!ready_.load() && !sync_blocked_) return SyncOnce();
  return {};
}

Coro::Result<void> ClientBase::StopEndpoint() {
  auto stopped = StopEvents();
  SetReady(false);
  link_up_ = false;
  sync_blocked_ = false;
  if (!stopped) return stopped.error();
  return {};
}

Coro::Result<QByteArray> ClientBase::Call(quint32 op,
                                          const QByteArray& args) {
  return CallWithRetry(op, args, config_.request_retry);
}

Coro::Result<QByteArray> ClientBase::CallWithRetry(
    quint32 op, const QByteArray& args, transport::RetryPolicy retry) {
  QByteArray buffer;
  QDataStream out(&buffer, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteRequestHeader(out, config_.schema_hash, op);
  if (!args.isEmpty()) {
    out.writeRawData(args.constData(), args.size());
  }
  if (out.status() != QDataStream::Ok) return RpcError(RpcErrc::kEncode);

  transport::Message request;
  request.payload = buffer;
  request.endpoint = transport::Endpoint::Service(config_.service_name);
  auto reply = node_.RequestForResultDirect(std::move(request), retry);
  if (!reply) return reply.error();

  QDataStream in(reply.value().payload);
  wire::Begin(in);
  const auto header = wire::ReadReplyHeader(in);
  if (!header) {
    SetLastErrorText(ErrorText(QStringLiteral("invalid reply header"),
                               header.error()));
    return header.error();
  }
  if (header.value().schema != config_.schema_hash) {
    const QString text = QStringLiteral("reply schema mismatch");
    SetLastErrorText(text);
    return RpcError(RpcErrc::kSchemaMismatch);
  }
  if (!header.value().ok) {
    const auto remote = wire::ReadError(in);
    if (!remote) {
      SetLastErrorText(ErrorText(QStringLiteral("invalid error reply"),
                                 remote.error()));
      return remote.error();
    }
    const QString text =
        remote.value().message.isEmpty()
            ? QString::fromStdString(remote.value().code.message())
            : remote.value().message;
    SetLastErrorText(text);
    return remote.value().code;
  }

  const QByteArray& payload = reply.value().payload;
  const qint64 position = in.device() ? in.device()->pos() : 0;
  if (position < 0 || position > payload.size()) {
    return RpcError(RpcErrc::kDecode);
  }
  const int offset = static_cast<int>(position);
  return QByteArray(payload.constData() + offset, payload.size() - offset);
}

Coro::Result<void> ClientBase::refreshProperties() {
  sync_blocked_ = false;
  ResetPropertiesForResync();
  return SyncOnce();
}

transport::DdsNode& ClientBase::node() { return node_; }

quint64 ClientBase::schema_hash() const { return config_.schema_hash; }

void ClientBase::EventLoop() {
  for (;;) {
    if (!events_) return;
    auto got = events_->Wait();
    if (!got) return;

    const transport::Message& message = got.value();
    QDataStream in(message.payload);
    wire::Begin(in);
    const auto header = wire::ReadEventHeader(in);
    if (!header) {
      const QString text =
          ErrorText(QStringLiteral("invalid event header"), header.error());
      SetLastErrorText(text);
      EmitSyncFailed(text);
      return;
    }
    if (header.value().schema != config_.schema_hash) {
      const QString text = QStringLiteral("event schema mismatch");
      SetLastErrorText(text);
      EmitSyncFailed(text);
      return;
    }

    bool stop = false;
    try {
      switch (header.value().kind) {
        case wire::EventKind::kSignal:
          DispatchSignal(header.value().id, in);
          break;
        case wire::EventKind::kProperty:
          DispatchProperty(header.value().id, header.value().revision, in);
          break;
        default: {
          const QString text = QStringLiteral("unknown event kind");
          SetLastErrorText(text);
          EmitSyncFailed(text);
          stop = true;
          break;
        }
      }
    } catch (const std::exception& error) {
      // 单个事件分发失败不应毒化整条链路，继续消费后续事件。
      SetLastErrorText(QStringLiteral("event dispatch failed: %1")
                           .arg(QString::fromUtf8(error.what())));
    } catch (...) {
      SetLastErrorText(QStringLiteral("event dispatch failed"));
    }
    if (stop) return;
  }
}

Coro::Result<void> ClientBase::RenewEvents() {
  auto stopped = StopEvents();
  if (!stopped) return stopped.error();
  return StartEvents();
}

Coro::Result<void> ClientBase::SyncOnce() {
  auto synced = CallWithRetry(wire::kSnapshotOp, {},
                              transport::RetryPolicy{
                                  std::chrono::milliseconds(500), 1});
  if (!synced) {
    const std::error_code error = synced.error();
    if (IsSyncBlockingError(error)) {
      QString text = lastErrorText();
      if (text.isEmpty()) text = QString::fromStdString(error.message());
      SetLastErrorText(text);
      sync_blocked_ = true;
      EmitSyncFailed(text);
    }
    SetReady(false);
    return error;
  }

  QDataStream in(synced.value());
  wire::Begin(in);
  auto applied = ApplyPropertySnapshot(in);
  if (!applied) {
    SetReady(false);
    return applied.error();
  }

  SetReady(true);
  return {};
}

void ClientBase::SetLastErrorText(const QString& text) {
  std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_text_ = text;
}

void ClientBase::SetReady(bool ready) {
  if (ready_.exchange(ready) == ready) return;
  EmitReadyChanged(ready);
}

}  // namespace ddsrpc
