#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <QByteArray>
#include <QDataStream>
#include <QString>

#include "detail/result.hpp"
#include "task/fibertask.h"
#include "ddsrpc/error.h"
#include "ddsrpc/wire.h"
#include "transport/io/dds/DdsTransport.hpp"
#include "transport/node/DdsNode.hpp"

namespace ddsrpc {

/// 客户端配置。service_name / event_topic 由生成代码按 IDL 填写；
/// schema_hash 是整份 IDL 的线缆兼容性标识。
struct ClientConfig {
  std::string service_name;
  std::string event_topic;
  quint64 schema_hash = 0;
  transport::RetryPolicy request_retry{std::chrono::milliseconds(2000), 3};
  std::chrono::milliseconds handshake_interval{1000};
};

/// 生成客户端公共基类：注册、RPC、事件循环、链路状态与应用层握手。
///
/// 生命周期与 ServiceBase 对称：`Prepare()` 必须在 node.Start 之前、且只做注册；
/// `StartEvents()` 必须在 node Running 之后。链路状态由生成代码的监控协程轮询
/// `DdsTransport::CurrentLinkState()` 后传入 `HandleLinkState()`，本类不持有 transport。
///
/// ready 是原子只读标志，可从任意线程查询；其余受保护方法预期在节点执行线程的
/// fiber 中调用。纯虚函数由生成代码实现，用于属性缓存 / 信号分发 / Qt 信号发射。
class ClientBase {
 public:
  ClientBase(transport::DdsNode& node, ClientConfig config);
  virtual ~ClientBase();
  ClientBase(const ClientBase&) = delete;
  ClientBase& operator=(const ClientBase&) = delete;

  [[nodiscard]] Coro::Result<void> Prepare();
  [[nodiscard]] bool ready() const;
  [[nodiscard]] QString lastErrorText() const;
  [[nodiscard]] Coro::Result<void> refreshProperties();

 protected:
  transport::DdsNode& node();
  quint64 schema_hash() const;
  [[nodiscard]] Coro::Result<void> StartEvents();
  [[nodiscard]] Coro::Result<void> StopEvents();
  [[nodiscard]] Coro::Result<void> HandleLinkState(transport::LinkState state);
  [[nodiscard]] Coro::Result<void> StopEndpoint();
  [[nodiscard]] Coro::Result<QByteArray> Call(quint32 op, const QByteArray& args);
  [[nodiscard]] Coro::Result<QByteArray> CallWithRetry(
      quint32 op, const QByteArray& args, transport::RetryPolicy retry);

  /// 链路 Up / flush 之前清空属性 revision 基线（保留缓存值）。
  virtual void ResetPropertiesForResync() = 0;
  /// 应用 op 0 返回的全量属性快照（已定位到 payload 起始处）。
  virtual Coro::Result<void> ApplyPropertySnapshot(QDataStream& in) = 0;
  /// 分发 SIGNAL 事件；流已定位到事件 payload。
  virtual void DispatchSignal(quint32 event_id, QDataStream& in) = 0;
  /// 分发 PROP 事件；revision 来自事件头。
  virtual void DispatchProperty(quint32 event_id, quint64 revision,
                                QDataStream& in) = 0;
  virtual void EmitReadyChanged(bool ready) = 0;
  virtual void EmitSyncFailed(const QString& message) = 0;

 private:
  void EventLoop();
  [[nodiscard]] Coro::Result<void> RenewEvents();
  [[nodiscard]] Coro::Result<void> SyncOnce();
  void SetLastErrorText(const QString& text);
  void SetReady(bool ready);

  transport::DdsNode& node_;
  ClientConfig config_;
  std::optional<transport::DdsNode::Ticket> events_;
  std::shared_ptr<Coro::FiberTask<void>> event_task_;
  std::atomic_bool ready_{false};
  mutable std::mutex error_mutex_;
  QString last_error_text_;
  bool link_up_ = false;
  bool sync_blocked_ = false;
  bool prepared_ = false;
  bool running_ = false;
};

}  // namespace ddsrpc
