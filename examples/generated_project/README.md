# 使用已生成代码的项目示例

这个目录演示 `ddsrpcgen` 生成代码之后，使用方项目应该怎样组织、编译和调用生成出来的类。
`generated/` 下的文件已经提交到仓库，**构建时不需要再次运行生成器**。

示例使用进程内的 `FakeDdsProvider`，不要求安装 Fast DDS。不过服务端和客户端仍然走真实的
DDS 注册、请求-响应、发布-订阅和属性同步流程。

## 目录结构

```text
examples/generated_project/
├── greeter.idl                          # 生成输入
├── generated/                           # 已经提交的生成文件
│   ├── greeter.ddsrpc.types.h
│   ├── greeter.ddsrpc.service.h
│   ├── greeter.ddsrpc.service.cpp
│   ├── greeter.ddsrpc.client.h
│   └── greeter.ddsrpc.client.cpp
├── main.cpp                             # 业务实现：派生 Service、使用 Client
├── CMakeLists.txt                       # 独立构建，也可以被根 examples 引用
└── README.md
```

`main.cpp` 不直接构造 `transport::Message`，只使用生成的 `demo::GreeterService` 和
`demo::GreeterClient`。

## 构建与运行

### 在 ddsrpc 仓库中构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DDDSRPC_BUILD_EXAMPLES=ON
cmake --build build --target generated_project_demo -j
./build/bin/generated_project_demo
```

### 单独构建这个目录

`CMakeLists.txt` 检测不到 `ddsrpc::runtime` 时，会把仓库根目录作为父项目加入：

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

这段输出演示了以下过程：客户端先启动、服务端后启动，`ready` 从 false 变为 true；SLOT 调用、
PROP 推送和 SIGNAL 订阅都通过生成的接口完成。

## 重新生成代码

`greeter.idl` 的内容如下：

```text
class Greeter {
    PROP(int count = 0)
    PROP(QString label READONLY)
    SIGNAL(announced(int count))
    SLOT(QString greet(QString name))
    SLOT(void bump())
};
```

构建好生成器后，可以重新生成已经提交的代码：

```bash
# 任选一种：生成器已经在 PATH 中，或者直接使用仓库的构建产物
ddsrpcgen --role all --out-dir examples/generated_project/generated \
  --namespace demo --depfile /tmp/greeter.d \
  examples/generated_project/greeter.idl
```

生成文件说明：

| 文件 | 内容 |
|---|---|
| `greeter.ddsrpc.types.h` | POD、枚举的 QDataStream 支持和类型注册函数；这个 IDL 没有自定义类型，所以只包含注册入口 |
| `greeter.ddsrpc.service.h/.cpp` | 抽象 `demo::GreeterService`、SLOT 分发、属性状态和快照、SIGNAL 发布 |
| `greeter.ddsrpc.client.h/.cpp` | `demo::GreeterClient`（QObject）、协程 RPC、属性缓存、Qt 信号和链路监控 |

注意：重新生成会覆盖 `generated/` 下的文件。如果需要长期手动修改，应该修改生成器，或者把生成
文件复制到自己的源码目录，不再执行上面的命令。

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
    return {};  // 可以在这里调用 setCount() 或 publishAnnounced()
  }
};
```

除了纯虚函数，`GreeterService` 还提供：

- `count()` / `setCount()`、`label()` / `setLabel()`：读取和修改属性，`setCount()` 会更新 revision
  并推送属性事件；
- `publishAnnounced(int)`：发布 `SIGNAL`；
- `prepare()` / `start()` / `stop()`：管理服务生命周期。

`PROP` 的修饰符决定生成哪些方法。本示例中 `count` 使用默认修饰符，客户端可以调用 `count()` 和
`pushCount()`；`label` 是 `READONLY`，客户端只能调用 `label()`，服务端仍然可以调用 `setLabel()`。

### 2. 创建并启动客户端

```cpp
transport::DdsNode client_node(client_transport,
                               std::make_unique<transport::DdsCodec>());
demo::GreeterClient client(client_node, client_transport);

client.prepare();        // 必须在 node.Start() 之前调用
client_node.Start();
client.start();          // 不要求服务端已经在线
```

客户端构造函数需要 `DdsNode&` 和创建该 node 时使用的同一个 `DdsTransport&`，并且 transport 的
生命周期必须长于 client。生成的链路监控会直接调用 `transport_.CurrentLinkState()`，用来完成首次
握手和断线重连后的属性刷新。

### 3. 连接 Qt 信号

```cpp
QObject::connect(&client, &demo::GreeterClient::announced,
                 [](int count) { /* SIGNAL */ });
QObject::connect(&client, &demo::GreeterClient::countChanged,
                 [](int value) { /* PROP 远端变化 */ });
```

生成的客户端还提供 `labelChanged`、`readyChanged` 和 `syncFailed` 信号。属性相关的回调在事件
协程中执行；不要在回调中调用 `client.stop()`，应把停止操作推迟到下一次 Qt 事件循环。

### 4. 调用 SLOT

```cpp
auto greeting = client.greet(QStringLiteral("generated"));
if (!greeting) {
  std::cerr << greeting.error().message() << "\n";
}
```

每个 `SLOT` 都会生成一个返回 `Coro::Result<T>` 的成员函数，必须在协程中调用。服务端会为每个
请求启动独立协程，所以多个请求可以同时处理。

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

Client 的析构函数是默认实现，不会自动 join 协程，也不会调用虚函数。因此，正在运行的 client
必须在销毁前显式调用 `stop()`。

## CMake 接入

### 方式 A：提交生成代码（本示例）

```cmake
set(CMAKE_AUTOMOC ON)  # client.h 包含 Q_OBJECT

add_executable(my_app
  main.cpp
  generated/greeter.ddsrpc.service.cpp
  generated/greeter.ddsrpc.client.cpp)
target_include_directories(my_app PRIVATE generated)
target_link_libraries(my_app PRIVATE
  Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)
```

`ddsrpc::runtime` 需要由上层工程提供（例如 `add_subdirectory(ddsrpc)`）。当前仓库还没有提供
安装目标或 `find_package` 包。

### 方式 B：构建时生成（推荐）

使用根目录 README 中的 `ddsrpc_add_idl`，让 CMake 在构建阶段调用 `ddsrpcgen`：

```cmake
ddsrpc_add_idl(my_app
  ROLES "service;client"
  NAMESPACE demo
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/greeter.idl")
```

这种方式不需要提交 `generated/`，但要求构建环境能找到 `ddsrpcgen` 和本仓库的 CMake helper。

## 与其他示例的关系

- `examples/hello`：使用 `ddsrpc_add_idl` 在构建时生成代码；
- `examples/generated_project`（本目录）：展示生成文件已经存在时，项目结构和 API 应该怎样使用；
- `examples/qmake_hello`：qmake 工程使用预生成的代码；
- `examples/fastdds_check` 和 `examples/fastdds_reconnect`：真实 Fast DDS 跨进程验证。

完整的 IDL 子集、属性修饰符和协议细节见仓库根目录的 `README.md`。
