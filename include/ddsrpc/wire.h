#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QString>

#include <cstdint>

#include "detail/result.hpp"
#include "ddsrpc/error.h"

namespace ddsrpc::wire {

/// 信封 magic：ASCII "DRPC"。
inline constexpr quint32 kMagic = 0x44525043;
/// 当前 wire 协议版本。
inline constexpr quint16 kVersion = 1;
/// 保留给握手 / 属性快照的 op id。
inline constexpr quint32 kSnapshotOp = 0;

/// 事件类型，编码固定为 0 = SIGNAL，1 = PROP。
enum class EventKind : quint8 { kSignal = 0, kProperty = 1 };

struct RequestHeader {
  quint64 schema = 0;
  quint32 op = 0;
};

struct ReplyHeader {
  quint64 schema = 0;
  quint32 op = 0;
  bool ok = false;
};

struct EventHeader {
  quint64 schema = 0;
  EventKind kind = EventKind::kSignal;
  quint32 id = 0;
  quint64 revision = 0;
};

struct RemoteError {
  std::error_code code;
  QString message;
};

/// 统一设置 QDataStream 的协议版本与大端字节序。
void Begin(QDataStream& stream);

void WriteRequestHeader(QDataStream& stream, quint64 schema, quint32 op);
[[nodiscard]] Coro::Result<RequestHeader> ReadRequestHeader(QDataStream& stream);

void WriteReplyHeader(QDataStream& stream, quint64 schema, quint32 op, bool ok);
[[nodiscard]] Coro::Result<ReplyHeader> ReadReplyHeader(QDataStream& stream);

void WriteEventHeader(QDataStream& stream, quint64 schema, EventKind kind, quint32 id,
                      quint64 revision);
[[nodiscard]] Coro::Result<EventHeader> ReadEventHeader(QDataStream& stream);

/// 写错误体：错误码、类别标记（0 = RpcErrc，1 = 其他）、文本。
void WriteError(QDataStream& stream, const std::error_code& error, const QString& message);
[[nodiscard]] Coro::Result<RemoteError> ReadError(QDataStream& stream);

}  // namespace ddsrpc::wire
