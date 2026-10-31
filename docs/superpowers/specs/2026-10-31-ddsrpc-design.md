# ddsrpc 第一版设计

- 日期：2026-10-31
- 状态：已与用户确认，待实现
- 依赖基线：
  - `Ste7an-cs/Transport` `1c79cd5`（0.6.0）
  - `Ste7an-cs/AsyncTask` `d231d64`（当前 master，覆盖 Transport 记录的旧 pin `6f4225`）
- 验收环境：Ubuntu 20.04 WSL + Qt 5.12 + FakeDdsProvider

## 1. 背景与目标

本项目的目标是在 Transport 的 DDS 传输/节点层和 AsyncTask 的协程运行时之上，实现一个由 Qt Remote Objects 的 `.rep` 风格接口描述驱动的 RPC 框架。

第一版需要让业务代码只面对生成的类型化门面，不直接拼装 `transport::Message`：

- SLOT 生成 DDS 请求-响应调用；
- SIGNAL 生成 DDS 发布-订阅；
- PROP 生成属性缓存、初始快照、变更推送和可选的远端写入；
- 服务端每个请求在独立 `Coro::makeTask` 中执行；
- 客户端 `prepare/start` 不依赖服务在线，后台协程完成首次握手和属性快照；
- 客户端监控 DDS 链路状态；服务端重启或网络中断后重连时，自动重新握手并刷新 PROP 缓存；
- 双端均由本生成器生成，载荷使用 QDataStream 编码。

## 2. 范围

### 2.1 本版包含

- 自定义 `.idl` 输入格式，语法覆盖 QtRO `.rep` 的 RPC 相关子集；
- `IMPORT "path.idl";` 递归导入 IDL；
- C++ 单行预处理指令（典型为 `#include`）透传到生成头；
- `class`、`POD`、顶层/类内 `ENUM`；
- `PROP`、`SIGNAL`、`SLOT`；
- 生成 service-only、client-only 或同时生成两种角色的代码；
- 客户端基于 DdsTransport 链路状态自动检测重连，并自动刷新 PROP；
- CMake 代码生成与构建集成；
- 一个 qmake 消费示例；
- 生成器单元测试、wire 单元测试、FakeDdsProvider 端到端测试；
- 生成本工程自身文档。

### 2.2 本版不包含

- `MODEL`、`CLASS`、`FLAG`、`USE_ENUM`；
- 花括号形式 POD；
- `PERSISTED`；
- `#HEADER`、`#FOOTER`；
- 标准 DDS/CDR、IDL4、其他语言互通；
- 真实 Fast DDS 跨进程验收；
- DDS QoS 的逐接口配置；
- 重连时自动重发/恢复在途 RPC（只自动刷新属性并恢复 ready）；
- 服务端并发上限与请求去重；
- qmake 与 CMake 的完整等价支持（qmake 仅提供消费示例）。

## 3. IDL 语言

### 3.1 文件与 IMPORT

- 文件扩展名为 `.idl`，UTF-8，允许 `//` 行注释和 `/* ... */` 块注释。根文件和 IMPORT 目标都必须是 `.idl`。
- 新导入指令：

  ```text
  IMPORT "common.idl";
  ```

  `IMPORT` 是大小写敏感的顶层关键字，分号必填。它不是 C++ `#include`。
- 解析顺序：先相对于当前文件所在目录，再按命令行 `-I/--include-dir` / CMake `INCLUDE_DIRS` 的顺序查找。
- 导入是传递的；每个文件只处理一次；循环导入、找不到文件、重复声明均报错。
- 所有 IDL 文件共享同一 C++ namespace；跨文件类型名不允许重复。
- C++ 指令（如 `#include <QString>`）按原顺序透传到对应生成 types 头的 namespace 之外。`#HEADER` / `#FOOTER` 明确报不支持。
- `.rep` 扩展名不是本版输入格式。

### 3.2 示例

```text
# include <QString>
IMPORT "common.idl";

POD Point(int x, int y);

ENUM Mode {
    Idle,
    Running,
    Fault = -1
}

class Calculator {
    ENUM State { New, Ready }

    PROP(int total = 0)
    PROP(QString name READONLY)
    PROP(Mode mode = Idle READWRITE)

    SIGNAL(calculated(int result))
    SIGNAL(faulted(QString reason))

    SLOT(int add(int a, int b))
    SLOT(void reset())
    SLOT(QList<Point> history())
};
```

### 3.3 顶层声明

- `POD Name(Type member[, Type member...]);`
  - 生成普通 C++ struct。
  - 成员为 public，名称保持 IDL 原样。
  - 生成默认构造、全参数构造、`operator==`、`operator!=`、`QDataStream` 读取/写入。
  - 不生成 `operator<`；如作为 `QMap` 键，由用户补充比较。
  - POD 依赖按拓扑顺序输出；无法排序的循环依赖报错。
- `ENUM Name { A, B = 2, C = 0x10, D = -1 };`
  - 生成 `enum class Name : qint32`。
  - 枚举值支持十进制、十六进制、负数；缺省值自动递增。
  - 生成 `QDataStream` 运算符和 metatype 注册。
