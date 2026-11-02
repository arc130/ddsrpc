# 自定义后端与代码生成器改造指南

这份文档说明如何把生成代码使用的 `transport::DdsNode`、`transport::DdsTransport` 替换成
自己的类，或者把两者聚合为一个 `System` 后再传给生成的 Service/Client。内容面向需要长期
维护自己分支的开发者。

先说一个原则：**不要长期手改生成文件**。生成器重新运行时会覆盖它们。除了一次性验证，修改
应该落在运行时基类和 `tools/ddsrpcgen/` 的生成器代码里。

## 1. 当前结构

```text
GreeterService
    └── ddsrpc::ServiceBase
            └── transport::DdsNode

GreeterClient
    ├── ddsrpc::ClientBase
    │       └── transport::DdsNode
    └── transport::DdsTransport&    // 只用于轮询 CurrentLinkState()
```

运行时和生成代码各自依赖什么：

| 位置 | 依赖内容 |
|---|---|
| 生成的 Service | `DdsNode&`，服务名、事件 topic、schema hash |
| 生成的 Client | `DdsNode&` 和 `DdsTransport&`，服务名、事件 topic、schema hash、重试参数 |
| `ServiceBase` | 服务注册、事件发布、接收请求、发送回复、`Message`/`Endpoint` |
| `ClientBase` | 客户端注册、事件订阅、请求-响应、事件循环、属性和握手 |
| `wire.h`、类型生成器 | 不依赖 `DdsNode`/`DdsTransport`，只使用 QDataStream |
| 宿主代码 | 创建 transport、node，调用 `prepare/start/stop` |

因此，“把构造参数换一个类”本身不难；真正的工作量取决于**新类是否提供相同的接口和消息模型**。

## 2. 先判断改造级别

| 级别 | 情况 | 需要改什么 |
|---|---|---|
| A | `System` 内部仍是现有 `DdsNode` 和 `DdsTransport` | 生成器、示例和测试 |
| B | `System` 内部是新的 Node/Transport，但接口可以兼容 | 运行时类型、生成器、适配器、测试 |
| C | 新类使用完全不同的请求/应答和订阅模型 | 运行时基类、消息适配、测试，工作量最大 |

多数情况下，你的需求属于 A：把 `System::node()` 和 `System::transport()` 暴露出来，写一个
访问器即可，运行时核心不需要改。

## 3. 推荐结构：System 作为唯一构造参数

让 `System` 同时管理 node 和 transport，并暴露两个访问器：

```cpp
namespace my {

class System {
 public:
  transport::DdsNode& node();
  transport::DdsTransport& transport();
};

}  // namespace my
```

生成的服务和客户端都只接收 `System&`：

```cpp
// GreeterService
explicit GreeterService(::my::System& system, Options options = {});

GreeterService::GreeterService(::my::System& system, Options options)
    : ::ddsrpc::ServiceBase(
          system.node(),
          ::ddsrpc::ServiceConfig{options.service_name,
                                  "ddsrpc." + options.service_name + ".events",
                                  kSchemaHash}) {
  registerTypes();
}
```

```cpp
// GreeterClient
explicit GreeterClient(::my::System& system,
                       Options options = {},
                       QObject* parent = nullptr);

GreeterClient::GreeterClient(::my::System& system,
                             Options options,
                             QObject* parent)
    : QObject(parent),
      ::ddsrpc::ClientBase(
          system.node(),
          ::ddsrpc::ClientConfig{options.service_name,
                                 "ddsrpc." + options.service_name + ".events",
                                 kSchemaHash,
                                 options.request_retry,
                                 options.handshake_interval}),
      transport_(system.transport()),
      options_(::std::move(options)) {
  registerTypes();
}
```

链路监控代码不需要变化：

```cpp
const auto state = transport_.CurrentLinkState();
```

如果 `System` 自己也提供 `CurrentLinkState()`，也可以保存 `System&`，然后调用
`system_.CurrentLinkState()`。两种方式都可以，保存 `transport_` 的改法更小。

