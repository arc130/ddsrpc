#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <boost/fiber/fiber.hpp>
#include <boost/fiber/mutex.hpp>

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
/// **析构生命周期契约**：生成 Client 必须在派生类仍存活时调用 `stop()`（通常在
/// 生成的析构函数体里、基类析构之前）停止事件协程与链路监控。`~ClientBase()` 不
/// 调用任何纯虚函数、不 join fiber；若析构时事件任务/订阅仍活跃，Debug 下断言，
/// 所有构建下打日志，因为此时已经无法安全清理（任务回调仍可能引用派生对象）。
///
/// **回调重入契约**：`EmitReadyChanged` / `EmitSyncFailed` 一律在释放
/// `refresh_mutex_` 之后发射，因此信号槽可以安全地重新调用 `refreshProperties()`
/// 或发起 RPC（不会重入刷新临界区）。但不得从任何客户端信号回调
/// （`DispatchSignal` / `DispatchProperty` / `EmitReadyChanged` / `EmitSyncFailed`
/// 触发的用户代码）调用生成的 `stop()`：`stop()` 会 join 事件 fiber；从该 fiber
/// 内调用就是自 join。`StopEvents()` 会拒绝这种重入并返回 `kInvalidState`，
/// 调用方应把停止操作推迟到回调之外。
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
  /// 显式刷新属性快照。与链路监控共用 `refresh_mutex_`，两路刷新不会交错执行
  /// `ResetPropertiesForResync()` / `ApplyPropertySnapshot()`。必须在 fiber 中调用。
  /// 端点已 `StopEndpoint()` 后返回 `kInvalidState`，不会把 ready 重新置真；
  /// 再次 `StartEvents()` 后恢复可用。
  [[nodiscard]] Coro::Result<void> refreshProperties();

 protected:
  transport::DdsNode& node();
  quint64 schema_hash() const;
  [[nodiscard]] Coro::Result<void> StartEvents();
  /// 停止并 join 事件协程。若在 EventLoop 的分发回调内调用，返回 `kInvalidState`
  /// 而不是自 join；调用方必须把停止推迟到回调返回之后。
  [[nodiscard]] Coro::Result<void> StopEvents();
  [[nodiscard]] Coro::Result<void> HandleLinkState(transport::LinkState state);
  [[nodiscard]] Coro::Result<void> StopEndpoint();
  [[nodiscard]] Coro::Result<QByteArray> Call(quint32 op, const QByteArray& args);
  [[nodiscard]] Coro::Result<QByteArray> CallWithRetry(
      quint32 op, const QByteArray& args, transport::RetryPolicy retry);

  /// 链路 Up / flush 之前清空属性 revision 基线（保留缓存值）。
  virtual void ResetPropertiesForResync() = 0;
  /// 应用 op 0 返回的全量属性快照（已定位到 payload 起始处）。返回错误会记录
  /// `lastErrorText()`、发 `syncFailed` 并阻塞自动握手，直到下一次 Up / 显式 refresh。
  virtual Coro::Result<void> ApplyPropertySnapshot(QDataStream& in) = 0;
  /// 分发 SIGNAL 事件；流已定位到事件 payload。
  virtual void DispatchSignal(quint32 event_id, QDataStream& in) = 0;
  /// 分发 PROP 事件；revision 来自事件头。
  virtual void DispatchProperty(quint32 event_id, quint64 revision,
                                QDataStream& in) = 0;
  virtual void EmitReadyChanged(bool ready) = 0;
  virtual void EmitSyncFailed(const QString& message) = 0;
  /// 在释放 `refresh_mutex_` 之后由 ClientBase 调用；生成代码在此发出在
  /// ApplyPropertySnapshot 期间暂存的 `<prop>Changed` 信号。
  virtual void EmitPendingPropertyChanges() = 0;

 private:
  /// 一次状态转换中累积的通知，在释放 `refresh_mutex_` 之后统一发射。
  /// 持有刷新锁时绝不调用 `EmitReadyChanged` / `EmitSyncFailed`：用户槽可能
  /// 重入刷新入口，Boost.Fiber 会抛 `lock_error` 或死锁。
  struct PendingNotifications {
    bool ready_changed = false;
    bool ready = false;
    bool sync_failed = false;
    QString sync_failed_message;
  };

  void EventLoop();
  [[nodiscard]] Coro::Result<void> RenewEvents();
  [[nodiscard]] Coro::Result<void> SyncOnce(PendingNotifications& pending);
  void SetLastErrorText(const QString& text);
  void SetReadyLocked(bool ready, PendingNotifications& pending);
  void EmitNotifications(const PendingNotifications& pending);

  transport::DdsNode& node_;
  ClientConfig config_;
  std::optional<transport::DdsNode::Ticket> events_;
  std::shared_ptr<Coro::FiberTask<void>> event_task_;
  std::atomic_bool ready_{false};
  mutable std::mutex error_mutex_;
  QString last_error_text_;
  /// 属性刷新临界区（fiber 感知，跨 `CallWithRetry` 的让出仍持锁）。
  /// 串行化链路监控与显式 refresh 的 reset / snapshot apply。持锁期间不发射
  /// 任何客户端信号；信号统一在解锁后由 `EmitNotifications` 发出。
  boost::fibers::mutex refresh_mutex_;
  bool link_up_ = false;
  bool sync_blocked_ = false;
  bool in_event_dispatch_ = false;
  boost::fibers::fiber::id event_dispatch_fiber_;
  /// `StopEndpoint()` 成功后置真；阻止停止前排队、停止后才执行的刷新把 ready
  /// 重新置真。成功 `StartEvents()` 后清除（支持 stop 后再次 start）。
  bool stopped_ = false;
  bool prepared_ = false;
  bool running_ = false;
};

}  // namespace ddsrpc