- `class Name { ... };`
  - 生成 `<Name>Service` / `<Name>Client` 与相关协议代码。
  - class 名在同一 namespace 内唯一。

### 3.4 class 成员

- `PROP(Type name [= default] [MODIFIER])`
  - 修饰符：`CONSTANT`、`READONLY`、`READPUSH`、`READWRITE`、`SOURCEONLYSETTER`；缺省 `READPUSH`。
  - 只允许一个可见性修饰符；`PERSISTED` 被识别但不支持，报错。
  - 默认值是原始 C++ 表达式；生成器保留它，并在类型为生成枚举且表达式是裸枚举值（如 `Idle`）时重写为 `EnumType::Idle`。
- `SIGNAL(name([parameters]));`
  - 不支持返回值；可带可选的分号。
- `SLOT([return-type] name([parameters]));`
  - 省略返回类型时为 `void`。
  - 接受 `const T&`、`T&`、未命名参数；wire/API 一律按值传递，引用修饰只在解析时接受，不传播可修改语义。
- 类内 `ENUM Name { ... };`
  - 生成命名空间级 `enum class <Class>_<Enum> : qint32`；
  - 在生成的 Service/Client 内提供 `using <Enum> = <Class>_<Enum>;` 方便实现类使用；
  - IDL 内在本类里使用裸 `<Enum>`，跨类引用使用 `<Class>::<Enum>`，生成时解析到规范类型。

### 3.5 类型语法与规则

- 支持：
  - C++ 标识符/限定名，如 `QString`、`Qt::DateFormat`；
  - 模板嵌套，如 `QMap<QString, QList<Point>>`；
  - `const T&`、`T&`；
  - 生成器已知的 POD/枚举；
  - 用户通过 `#include` 引入、且实现了 QDataStream 运算符的外部类型。
- 拒绝：
  - 指针类型、引用返回类型；
  - `void` 属性；
  - 无法构造的默认值表达式（语法层面无法验证时，留给 C++ 编译报错）。
- POD/枚举声明顺序与名称解析：
  - 全局 POD/枚举用裸名称引用；
  - 类内枚举在本类内用裸名称，跨类用 `Class::Enum`；
  - 未在生成器符号表中找到的类型原样作为外部 C++ 类型输出。
- 序列化：所有 wire 类型必须支持 `QDataStream` 的 `operator<<`/`operator>>`。生成器为 POD、枚举和 Qt 类型提供支持；用户自定义类型需自行提供运算符与必要的 metatype 注册。

### 3.6 生成期校验与诊断

每个错误包含文件、行、列和原因。至少校验：

- 非法/保留 C++ 标识符、名称重复；
- class 内 PROP/SIGNAL/SLOT 名称冲突；
- `<property>Changed` 与 IDL SIGNAL 或保留信号名冲突；
- 生成的 service/client 成员名冲突（生命周期方法、属性访问器、publish 方法、`ready`、`readyChanged`、`syncFailed`、`lastErrorText` 等为保留名）；
- 不支持的语法或修饰符；
- IMPORT 循环、缺失文件、重复声明；
- 重复输出文件名（不同目录下相同 stem）；
- POD 循环依赖；
- 未命名参数自动生成 `arg0`、`arg1`… 后的重名。

## 4. 生成物与命名

### 4.1 角色化文件

生成器 CLI 至少支持：

```text
ddsrpcgen --role types|service|client|all
          --out-dir DIR
          [--namespace NS]
          [-I DIR ...]
          [--depfile FILE]
          <root.idl>...
```

每个 `.idl` 生成以下文件（仅生成被请求的角色）：

```text
<stem>.ddsrpc.types.h
<stem>.ddsrpc.service.h
<stem>.ddsrpc.service.cpp
<stem>.ddsrpc.client.h
<stem>.ddsrpc.client.cpp
```

- `types.h`：POD、枚举、QDataStream 运算符、inline `registerTypes()`（幂等注册生成类型的 metatype）、接口常量。服务端/客户端共用，不依赖 QObject/moc；生成的 Service/Client 构造时调用 `registerTypes()`。
- `service.*`：`<Class>Service`，纯 C++ 抽象基类 + 通信实现。
- `client.*`：`<Class>Client`，`Q_OBJECT`，Qt 信号 + 协程 RPC 门面。
- 被 IMPORT 的文件按相同角色生成；生成的 types 头通过 `#include` 形成依赖。
- 生成文件为 UTF-8、LF 换行、稳定顺序，采用 include guard，内容可复现。

### 4.2 命名规则

| IDL 声明 | C++ 名称 |
|---|---|
| `POD Point` | `Point` struct |
| 顶层 `ENUM Mode` | `enum class Mode` |
| class `Foo` | `FooService` / `FooClient` |
| class 内 `ENUM State` | `Foo_State`，并在 Service/Client 中 `using State = Foo_State` |
| `PROP int total` | 服务端/客户端 getter `total()`；服务端 setter `setTotal()`；客户端 READWRITE setter `setTotal()`、READPUSH setter `pushTotal()` |
| 属性变化 | Qt 信号 `totalChanged(int total)` |
| `SIGNAL(calculated(...))` | 客户端 Qt 信号 `calculated(...)`；服务端发布方法 `publishCalculated(...)` |
| `SLOT(int add(...))` | 服务端纯虚方法 `Coro::Result<int> add(...)`；客户端协程方法同名 |