### 生命周期要求

`System` 必须比 Service/Client 活得久，并且 `System` 内部的 transport 要比 node 活得久。
推荐顺序：

```cpp
my::System system(config);

system.transport().Start();
auto& node = system.node();

service.prepare();
client.prepare();
node.Start();
service.start();
client.start();

// 关闭时
client.stop();
service.stop();
node.Close();
node.WaitClosed();
system.transport().Close();
system.transport().WaitClosed();
```

如果 `System` 自己封装了 `Start/Close`，就让宿主调用 `System` 的方法，但要保证生成代码调用
`prepare()` 时 transport 和 node 已经处于约定的生命周期阶段。

## 4. 级别 A：只替换构造参数

如果 `System` 返回现有的 `DdsNode` 和 `DdsTransport`，`ServiceBase`、`ClientBase` 都不用改。

### 4.1 修改生成器

涉及两个文件：

#### `tools/ddsrpcgen/emit_service.cpp`

1. `EmitClassHeader` 中的服务构造函数声明；
2. `EmitConstructor` 中的构造函数定义；
3. 生成头文件时加入 `System` 的头文件，或者至少前置声明 `System`，并在 cpp 中包含完整定义。

目标形式：

```cpp
explicit GreeterService(::my::System& system, Options options = {});
```

```cpp
GreeterService::GreeterService(::my::System& system, Options options)
    : ::ddsrpc::ServiceBase(system.node(), config) { ... }
```

#### `tools/ddsrpcgen/emit_client.cpp`

1. `EmitClassHeader` 中的客户端构造函数声明；
2. `EmitConstructor` 中的构造函数定义；
3. private 区域的 `transport_` 或 `system_` 成员类型；
4. `LinkMonitorLoop` 中不需要改调用，只要 `transport_` 仍然有 `CurrentLinkState()`；
5. 生成头文件中的 `#include <transport/io/dds/DdsTransport.hpp>` 改成 `System` 头文件；
6. 如果保存的是 `System&`，则成员和链路监控调用一起改。

构造函数目标形式：

```cpp
explicit GreeterClient(::my::System& system,
                       Options options = {},
                       QObject* parent = nullptr);
```

构造函数定义使用 `system.node()` 初始化 `ClientBase`，并用 `system.transport()` 初始化
`transport_`；具体写法见第 3 节。

### 4.2 同步修改生成器测试

`tests/generator/codegen_test.cpp` 中有对生成文本的断言，例如当前客户端构造函数会断言：

```cpp
EXPECT_NE(client_h.indexOf(QStringLiteral("::transport::DdsTransport& transport")), -1);
```

改成 System 参数后，这条断言也要改成新的签名。否则代码能生成，但生成器测试会失败。

### 4.3 更新已经提交的生成代码

`examples/generated_project/generated/` 下的文件是提交到仓库的生成结果。改完生成器后要
重新生成，不能只改 `.idl` 或 `main.cpp`。重新生成命令见该目录的 README。

### 4.4 更新宿主代码和测试

所有出现下面这种写法的地方都要改成传 `System`：

```cpp
TestClient client(node, transport);
// 或
TestClient client(client.node(), client.transport());
```

改成：

```cpp
TestClient client(system);
```

需要检查：

- `tests/support/fake_host.hpp`：可以把 `Host` 改成 `System`，或者给 `Host` 增加
  `node()`/`transport()` 访问器后继续用；
- `tests/runtime/service_base_test.cpp`、`client_base_test.cpp`、`stability_test.cpp`；
- `examples/hello`、`examples/generated_project`、`examples/fastdds_check`、
  `examples/fastdds_reconnect`；
- 如果生成代码里服务端也接收 `System&`，服务端测试同样要传 `System`。

### 4.5 CMake

如果 `System` 是独立库：

