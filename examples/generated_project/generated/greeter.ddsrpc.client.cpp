#include "greeter.ddsrpc.client.h"

#include <QDebug>
#include <QIODevice>
#include <thread>
#include <utility>

#include <await/delay.hpp>
#include <boost/fiber/operations.hpp>

namespace demo {

  GreeterClient::GreeterClient(::transport::DdsNode& node, ::transport::DdsTransport& transport,
      Options options, ::QObject* parent)
      : ::QObject(parent),
        ::ddsrpc::ClientBase(node,
                             ::ddsrpc::ClientConfig{options.service_name,
                                                   "ddsrpc." + options.service_name + ".events",
                                                   0xc14962d926f40e5aULL,
                                                   options.request_retry,
                                                   options.handshake_interval}),
        transport_(transport),
        options_(::std::move(options)) {
    registerTypes();
  }

  GreeterClient::Options::Options() = default;

  ::Coro::Result<void> GreeterClient::prepare() {
    return this->Prepare();
  }

  GreeterClient::~GreeterClient() = default;

  ::Coro::Result<void> GreeterClient::start() {
    if (running_) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kInvalidState);
    }
    auto started = this->StartEvents();
    if (!started) return started;
    running_ = true;
    link_monitor_ = ::std::make_shared<::Coro::FiberTask<void>>(
      ::Coro::makeTask([this] { this->LinkMonitorLoop(); }, ::Coro::Priority::Normal,
                       ::Coro::Affinity::fixed(::std::this_thread::get_id())));
    return {};
  }

  ::Coro::Result<void> GreeterClient::stop() {
    if (!running_) return {};
    if (link_monitor_ &&
        link_monitor_fiber_ == ::boost::this_fiber::get_id()) {
      ::QMessageLogger(__FILE__, __LINE__, __func__)
        .warning("ddsrpc: stop() called from the link-monitor callback; defer it until the callback returns");
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kInvalidState);
    }
    if (link_monitor_) {
      link_monitor_->cancel();
      (void)link_monitor_->get();
      link_monitor_.reset();
    }
    auto stopped = this->StopEndpoint();
    if (!stopped) return stopped;
    running_ = false;
    this->ResetPropertyCacheToDefaults();
    return {};
  }

  ::Coro::Result<::QString> GreeterClient::greet(::QString name) {
    ::QByteArray args;
    ::QDataStream out(&args, ::QIODevice::WriteOnly);
    ::ddsrpc::wire::Begin(out);
    out << name;
    if (out.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);
    }
    auto reply = this->Call(2, args);
    if (!reply) return reply.error();
    ::QDataStream in(reply.value());
    ::ddsrpc::wire::Begin(in);
    ::QString value{};
    in >> value;
    if (in.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode);
    }
    return value;
  }

  ::Coro::Result<void> GreeterClient::bump() {
    ::QByteArray args;
    auto reply = this->Call(3, args);
    if (!reply) return reply.error();
    return {};
  }

  int GreeterClient::count() const { return count_; }

  ::Coro::Result<void> GreeterClient::pushCount(int value) {
    ::QByteArray ddsrpc_args;
    ::QDataStream ddsrpc_out(&ddsrpc_args, ::QIODevice::WriteOnly);
    ::ddsrpc::wire::Begin(ddsrpc_out);
    ddsrpc_out << value;
    if (ddsrpc_out.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);
    }
    auto ddsrpc_reply = this->Call(1, ddsrpc_args);
    if (!ddsrpc_reply) return ddsrpc_reply.error();
    ::QDataStream ddsrpc_in(ddsrpc_reply.value());
    ::ddsrpc::wire::Begin(ddsrpc_in);
    ::quint64 ddsrpc_revision = 0;
    int ddsrpc_value{};
    ddsrpc_in >> ddsrpc_revision >> ddsrpc_value;
    if (ddsrpc_in.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode);
    }
    if (ddsrpc_revision > count_revision_) {
      count_ = ddsrpc_value;
      count_revision_ = ddsrpc_revision;
      Q_EMIT countChanged(ddsrpc_value);
    }
    return {};
  }

  ::QString GreeterClient::label() const { return label_; }

  void GreeterClient::ResetPropertyCacheToDefaults() {
    count_ = 0;
    count_revision_ = 0;
    label_ = ::QString{};
    label_revision_ = 0;
    pending_property_changes_.clear();
  }

  void GreeterClient::ResetPropertiesForResync() {
    pending_property_changes_.clear();
    count_revision_ = 0;
    label_revision_ = 0;
  }

  ::Coro::Result<void> GreeterClient::ApplyPropertySnapshot(::QDataStream& in) {
    pending_property_changes_.clear();
    ::quint32 count = 0;
    in >> count;
    if (in.status() != ::QDataStream::Ok) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode);
    }
    if (count != 2) {
      return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kProtocol);
    }
    for (::quint32 ddsrpc_i = 0; ddsrpc_i < count; ++ddsrpc_i) {
      ::quint32 ddsrpc_id = 0;
      ::quint64 ddsrpc_revision = 0;
      in >> ddsrpc_id >> ddsrpc_revision;
      if (in.status() != ::QDataStream::Ok) {
        return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode);
      }
      switch (ddsrpc_id) {
        case 1: {
          int ddsrpc_value{};
          in >> ddsrpc_value;
          if (in.status() != ::QDataStream::Ok) {
            return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode);
          }
          if (ddsrpc_revision >= count_revision_) {
            count_ = ddsrpc_value;
            count_revision_ = ddsrpc_revision;
            pending_property_changes_.push_back([this, ddsrpc_value] {
              Q_EMIT countChanged(ddsrpc_value);
            });
          }
          break;
        }
        case 2: {
          ::QString ddsrpc_value{};
          in >> ddsrpc_value;
          if (in.status() != ::QDataStream::Ok) {
            return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode);
          }
          if (ddsrpc_revision >= label_revision_) {
            label_ = ddsrpc_value;
            label_revision_ = ddsrpc_revision;
            pending_property_changes_.push_back([this, ddsrpc_value] {
              Q_EMIT labelChanged(ddsrpc_value);
            });
          }
          break;
        }
        default:
          return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kProtocol);
      }
    }
    return {};
  }

  void GreeterClient::DispatchSignal(::quint32 event_id, ::QDataStream& in) {
    switch (event_id) {
      case 1: {
        int count{};
        in >> count;
        if (in.status() != ::QDataStream::Ok) return;
        Q_EMIT announced(count);
        return;
      }
      default: return;
    }
  }

  void GreeterClient::DispatchProperty(::quint32 event_id, ::quint64 revision, ::QDataStream& in) {
    switch (event_id) {
      case 1: {
        int ddsrpc_value{};
        in >> ddsrpc_value;
        if (in.status() != ::QDataStream::Ok) return;
        if (revision <= count_revision_) return;
        count_ = ddsrpc_value;
        count_revision_ = revision;
        Q_EMIT countChanged(ddsrpc_value);
        return;
      }
      case 2: {
        ::QString ddsrpc_value{};
        in >> ddsrpc_value;
        if (in.status() != ::QDataStream::Ok) return;
        if (revision <= label_revision_) return;
        label_ = ddsrpc_value;
        label_revision_ = revision;
        Q_EMIT labelChanged(ddsrpc_value);
        return;
      }
      default: return;
    }
  }

  void GreeterClient::EmitPendingPropertyChanges() {
    auto pending = ::std::move(pending_property_changes_);
    pending_property_changes_.clear();
    for (auto& ddsrpc_emit : pending) ddsrpc_emit();
  }

  void GreeterClient::EmitReadyChanged(bool ready) {
    Q_EMIT readyChanged(ready);
  }

  void GreeterClient::EmitSyncFailed(const ::QString& message) {
    Q_EMIT syncFailed(message);
  }

  void GreeterClient::LinkMonitorLoop() {
    link_monitor_fiber_ = ::boost::this_fiber::get_id();
    for (;;) {
      const auto state = transport_.CurrentLinkState();
      (void)this->HandleLinkState(state);
      const auto interval = (!this->ready() && state == ::transport::LinkState::kUp)
        ? options_.handshake_interval
        : options_.link_poll_interval;
      const auto delayed = ::Coro::delay_for(interval);
      if (!delayed) break;
    }
    link_monitor_fiber_ = ::boost::fibers::fiber::id();
  }

}
