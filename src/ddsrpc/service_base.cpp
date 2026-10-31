#include "ddsrpc/service_base.h"

#include <exception>
#include <system_error>
#include <utility>

#include "task_group.h"

namespace ddsrpc {
namespace {

std::error_code RpcError(RpcErrc error) { return make_error_code(error); }

}  // namespace

OperationResult OperationResult::Success(QByteArray payload) {
  OperationResult result;
  result.payload = std::move(payload);
  return result;
}

OperationResult OperationResult::Failure(std::error_code error, QString text) {
  OperationResult result;
  result.error = error;
  result.error_text = std::move(text);
  return result;
}

ServiceBase::ServiceBase(transport::DdsNode& node, ServiceConfig config)
    : node_(node), config_(std::move(config)) {}

ServiceBase::~ServiceBase() {
  if (running_) {
    (void)Stop();
  }
}

Coro::Result<void> ServiceBase::Prepare() {
  if (running_) return RpcError(RpcErrc::kInvalidState);
  if (prepared_) return {};
  auto services = node_.RegisterServices({config_.service_name});
  if (!services) return services.error();
  auto publishers = node_.RegisterPublishers({config_.event_topic});
  if (!publishers) {
    (void)node_.UnregisterServices({config_.service_name});
    return publishers.error();
  }
  prepared_ = true;
  return {};
}

Coro::Result<void> ServiceBase::Start() {
  if (running_ || !prepared_) return RpcError(RpcErrc::kInvalidState);

  auto requests = node_.ServeRequests(config_.service_name);
  if (!requests) return requests.error();
  requests_.emplace(std::move(requests).value());

  task_group_ = std::make_unique<TaskGroup>();
  task_group_->Start();
  accept_task_ = std::make_shared<Coro::FiberTask<void>>(
      Coro::makeTask([this] { AcceptLoop(); }));
  running_ = true;
  return {};
}

Coro::Result<void> ServiceBase::Stop() {
  if (!running_) return {};
  running_ = false;

  if (requests_ && requests_->mailbox()) {
    requests_->mailbox()->close(std::make_error_code(std::errc::interrupted));
  }
  if (accept_task_) {
    accept_task_->cancel();
    (void)accept_task_->get();
    accept_task_.reset();
  }
  Coro::Result<void> stopped;
  if (task_group_) {
    stopped = task_group_->Stop();
    task_group_.reset();
  }
  requests_.reset();
  if (!stopped) return stopped.error();
  return {};
}

bool ServiceBase::prepared() const { return prepared_; }

bool ServiceBase::running() const { return running_; }

transport::DdsNode& ServiceBase::node() { return node_; }

quint64 ServiceBase::schema_hash() const { return config_.schema_hash; }

quint64 ServiceBase::NextRevision() { return ++revision_; }

Coro::Result<void> ServiceBase::PublishEvent(wire::EventKind kind, quint32 id,
                                             quint64 revision,
                                             const QByteArray& payload) {
  QByteArray buffer;
  QDataStream out(&buffer, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteEventHeader(out, config_.schema_hash, kind, id, revision);
  if (!payload.isEmpty()) {
    out.writeRawData(payload.constData(), payload.size());
  }
  if (out.status() != QDataStream::Ok) return RpcError(RpcErrc::kEncode);

  transport::Message message;
  message.payload = buffer;
  message.endpoint = transport::Endpoint::Topic(config_.event_topic);
  return node_.Publish(std::move(message));
}

void ServiceBase::AcceptLoop() {
  while (true) {
    auto got = requests_->Wait();
    if (!got) break;
    transport::Message request = std::move(got).value();
    if (!task_group_->Add([this, request = std::move(request)]() mutable {
          HandleRequest(std::move(request));
        })) {
      break;
    }
  }
}

void ServiceBase::HandleRequest(transport::Message request) {
  QDataStream in(request.payload);
  wire::Begin(in);
  const auto header = wire::ReadRequestHeader(in);
  if (!header) {
    (void)SendReply(request, 0,
                    OperationResult::Failure(RpcError(RpcErrc::kProtocol),
                                             QStringLiteral("invalid request header")));
    return;
  }

  const quint32 op = header.value().op;
  if (header.value().schema != config_.schema_hash) {
    (void)SendReply(request, op,
                    OperationResult::Failure(RpcError(RpcErrc::kSchemaMismatch),
                                             QStringLiteral("request schema mismatch")));
    return;
  }

  OperationResult result;
  try {
    result = op == wire::kSnapshotOp ? BuildPropertySnapshot()
                                     : HandleOperation(op, in);
  } catch (const std::exception& error) {
    result = OperationResult::Failure(RpcError(RpcErrc::kRemoteError),
                                      QString::fromUtf8(error.what()));
  } catch (...) {
    result = OperationResult::Failure(RpcError(RpcErrc::kRemoteError),
                                      QStringLiteral("unknown handler exception"));
  }
  (void)SendReply(request, op, result);
}

Coro::Result<void> ServiceBase::SendReply(const transport::Message& request,
                                          quint32 op,
                                          const OperationResult& result) {
  QByteArray buffer;
  QDataStream out(&buffer, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteReplyHeader(out, config_.schema_hash, op, !result.error);
  if (result.error) {
    wire::WriteError(out, result.error, result.error_text);
  } else if (!result.payload.isEmpty()) {
    out.writeRawData(result.payload.constData(), result.payload.size());
  }
  if (out.status() != QDataStream::Ok) return RpcError(RpcErrc::kEncode);

  transport::Message reply;
  reply.payload = buffer;
  return node_.Reply(request, std::move(reply));
}

}  // namespace ddsrpc