```cmake
target_include_directories(my_app PRIVATE path/to/system/include)
target_link_libraries(my_app PRIVATE my_system ddsrpc::runtime)
```

`ddsrpc_runtime` 目前 `PUBLIC` 链接 `transport`。只要 `System` 内部仍然使用现有的
`DdsNode`/`DdsTransport`，这行不需要改。如果新实现不再依赖 Transport，需要改
`CMakeLists.txt` 中 `ddsrpc_runtime` 的链接项。

## 5. 改成可配置的生成器参数

如果只维护一个 `System`，直接改 emitters 最简单。如果需要同时支持 DDS 和其他后端，建议
增加生成器选项：

```text
ddsrpcgen --system-type ::my::System --system-header my/system.h ...
```

需要修改：

1. `tools/ddsrpcgen/cli.h`：在 `CliOptions` 中增加 `system_type`、`system_header`；
2. `tools/ddsrpcgen/cli.cpp`：增加参数解析和 usage 文本，并传给 emitter；
3. `tools/ddsrpcgen/emit_service.h`、`emit_client.h`：给 `Emit*Header/Source` 增加一个
   轻量的 `BackendOptions` 参数，避免继续增加函数参数；
4. `emit_service.cpp`、`emit_client.cpp`：用选项拼接构造参数和 include；
5. `cmake/DdsRpcHelpers.cmake`：给 `ddsrpc_add_idl` 增加 `SYSTEM_TYPE`、`SYSTEM_HEADER`
   两个 one-value 参数，并在 `add_custom_command` 中传给 `ddsrpcgen`；
6. 生成器测试：覆盖默认值和自定义值两条路径。

这样生成文件本身不包含后端选择逻辑，切换只发生在生成阶段。

## 6. 级别 B：Node/Transport 类型也变了

如果 `System::node()`/`transport()` 返回的是新类型，而不是现有的 `transport::DdsNode` 和
`transport::DdsTransport`，运行时也要改。推荐先集中类型，不要在每个文件里散着替换。

### 6.1 增加 backend 类型入口

新建 `include/ddsrpc/backend.h`：

```cpp
#pragma once

#include <my/node.hpp>
#include <my/transport.hpp>

namespace ddsrpc::backend {

using Node = my::Node;
using Transport = my::Transport;
using Message = my::Message;
using Endpoint = my::Endpoint;
using MessageKind = my::MessageKind;
using RetryPolicy = my::RetryPolicy;
using LinkState = my::LinkState;
using Ticket = my::Node::Ticket;

}  // namespace ddsrpc::backend
```

然后把运行时和生成器中的 `transport::DdsNode`、`transport::DdsTransport`、
`transport::Message` 等统一改成 `ddsrpc::backend::*`。

需要改的运行时代码：

- `include/ddsrpc/service_base.h` / `src/ddsrpc/service_base.cpp`
  - 构造函数、`node()`、`node_`、`requests_`
  - 请求分发、`SendReply`、`PublishEvent` 中的 `Message`/`Endpoint`
- `include/ddsrpc/client_base.h` / `src/ddsrpc/client_base.cpp`
  - `ClientConfig::request_retry` 的类型
  - 构造函数、`node()`、`node_`、`events_`
  - `HandleLinkState` 的 `LinkState`
  - `CallWithRetry`、`EventLoop`、`SyncOnce` 中的消息类型
- 生成器 `emit_service.cpp`、`emit_client.cpp` 中的类型名和 include

### 6.2 新类必须满足的接口

即使使用类型别名，新 Node 也必须提供现有运行时实际用到的方法：

```text
RegisterServices / RegisterPublishers / UnregisterServices
RegisterClients / RegisterSubscribers / UnregisterClients
ServeRequests / Subscribe
Reply / Publish / RequestForResultDirect
```

`Ticket` 需要支持：

```text
Wait()                     // 等待下一条消息
mailbox()->close(error)    // 停止时唤醒等待协程
```

