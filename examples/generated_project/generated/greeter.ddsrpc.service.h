#ifndef DDSRPC_GEN_SERVICE_GREETER_H
#define DDSRPC_GEN_SERVICE_GREETER_H

#include "greeter.ddsrpc.types.h"

#include <QDataStream>
#include <string>

#include <ddsrpc/service_base.h>

namespace demo {

  class GreeterService : public ::ddsrpc::ServiceBase {
  public:
    struct Options {
      ::std::string service_name = "Greeter";

      Options();
    };

    explicit GreeterService(::transport::DdsNode& node, Options options = {});

    ::Coro::Result<void> prepare();
    ::Coro::Result<void> start();
    ::Coro::Result<void> stop();

    GreeterService(const GreeterService&) = delete;
    GreeterService& operator=(const GreeterService&) = delete;

    virtual ::Coro::Result<::QString> greet(::QString name) = 0;
    virtual ::Coro::Result<void> bump() = 0;

    ::Coro::Result<void> publishAnnounced(int count);

    int count() const;
    ::Coro::Result<void> setCount(int value);
    ::QString label() const;
    ::Coro::Result<void> setLabel(::QString value);

  protected:
    ::ddsrpc::OperationResult HandleOperation(::quint32 op, ::QDataStream& args) override;
    ::ddsrpc::OperationResult BuildPropertySnapshot() override;
  private:
    int count_ = 0;
    ::quint64 count_revision_ = 0;
    ::Coro::Result<::quint64> ddsrpc_set_count(int value);
    ::QString label_ = {};
    ::quint64 label_revision_ = 0;
    ::Coro::Result<::quint64> ddsrpc_set_label(::QString value);
  };

}

#endif  // DDSRPC_GEN_SERVICE_GREETER_H
