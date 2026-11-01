#ifndef DDSRPC_GEN_CLIENT_GREETER_H
#define DDSRPC_GEN_CLIENT_GREETER_H

#include "greeter.ddsrpc.types.h"

#include <QDataStream>
#include <QObject>
#include <QString>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/fiber/fiber.hpp>
#include <ddsrpc/client_base.h>
#include <transport/io/dds/DdsTransport.hpp>

namespace demo {

  class GreeterClient : public ::QObject, public ::ddsrpc::ClientBase {
    Q_OBJECT

  public:
    struct Options {
      ::std::string service_name = "Greeter";
      ::transport::RetryPolicy request_retry{::std::chrono::milliseconds(2000), 3};
      ::std::chrono::milliseconds handshake_interval{1000};
      ::std::chrono::milliseconds link_poll_interval{100};

      Options();
    };

    explicit GreeterClient(::transport::DdsNode& node, ::transport::DdsTransport& transport,
        Options options = {}, ::QObject* parent = nullptr);
    ~GreeterClient() override;

    GreeterClient(const GreeterClient&) = delete;
    GreeterClient& operator=(const GreeterClient&) = delete;

    // Lifecycle: call stop() before destruction. The destructor
    // is defaulted: it neither calls stop(), joins fibers, nor calls
    // virtual hooks. Destroying a running client is a usage error.
    //
    // readyChanged/syncFailed may be emitted on the link-monitor
    // fiber (HandleLinkState), on the event fiber (event protocol
    // failures), or on the fiber calling refreshProperties()/stop().
    // IDL signal/property handlers run on the event fiber. Do not call
    // stop() from any of these callbacks: the link monitor would
    // cancel/join itself and the event fiber would self-join. Defer
    // stop() to the next Qt event-loop turn.
    // Those callbacks may safely call refreshProperties() or slots.
    ::Coro::Result<void> prepare();
    ::Coro::Result<void> start();
    ::Coro::Result<void> stop();

    ::Coro::Result<::QString> greet(::QString name);
    ::Coro::Result<void> bump();

    int count() const;
    ::Coro::Result<void> pushCount(int value);
    ::QString label() const;

  Q_SIGNALS:
    void announced(int count);
    void countChanged(int value);
    void labelChanged(::QString value);
    void readyChanged(bool ready);
    void syncFailed(::QString message);

  protected:
    void ResetPropertiesForResync() override;
    ::Coro::Result<void> ApplyPropertySnapshot(::QDataStream& in) override;
    void DispatchSignal(::quint32 event_id, ::QDataStream& in) override;
    void DispatchProperty(::quint32 event_id, ::quint64 revision, ::QDataStream& in) override;
    void EmitReadyChanged(bool ready) override;
    void EmitSyncFailed(const ::QString& message) override;
    void EmitPendingPropertyChanges() override;

  private:
    void LinkMonitorLoop();
    void ResetPropertyCacheToDefaults();

    int count_ = 0;
    ::quint64 count_revision_ = 0;
    ::QString label_ = {};
    ::quint64 label_revision_ = 0;
    ::std::vector<::std::function<void()>> pending_property_changes_;

    ::transport::DdsTransport& transport_;
    Options options_;
    ::std::shared_ptr<::Coro::FiberTask<void>> link_monitor_;
    ::boost::fibers::fiber::id link_monitor_fiber_{};
    bool running_ = false;
  };

}

#endif  // DDSRPC_GEN_CLIENT_GREETER_H