生成客户端只需要新 Transport 或 System 提供链路状态查询。最简单的形式是
`CurrentLinkState()`；新接口也可以是 `IsConnected()`、`state()` 或回调，只要在生成的链路
监控里映射成 Up/非 Up。

如果新类的请求-应答接口、订阅接口或 `Ticket` 语义不同，优先写适配器，而不是直接改生成
代码：

```cpp
class MyNodeAdapter {
 public:
  using Ticket = MyTicket;

  Coro::Result<void> RegisterServices(const std::vector<std::string>& names);
  Coro::Result<void> RegisterPublishers(const std::vector<std::string>& topics);
  Coro::Result<void> UnregisterServices(const std::vector<std::string>& names);
  Coro::Result<void> RegisterClients(const std::vector<std::string>& names);
  Coro::Result<void> RegisterSubscribers(const std::vector<std::string>& topics);
  Coro::Result<void> UnregisterClients(const std::vector<std::string>& names);
  Coro::Result<Ticket> ServeRequests(const std::string& service);
  Coro::Result<Ticket> Subscribe(/* topic, kind */);
  Coro::Result<Message> RequestForResultDirect(Message request, RetryPolicy retry);
  Coro::Result<void> Reply(const Message& request, Message reply);
  Coro::Result<void> Publish(Message message);
};
```

适配器内部负责把新类的消息格式、关联 ID、订阅回调和停止语义转换成运行时需要的形状。
这里最容易出问题的是 `Ticket` 的停止行为：如果 `mailbox()` 不能关闭，客户端或服务端的停止
流程可能永久等待。

### 6.3 消息模型

运行时目前直接使用 `transport::Message` 的以下字段/构造：

- `message.payload`：请求、回复、事件的二进制载荷；
- `message.endpoint`：`Endpoint::Service(name)`、`Endpoint::Topic(topic)`；
- `MessageKind::kNotify`：事件订阅；
- `RetryPolicy`：客户端请求重试参数。

如果新消息类型没有这些字段，适配器需要提供等价映射。不能只替换构造参数类型，否则
`client_base.cpp` 和 `service_base.cpp` 会在编译期报错。

### 6.4 连接状态接口变了

这个改动只影响客户端重连逻辑。服务端、`ServiceBase`、请求-响应和事件编解码都不依赖
`CurrentLinkState()`。

当前生成客户端的链路监控在 `tools/ddsrpcgen/emit_client.cpp` 的 `LinkMonitorLoop` 中：

```cpp
const auto state = transport_.CurrentLinkState();
(void)this->HandleLinkState(state);
const auto interval =
    (!this->ready() && state == ::transport::LinkState::kUp)
        ? options_.handshake_interval
        : options_.link_poll_interval;
```

运行时只关心一件事：**链路是不是 Up**。`ClientBase::HandleLinkState` 收到非 Up 会清掉
`ready`；收到 Up 会重新订阅事件、重置属性 revision 并请求快照。

#### 情况一：新接口仍返回状态值

如果新接口是 `IsConnected()`、`LinkState()` 或 `ConnectionState()`，在生成的监控循环里
做一次映射即可。最干净的做法是让 `ddsrpc` 自己定义一个简单的链路状态类型，不把这个
第三方枚举带进运行时：

```cpp
namespace ddsrpc {
enum class LinkState { kDown, kEstablishing, kUp };
}
```

然后把 `ClientBase::HandleLinkState` 的参数改成 `ddsrpc::LinkState`。生成代码中：

```cpp
const auto raw = system_.connectionState();
const ddsrpc::LinkState state =
    raw == my::ConnectionState::Connected
        ? ddsrpc::LinkState::kUp
        : ddsrpc::LinkState::kDown;
(void)this->HandleLinkState(state);
```

如果新接口只有布尔值，也可以直接把 `HandleLinkState` 的参数改成 `bool up`，把比较逻辑
改成：