- 所有生成符号位于 `--namespace` 指定的命名空间；支持 `a::b` 形式。缺省为空（全局命名空间）。
- 同一 IMPORT 图必须使用同一 namespace。
- 生成的 class/类型名必须在目标 namespace 内唯一；`Options` 与生成的 Service/Client 成员名同属保留名空间。
- 生成 class 只借用 `transport::DdsNode&`，不拥有 transport/node。

### 4.3 生成器实现

生成器是 C++17 + Qt Core 的单体命令行程序，采用手写递归下降解析器，不依赖 bison/ANTLR。

```text
CLI
 → IMPORT 递归加载
 → 手写 lexer
 → 递归下降 parser
 → AST
 → 语义分析 / 类型解析 / id 与 schema 计算
 → types/service/client emitters
 → 原子写文件 + depfile
```

**Lexer**

- 手写扫描器，token 含标识符、整数、字符串、单行 `#` 指令、标点和文件结束符；
- 跳过 `//` 与 `/* ... */` 注释，保留文件/行/列；
- 记录 token 的源码偏移，用于捕获 PROP 默认表达式、SLOT 参数等 C++ 片段；
- `IMPORT`、`POD`、`ENUM`、`class`、`PROP`、`SIGNAL`、`SLOT` 是大小写敏感关键字；不支持的 `MODEL`、`CLASS`、`FLAG`、`USE_ENUM` 也识别并给出定向错误。

**Parser**

- 递归下降，单次解析出 AST；语法错误立即失败，不生成半成品。
- 顶层：`IMPORT`、C++ 指令、`POD`、`ENUM`、`class`。
- class 内：`PROP`、`SIGNAL`、`SLOT`、`ENUM`。
- 独立 `parseType()` 递归处理限定名与模板，如 `QMap<QString, QList<Point>>`、`const T&`。
- `parseParams()` 复用 `parseType()`，支持未命名参数并生成 `arg0..`。
- `SLOT` 先定位函数参数左括号：括号前最后一个标识符是函数名，其余 token 组成返回类型（空则为 `void`）。
- `PROP` 先解析 type/name；`=` 后的内容按源码区间保存为默认表达式，末尾的已知修饰符单独提取。
- 未支持构造、重复/非法标识符等由 parser 或语义层报告带 `file:line:col` 的诊断。

**AST 与语义分析**

- AST 包含 `SourceFile`、`ImportDecl`、`PodDecl`、`EnumDecl`、`ClassDecl`、`PropDecl`、`SignalDecl`、`SlotDecl`、`TypeRef`、`Param`、`Directive`。
- 语义层建立全局 POD/ENUM 表与 class 成员表，解析类内枚举和外部 C++ 类型，校验保留名/重名/修饰符。
- 为 SLOT、属性 setter、SIGNAL 分配 op/event id；对 POD 做拓扑排序并检测循环。
- 每个 IDL 的 schema hash 由规范化 AST（声明顺序、类型、修饰符、默认值）和 IMPORT 图中的类型声明计算；使用 SHA-256 截断为 64 位。

**代码发射**

- 简单 `CodeWriter`（缩进、命名空间、头文件保护、参数/类型渲染）加三个 emitter：`TypesEmitter`、`ServiceEmitter`、`ClientEmitter`；不使用模板引擎。
- Emitter 只消费语义分析后的模型，不回读源文本。
- 生成内容固定顺序、无时间戳/绝对路径；相同输入产生字节级相同输出。
- 所有文件先写临时文件，全部成功后再替换目标文件；失败不留部分生成物。
- depfile 由生成器输出，列出所有被 IMPORT 的 IDL。
- 生成的 Client 链路监控循环直接调用 `transport_.CurrentLinkState()`，不额外包一层连接状态抽象；后续 SDK 变化时直接修改生成代码。

**CMake 与生成器的分工**

- CMake configure 阶段只做最小 IMPORT 图扫描，用于确定输出文件与 moc 输入；
- build 阶段调用一次生成器完成完整解析、语义检查和代码生成；
- CMake 扫描与生成器共用相对路径 + `INCLUDE_DIRS` 规则；两者不一致时以生成器的构建期错误为准。

手写递归下降的原因：当前语法规模小，真正复杂的是嵌入的 C++ 类型/默认值片段和诊断，而不是上下文无关语法本身；手写方案零额外工具链依赖，且生成代码和解析代码都便于后续直接修改。

## 5. 运行时架构

### 5.1 组件

```text
业务代码
  ├── XxxService : ddsrpc::ServiceBase
  └── XxxClient  : QObject + ddsrpc::ClientBase
          │
      ddsrpc runtime
      ├── 信封编解码 / schema 校验 / 错误码
      ├── 服务端请求接收 + 每请求 Coro task
      ├── 客户端事件接收 + 后台握手 + 属性缓存
      └── RpcErrc / std::error_category
          │
      transport::DdsNode / DdsCodec
          │
      transport::DdsTransport（宿主所有）
```

