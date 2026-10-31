#pragma once
#include <memory>
#include <optional>
#include <string>

#include <QByteArray>
#include <QDataStream>

#include "detail/result.hpp"
#include "task/fibertask.h"
#include "ddsrpc/error.h"
#include "ddsrpc/wire.h"
#include "transport/node/DdsNode.hpp"

namespace ddsrpc {

class TaskGroup;

struct OperationResult {
  std::error_code error;
  QString error_text;
  QByteArray payload;
  static OperationResult Success(QByteArray payload = {});
  static OperationResult Failure(std::error_code error, QString text = {});
};

struct ServiceConfig {
  std::string service_name;
  std::string event_topic;
  quint64 schema_hash = 0;
};

class ServiceBase {
 public:
  ServiceBase(transport::DdsNode& node, ServiceConfig config);
  virtual ~ServiceBase();
  ServiceBase(const ServiceBase&) = delete;
  ServiceBase& operator=(const ServiceBase&) = delete;

  [[nodiscard]] Coro::Result<void> Prepare();
  [[nodiscard]] Coro::Result<void> Start();
  [[nodiscard]] Coro::Result<void> Stop();
  [[nodiscard]] bool prepared() const;
  [[nodiscard]] bool running() const;

 protected:
  transport::DdsNode& node();
  quint64 schema_hash() const;
  quint64 NextRevision();
  [[nodiscard]] Coro::Result<void> PublishEvent(wire::EventKind kind, quint32 id,
                                                quint64 revision,
                                                const QByteArray& payload);

  virtual OperationResult HandleOperation(quint32 op, QDataStream& args) = 0;
  virtual OperationResult BuildPropertySnapshot() = 0;

 private:
  void AcceptLoop();
  void HandleRequest(transport::Message request);
  [[nodiscard]] Coro::Result<void> SendReply(const transport::Message& request,
                                             quint32 op, const OperationResult& result);

  transport::DdsNode& node_;
  ServiceConfig config_;
  std::optional<transport::DdsNode::Ticket> requests_;
  std::shared_ptr<Coro::FiberTask<void>> accept_task_;
  std::unique_ptr<TaskGroup> task_group_;
  quint64 revision_ = 0;
  bool prepared_ = false;
  bool running_ = false;
};

}  // namespace ddsrpc