```cpp
const auto up = system_.IsConnected();
(void)this->HandleLinkState(up);
const auto interval =
    (!this->ready() && up) ? options_.handshake_interval
                           : options_.link_poll_interval;
```

改 `HandleLinkState` 参数类型后，需要同步修改：

- `include/ddsrpc/client_base.h`
- `src/ddsrpc/client_base.cpp` 中的 `state != kUp` 等比较
- `tests/runtime/client_base_test.cpp` 里的 `Sync()`/`Down()` 辅助方法
- 其他直接调用 `HandleLinkState` 的测试或适配代码

生成代码里只保留 `transport_` 或 `system_` 的引用，具体调用什么方法由 emitter 决定。

#### 情况二：新接口改成回调

如果新接口只提供“状态变化回调”，不要直接在回调里调用 `HandleLinkState`。这个回调可能
来自 transport 自己的线程，而 `HandleLinkState` 会操作 fiber 锁、取消事件订阅并执行
`RenewEvents()`；跨线程直接调用会破坏客户端的执行模型。

推荐在 `System` 里缓存最新状态，生成代码继续轮询：

```cpp
class System {
 public:
  void OnConnectionChanged(bool up) { connected_.store(up); }
  bool IsConnected() const { return connected_.load(); }

 private:
  std::atomic_bool connected_{false};
};
```

链路监控仍然每 `link_poll_interval` 调用一次 `IsConnected()`。这样不需要改变
`ClientBase` 的 fiber 内执行约定，也不会在回调线程里执行网络操作。

如果确实要事件驱动，需要把状态变化投递到客户端所在的事件循环或 fiber 队列，再由该 fiber
调用 `HandleLinkState`。不要在任意线程里直接执行握手、订阅或 `refreshProperties()`。

#### 情况三：新接口不再提供连接状态

如果新类没有链路状态，只能通过请求失败或握手超时推断服务不可达，那么需要重写客户端的
重连判定逻辑。常见做法是周期性发一个轻量请求；连续失败后把 `ready` 置为 false，成功后再
刷新属性。这个方案会让重连检测变慢，也需要处理“服务慢但未断开”的情况，工作量明显大于
前两种。

#### 需要修改的文件

- `tools/ddsrpcgen/emit_client.cpp`：`LinkMonitorLoop` 中的状态调用和比较；
- `include/ddsrpc/client_base.h`、`src/ddsrpc/client_base.cpp`：如果修改了
  `HandleLinkState` 的参数类型；
- `tests/runtime/client_base_test.cpp`：`BareClient` 的 `Sync()`/`Down()` 和断言；
- `tests/runtime/e2e_reconnect_test.cpp`：如果测试使用的是生成客户端，需要确认新状态
  能正确驱动 Down/Up；
- `examples/fastdds_reconnect`：真实 transport 的状态接口变化后，需要重新验证。

#### 改动量

- 仍然返回状态值、只做映射：约 0.5 个工作日，改动集中在生成器和少量测试；
- 改成回调，但保留轮询适配：约 0.5–1 个工作日，另外增加 `System` 状态缓存和线程安全
  测试；
- 完全没有连接状态、需要靠请求失败推断：1–3 个工作日，具体取决于超时和健康检查策略。

## 7. 级别 C：完全不同的消息模型

如果新类不使用 DDS request/reply 和 pub/sub 的概念，而是比如本地队列、HTTP 或自定义总线，
就需要重新设计：

- `ClientBase::CallWithRetry` 如何发请求、等待回复、关联请求 ID；
- `ServiceBase::AcceptLoop` 如何接收请求；
- 事件订阅如何取消并在重连后重建；
- `Ticket` 如何取消等待；
- `CurrentLinkState` 如何表达链路可用性；
- 重试策略和错误码如何映射。

这个级别不适合只改几个类型别名，建议先写一个独立适配层，再逐步把运行时迁移过去。

## 8. 改动量评估

下面的估算按当前仓库规模计算，不包含新的 Node/Transport 实现本身，也不包含真实网络环境联调。