- `ddsrpc_runtime` 是一个小型静态库，链接 Transport、AsyncTask、Qt Core/Network。
- 生成器 `ddsrpcgen` 是独立的 C++17 + Qt Core 命令行程序。
- 生成代码依赖 runtime 的公共头，不复制协议实现。

### 5.2 运行时职责

- 统一 magic/version/schema/op/event 信封的编码与校验；
- `ServiceBase`：注册服务/发布者、接收请求、为每个请求启动协程、追踪并汇合处理协程、回包；
- `ClientBase`：注册客户端/订阅者、事件接收协程、链路状态监控与自动重连、后台握手/属性刷新协程、属性 revision 合并、远端错误映射；
- 生成代码负责类型化参数编解码、Qt 信号发射和业务虚函数调用；
- 不依赖 Qt Remote Objects运行时。

## 6. 协议

### 6.1 DDS topic

- 请求/响应 topic 由 Transport 按服务名派生：
  - `cfg.<service>.request`
  - `cfg.<service>.response`
- 事件 topic：

  ```text
  ddsrpc.<service>.events
  ```

  使用 `MessageKind::kNotify`，一个接口的全部 SIGNAL 与 PROP 变更复用它。
- `service_name` 缺省为 IDL class 名，可由生成的 `Options` 覆盖。
- 同一 DdsNode 上同一服务名不能同时注册为 client 和 service；由 Transport 报错。
- 事件 topic 不以 `cfg.` 开头，满足 DdsNode 的保留前缀约束。

### 6.2 信封

所有载荷使用 QDataStream，固定 `setVersion(QDataStream::Qt_5_12)`，大端字节序。

**Request**

| 字段 | 类型 |
|---|---|
| magic | `quint32` = `0x44525043`（`DRPC`） |
| wire version | `quint16` = 1 |
| schema hash | `quint64` |
| op id | `quint32` |
| 参数 | 由生成代码顺序读写 |

**Reply**

| 字段 | 类型 |
|---|---|
| magic / wire version / schema hash | 同上 |
| op id | `quint32` |
| status | `quint8`（0 = ok，1 = error） |
| 成功载荷或错误 | 由生成代码顺序读写；错误含错误码、类别标记、文本 |

**Event**

| 字段 | 类型 |
|---|---|
| magic / wire version / schema hash | 同上 |
| event kind | `quint8`（signal / property） |
| event id | `quint32` |
| revision | `quint64` |
| 载荷 | 由生成代码顺序读写 |

- magic/version 不匹配返回 `kProtocol`。
- schema hash 不匹配返回 `kSchemaMismatch`；服务端在解码请求后立即校验，客户端对每条回复和事件同样校验。
- op id `0` 保留给“握手/属性快照”；远端可写属性的 setter op 按属性声明顺序从 1 开始，随后 SLOT op 在同一序列继续递增。该顺序由 schema hash 固定。
- **一个 IDL 文件对应一个 schema hash**：规范化该文件的接口/类型声明（含顺序、类型、修饰符、默认值）及 IMPORT 图中依赖的类型声明，取 SHA-256 后截断为 64 位。任何不兼容变更都会改变 hash；客户端与同一 IDL 生成的服务端 hash 相等才视为匹配。
- 不引入 instance UUID、会话 ID 或服务实例跟踪；schema 匹配是应用层握手的唯一判据。
- event kind 固定编码：`0` = SIGNAL，`1` = PROP。Reply 错误类别标记：`0` = RpcErrc，`1` = 其他/通用错误。错误码数值一经发布保持稳定，只追加。
- 属性快照的 Reply 载荷为：属性数量、逐项 `(prop id, revision, value)`。

### 6.3 错误模型

- 生成 API 全部返回 `Coro::Result<T>`，错误类型为 `std::error_code`。
- 新增 `RpcErrc` 及 `std::error_category`：

  ```text
  kInvalidArgument
  kInvalidState
  kConfiguration
  kProtocol
  kSchemaMismatch
  kEncode
  kDecode
  kCancelled
  kRemoteError
  kInternal
  ```

- Transport 层错误原样透传（`TransportErrc::kTimeout`、`kNotAccepted`、`kClosed` 等），不包装成 `RpcErrc`。
- 远端业务错误：wire 携带错误码、类别标记和文本；客户端尽量用 `RpcErrc` 重建；非本框架错误映射为 `kRemoteError`。
- 客户端提供 `QString lastErrorText() const` 保存最近一次远端/协议错误文本；访问需线程安全。
- 服务端捕获用户异常，回复 `kRemoteError`，生成/接收协程不被异常终止。

## 7. 服务端生成 API 与行为

### 7.1 API

```cpp
class CalculatorService : public ddsrpc::ServiceBase {
public:
    struct Options {
        std::string service_name = "Calculator";
    };

    explicit CalculatorService(transport::DdsNode& node, Options options = {});

    Coro::Result<void> prepare();
    Coro::Result<void> start();
    Coro::Result<void> stop();

    virtual Coro::Result<int> add(int a, int b) = 0;
    virtual Coro::Result<void> reset() = 0;

    int total() const;
    Coro::Result<void> setTotal(int value);

    QString name() const;
    Coro::Result<void> setName(QString value);

    Coro::Result<void> publishCalculated(int result);
    Coro::Result<void> publishFaulted(QString reason);

    using State = Calculator_State;  // 类内枚举别名（若有）
};
```

