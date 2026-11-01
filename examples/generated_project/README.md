# 代码生成后的项目示例

本目录展示 `ddsrpcgen` 生成之后，一个消费方工程如何组织、编译和使用生成代码。
`generated/` 下的文件已经提交到仓库，**构建本示例不需要运行生成器**。

示例用一个进程内的 `FakeDdsProvider`，因此不要求安装 Fast DDS；服务端和客户端
仍按真实的 DDS 注册、请求-响应、发布-订阅和属性同步路径运行。

## 目录结构

```text
examples/generated_project/
├── greeter.idl                          # 生成输入
├── generated/                           # 已提交的生成结果
│   ├── greeter.ddsrpc.types.h
│   ├── greeter.ddsrpc.service.h
│   ├── greeter.ddsrpc.service.cpp
│   ├── greeter.ddsrpc.client.h
│   └── greeter.ddsrpc.client.cpp
├── main.cpp                             # 业务实现：派生 Service、使用 Client
├── CMakeLists.txt                       # 独立构建，也可被根 examples 引用
└── README.md
```

`main.cpp` 不直接构造 `transport::Message`，只使用生成的
`demo::GreeterService` 和 `demo::GreeterClient`。

## 构建与运行

### 作为 ddsrpc 仓库的一部分

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DDDSRPC_BUILD_EXAMPLES=ON
cmake --build build --target generated_project_demo -j
./build/bin/generated_project_demo
```

### 独立构建本目录

`CMakeLists.txt` 在检测不到 `ddsrpc::runtime` 时会把仓库根目录作为父项目加入：

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

这个输出同时演示了：客户端先启动、服务端后启动时 `ready` 从 false 变 true；
SLOT 请求-响应、PROP 变化推送和 SIGNAL 发布订阅都通过生成的门面完成。

## 从 IDL 重新生成

`greeter.idl`：

```text
class Greeter {
    PROP(int count = 0)
    PROP(QString label READONLY)
    SIGNAL(announced(int count))
    SLOT(QString greet(QString name))
    SLOT(void bump())
};
```

构建生成器后重新生成已提交的代码：

```bash
# 任选其一：已在 PATH 中，或使用仓库构建产物
ddsrpcgen --role all --out-dir examples/generated_project/generated \
  --namespace demo --depfile /tmp/greeter.d \
  examples/generated_project/greeter.idl
```

生成物含义：

| 文件 | 内容 |
|---|---|
| `greeter.ddsrpc.types.h` | POD/枚举的 QDataStream 支持和类型注册函数；本 IDL 没有自定义类型，因此只含注册入口 |
| `greeter.ddsrpc.service.h/.cpp` | 抽象 `demo::GreeterService`、SLOT 分发、属性状态/快照、SIGNAL 发布 |
| `greeter.ddsrpc.client.h/.cpp` | `demo::GreeterClient`（QObject）、协程 RPC、属性缓存、Qt 信号、链路监控 |

注意：重新生成会覆盖 `generated/` 下的文件；需要长期手改时请修改生成器，或复制到
自己的源码目录并停止重新生成。

## 生成代码的用法

### 1. 实现服务端

从生成的 `XxxService` 派生，实现 IDL 中每个 `SLOT` 对应的纯虚函数：

```cpp
class DemoGreeterService final : public demo::GreeterService {
 public:
  using GreeterService::GreeterService;

 protected:
  Coro::Result<QString> greet(QString name) override {
    return QStringLiteral("hello ") + name;
  }
  Coro::Result<void> bump() override {
    return {};  // 可在这里调用 setCount()/publishAnnounced()
  }
};
```

除了纯虚 SLOT，`GreeterService` 还提供：

- `count()` / `setCount()`、`label()` / `setLabel()`：属性读取和变更；`setCount` 会更新
  revision 并推送属性事件；
- `publishAnnounced(int)`：发布 `SIGNAL`；
- `prepare()` / `start()` / `stop()`：服务生命周期。

`PROP` 修饰符决定生成哪些方法。本示例中 `count` 是默认可推送属性，客户端获得
`count()` 和 `pushCount()`；`label` 是 `READONLY`，客户端只有 `label()`，服务端仍有
`setLabel()`。

### 2. 构造和启动客户端

```cpp
transport::DdsNode client_node(client_transport,
                               std::make_unique<transport::DdsCodec>());
demo::GreeterClient client(client_node, client_transport);

client.prepare();        // 必须在 node.Start() 之前
client_node.Start();
client.start();          // 不要求服务端已在线
```

客户端构造需要 `DdsNode&` 和构造该 node 所用的同一个 `DdsTransport&`，且 transport
必须比 client 活得久。生成的链路监控直接调用
`transport_.CurrentLinkState()`，用于首次握手和断线重连后的自动属性刷新。

### 3. 连接 Qt 信号

```cpp
QObject::connect(&client, &demo::GreeterClient::announced,
                 [](int count) { /* SIGNAL */ });
QObject::connect(&client, &demo::GreeterClient::countChanged,
                 [](int value) { /* PROP 远端变化 */ });
```

生成的客户端信号还包括 `labelChanged`、`readyChanged`、`syncFailed`。属性回调运行在
事件 fiber 上；不要在回调中调用 `client.stop()`，应推迟到下一次 Qt 事件循环。

### 4. 调用 SLOT

```cpp
auto greeting = client.greet(QStringLiteral("generated"));
if (!greeting) {
  std::cerr << greeting.error().message() << "\n";
}
```

每个 SLOT 都生成一个返回 `Coro::Result<T>` 的成员函数，必须在 fiber 中调用。
服务端请求处理在独立协程中执行，不同请求可以并发。

### 5. 停止

```cpp
client.stop();
service.stop();
client_node.Close();
client_node.WaitClosed();
server_node.Close();
server_node.WaitClosed();
client_transport.Close();
client_transport.WaitClosed();
server_transport.Close();
server_transport.WaitClosed();
```

生成的 Client 析构函数是 `= default`，不会自动 join fiber 或调用虚函数，因此运行中的
client 必须在销毁前显式 `stop()`。

## CMake 接入方式

### 方式 A：提交生成代码（本示例）

```cmake
set(CMAKE_AUTOMOC ON)  # client.h 含 Q_OBJECT

add_executable(my_app
  main.cpp
  generated/greeter.ddsrpc.service.cpp
  generated/greeter.ddsrpc.client.cpp)
target_include_directories(my_app PRIVATE generated)
target_link_libraries(my_app PRIVATE
  Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)
```

`ddsrpc::runtime` 需要由父工程提供（`add_subdirectory(ddsrpc)`），当前仓库没有提供
安装/`find_package` 包。

### 方式 B：构建时生成（推荐保持 IDL 与代码同步）

使用根 README 中的 `ddsrpc_add_idl`，由 CMake 在构建阶段调用 `ddsrpcgen`：

```cmake
ddsrpc_add_idl(my_app
  ROLES "service;client"
  NAMESPACE demo
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/greeter.idl")
```

这种方式不需要提交 `generated/`，但要求构建环境能找到 `ddsrpcgen` 和本仓库的
CMake helper。

## 与完整工程示例的关系

- `examples/hello`：使用 `ddsrpc_add_idl` 在构建时生成代码；
- `examples/generated_project`（本目录）：展示生成结果已存在时的工程结构与 API 用法；
- `examples/qmake_hello`：qmake 工程消费预生成代码；
- `examples/fastdds_check` / `fastdds_reconnect`：真实 Fast DDS 跨进程验证。

完整 IDL 子集、属性修饰符和协议细节见仓库根目录 `README.md`。
