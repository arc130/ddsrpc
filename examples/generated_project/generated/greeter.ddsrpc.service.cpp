#include "greeter.ddsrpc.service.h"

#include <QIODevice>
#include <QString>

namespace demo {

  GreeterService::GreeterService(::transport::DdsNode& node,
      Options options)
      : ::ddsrpc::ServiceBase(node,
                              ::ddsrpc::ServiceConfig{options.service_name,
                                                    "ddsrpc." + options.service_name + ".events",
                                                    0xc14962d926f40e5aULL}) {
    registerTypes();
  }

  GreeterService::Options::Options() = default;

  ::Coro::Result<void> GreeterService::prepare() {
    return this->Prepare();
  }

  ::Coro::Result<void> GreeterService::start() {
    return this->Start();
  }

  ::Coro::Result<void> GreeterService::stop() {
    return this->Stop();
  }

  int GreeterService::count() const { return count_; }

  ::Coro::Result<void> GreeterService::setCount(int value) {
    const auto revision = this->ddsrpc_set_count(value);
    if (!revision) return revision.error();
    return {};
  }

  ::Coro::Result<::quint64> GreeterService::ddsrpc_set_count(int value) {
    count_ = value;
    const auto revision = this->NextRevision();
    count_revision_ = revision;
    ::QByteArray ddsrpc_payload;
    ::QDataStream ddsrpc_out(&ddsrpc_payload, ::QIODevice::WriteOnly);
    ::ddsrpc::wire::Begin(ddsrpc_out);
    ddsrpc_out << value;
    if (ddsrpc_out.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);
    }
    const auto published = this->PublishEvent(::ddsrpc::wire::EventKind::kProperty, 1, revision, ddsrpc_payload);
    if (!published) return published.error();
    return revision;
  }

  ::QString GreeterService::label() const { return label_; }

  ::Coro::Result<void> GreeterService::setLabel(::QString value) {
    const auto revision = this->ddsrpc_set_label(value);
    if (!revision) return revision.error();
    return {};
  }

  ::Coro::Result<::quint64> GreeterService::ddsrpc_set_label(::QString value) {
    label_ = value;
    const auto revision = this->NextRevision();
    label_revision_ = revision;
    ::QByteArray ddsrpc_payload;
    ::QDataStream ddsrpc_out(&ddsrpc_payload, ::QIODevice::WriteOnly);
    ::ddsrpc::wire::Begin(ddsrpc_out);
    ddsrpc_out << value;
    if (ddsrpc_out.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);
    }
    const auto published = this->PublishEvent(::ddsrpc::wire::EventKind::kProperty, 2, revision, ddsrpc_payload);
    if (!published) return published.error();
    return revision;
  }

  ::ddsrpc::OperationResult GreeterService::HandleOperation(::quint32 op, ::QDataStream& args) {
    switch (op) {
      case 2: {
        ::QString name{};
        args >> name;
        if (args.status() != ::QDataStream::Ok) {
          return ::ddsrpc::OperationResult::Failure(
            ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode),
            ::QString::fromLatin1("Greeter::greet: failed to decode arguments"));
        }
        const auto result = this->greet(name);
        if (!result) {
          return ::ddsrpc::OperationResult::Failure(
            result.error(),
            ::QString::fromStdString(result.error().message()));
        }
        ::QByteArray payload;
        ::QDataStream out(&payload, ::QIODevice::WriteOnly);
        ::ddsrpc::wire::Begin(out);
        out << result.value();
        if (out.status() != ::QDataStream::Ok) {
          return ::ddsrpc::OperationResult::Failure(
            ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode),
            ::QString::fromLatin1("Greeter::greet: failed to encode the reply"));
        }
        return ::ddsrpc::OperationResult::Success(payload);
      }
      case 3: {
        const auto result = this->bump();
        if (!result) {
          return ::ddsrpc::OperationResult::Failure(
            result.error(),
            ::QString::fromStdString(result.error().message()));
        }
        return ::ddsrpc::OperationResult::Success();
      }
      case 1: {
        int ddsrpc_value{};
        args >> ddsrpc_value;
        if (args.status() != ::QDataStream::Ok) {
          return ::ddsrpc::OperationResult::Failure(
            ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode),
            ::QString::fromLatin1("Greeter::count: failed to decode the property value"));
        }
        const auto ddsrpc_revision = this->ddsrpc_set_count(ddsrpc_value);
        if (!ddsrpc_revision) {
          return ::ddsrpc::OperationResult::Failure(
            ddsrpc_revision.error(),
            ::QString::fromStdString(ddsrpc_revision.error().message()));
        }
        ::QByteArray ddsrpc_reply_payload;
        ::QDataStream ddsrpc_reply_out(&ddsrpc_reply_payload, ::QIODevice::WriteOnly);
        ::ddsrpc::wire::Begin(ddsrpc_reply_out);
        ddsrpc_reply_out << ddsrpc_revision.value() << ddsrpc_value;
        if (ddsrpc_reply_out.status() != ::QDataStream::Ok) {
          return ::ddsrpc::OperationResult::Failure(
            ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode),
            ::QString::fromLatin1("Greeter::count: failed to encode the property reply"));
        }
        return ::ddsrpc::OperationResult::Success(ddsrpc_reply_payload);
      }
      default:
        return ::ddsrpc::OperationResult::Failure(
          ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kInvalidArgument),
          ::QString::fromLatin1("unknown operation %1").arg(op));
    }
  }

  ::Coro::Result<void> GreeterService::publishAnnounced(int count) {
    ::QByteArray payload;
    ::QDataStream out(&payload, ::QIODevice::WriteOnly);
    ::ddsrpc::wire::Begin(out);
    out << count;
    if (out.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);
    }
    return this->PublishEvent(::ddsrpc::wire::EventKind::kSignal, 1, this->NextRevision(), payload);
  }

  ::ddsrpc::OperationResult GreeterService::BuildPropertySnapshot() {
    ::QByteArray payload;
    ::QDataStream out(&payload, ::QIODevice::WriteOnly);
    ::ddsrpc::wire::Begin(out);
    out << ::quint32(2);
    out << ::quint32(1) << count_revision_ << count_;
    out << ::quint32(2) << label_revision_ << label_;
    if (out.status() != ::QDataStream::Ok) {
      return ::ddsrpc::OperationResult::Failure(
        ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode),
        ::QString::fromLatin1("failed to encode the property snapshot"));
    }
    return ::ddsrpc::OperationResult::Success(payload);
  }

}