### 7.2 注册与启动

- `prepare()`：在 DdsNode 的 `Created` 阶段调用（也允许 Running，但不推荐）：
  - `RegisterServices({service_name})`
  - `RegisterPublishers({event_topic})`
  - 任一步失败都回滚本端已完成的注册。
  - 立即返回，不等到服务在线。
- `start()`：要求 node 已 Running：
  - 调 `ServeRequests(service_name)` 获取请求订阅；
  - 启动请求接收协程；
  - 立即返回。
- `stop()`：
  - 关闭请求订阅邮箱；
  - 取消并汇合接收协程与全部在途请求处理协程；
  - 幂等；
  - 不注销 `prepare()` 建立的注册项（避免影响同节点其他 endpoint）；重复 `start()` 允许在 stop 后再次订阅。
- 重复 `prepare()` 在已 prepared 状态下为幂等成功；运行中再次 `prepare()`/`start()` 返回 `kInvalidState`；未 start 时 `stop()` 为幂等成功。
- 对象必须显式 `stop()` 后才可销毁；运行中析构视为使用错误。

### 7.3 请求分发与并发

- 请求接收协程只做：等待 Ticket 消息、校验信封/schema、解析 op id、复制所需输入、`Coro::makeTask` 启动处理协程。
- **每个请求一条独立协程，无框架并发上限。**
- 处理协程：
  - 解码参数；
  - 调用用户实现的纯虚 SLOT；
  - 将返回值或错误编码为 Reply；
  - 调 `DdsNode::Reply(request, reply)`。
- 回复可乱序；DdsNode 按 correlation id 配对。
- 所有生成 endpoint 操作必须在节点所属 fiber 执行域内；处理协程通过 `makeTask` 固定在当前线程（默认 `Affinity::fixed(std::this_thread::get_id())`）。
- 生成属性状态/事件 revision 的修改在同一线程协作执行；生成代码内部的属性更新段不包含挂起点。
- 在途处理协程的句柄由 ServiceBase 跟踪；`stop()` 先 cancel 再 join。用户处理器需对协作式取消作出响应。
- 重试可能造成同一 SLOT 被多次执行；框架不做去重，业务需幂等或按 correlation id 自行处理。

### 7.4 属性

- 服务端生成属性存储和初始值（来自 IDL 默认表达式）。
- 生成的 `set<Prop>(value)`：
  - 更新值；
  - 分配单调递增 revision；
  - 通过事件 topic 发布属性变更；
  - 返回 `Coro::Result<void>`。
- `CONSTANT` 属性不生成服务端 setter，只在初始化时使用默认值。
- `READONLY` / `SOURCEONLYSETTER` / `READWRITE` / `READPUSH` 服务端都可发布变更；远端可写性见第 9 节。
- 属性快照通过内部 op 0 处理，返回全部属性的 `(prop id, revision, value)`。

### 7.5 SIGNAL

- 生成 `publish<Signal>(args...)`：
  - 分配 revision；
  - 编码事件；
  - `DdsNode::Publish` 到 `ddsrpc.<service>.events`；
  - 返回入队结果；成功不表示已送达。
- SIGNAL 是 fire-and-forget，无重发和留存；客户端启动前的信号不会补发。

## 8. 客户端生成 API 与行为

### 8.1 API

```cpp
class CalculatorClient : public QObject, public ddsrpc::ClientBase {
    Q_OBJECT
public:
    struct Options {
        std::string service_name = "Calculator";
        transport::RetryPolicy request_retry{std::chrono::milliseconds(2000), 3};
        std::chrono::milliseconds handshake_interval{1000};
        std::chrono::milliseconds link_poll_interval{100};
    };

    explicit CalculatorClient(transport::DdsNode& node,
                              transport::DdsTransport& transport,
                              Options options = {},
                              QObject* parent = nullptr);
    ~CalculatorClient() override;

    Coro::Result<void> prepare();
    Coro::Result<void> start();
    Coro::Result<void> stop();

    Coro::Result<int> add(int a, int b);
    Coro::Result<void> reset();

    int total() const;
    Coro::Result<void> pushTotal(int value);  // READPUSH
    QString name() const;                     // READONLY：无远端写方法

    bool ready() const;
    QString lastErrorText() const;
    Coro::Result<void> refreshProperties();

signals:
    void calculated(int result);
    void faulted(QString reason);
    void totalChanged(int total);
    void nameChanged(QString name);
    void readyChanged(bool ready);
    void syncFailed(QString message);
};
```

### 8.2 注册与启动

- `prepare()`：在 node.Start 之前调用：
  - `RegisterClients({service_name})`
  - `RegisterSubscribers({event_topic})`
  - 失败回滚；立即返回，不探测服务。
- `start()`：
  - 不等待服务上线，不阻塞；
  - `Subscribe(event_topic, kNotify)`，启动事件接收协程；
  - 启动链路监控协程；
  - 返回成功。node 未 Running 时原样返回 DdsNode/Transport 错误（通常为 `kClosed`）。
