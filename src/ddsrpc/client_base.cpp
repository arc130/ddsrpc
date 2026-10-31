#include "ddsrpc/client_base.h"

#include <exception>
#include <mutex>
#include <system_error>
#include <utility>

#include <QDebug>
#include <QIODevice>

#include <boost/fiber/operations.hpp>

namespace ddsrpc {
namespace {

std::error_code RpcError(RpcErrc error) { return make_error_code(error); }

/// 标记事件回调所属 fiber，使同一 fiber 内的 StopEvents() 能拒绝自 join；
/// 其它 fiber（例如链路监控）仍可正常停止事件。
class ScopedEventDispatch {
 public:
  ScopedEventDispatch(bool* active, boost::fibers::fiber::id* dispatch_fiber)
      : active_(active) {
    *active_ = true;
    *dispatch_fiber = boost::this_fiber::get_id();
  }
  ~ScopedEventDispatch() { *active_ = false; }

  ScopedEventDispatch(const ScopedEventDispatch&) = delete;
  ScopedEventDispatch& operator=(const ScopedEventDispatch&) = delete;

 private:
  bool* active_;
};

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
  // 生成 Client 必须先在其派生析构函数体里 stop()。这里既不能发射纯虚信号，
  // 也不能 join 仍可能回调派生对象的 fiber；只做诊断，不做清理。
  const bool active = running_ || events_.has_value() || event_task_ != nullptr;
  if (!active) return;

  qWarning("ddsrpc::ClientBase destroyed while its event endpoint is active; "
           "call stop() on the derived client before destruction");
  Q_ASSERT_X(!active, "ClientBase::~ClientBase",
             "generated client must call stop() before base destruction");
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
  if (in_event_dispatch_ &&
      event_dispatch_fiber_ == boost::this_fiber::get_id()) {
    const QString text =
        QStringLiteral("stop() called from an event-dispatch callback; "
                       "defer it until the callback returns");
    SetLastErrorText(text);
    return RpcError(RpcErrc::kInvalidState);
  }
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
    std::unique_lock<boost::fibers::mutex> lock(refresh_mutex_);
    link_up_ = false;
    sync_blocked_ = false;
    // 即使此前没有观察到 Up（例如显式 refreshProperties() 已置 ready），
    // Down 也必须撤销 ready。
    SetReady(false);
    return {};
  }

  bool rising = false;
  {
    std::unique_lock<boost::fibers::mutex> lock(refresh_mutex_);
    rising = !link_up_;
    if (rising) {
      sync_blocked_ = false;
    } else if (!ready_.load() && !sync_blocked_) {
      return SyncOnce();
    } else {
      return {};
    }
  }

  // RenewEvents() 会 join 事件 fiber；事件回调可以合法地调用 refreshProperties()，
  // 因此不能在持有 refresh_mutex_ 时做 join，否则会与回调争锁而互等。
  auto renewed = RenewEvents();
  if (!renewed) {
    std::unique_lock<boost::fibers::mutex> lock(refresh_mutex_);
    link_up_ = false;
    SetReady(false);
    return renewed.error();
  }

  std::unique_lock<boost::fibers::mutex> lock(refresh_mutex_);
  link_up_ = true;
  sync_blocked_ = false;
  SetReady(false);
  ResetPropertiesForResync();
  if (!ready_.load() && !sync_blocked_) return SyncOnce();
  return {};
}

Coro::Result<void> ClientBase::StopEndpoint() {
  // 先停止并 join 事件 fiber，再取刷新锁：回调中的 refreshProperties() 可能在等
  // refresh_mutex_，持锁 join 会形成死锁。
  auto stopped = StopEvents();
  std::unique_lock<boost::fibers::mutex> lock(refresh_mutex_);
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
  if (header.value().op != op) {
    SetLastErrorText(QStringLiteral("reply op mismatch: requested %1, got %2")
                         .arg(op)
                         .arg(header.value().op));
    return RpcError(RpcErrc::kProtocol);
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
  std::unique_lock<boost::fibers::mutex> lock(refresh_mutex_);
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
      ScopedEventDispatch dispatching(&in_event_dispatch_,
                                      &event_dispatch_fiber_);
      SetLastErrorText(text);
      EmitSyncFailed(text);
      return;
    }
    if (header.value().schema != config_.schema_hash) {
      const QString text = QStringLiteral("event schema mismatch");
      ScopedEventDispatch dispatching(&in_event_dispatch_,
                                      &event_dispatch_fiber_);
      SetLastErrorText(text);
      EmitSyncFailed(text);
      return;
    }

    bool stop = false;
    try {
      ScopedEventDispatch dispatching(&in_event_dispatch_,
                                      &event_dispatch_fiber_);
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
    const std::error_code error = applied.error();
    const QString text =
        ErrorText(QStringLiteral("property snapshot apply failed"), error);
    SetLastErrorText(text);
    // 快照无法应用是协议/语义错误；自动重试只会重复同一份坏快照，按同步阻塞
    // 处理，等待下一次链路 Up 或显式 refreshProperties()。
    sync_blocked_ = true;
    EmitSyncFailed(text);
    SetReady(false);
    return error;
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
