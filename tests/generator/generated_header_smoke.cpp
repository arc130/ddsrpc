// Compile/link smoke test for generated service and client headers. The
// generated sources exercise QDataStream emission for every accepted builtin
// and container type; AUTOMOC processes the generated Q_OBJECT client.
#include <type_traits>

#include "builtin_types.ddsrpc.client.h"
#include "builtin_types.ddsrpc.service.h"

namespace {

class BuiltinSmokeService final : public ddsrpc_smoke::BuiltinApiService {
 public:
  using BuiltinApiService::BuiltinApiService;

 protected:
  Coro::Result<ddsrpc_smoke::BuiltinValues> echo(
      ddsrpc_smoke::BuiltinValues value) override {
    return value;
  }
  Coro::Result<void> notify(QString, int) override { return {}; }
  Coro::Result<double> scale(double value, float factor) override {
    return value * factor;
  }
  Coro::Result<ddsrpc_smoke::BuiltinApi_State> nextState(
      ddsrpc_smoke::BuiltinApi_State state) override {
    return state;
  }
  Coro::Result<int> args(int args, int reply, int in, int out) override {
    return args + reply + in + out;
  }
  Coro::Result<QVector<int>> values(QVector<int> input) override {
    return input;
  }
};

}  // namespace

int main() {
  static_assert(
      std::is_base_of<ddsrpc::ServiceBase, BuiltinSmokeService>::value,
      "generated service must derive from ServiceBase");
  static_assert(
      std::is_base_of<ddsrpc::ClientBase, ddsrpc_smoke::BuiltinApiClient>::value,
      "generated client must derive from ClientBase");
  static_assert(
      std::is_same<ddsrpc_smoke::BuiltinApiService::State,
                   ddsrpc_smoke::BuiltinApi_State>::value,
      "service must expose the class enum alias");
  static_assert(
      std::is_same<ddsrpc_smoke::BuiltinApiClient::State,
                   ddsrpc_smoke::BuiltinApi_State>::value,
      "client must expose the class enum alias");
  return 0;
}