- 重复 `prepare()` 在已 prepared 状态下为幂等成功；运行中再次 `prepare()`/`start()` 返回 `kInvalidState`；未 start 时 `stop()` 为幂等成功。
- `stop()`：
  - 置停止标志；
  - 关闭事件订阅邮箱并唤醒链路监控/刷新的等待；
  - cancel + join 事件协程、链路监控协程和在途刷新协程；
  - 幂等；把 `ready` 置回 false、属性缓存重置为 IDL 默认值并清空 revision，再次 `start()` 时重新握手。
- `~CalculatorClient()` 要求已 stop；运行中析构属于使用错误。

### 8.3 链路监控、自动重连与属性刷新

> **接口约束**：`transport::DdsNode` 0.6.0 没有公开连接状态接口；链路状态来自它借用的 `transport::DdsTransport::CurrentLinkState()`。生成 Client 的 `DdsTransport&` 构造参数必须就是构造 node 所用的那一个，且寿命长于 Client。这里**不做 LinkStateSource 之类的抽象层**；生成的 `client.cpp` 直接调用 `transport_.CurrentLinkState()`，后续 SDK/API 变化时直接修改生成代码。transport 必须比 client 活得久。

- 生成的 Client 启动一个链路监控协程（生成在 `client.cpp` 中），按 `Options::link_poll_interval`（默认 100 ms）轮询 `transport_.CurrentLinkState()`。
- 状态机：
  1. **未 Up**（`kDown`/`kEstablishing`）：若此前 `ready==true`，置 `ready=false` 并发 `readyChanged(false)`；不刷新属性，等待链路。
  2. **首次 Up**：发起首次应用层握手（内部 op 0，携带 schema hash 和属性快照请求）。成功后置 `ready=true`，发 `readyChanged(true)`。
  3. **再次 Up**（此前观察到非 Up，即服务端重启或网络中断后的重连）：先废弃旧事件订阅、重新订阅事件 topic，再重置属性 revision 基线并发起全量属性快照，完成后置 `ready=true`。这就是自动 PROP 刷新。
- 应用层握手请求使用固定的 `RetryPolicy{500ms, 1}`，避免服务离线时长期占用协程。链路 Up 但服务尚不可达时，按 `Options::handshake_interval` 重试；`kSchemaMismatch`/`kProtocol` 会置 `ready=false`、记录 `lastErrorText()`、发 `syncFailed(message)` 并停止自动重试，直到下一次链路 Up 或显式 `refreshProperties()`。
- 重连刷新期间：
  - 事件接收协程继续工作；旧订阅在 Up 转移时被关闭并重建，避免把断线前积压的旧事件用于新会话。
  - 新快照应用前重置每个属性的 revision 基线；快照和刷新期间收到的事件按 revision 合并：某属性已有更新 revision 时不回退到快照值。
- `refreshProperties()` 是显式的协程刷新入口：与链路监控共用同一个刷新互斥区，避免并发快照；它会重新请求全量属性并重置 revision 基线。链路未 Up 时可尝试请求，失败则按 Transport 错误返回。
- `ready` 以原子标志实现，可由其他线程只读查询；所有信号从节点执行线程发射。
- 链路状态由 `DdsTransport` 按“已声明端点整体”聚合，不是按服务/接口区分。同一 client node 上若有多个服务，本接口的自动刷新可能由其他端点的状态变化触发；此时以快照请求成功/失败作为本服务是否真正可达的最终判据。
- 本版仍不自动重发重连前在途的 RPC，也不保证每个 RPC 语义上的“断线重试”；调用方负责重试。自动恢复范围仅限 `ready` 状态与 PROP 缓存。

### 8.4 事件接收

- 单一事件协程消费 `DdsNode::Ticket`，按信封 event kind 分发：
  - SIGNAL：解码参数，发射同名 Qt 信号；
  - PROP：按 revision 决定是否更新缓存，更新后发射 `<prop>Changed`。
- 客户端信号在节点执行线程发射；Qt 自动连接在跨线程时排队。
- schema/version 不匹配时记录错误并停止该事件处理（发一次 `syncFailed`）。

### 8.5 RPC

- SLOT 客户端方法构造请求，调用 `DdsNode::RequestForResultDirect`，按 `request_retry` 重发；解析 reply，返回 `Coro::Result<T>`。
- READWRITE / READPUSH 的远端 setter 也走请求-响应，等待服务端确认；成功应答携带新 revision 与值，客户端按 revision 合并缓存。
- 客户端方法应在协程中调用；可在多个协程中并发发起不同请求。
- 请求重试可能造成服务端重复执行；语义与 Transport 一致，业务需幂等。

### 8.6 客户端上行消息

- 客户端没有独立的 `Publish` API，也不是 DDS 事件 topic 的 publisher；客户端向服务端发送消息只能通过 `SLOT` 请求-响应：
  - IDL 中声明 `SLOT(void report(...))`；
  - 客户端调用对应的协程方法；
  - 服务端在纯虚处理函数中接收并处理。