| 方案 | 主要改动 | 手写代码规模 | 预计工作量 | 风险 |
|---|---|---|---|---|
| A. System 聚合现有 Dds 类 | 2 个 emitter、生成器测试、示例和测试构造点、重新生成 | 约 150–300 行，涉及 10 个左右手写文件 | 0.5–1 个工作日 | 低 |
| A + 生成器参数化 | 在 A 的基础上增加 CLI、emitter 参数和 CMake helper | 再加约 100–200 行 | 增加 0.5–1 个工作日 | 低到中 |
| B. 新 Node/Transport，接口可适配 | 运行时 4 个文件、2 个 emitter、backend.h、适配器、测试 | 约 400–800 行，涉及 15–20 个文件 | 2–4 个工作日 | 中 |
| B + 完全自定义适配器/Ticket | 在 B 的基础上增加消息和等待语义适配 | 约 600–1000 行 | 3–5 个工作日 | 中高 |
| C. 新消息模型 | 运行时、生成器、测试基本重写 | 1000 行以上 | 1–2 周 | 高 |

当前仓库的耦合规模大致是：

- 运行时：4 个文件，约 35 处 Dds 类型引用；
- 生成器：2 个 emitter，约 10 处硬编码类型/调用；
- 宿主和测试：约 10 个手写文件，主要改构造函数调用；
- `examples/generated_project/generated/` 下的文件只需重新生成，不按手写代码计。

实际用时主要花在三件事上：新类与现有 `Ticket`/消息语义的对齐、生成器文本断言的修正、以及
把所有示例和稳定性测试重新跑通。单纯的类型替换通常不是最耗时的部分。

## 9. 修改后的验收清单

1. 生成器测试通过：`ddsrpcgen_tests`；
2. 生成代码编译检查通过：`ddsrpc_generated_compile_check`；
3. 运行时测试通过：`ddsrpc_runtime_tests`，包括 `StabilityTest.*`；
4. `hello` 和 `generated_project` 示例能够运行；
5. `examples/generated_project/generated/` 与生成器输出一致；
6. 如果使用 Fast DDS，再运行 `fastdds_check` 和 `fastdds_reconnect`；
7. 检查生成文件只包含预期的后端差异，没有时间戳、绝对路径或不确定内容。

## 10. 常见问题

### 只改生成文件可以吗？

可以用于一次性验证，但下次运行生成器会被覆盖。长期方案必须改 emitters，或者改生成器
提供的参数。

### 为什么不能只改构造函数？

因为 `ServiceBase` 和 `ClientBase` 在内部仍然调用 `DdsNode` 的注册、请求和事件接口，并且
使用 Transport 的 `Message`、`Endpoint`、`Ticket` 等类型。构造函数只是入口，类型和方法
签名必须一起满足。

### System 需要自己启动 transport 吗？

需要由宿主保证调用的先后顺序：transport 先启动，node 再启动；生成代码的 `prepare()` 要在
`node.Start()` 之前完成。`System` 可以封装这些步骤，但不要改变生成代码依赖的时序。

### `CurrentLinkState()` 需要满足什么？

生成客户端的链路监控协程会每 `link_poll_interval` 调用一次。它必须足够轻量，并且能在节点
运行期间安全地从 fiber 中查询。只要返回值能区分 Up 和非 Up，就能驱动现有的重连逻辑。

### 新增 System 头文件后，为什么 CMake 编译不到？

生成代码引用了 `System` 的类型和 `Q_OBJECT` 客户端头文件。需要：

- 把 System 头文件所在目录加入目标的 `target_include_directories`；
- 确保包含客户端头文件的目标开启 `AUTOMOC`；
- 如果 System 在独立库中，链接该库。

### 生成器测试为什么失败？

生成器测试会直接检查生成文本，例如客户端构造函数参数和 `transport_` 成员。修改 emitter 后
要同步修改这些断言，或者把断言改成更稳定的行为检查。
