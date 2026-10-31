#include "ddsrpc/wire.h"

#include <utility>

namespace ddsrpc::wire {
namespace {

std::error_code ProtocolError() { return make_error_code(RpcErrc::kProtocol); }

std::error_code DecodeError() { return make_error_code(RpcErrc::kDecode); }

void WriteEnvelopePrefix(QDataStream& stream, quint64 schema) {
  stream << kMagic << kVersion << schema;
}

/// 读取 magic、version 与 schema；magic/version 不符为 kProtocol，
/// QDataStream 读取失败（含截断）为 kDecode。
Coro::Result<void> ReadEnvelopePrefix(QDataStream& stream, quint64* schema) {
  quint32 magic = 0;
  stream >> magic;
  if (stream.status() != QDataStream::Ok) return DecodeError();
  if (magic != kMagic) return ProtocolError();

  quint16 version = 0;
  stream >> version;
  if (stream.status() != QDataStream::Ok) return DecodeError();
  if (version != kVersion) return ProtocolError();

  quint64 parsed_schema = 0;
  stream >> parsed_schema;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  *schema = parsed_schema;
  return {};
}

}  // namespace

void Begin(QDataStream& stream) {
  stream.setVersion(QDataStream::Qt_5_12);
  stream.setByteOrder(QDataStream::BigEndian);
}

void WriteRequestHeader(QDataStream& stream, quint64 schema, quint32 op) {
  WriteEnvelopePrefix(stream, schema);
  stream << op;
}

Coro::Result<RequestHeader> ReadRequestHeader(QDataStream& stream) {
  RequestHeader header;
  const auto prefix = ReadEnvelopePrefix(stream, &header.schema);
  if (!prefix) return prefix.error();

  stream >> header.op;
  if (stream.status() != QDataStream::Ok) return DecodeError();
  return header;
}

void WriteReplyHeader(QDataStream& stream, quint64 schema, quint32 op, bool ok) {
  WriteEnvelopePrefix(stream, schema);
  stream << op << static_cast<quint8>(ok ? 0 : 1);
}

Coro::Result<ReplyHeader> ReadReplyHeader(QDataStream& stream) {
  ReplyHeader header;
  const auto prefix = ReadEnvelopePrefix(stream, &header.schema);
  if (!prefix) return prefix.error();

  stream >> header.op;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  quint8 status = 0;
  stream >> status;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  header.ok = status == 0;
  return header;
}

void WriteEventHeader(QDataStream& stream, quint64 schema, EventKind kind, quint32 id,
                      quint64 revision) {
  WriteEnvelopePrefix(stream, schema);
  stream << static_cast<quint8>(kind) << id << revision;
}

Coro::Result<EventHeader> ReadEventHeader(QDataStream& stream) {
  EventHeader header;
  const auto prefix = ReadEnvelopePrefix(stream, &header.schema);
  if (!prefix) return prefix.error();

  quint8 kind = 0;
  stream >> kind;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  stream >> header.id;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  stream >> header.revision;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  header.kind = static_cast<EventKind>(kind);
  return header;
}

void WriteError(QDataStream& stream, const std::error_code& error, const QString& message) {
  const quint8 category_tag = error.category() == rpc_error_category() ? 0 : 1;
  stream << static_cast<quint32>(error.value()) << category_tag << message;
}

Coro::Result<RemoteError> ReadError(QDataStream& stream) {
  if (stream.status() != QDataStream::Ok) return DecodeError();

  quint32 code = 0;
  stream >> code;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  quint8 category_tag = 0;
  stream >> category_tag;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  QString message;
  stream >> message;
  if (stream.status() != QDataStream::Ok) return DecodeError();

  RemoteError error;
  error.code = category_tag == 0
                   ? make_error_code(static_cast<RpcErrc>(code))
                   : make_error_code(RpcErrc::kRemoteError);
  error.message = std::move(message);
  return error;
}

}  // namespace ddsrpc::wire