- 该路径返回服务端的处理结果/错误，不是 fire-and-forget；Transport 重试可能使处理函数被执行多次，业务需幂等。
- 如果上行处理需要再通知订阅者，由服务端处理函数调用生成的 `publish<Signal>()`。
- 第一版不提供客户端→服务端的单向调用，也不允许客户端发布 SIGNAL/PROP 事件。

## 9. PROP 映射表

| 修饰符 | 服务端 setter | 客户端写方法 | 客户端可观察 |
|---|---|---|---|
| `READPUSH`（缺省） | 有 | `push<Prop>` | getter + `<prop>Changed` |
| `READWRITE` | 有 | `set<Prop>` | getter + `<prop>Changed` |
| `READONLY` | 有 | 无 | getter + `<prop>Changed` |
| `SOURCEONLYSETTER` | 有 | 无 | getter + `<prop>Changed` |
| `CONSTANT` | 无 | 无 | getter + 初始快照 |

- 属性的初始值来自 IDL 默认表达式；客户端先以该值初始化缓存，握手成功后由服务端快照覆盖。
- 快照与事件均带 revision；客户端只接受更高的 revision。先订阅事件、后请求快照，避免启动窗口丢更新。
- 服务端属性变更 revision 单调递增；属性事件和 SIGNAL 事件共享同一服务运行期间的事件序号空间。revision 只用于同一轮服务运行内的排序/去重，不承担实例身份识别；链路重连后的自动刷新和 `refreshProperties()` 都会用快照重置客户端 revision 基线。
- `PERSISTED` 不在本版支持。

## 10. 生命周期与线程契约

- 宿主拥有并启停 `DdsTransport`；`DdsNode` 借用 transport；生成的 Service 借用 node，生成的 Client 借用 node 和同一个 transport（用于直接轮询链路状态）。transport 必须比 client 活得久。
- 推荐顺序：

```text
transport.Start()
node creation
service/client.prepare()          // 注册
node.Start()
service.start(); client.start()   // 订阅/握手；client 不等待服务
...
client.stop(); service.stop()
node.Close(); node.WaitClosed()
transport.Close(); transport.WaitClosed()
```

- 所有 endpoint 方法在 node 的 fiber 执行域内调用；`start/stop` 是协程方法。
- 客户端 `prepare/start` 均不阻塞等待服务；只有实际 RPC 调用按 `request_retry` 等待结果。
- 服务端每个请求独立 Coro task；无框架并发限制。
- 显式 stop 是销毁前的前置条件。运行时析构只做断言/诊断，不替调用方在非协程上下文 join。
- 同一 DdsNode 上的同一服务名不能同时是 client 和 service；由 Transport 返回错误。
- 生成属性状态不跨线程访问；跨线程使用 endpoint 不受支持。

## 11. 构建与依赖

### 11.1 子模块

```text
third_party/AsyncTask   -> d231d64
third_party/Transport   -> 1c79cd5 (0.6.0)
```

- 根工程锁定以上 commit。
- 不初始化 Transport 自带的旧 AsyncTask 子模块；CMake 将 `ASYNCTASK_DIR` 指向根 `third_party/AsyncTask`。
- CMake 提供 `DDSRPC_TRANSPORT_DIR`、`DDSRPC_ASYNCTASK_DIR` 覆盖变量，缺省指向上述子模块；子模块缺失时 configure 直接给出明确错误。
- 需在 WSL/Ubuntu 安装 `libqt5serialport5-dev`；Boost 1.72 已安装在 `/usr/local`，Qt 5.12.8、CMake 3.16 可用。
- 两个显式兼容性风险，实现阶段先做 spike 验证：Transport 0.6.0 上游锁的是 AsyncTask `6f4225`，本工程改用 `d231d64`；AsyncTask README 声称 Boost 1.89，而当前 WSL 为 1.72。若编译失败，先报告具体缺项（升级 AsyncTask/Boost），不静默回退。

### 11.2 CMake

顶层目标：

- `ddsrpcgen`：生成器；
- `ddsrpc_runtime`：静态运行时库，别名 `ddsrpc::runtime`；
- `ddsrpcgen_tests`；
- `ddsrpc_runtime_tests`；
- CMake 示例 `hello`：单进程内创建共享 FakeDdsProvider 总线上的 service/client 两个 DdsNode，演示先启动 client、再启动 service 后的自动握手/属性刷新和 RPC/信号。
  - FakeDdsProvider 是进程内总线，因此不提供跨进程的 hello_server/hello_client；跨进程版本需后续使用 Fast DDS。

接入函数：

```cmake
ddsrpc_add_idl(<target>
    ROLES service|client|"service;client"
    [NAMESPACE <ns>]
    [INCLUDE_DIRS <dirs...>]
    IDL <root.idl...>)
```

行为：

- 在 configure 阶段按与生成器一致的规则扫描 `IMPORT` 图，计算所有输入和输出文件；
- 在 build 阶段对每个目标发起一次可能覆盖多个根 IDL 的生成调用；
- 将生成文件加入 `target_sources`；
- 只把 client 角色头加入 moc 处理；
- 设置 include 目录并链接 `ddsrpc::runtime`；
- 用 depfile 跟踪被导入 IDL 的增量依赖；
- 不同 stem 导致的输出冲突在 configure 阶段报错。

