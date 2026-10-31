#include "ddsrpc/error.h"

#include <string>

namespace ddsrpc {
namespace {

class RpcErrorCategory final : public std::error_category {
 public:
  const char* name() const noexcept override { return "ddsrpc.error"; }

  std::string message(int value) const override {
    switch (static_cast<RpcErrc>(value)) {
      case RpcErrc::kInvalidArgument: return "invalid argument";
      case RpcErrc::kInvalidState: return "invalid state";
      case RpcErrc::kConfiguration: return "configuration error";
      case RpcErrc::kProtocol: return "protocol error";
      case RpcErrc::kSchemaMismatch: return "schema mismatch";
      case RpcErrc::kEncode: return "encode error";
      case RpcErrc::kDecode: return "decode error";
      case RpcErrc::kCancelled: return "operation cancelled";
      case RpcErrc::kRemoteError: return "remote error";
      case RpcErrc::kInternal: return "internal error";
    }
    return "unknown ddsrpc error";
  }
};

}  // namespace

const std::error_category& rpc_error_category() noexcept {
  static const RpcErrorCategory category;
  return category;
}

std::error_code make_error_code(RpcErrc error) noexcept {
  return {static_cast<int>(error), rpc_error_category()};
}

}  // namespace ddsrpc
