#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QString>

#include "ddsrpc/service_base.h"

namespace test_support {

inline constexpr quint64 kEchoSchema = 0x12345678ULL;

class EchoService final : public ddsrpc::ServiceBase {
 public:
  explicit EchoService(transport::DdsNode& node)
      : ddsrpc::ServiceBase(
            node, ddsrpc::ServiceConfig{"echo", "ddsrpc.echo.events", kEchoSchema}) {}

 protected:
  ddsrpc::OperationResult HandleOperation(quint32 op, QDataStream& in) override {
    if (op != 1) {
      return ddsrpc::OperationResult::Failure(
          ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidArgument));
    }
    QString text;
    in >> text;
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out << text;
    return ddsrpc::OperationResult::Success(payload);
  }

  ddsrpc::OperationResult BuildPropertySnapshot() override {
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out << quint32(0);
    return ddsrpc::OperationResult::Success(payload);
  }
};

}  // namespace test_support