### 11.3 qmake 示例

- `examples/qmake_hello` 使用 `QMAKE_EXTRA_COMPILERS` 调用 `ddsrpcgen --role all`；
- 链接 CMake 构建出的 `ddsrpc_runtime` / `transport` / `asynctask` 静态库及 Boost；
- 目标是证明 qmake 消费路径可用，不承诺与 CMake 等价的完整库构建支持。

## 12. 测试与验收

### 12.1 生成器测试

- 语法：注释、POD、枚举（含负数/十六进制）、PROP 全部修饰符、默认值、SIGNAL/SLOT、未命名参数、嵌套模板；
- IMPORT：相对路径、`-I`、传递导入、循环、缺失、重复、同 stem 冲突；
- 非法名/保留名/名称冲突；
- 不支持构造（`MODEL/CLASS/FLAG/USE_ENUM/PERSISTED`、花括号 POD、`#HEADER/#FOOTER`）的错误定位；
- `--role` 输出集合；
- 同一输入生成内容确定性；
- schema hash 对声明变化敏感。

### 12.2 Wire 测试

- Request/Reply/Event 编解码往返；
- magic、version、schema 校验；
- 截断、尾随数据、错误状态；
- 生成 POD/枚举/Qt 容器类型的 QDataStream 往返。

### 12.3 端到端测试

使用生成的 fixture IDL、两个独立 DdsNode、一个进程内共享的 FakeDdsProvider 总线：

- 先启动 client、后启动 service：`start()` 立即成功、`ready()` 初始为 false；service 上线后后台握手使 `ready()` 变 true；
- SLOT 返回值/void、多个并发调用（不同 Coro task 处理，允许乱序回复）；
- 客户端通过上行 SLOT 上报，服务端处理后发布 SIGNAL，其他订阅客户端收到；
- handler 返回 `RpcErrc` 与抛异常两条错误路径；
- SIGNAL 发布/订阅；
- 属性初始快照、变更推送、READWRITE/READPUSH 远端写入；
- 不支持远端写的属性不生成客户端 setter（在生成器测试中断言生成文本）；
- schema hash 不匹配；
- 生命周期误用：node 未 Running 时 start、重复 stop、stop 时存在在途请求；
- 先 client 后 service 的自动握手；
- 服务端停止再以相同 schema 重启：链路状态由 Up 变非 Up，客户端 `ready` 变 false；再次 Up 后自动重新订阅、刷新 PROP，缓存反映重启后的新值，`ready` 恢复 true；
- 网络中断/恢复路径（用 FakeDdsProvider 停止/重建端点模拟）同样触发自动刷新；
- 重连不自动重发在途 RPC（调用方收到错误/超时）；
- import 后跨文件类型/接口可用。

运行方式：`ctest --test-dir build --output-on-failure`。

### 12.4 qmake 示例验收

- 用 qmake 执行生成、编译并运行 fake 单进程示例。

### 12.5 验收标准

- WSL 下 CMake 构建、全部测试通过；
- qmake 示例构建运行通过；
- service-only / client-only 目标只包含对应角色生成物；
- 服务端重启或模拟断线重连后，客户端能通过链路状态检测自动完成 `ready` 恢复和 PROP 全量刷新；
- 未支持语法产生确定性诊断；
- 文档与实际生成 API 一致；
- 真实 Fast DDS 不作为第一版验收条件。

## 13. 限制与后续方向

### 13.1 已知限制

- 双端必须使用同一生成器/Qt 序列化路径；不支持与标准 CDR DDS 系统/其他语言互通。
- SIGNAL 可丢失；属性需依赖快照；DDS 读队列仍会满时丢最旧。
- 无请求去重，自动重试可能重复执行 SLOT。
- 服务端无并发上限，慢处理器/请求洪水可能导致协程过多。
- READPUSH/READWRITE 都等待服务端确认，和 QtRO 的异步 push 语义不完全相同。
- 客户端不能直接发布 SIGNAL/PROP，也没有上行单向调用；客户端→服务端只走 SLOT 请求-响应。
- 重连后会自动刷新 PROP 并恢复 `ready`，但不自动重发/恢复在途 RPC；调用方需自行重试。
- 不引入 UUID/会话跟踪；断线检测直接调用 `DdsTransport::CurrentLinkState()`，该状态是已声明端点整体聚合，不是按服务区分。若链路状态未发生可观察的下降，自动刷新可能不会触发；同一 client node 挂多个服务时也可能由其他端点状态触发刷新。后续 SDK 变化时直接修改生成代码中的状态查询。
- qmake 只是消费示例，不是完整构建体系。
- 仅验证 FakeDdsProvider；真实 Fast DDS 路径待后续验证。

### 13.2 后续

- `MODEL/CLASS/FLAG/USE_ENUM`、`PERSISTED`、花括号 POD；
- 标准 CDR/Fast DDS 类型映射与其他语言互通；
- 在途 RPC 的自动重试/幂等恢复；
- 按服务/接口的链路状态与重连检测；
- 并发上限、请求去重、背压；
- qmake 与 CMake 等价支持；
- 可观测性与指标。
