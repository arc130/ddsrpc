#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QDataStream>

#include "ddsrpc/error.h"
#include "ddsrpc/wire.h"

using namespace ddsrpc;

TEST(ErrorTest, CategoryRoundTripsKnownCodes) {
  const auto ec = make_error_code(RpcErrc::kSchemaMismatch);
  EXPECT_EQ(ec.category(), rpc_error_category());
  EXPECT_EQ(ec.value(), static_cast<int>(RpcErrc::kSchemaMismatch));
  EXPECT_FALSE(ec.message().empty());
}

TEST(ErrorTest, KeepsPublishedCodeValuesAndMessages) {
  EXPECT_EQ(static_cast<int>(RpcErrc::kInvalidArgument), 1);
  EXPECT_EQ(static_cast<int>(RpcErrc::kInvalidState), 2);
  EXPECT_EQ(static_cast<int>(RpcErrc::kConfiguration), 3);
  EXPECT_EQ(static_cast<int>(RpcErrc::kProtocol), 4);
  EXPECT_EQ(static_cast<int>(RpcErrc::kSchemaMismatch), 5);
  EXPECT_EQ(static_cast<int>(RpcErrc::kEncode), 6);
  EXPECT_EQ(static_cast<int>(RpcErrc::kDecode), 7);
  EXPECT_EQ(static_cast<int>(RpcErrc::kCancelled), 8);
  EXPECT_EQ(static_cast<int>(RpcErrc::kRemoteError), 9);
  EXPECT_EQ(static_cast<int>(RpcErrc::kInternal), 10);

  for (int value = 1; value <= 10; ++value) {
    const std::error_code ec(value, rpc_error_category());
    EXPECT_FALSE(ec.message().empty()) << "missing message for value " << value;
  }
  EXPECT_FALSE(std::error_code(999, rpc_error_category()).message().empty());
}

TEST(WireTest, RequestReplyAndEventHeadersRoundTrip) {
  QByteArray request;
  QDataStream out(&request, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteRequestHeader(out, 0x1122334455667788ULL, 7);

  QDataStream in(request);
  wire::Begin(in);
  const auto request_header = wire::ReadRequestHeader(in);
  ASSERT_TRUE(request_header);
  EXPECT_EQ(request_header.value().schema, 0x1122334455667788ULL);
  EXPECT_EQ(request_header.value().op, 7u);

  QByteArray reply;
  QDataStream reply_out(&reply, QIODevice::WriteOnly);
  wire::Begin(reply_out);
  wire::WriteReplyHeader(reply_out, 42, 7, false);
  wire::WriteError(reply_out, make_error_code(RpcErrc::kRemoteError), QStringLiteral("boom"));
  QDataStream reply_in(reply);
  wire::Begin(reply_in);
  const auto reply_header = wire::ReadReplyHeader(reply_in);
  ASSERT_TRUE(reply_header);
  EXPECT_FALSE(reply_header.value().ok);
  const auto error = wire::ReadError(reply_in);
  ASSERT_TRUE(error);
  EXPECT_EQ(error.value().message, QStringLiteral("boom"));

  QByteArray event;
  QDataStream event_out(&event, QIODevice::WriteOnly);
  wire::Begin(event_out);
  wire::WriteEventHeader(event_out, 99, wire::EventKind::kProperty, 3, 15);
  QDataStream event_in(event);
  wire::Begin(event_in);
  const auto event_header = wire::ReadEventHeader(event_in);
  ASSERT_TRUE(event_header);
  EXPECT_EQ(event_header.value().kind, wire::EventKind::kProperty);
  EXPECT_EQ(event_header.value().id, 3u);
  EXPECT_EQ(event_header.value().revision, 15u);
}

TEST(WireTest, ReplyHeaderCarriesSuccessStatus) {
  QByteArray reply;
  QDataStream out(&reply, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteReplyHeader(out, 77, 3, true);

  QDataStream in(reply);
  wire::Begin(in);
  const auto header = wire::ReadReplyHeader(in);
  ASSERT_TRUE(header);
  EXPECT_EQ(header.value().schema, 77u);
  EXPECT_EQ(header.value().op, 3u);
  EXPECT_TRUE(header.value().ok);
}

TEST(WireTest, ErrorEncodingPreservesRpcCategory) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteError(out, make_error_code(RpcErrc::kCancelled), QStringLiteral("stopped"));

  QDataStream in(data);
  wire::Begin(in);
  const auto error = wire::ReadError(in);
  ASSERT_TRUE(error);
  EXPECT_EQ(error.value().code, make_error_code(RpcErrc::kCancelled));
  EXPECT_EQ(error.value().message, QStringLiteral("stopped"));
}

TEST(WireTest, ForeignErrorCategoriesCollapseToRemoteError) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  wire::Begin(out);
  wire::WriteError(out, std::make_error_code(std::errc::timed_out), QStringLiteral("late"));

  QDataStream in(data);
  wire::Begin(in);
  const auto error = wire::ReadError(in);
  ASSERT_TRUE(error);
  EXPECT_EQ(error.value().code, make_error_code(RpcErrc::kRemoteError));
  EXPECT_EQ(error.value().message, QStringLiteral("late"));
}

TEST(WireTest, RejectsBadMagicAndTruncatedHeader) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  out << quint32(0xdeadbeef);
  QDataStream in(data);
  const auto header = wire::ReadRequestHeader(in);
  EXPECT_FALSE(header);
  EXPECT_EQ(header.error(), make_error_code(RpcErrc::kProtocol));
}

TEST(WireTest, RejectsUnsupportedWireVersion) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  wire::Begin(out);
  out << wire::kMagic << static_cast<quint16>(wire::kVersion + 1) << quint64(0) << quint32(0);

  QDataStream in(data);
  wire::Begin(in);
  const auto header = wire::ReadRequestHeader(in);
  EXPECT_FALSE(header);
  EXPECT_EQ(header.error(), make_error_code(RpcErrc::kProtocol));
}

TEST(WireTest, RejectsTruncatedEnvelopeAsDecodeError) {
  QByteArray empty;
  QDataStream empty_in(empty);
  const auto empty_header = wire::ReadRequestHeader(empty_in);
  EXPECT_FALSE(empty_header);
  EXPECT_EQ(empty_header.error(), make_error_code(RpcErrc::kDecode));

  QByteArray truncated;
  QDataStream out(&truncated, QIODevice::WriteOnly);
  wire::Begin(out);
  out << wire::kMagic << wire::kVersion << quint32(1);
  QDataStream in(truncated);
  wire::Begin(in);
  const auto header = wire::ReadRequestHeader(in);
  EXPECT_FALSE(header);
  EXPECT_EQ(header.error(), make_error_code(RpcErrc::kDecode));
}

TEST(WireTest, BeginEnforcesBigEndianEnvelope) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  out.setByteOrder(QDataStream::LittleEndian);
  wire::Begin(out);
  wire::WriteRequestHeader(out, 0x0102030405060708ULL, 0x11223344u);

  ASSERT_EQ(data.size(), 18);
  EXPECT_EQ(data.left(6), QByteArray::fromHex("445250430001"));
  EXPECT_EQ(data.mid(6, 8), QByteArray::fromHex("0102030405060708"));
  EXPECT_EQ(data.mid(14), QByteArray::fromHex("11223344"));
}
