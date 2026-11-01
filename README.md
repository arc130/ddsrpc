# ddsrpc

ddsrpc 是一个基于 [Transport](https://github.com/Ste7an-cs/Transport) 的 DDS 传输层和
[AsyncTask](https://github.com/Ste7an-cs/AsyncTask) 协程运行时的 RPC 框架。它读取 Qt Remote
Objects 风格的 `.idl` 文件，生成服务端和客户端代码，并把 `SLOT`、`SIGNAL`、`PROP` 分别映射为
DDS 请求-响应、发布-订阅和属性同步。

业务代码只需要使用生成的 `XxxService` 和 `XxxClient`，不需要自己拼装 `transport::Message`：

- `SLOT`：客户端发起请求，服务端返回结果，走 DDS 请求-响应；
- `SIGNAL`：服务端发布事件，客户端订阅接收，走 DDS 发布-订阅；
- `PROP`：服务端保存属性状态并推送变化，客户端维护缓存；客户端启动时通过快照同步，链路重连后自动刷新；
- 服务端为每个请求创建独立的 `Coro::makeTask` 协程，慢请求不会阻塞接收循环；
- 客户端调用 `prepare()/start()` 时不需要服务端已经在线；链路可用后，后台协程会完成 schema 握手和属性快照；
- 服务端重启或网络恢复后，链路重新变为 Up 时，客户端会自动重新订阅事件、刷新属性，并把 `ready` 置回 true；
- 客户端和服务端都由本生成器生成，载荷使用 QDataStream（协议版本 `Qt_5_12`），不能与标准 DDS CDR 互通。

## 环境与依赖

验收环境为 Ubuntu 20.04 WSL。构建需要：

- C++17 编译器；
- Qt 5.12 的 Core、Network、SerialPort 模块；
- CMake 3.16 或更高版本；
- Boost 1.89 或更高版本（fiber、context、thread、chrono），当前环境安装在 `/usr/local`；
- `third_party/Transport` 0.6.0（提交 `1c79cd5`）；
- `third_party/AsyncTask`（提交 `d231d64`）；
- GoogleTest 直接使用 Transport 自带的 `third_party/googletest`。

Fast DDS 是可选依赖。日常测试使用进程内的 `FakeDdsProvider`，不需要安装 Fast DDS；安装后会额外构建
并运行 `fastdds_check` 和 `fastdds_reconnect` 两个跨进程示例。

## 构建与测试

```bash
git submodule update --init third_party/AsyncTask third_party/Transport
sudo apt-get install -y libqt5serialport5-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DDDSRPC_BUILD_EXAMPLES=ON
cmake --build build -j
(cd build && ctest --output-on-failure)
```

## 示例

### hello：最小可用示例

`hello` 使用进程内的 `FakeDdsProvider`，演示客户端先启动、服务端后启动的流程：

```bash
./build/bin/hello
```

预期输出：

```text
ready-before-service=0
ready-after-service=1
greet=hello demo
count=1
announced=1
```

### generated_project：使用已经生成的代码

`examples/generated_project` 演示生成代码已经提交到仓库时，项目应该怎样组织。`generated/`
目录直接包含 types、service、client 文件，构建时不会再调用生成器。

作为仓库的一部分构建：

```bash
cmake --build build --target generated_project_demo -j
./build/bin/generated_project_demo
```

也可以把它当作独立项目构建：

```bash
cmake -S examples/generated_project -B build-generated-project
cmake --build build-generated-project -j
./build-generated-project/generated_project_demo
```

预期输出：

```text
generated-project ready-before-service=0
generated-project ready-after-service=1
generated-project greet=hello generated
generated-project count=1
generated-project announced=1
generated-project label=demo
```

生成文件说明、Service/Client 的用法和 CMake 接入方式，见
[`examples/generated_project/README.md`](examples/generated_project/README.md)。

### qmake 示例

qmake 示例使用 CMake 已经构建好的生成器、runtime 和 Transport 静态库：

```bash
mkdir -p /tmp/ddsrpc-qmake/generated
./build/bin/ddsrpcgen --role all --out-dir /tmp/ddsrpc-qmake/generated \
  --namespace demo examples/hello/hello.idl
cd /tmp/ddsrpc-qmake
qmake /path/to/ddsrpc/examples/qmake_hello/qmake_hello.pro
make -j
./qmake_hello
```

示例复用 `examples/hello/main.cpp`，链接 `libddsrpc_runtime.a`、`libtransport.a` 和
`libasynctask.a`。qmake 示例只用于验证 qmake 使用生成代码的流程，不等同于 CMake 的完整构建体系。

### Fast DDS 跨进程检查

以下结果使用 Fast DDS 3.6.1、Fast CDR 2.3.5 和 foonathan_memory 0.7-4 验证，三者都安装在
`/usr/local`：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DDDSRPC_BUILD_EXAMPLES=ON -DCMAKE_PREFIX_PATH=/usr/local
cmake --build build -j

# 终端 A
./build/bin/fastdds_check --role server --domain 43
# 终端 B
./build/bin/fastdds_check --role client --domain 43
```

预期客户端输出：

```text
FASTDDS_CLIENT_GREET=fastdds hello client
FASTDDS_CLIENT_ANNOUNCED=1
FASTDDS_CLIENT_COUNT=7
```

服务端会输出 `FASTDDS_SERVER_READY domain=43`。这个示例在两个独立进程之间验证了 RPC、SIGNAL
和 PROP 远端写入。未安装 Fast DDS 时，该示例不会参与构建，`FakeDdsProvider` 测试不受影响。

### Fast DDS 重连与属性刷新检查

`examples/fastdds_reconnect` 在两个进程之间验证服务端重启后的自动恢复流程：

1. 启动 server1（`name=before`、`count=1`），客户端完成首次 ready 并读取属性快照；
2. 杀掉 server1，客户端观察到链路变为 Down，`ready` 变为 false；
3. 启动 server2（`name=after`、`count=2`），客户端重新订阅事件、刷新属性、恢复 ready，收到新的
   SIGNAL，并成功完成一次 RPC。

在 WSL 中运行（脚本会自动启动和结束服务端进程，默认使用 `build-fastdds` 构建目录）：

```bash
BUILD_DIR=build-fastdds bash examples/fastdds_reconnect/run_check.sh
```

预期输出：

```text
RECONNECT_CLIENT_READY_1 name=before count=1
RECONNECT_CLIENT_DOWN
RECONNECT_CLIENT_READY_2 name=after count=2
RECONNECT_CLIENT_GREET=reconnect hello after announced=2
CLIENT_RC=0
---SERVER---
RECONNECT_SERVER_READY name=before count=1
RECONNECT_SERVER_READY name=after count=2
```

脚本大约需要 20–25 秒。Fast DDS 对被强杀的对端要等到 participant lease 超时，链路状态才会变为
非 Up。脚本会先等客户端确认 Down，再启动新服务端；如果服务端重启太快、客户端还没来得及观察 Down，
本版本不保证自动刷新，详见“限制”。

## IDL 语法

输入文件扩展名为 `.idl`，支持 QtRO `.rep` 的 RPC 子集。示例：

```text
IMPORT "common.idl";

POD Point(int x, int y)
ENUM Mode { Idle, Running }

class Calculator {
    PROP(int total = 0)
    PROP(QString name READONLY)
    PROP(Mode mode = Idle READWRITE)

    SIGNAL(calculated(int result))

    SLOT(int add(int a, int b))
    SLOT(void reset())
    SLOT(QString echo(QString text))
};
```

目前支持 `IMPORT`、单行 `#...` C++ 指令、`POD`、顶层和类内 `ENUM`、`PROP`、`SIGNAL`、`SLOT`，
以及 `CONSTANT`、`READONLY`、`READPUSH`、`READWRITE`、`SOURCEONLYSETTER` 属性修饰符。

暂不支持 `MODEL`、`CLASS`、`FLAG`、`USE_ENUM`、`PERSISTED`、花括号 POD 和 `#HEADER/#FOOTER`。
遇到这些输入时，生成器会给出带 `file:line:column` 的错误信息。

属性修饰符对应的生成规则：

- `READWRITE`：客户端生成 `setX(value)`；
- `READPUSH`：客户端生成 `pushX(value)`；
- 上面两种写操作都会等待服务端确认；
- `READONLY` 和 `SOURCEONLYSETTER` 不生成客户端写方法；
- `CONSTANT` 不生成任何写入方法；
- 客户端通过 `<prop>Changed(value)` Qt 信号获知属性变化。

客户端只能通过 `SLOT` 请求-响应向服务端发消息，不能直接发布事件。

## 生成代码

生成器支持 `types`、`service`、`client`、`all` 四种角色。每个 IDL 会生成以下文件：

```text
<stem>.ddsrpc.types.h
<stem>.ddsrpc.service.h / .cpp
<stem>.ddsrpc.client.h / .cpp
```

- types：定义 POD、枚举，生成对应的 QDataStream 读写和 metatype 注册；
- service：生成抽象基类 `XxxService`、RPC 分发、属性状态和快照、SIGNAL 发布；
- client：生成 QObject 类 `XxxClient`、协程 RPC、属性缓存、Qt 信号和链路监控。

生成的 Client 在构造时直接引用 `transport::DdsNode&` 和 `transport::DdsTransport&`。链路监控循环在
生成的 `client.cpp` 中直接调用 `transport_.CurrentLinkState()`，没有额外的链路状态抽象层。后续如果
SDK 接口发生变化，直接修改生成器或生成代码即可。

Client 的析构函数是默认实现，不会调用 `stop()`、不会 join 协程，也不会调用虚函数。因此，必须在
销毁前显式调用 `stop()`。不要在 `readyChanged`、`syncFailed` 或事件回调中调用 `stop()`，应把停止
操作推迟到下一次 Qt 事件循环。回调中可以安全地调用 `refreshProperties()` 和 RPC。

## CMake 接入

```cmake
ddsrpc_add_idl(calc_server
  ROLES service
  NAMESPACE demo
  IDL idl/calc.idl)

ddsrpc_add_idl(calc_client
  ROLES client
  NAMESPACE demo
  IDL idl/calc.idl)
```

几点说明：

- 支持多个根 IDL，也支持通过 `INCLUDE_DIRS` 指定导入搜索路径；
- `IMPORT` 依赖图在 configure 阶段扫描，代码在 build 阶段一次生成；
- `ROLES` 为 `service` 或 `client` 时，只把对应角色的文件加入目标，生成的 client 头文件会自动启用 `AUTOMOC`；
- 生成器的 depfile 以 `BYPRODUCTS` 声明，依赖关系由扫描出的 IDL 文件列表通过 `DEPENDS` 显式指定，
  兼容 CMake 3.16 和 Unix Makefiles。

## 协议与生命周期

- 请求和响应 topic 分别为 `cfg.<service>.request` 和 `cfg.<service>.response`，由 Transport 自动派生；
- 事件 topic 为 `ddsrpc.<service>.events`；
- 每个 IDL 对应一个 schema hash。请求、回复和事件都携带 schema，不匹配时立即返回 `kSchemaMismatch`；
- 客户端在 schema 和属性快照握手成功前保持 `ready=false`。链路变为 Down 时 `ready=false`，链路恢复并刷新成功后 `ready=true`；
- 客户端和服务端都不使用 UUID 或 session 身份；
- 请求、回复和事件使用 QDataStream 和固定的 wire version；
- 服务端由一个接收协程顺序收取请求，但每个请求都在独立的 `Coro::makeTask` 中处理，因此回复顺序可能与请求顺序不同；
- 属性快照和变更事件按 revision 合并；也可以调用 `refreshProperties()` 手动重新同步；
- 错误统一通过 `Coro::Result` 和 `ddsrpc::RpcErrc` 返回，Transport 的错误会原样传出。

推荐的生命周期：

```text
transport.Start()
创建 DdsNode
service/client.prepare()
node.Start()
service.start(); client.start()
...
client.stop(); service.stop()
node.Close(); node.WaitClosed()
transport.Close(); transport.WaitClosed()
```

## 限制

- 只支持上面列出的 RPC 子集，`MODEL`、`CLASS`、`FLAG`、`USE_ENUM`、`PERSISTED` 等语法暂不支持。
- 客户端和服务端都必须使用本生成器；不支持标准 DDS/CDR，也不能与其他语言实现互通。
- SIGNAL 可能丢失；DDS 读队列满时仍会丢弃最旧的样本。
- RPC 自动重试可能导致同一个 SLOT 被执行多次，处理函数需要保持幂等，或由业务自行去重。
- 服务端不会限制并发处理协程的数量，业务需要控制慢处理器和请求量。
- 重连时不会自动重发已经发出但尚未完成的 RPC，只会自动刷新 `ready` 和 PROP 缓存。
- DdsTransport 的链路状态默认按节点上的所有端点聚合，不区分服务。同一个 client node 上挂多个服务时，
  其他端点的状态变化也可能触发本服务的刷新。
- 自动刷新有一个前提：链路状态要先从 Up 变为非 Up，再恢复为 Up。Fast DDS 对被强杀的对端要等
  participant lease 超时（默认 20 秒）才让匹配计数归零，因此检测可能较慢。如果新服务端在链路状态
  下降之前就完成重启，客户端可能察觉不到重连，属性缓存会停留在旧值。本版本不额外探测服务实例；
  `fastdds_reconnect` 会先等客户端观察到 Down，再启动新服务端。
- 日常自动化测试默认使用 FakeDdsProvider。真实 Fast DDS 跨进程已经验证了基础 RPC、SIGNAL、PROP，
  以及明确观察到掉线后的重连、属性刷新、SIGNAL 重订阅和 RPC 恢复。
- 重新生成会覆盖生成文件。如果需要长期手动修改，应修改生成器，或者停止重新生成。

## 测试

- `ddsrpcgen_tests`：词法、语法、导入、语义、类型生成和 CLI；
- `ddsrpc_generated_compile_check`：编译并链接生成的 types/service/client，覆盖容易冲突的参数名；
- `ddsrpc_runtime_tests`：wire、TaskGroup、ServiceBase、ClientBase、RPC、SIGNAL、PROP、客户端先启动
  的握手流程和服务端重启后的自动刷新；
- 其中 `StabilityTest.*` 覆盖高频并发和错误输入，包括 512 路并发 RPC、混合非法请求、随机字节请求、
  handler 异常、属性/SIGNAL 突发、恶意回复和损坏的事件载荷。可以单独运行：

  ```bash
  ./build/bin/ddsrpc_runtime_tests --gtest_filter='StabilityTest.*'
  ```
