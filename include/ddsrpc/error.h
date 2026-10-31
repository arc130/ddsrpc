#pragma once

#include <system_error>
#include <type_traits>

namespace ddsrpc {

/// 框架错误码。
///
/// 数值一经发布保持稳定，新增错误只能追加在末尾，不能插入或重排。
enum class RpcErrc {
  kInvalidArgument = 1,
  kInvalidState,
  kConfiguration,
  kProtocol,
  kSchemaMismatch,
  kEncode,
  kDecode,
  kCancelled,
  kRemoteError,
  kInternal,
};

/// 返回 ddsrpc 错误码所属的分类。
const std::error_category& rpc_error_category() noexcept;

/// 由 RpcErrc 构造 std::error_code（支持隐式转换）。
std::error_code make_error_code(RpcErrc error) noexcept;

}  // namespace ddsrpc

namespace std {
template <>
struct is_error_code_enum<ddsrpc::RpcErrc> : true_type {};
}  // namespace std
