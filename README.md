# ddsrpc

基于 [Transport](https://github.com/Ste7an-cs/Transport) 的 DDS 传输/DdsNode 与
[AsyncTask](https://github.com/Ste7an-cs/AsyncTask) 协程运行时之上的 `.idl` RPC 代码生成器与运行时。

业务代码只使用生成的 `XxxService` / `XxxClient` 门面，不直接构造 `transport::Message`：

- `SLOT` → DDS 请求-响应；
- `SIGNAL` → DDS 发布-订阅；
- `PROP` → 客户端缓存、服务端变更推送、初始快照和可选远端 setter；
- 服务端每个请求在独立 `Coro::makeTask` 中执行；
- 客户端 `prepare()/start()` 不等待服务上线，后台链路监控在连接可用后完成 schema 握手和属性快照；
- 服务重启或网络中断后，链路重新 Up 时自动重新订阅、刷新 PROP 并恢复 `ready`；
- 双端都由本生成器生成，载荷使用 QDataStream（QDataStream Qt_5_12，非标准 DDS CDR）。

## 依赖

验收环境：Ubuntu 20.04 WSL。

- C++17、Qt 5.12（Core、Network、SerialPort）
- CMake 3.16+
- Boost >= 1.89（fiber/context/thread/chrono，安装在 `/usr/local`）
- `third_party/Transport` = `1c79cd5`（0.6.0）
- `third_party/AsyncTask` = `d231d64`
- GoogleTest 来自 Transport 的 vendored `third_party/googletest`

Fast DDS 是 Transport 的可选依赖；本版本验收使用进程内 `FakeDdsProvider`，不要求安装 Fast DDS。

## 构建与测试

```bash
git submodule update --init third_party/AsyncTask third_party/Transport
sudo apt-get install -y libqt5serialport5-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DDDSRPC_BUILD_EXAMPLES=ON
cmake --build build -j
(cd build && ctest --output-on-failure)
```

`hello` 示例（单进程 Fake provider，client 先启动、service 后启动）：

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

### qmake 消费示例

先由 CMake 构建生成器、runtime 和 Transport 静态库，再生成/构建 qmake 示例：

```bash
mkdir -p /tmp/ddsrpc-qmake/generated
./build/bin/ddsrpcgen --role all --out-dir /tmp/ddsrpc-qmake/generated \
  --namespace demo examples/hello/hello.idl
cd /tmp/ddsrpc-qmake
qmake /path/to/ddsrpc/examples/qmake_hello/qmake_hello.pro
make -j
./qmake_hello
```

qmake 示例复用 `examples/hello/main.cpp`，链接 CMake 构建出的
`libddsrpc_runtime.a`、`libtransport.a`、`libasynctask.a`。qmake 不是与 CMake 等价的完整构建体系。

## IDL

输入扩展名为 `.idl`，采用 QtRO `.rep` 的 RPC 子集。示例：

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

支持：`IMPORT`、单行 `#...` C++ 指令、`POD`、顶层/类内 `ENUM`、`PROP`、`SIGNAL`、`SLOT`，
以及 `CONSTANT` / `READONLY` / `READPUSH` / `READWRITE` / `SOURCEONLYSETTER` 属性修饰符。

暂不支持：`MODEL`、`CLASS`、`FLAG`、`USE_ENUM`、`PERSISTED`、花括号 POD、`#HEADER/#FOOTER`；
这些输入会得到带 `file:line:column` 的明确诊断。

属性映射：

- `READWRITE` 客户端生成 `setX(value)`；`READPUSH` 生成 `pushX(value)`；两者都等待服务端确认；
- `READONLY` / `SOURCEONLYSETTER` 无客户端写方法；
- `CONSTANT` 无写入方法；
- 客户端属性变更使用 `<prop>Changed(value)` Qt 信号；
- `PERSISTED` 不支持。

客户端向服务端上行消息只能通过 `SLOT` 请求-响应；客户端不是事件 publisher。

## 生成物

生成器角色：`types`、`service`、`client`、`all`。

每个 IDL 生成：

```text
<stem>.ddsrpc.types.h
<stem>.ddsrpc.service.h / .cpp
<stem>.ddsrpc.client.h / .cpp
```

- types：POD、枚举、QDataStream、metatype 注册；
- service：`XxxService` 抽象业务基类、RPC dispatch、属性状态/快照、SIGNAL 发布；
- client：`XxxClient` QObject 门面、协程 RPC、属性缓存、Qt 信号、链路监控。

生成的 Client 构造函数直接借用 `transport::DdsNode&` 和 `transport::DdsTransport&`。
链路监控循环在生成的 `client.cpp` 中**直接调用** `transport_.CurrentLinkState()`，没有额外
LinkState 抽象；后续 SDK 变化时直接修改生成代码/生成器即可。

Client 析构函数是 `= default`：不调用 `stop()`、不 join fiber、不调用虚函数。调用方必须在销毁前
显式调用 `stop()`；不要从 `readyChanged`/`syncFailed`/事件回调中调用 `stop()`，应推迟到下一次
Qt 事件循环。回调中可以安全调用 `refreshProperties()` 和 RPC。

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

- 支持多个根 IDL 和 `INCLUDE_DIRS`；
- `IMPORT` 图在 configure 阶段扫描，build 阶段一次生成；
- `ROLES service/client` 只把对应角色文件加入目标，client 头自动 `AUTOMOC`；
- 生成器 depfile 作为 `BYPRODUCTS` 声明，依赖由扫描得到的输入 IDL 显式 `DEPENDS` 保证
  （兼容 CMake 3.16 + Unix Makefiles）。

## 协议与生命周期

- DDS 请求/响应 topic：`cfg.<service>.request` / `cfg.<service>.response`（Transport 派生）；
- 事件 topic：`ddsrpc.<service>.events`；
- 一个 IDL 一个 schema hash；请求、回复和事件都携带 schema，不匹配立即报 `kSchemaMismatch`；
- 客户端 `ready=false` 直到 schema/属性快照握手成功；链路 Down 时 `ready=false`，Up 时自动刷新；
- 客户端与服务端都不使用 UUID/session 身份；
- 请求/回复/事件使用 QDataStream + 固定 wire version；
- 服务端接收协程串行接收、每请求独立 `Coro::makeTask`，回复可乱序；
- 属性快照和变更事件按 revision 合并；`refreshProperties()` 可手动重同步；
- 错误通过 `Coro::Result` + `ddsrpc::RpcErrc` 返回，Transport 错误原样透传。

推荐生命周期：

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

- 仅支持上述 RPC 子集；`MODEL/CLASS/FLAG/USE_ENUM/PERSISTED` 等暂不支持。
- 双端必须使用本生成器；不支持标准 DDS/CDR 或其他语言互通。
- SIGNAL 可丢失；DDS 读队列满时仍会丢最旧样本。
- RPC 自动重试可能重复执行 SLOT；处理函数需幂等或自行去重。
- 服务端请求处理协程无框架并发上限；业务需控制慢处理器和请求洪水。
- 不自动重发重连前在途的 RPC；只自动刷新 `ready` 和 PROP 缓存。
- 默认 DdsTransport 链路状态是端点整体聚合，不是按服务区分；同一 client node 挂多个服务时可能
  由其他端点状态触发刷新。
- 验收使用 FakeDdsProvider；真实 Fast DDS 跨进程待后续验证。
- 生成文件默认会被下一次生成覆盖；需要长期手改时修改生成器或停止重新生成。

## 测试

- `ddsrpcgen_tests`：lexer/parser/import/semantic/types/CLI；
- `ddsrpc_generated_compile_check`：生成 types/service/client 的编译/链接 smoke，覆盖 hostile 参数名；
- `ddsrpc_runtime_tests`：wire、TaskGroup、ServiceBase、ClientBase、RPC、SIGNAL、PROP、
  client-first 握手和服务重启自动刷新端到端测试。
