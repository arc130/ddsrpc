#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include "cli.h"
#include "emit_client.h"
#include "emit_service.h"
#include "emit_types.h"
#include "model.h"

namespace {

void WriteFile(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(data);
}

QByteArray ReadFile(const QString& path) {
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

}  // namespace

TEST(CodegenTest, EmitsEnumPodAndStreamOperators) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("common");
  ddsrpcgen::PodModel pod;
  pod.name = QStringLiteral("Point");
  pod.fields = {
      ddsrpcgen::PodFieldModel{QStringLiteral("x"), QStringLiteral("int")},
      ddsrpcgen::PodFieldModel{QStringLiteral("y"), QStringLiteral("int")},
  };
  file.pods = {pod};
  ddsrpcgen::EnumModel mode;
  mode.idl_name = QStringLiteral("Mode");
  mode.cpp_name = QStringLiteral("Mode");
  mode.values = {{QStringLiteral("Idle"), 0}, {QStringLiteral("Running"), 1}};
  file.enums = {mode};

  const QString text = ddsrpcgen::EmitTypesHeader(file, QStringLiteral("demo"));
  EXPECT_NE(text.indexOf(QStringLiteral("enum class Mode : qint32")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("struct Point")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("operator<<(QDataStream")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("registerTypes")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("namespace demo")), -1);
  EXPECT_EQ(text.count(QStringLiteral("namespace demo")), 1);
}

TEST(CodegenTest, EmitsServiceAndClientForSlots) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("calc");
  file.schema_hash = 0x42;
  ddsrpcgen::ClassModel cls;
  cls.idl_name = QStringLiteral("Calc");
  cls.slots = {
      ddsrpcgen::SlotModel{QStringLiteral("add"),
                           QStringLiteral("int"),
                           {{QStringLiteral("a"), QStringLiteral("int"), true},
                            {QStringLiteral("b"), QStringLiteral("int"), true}},
                           1},
  };
  file.classes = {cls};
  const QString service_h = ddsrpcgen::EmitServiceHeader(file, QStringLiteral("demo"));
  EXPECT_NE(service_h.indexOf(QStringLiteral("class CalcService")), -1);
  EXPECT_NE(service_h.indexOf(QStringLiteral(
                "virtual ::Coro::Result<int> add(int a, int b) = 0;")),
            -1);
  const QString client_h = ddsrpcgen::EmitClientHeader(file, QStringLiteral("demo"));
  EXPECT_NE(client_h.indexOf(QStringLiteral("class CalcClient")), -1);
  EXPECT_NE(client_h.indexOf(QStringLiteral("Q_OBJECT")), -1);
  EXPECT_NE(client_h.indexOf(QStringLiteral("::Coro::Result<int> add(int a, int b)")), -1);
  EXPECT_NE(client_h.indexOf(QStringLiteral("::transport::DdsTransport& transport")), -1);
}

TEST(CodegenTest, EmitsServiceLifecycleAndRpcDispatch) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("calc");
  file.schema_hash = 0x42;
  ddsrpcgen::EnumModel state;
  state.idl_name = QStringLiteral("State");
  state.cpp_name = QStringLiteral("Calc_State");
  state.class_owner = QStringLiteral("Calc");
  state.values = {{QStringLiteral("Idle"), 0}, {QStringLiteral("Ready"), 1}};
  ddsrpcgen::ClassModel cls;
  cls.idl_name = QStringLiteral("Calc");
  cls.class_enums = {state};
  cls.slots = {
      ddsrpcgen::SlotModel{QStringLiteral("add"),
                           QStringLiteral("int"),
                           {{QStringLiteral("a"), QStringLiteral("int"), true},
                            {QStringLiteral("b"), QStringLiteral("int"), true}},
                           1},
      ddsrpcgen::SlotModel{QStringLiteral("reset"),
                           QStringLiteral("void"),
                           {},
                           2},
  };
  file.classes = {cls};

  const QString header = ddsrpcgen::EmitServiceHeader(file, QStringLiteral("demo"));
  EXPECT_NE(header.indexOf(QStringLiteral(
                "class CalcService : public ::ddsrpc::ServiceBase")),
            -1);
  EXPECT_NE(header.indexOf(QStringLiteral("struct Options")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::std::string service_name = \"Calc\";")), -1);
  // Nested defaults plus an in-class `Options options = {}` default argument
  // are rejected by GCC 9; the out-of-line constructor avoids that.
  EXPECT_NE(header.indexOf(QStringLiteral("Options();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::Coro::Result<void> prepare();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::Coro::Result<void> start();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::Coro::Result<void> stop();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("using State = Calc_State;")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral(
                "::ddsrpc::OperationResult HandleOperation(::quint32 op, "
                "::QDataStream& args) override;")),
            -1);
  EXPECT_NE(header.indexOf(QStringLiteral(
                "::ddsrpc::OperationResult BuildPropertySnapshot() override;")),
            -1);

  const QString source = ddsrpcgen::EmitServiceSource(file, QStringLiteral("demo"));
  EXPECT_NE(source.indexOf(QStringLiteral("0x0000000000000042ULL")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("\"ddsrpc.\" + options.service_name + \".events\"")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("registerTypes();")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("CalcService::prepare()")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("return this->Prepare();")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("return this->Start();")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("return this->Stop();")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("case 1:")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("args >> a;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("args >> b;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("args.status() != ::QDataStream::Ok")), -1);
  // this-> keeps a parameter with the slot's name from hiding the virtual.
  EXPECT_NE(source.indexOf(QStringLiteral("const auto result = this->add(a, b);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::wire::Begin(out);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("out << result.value();")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("case 2:")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("const auto result = this->reset();")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::RpcErrc::kDecode")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::RpcErrc::kEncode")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::RpcErrc::kInvalidArgument")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::QByteArray payload;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::QDataStream out(&payload, ::QIODevice::WriteOnly);")),
            -1);
  EXPECT_NE(source.indexOf(QStringLiteral("out << ::quint32(0);")), -1);
}

TEST(CodegenTest, EmitsClientLifecycleSignalsAndSlotCalls) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("calc");
  file.schema_hash = 0x42;
  ddsrpcgen::EnumModel state;
  state.idl_name = QStringLiteral("State");
  state.cpp_name = QStringLiteral("Calc_State");
  state.class_owner = QStringLiteral("Calc");
  state.values = {{QStringLiteral("Idle"), 0}};
  ddsrpcgen::ClassModel cls;
  cls.idl_name = QStringLiteral("Calc");
  cls.class_enums = {state};
  cls.signals = {ddsrpcgen::SignalModel{
      QStringLiteral("calculated"),
      {{QStringLiteral("result"), QStringLiteral("int"), true}},
      1}};
  cls.slots = {
      ddsrpcgen::SlotModel{QStringLiteral("add"),
                           QStringLiteral("int"),
                           {{QStringLiteral("a"), QStringLiteral("int"), true},
                            {QStringLiteral("b"), QStringLiteral("int"), true}},
                           1},
      ddsrpcgen::SlotModel{QStringLiteral("reset"),
                           QStringLiteral("void"),
                           {},
                           2},
  };
  file.classes = {cls};

  const QString header = ddsrpcgen::EmitClientHeader(file, QStringLiteral("demo"));
  EXPECT_NE(header.indexOf(QStringLiteral(
                "class CalcClient : public ::QObject, public ::ddsrpc::ClientBase")),
            -1);
  EXPECT_NE(header.indexOf(QStringLiteral("Q_OBJECT")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("using State = Calc_State;")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("void calculated(int result);")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("void readyChanged(bool ready);")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("void syncFailed(::QString message);")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::transport::RetryPolicy request_retry")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral(
                "::std::chrono::milliseconds link_poll_interval{100};")),
            -1);
  EXPECT_NE(header.indexOf(QStringLiteral("Options();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("~CalcClient() override;")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::Coro::Result<void> prepare();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::Coro::Result<void> start();")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("::Coro::Result<void> stop();")), -1);
  // The destructor contract is documented and readyChanged/syncFailed are
  // attributed to both the link-monitor and event fibers.
  EXPECT_NE(header.indexOf(QStringLiteral("call stop() before destruction")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("is defaulted: it neither calls stop()")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("link-monitor")), -1);
  EXPECT_NE(header.indexOf(QStringLiteral("event fiber")), -1);

  const QString source = ddsrpcgen::EmitClientSource(file, QStringLiteral("demo"));
  EXPECT_NE(source.indexOf(QStringLiteral("0x0000000000000042ULL")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("CalcClient::prepare()")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("CalcClient::~CalcClient() = default;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("CalcClient::Options::Options() = default;")), -1);
  const int subscribe = source.indexOf(QStringLiteral("this->StartEvents()"));
  const int monitor = source.indexOf(QStringLiteral("::Coro::makeTask"));
  ASSERT_NE(subscribe, -1);
  ASSERT_NE(monitor, -1);
  EXPECT_LT(subscribe, monitor) << "start() must subscribe before creating the link monitor";
  const int stop = source.indexOf(QStringLiteral("CalcClient::stop()"));
  ASSERT_NE(stop, -1);
  const int cancel = source.indexOf(QStringLiteral("link_monitor_->cancel();"), stop);
  const int endpoint = source.indexOf(QStringLiteral("this->StopEndpoint()"), stop);
  ASSERT_NE(cancel, -1);
  ASSERT_NE(endpoint, -1);
  EXPECT_LT(cancel, endpoint) << "stop() must join the link monitor before StopEndpoint()";
  EXPECT_NE(source.indexOf(QStringLiteral(
                "link_monitor_fiber_ == ::boost::this_fiber::get_id()")),
            -1);
  EXPECT_NE(source.indexOf(QStringLiteral(
                "::QMessageLogger(__FILE__, __LINE__, __func__)")),
            -1);
  EXPECT_NE(source.indexOf(QStringLiteral("defer it until the callback returns")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("transport_.CurrentLinkState()")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::Coro::delay_for(interval)")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral(
                "link_monitor_fiber_ = ::boost::fibers::fiber::id();")),
            -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::wire::Begin(out);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::QDataStream in(reply.value());")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::wire::Begin(in);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("in >> value;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::ddsrpc::RpcErrc::kDecode")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("Q_EMIT readyChanged(ready);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("Q_EMIT syncFailed(message);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("out << a;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("::QByteArray args;")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("auto reply = this->Call(1, args);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("auto reply = this->Call(2, args);")), -1);
  EXPECT_NE(source.indexOf(QStringLiteral("in >> count;")), -1);
}

TEST(CodegenTest, EmitsShadowSafeNamesForHostileParameters) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("hostile");
  file.schema_hash = 0x7;
  ddsrpcgen::ClassModel cls;
  cls.idl_name = QStringLiteral("Hostile");
  cls.slots = {
      ddsrpcgen::SlotModel{
          QStringLiteral("call"),
          QStringLiteral("demo::Values"),
          {{QStringLiteral("Call"), QStringLiteral("int"), true},
           {QStringLiteral("ddsrpc"), QStringLiteral("QByteArray"), true},
           {QStringLiteral("QDataStream"), QStringLiteral("QString"), true},
           {QStringLiteral("out"), QStringLiteral("int"), true},
           {QStringLiteral("in"), QStringLiteral("QByteArray"), true},
           {QStringLiteral("reply"), QStringLiteral("int"), true}},
          1},
      ddsrpcgen::SlotModel{QStringLiteral("vector"),
                           QStringLiteral("QVector<int>"),
                           {{QStringLiteral("QVector"), QStringLiteral("int"), true},
                            {QStringLiteral("items"), QStringLiteral("QVector<int>"), true}},
                           2},
  };
  file.classes = {cls};

  const QString service_h = ddsrpcgen::EmitServiceHeader(file, QStringLiteral("demo"));
  EXPECT_NE(service_h.indexOf(QStringLiteral(
                "virtual ::Coro::Result<::demo::Values> call("
                "int Call, ::QByteArray ddsrpc, ::QString QDataStream, int out, "
                "::QByteArray in, int reply) = 0;")),
            -1);
  EXPECT_NE(service_h.indexOf(QStringLiteral(
                "::Coro::Result<::QVector<int>> vector(int QVector, "
                "::QVector<int> items) = 0;")),
            -1);

  const QString service_src = ddsrpcgen::EmitServiceSource(file, QStringLiteral("demo"));
  EXPECT_NE(service_src.indexOf(QStringLiteral("int Call{};")), -1);
  EXPECT_NE(service_src.indexOf(QStringLiteral("::QByteArray ddsrpc{};")), -1);
  EXPECT_NE(service_src.indexOf(QStringLiteral("::QString QDataStream{};")), -1);
  EXPECT_NE(service_src.indexOf(QStringLiteral(
                "this->call(Call, ddsrpc, QDataStream, out, in, reply)")),
            -1);
  EXPECT_NE(service_src.indexOf(QStringLiteral(
                "::QDataStream out_(&payload, ::QIODevice::WriteOnly);")),
            -1);

  const QString client_h = ddsrpcgen::EmitClientHeader(file, QStringLiteral("demo"));
  EXPECT_NE(client_h.indexOf(QStringLiteral(
                "::Coro::Result<::demo::Values> call("
                "int Call, ::QByteArray ddsrpc, ::QString QDataStream, int out, "
                "::QByteArray in, int reply);")),
            -1);
  EXPECT_NE(client_h.indexOf(QStringLiteral(
                "::Coro::Result<::QVector<int>> vector(int QVector, "
                "::QVector<int> items);")),
            -1);

  const QString client_src = ddsrpcgen::EmitClientSource(file, QStringLiteral("demo"));
  EXPECT_NE(client_src.indexOf(QStringLiteral(
                "auto reply_ = this->Call(1, args);")),
            -1);
  EXPECT_NE(client_src.indexOf(QStringLiteral(
                "::QDataStream in_(reply_.value());")),
            -1);
  EXPECT_NE(client_src.indexOf(QStringLiteral("::demo::Values value{};")), -1);
  EXPECT_NE(client_src.indexOf(QStringLiteral("::QVector<int> value{};")), -1);
}

TEST(CodegenTest, CliGeneratesTypesFileAtomically) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString idl = tmp.filePath("wire.idl");
  QFile in(idl);
  ASSERT_TRUE(in.open(QIODevice::WriteOnly | QIODevice::Truncate));
  in.write("POD Pair(int a, int b)\n");
  in.close();
  const QString out = tmp.filePath("generated");
  QStringList args{
      QStringLiteral("ddsrpcgen"),
      QStringLiteral("--role"), QStringLiteral("types"),
      QStringLiteral("--out-dir"), out,
      QStringLiteral("--namespace"), QStringLiteral("demo"),
      idl,
  };
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/wire.ddsrpc.types.h")));
  QFile generated(out + QStringLiteral("/wire.ddsrpc.types.h"));
  ASSERT_TRUE(generated.open(QIODevice::ReadOnly));
  const QString text = QString::fromUtf8(generated.readAll());
  EXPECT_NE(text.indexOf(QStringLiteral("namespace demo")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("struct Pair")), -1);
}

TEST(CodegenTest, CliMergesSharedImportsAcrossRoots) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  QFile common(tmp.filePath("common.idl"));
  ASSERT_TRUE(common.open(QIODevice::WriteOnly | QIODevice::Truncate));
  common.write("POD Point(int x, int y)\n");
  common.close();
  QFile a(tmp.filePath("a.idl"));
  ASSERT_TRUE(a.open(QIODevice::WriteOnly | QIODevice::Truncate));
  a.write("IMPORT \"common.idl\";\nclass A { }\n");
  a.close();
  QFile b(tmp.filePath("b.idl"));
  ASSERT_TRUE(b.open(QIODevice::WriteOnly | QIODevice::Truncate));
  b.write("IMPORT \"common.idl\";\nclass B { }\n");
  b.close();
  const QString out = tmp.filePath("generated");
  QStringList args{
      QStringLiteral("ddsrpcgen"), QStringLiteral("--role"), QStringLiteral("types"),
      QStringLiteral("--out-dir"), out, QStringLiteral("--namespace"),
      QStringLiteral("demo"), tmp.filePath("a.idl"), tmp.filePath("b.idl"),
  };
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/a.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/b.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/common.ddsrpc.types.h")));
}

TEST(CodegenTest, EmitsNestedNamespaceClassEnumsAndImports) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("wire");
  file.imports = {QStringLiteral("/tmp/idl/common.idl")};
  file.directives = {ddsrpcgen::Directive{QStringLiteral("#include <QString>"),
                                          ddsrpcgen::SourceLocation{}}};
  ddsrpcgen::EnumModel mode;
  mode.idl_name = QStringLiteral("Mode");
  mode.cpp_name = QStringLiteral("Mode");
  mode.values = {{QStringLiteral("Idle"), 0}};
  ddsrpcgen::EnumModel state;
  state.idl_name = QStringLiteral("State");
  state.cpp_name = QStringLiteral("Foo_State");
  state.class_owner = QStringLiteral("Foo");
  state.values = {{QStringLiteral("Off"), 0}};
  ddsrpcgen::ClassModel cls;
  cls.class_enums = {state};
  file.classes = {cls};
  file.enums = {mode};
  ddsrpcgen::PodModel point;
  point.name = QStringLiteral("Point");
  point.fields = {
      ddsrpcgen::PodFieldModel{QStringLiteral("mode"), QStringLiteral("a::b::Mode")},
  };
  file.pods = {point};

  const QString text = ddsrpcgen::EmitTypesHeader(file, QStringLiteral("a::b"));
  EXPECT_TRUE(text.startsWith(QStringLiteral("#ifndef DDSRPC_GEN_TYPES_WIRE_H")));
  EXPECT_NE(text.indexOf(QStringLiteral("#include \"common.ddsrpc.types.h\"")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("enum class Mode : qint32")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("enum class Foo_State : qint32")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("struct Point")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("a::b::Mode mode{};")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("Q_DECLARE_METATYPE(a::b::Mode)")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("Q_DECLARE_METATYPE(a::b::Foo_State)")), -1);
  EXPECT_EQ(text.count(QStringLiteral("namespace a")), 1);
  EXPECT_EQ(text.count(QStringLiteral("namespace b")), 1);
  EXPECT_NE(text.indexOf(QStringLiteral("static std::once_flag")), -1);
  EXPECT_EQ(text.indexOf(QStringLiteral("operator<(")), -1);
  EXPECT_LT(text.indexOf(QStringLiteral("#include <QString>")),
            text.indexOf(QStringLiteral("#include <QtCore/QDataStream>")));
  EXPECT_TRUE(text.endsWith(QLatin1Char('\n')));
}

TEST(CodegenTest, CliRejectsUsageErrors) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString idl = tmp.filePath("wire.idl");
  WriteFile(idl, "POD Pair(int a)\n");
  const QString out = tmp.filePath("generated");
  using ddsrpcgen::RunCli;
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen")}), 2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--bogus"),
                    QStringLiteral("--out-dir"), out, idl}),
            2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                    QStringLiteral("types"), idl}),
            2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                    QStringLiteral("bogus"), QStringLiteral("--out-dir"), out, idl}),
            2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                    QStringLiteral("types"), QStringLiteral("--out-dir"), out}),
            2);
  EXPECT_FALSE(QFile::exists(out));
}

TEST(CodegenTest, CliGeneratesServiceClientAndAllRoles) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString typed_idl = tmp.filePath("calc.idl");
  WriteFile(typed_idl, "class Calc { SLOT(int add(int a, int b)) };\n");
  const QString empty_idl = tmp.filePath("only.idl");
  WriteFile(empty_idl, "POD Only(int value)\n");

  const QString out = tmp.filePath("all");
  QStringList args{QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                   QStringLiteral("all"), QStringLiteral("--out-dir"), out,
                   QStringLiteral("--namespace"), QStringLiteral("demo"), typed_idl,
                   empty_idl};
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  for (const QString& suffix :
       {QStringLiteral(".ddsrpc.types.h"), QStringLiteral(".ddsrpc.service.h"),
        QStringLiteral(".ddsrpc.service.cpp"), QStringLiteral(".ddsrpc.client.h"),
        QStringLiteral(".ddsrpc.client.cpp")}) {
    EXPECT_TRUE(QFile::exists(out + QStringLiteral("/calc") + suffix)) << suffix.toStdString();
    EXPECT_TRUE(QFile::exists(out + QStringLiteral("/only") + suffix)) << suffix.toStdString();
  }
  const QString service = QString::fromUtf8(
      ReadFile(out + QStringLiteral("/calc.ddsrpc.service.h")));
  EXPECT_NE(service.indexOf(QStringLiteral("#include \"calc.ddsrpc.types.h\"")), -1);
  EXPECT_NE(service.indexOf(QStringLiteral("class CalcService")), -1);
  const QString empty_service = QString::fromUtf8(
      ReadFile(out + QStringLiteral("/only.ddsrpc.service.h")));
  EXPECT_NE(empty_service.indexOf(QStringLiteral("#include \"only.ddsrpc.types.h\"")), -1);
  EXPECT_EQ(empty_service.indexOf(QStringLiteral("class ")), -1);
  const QString empty_client = QString::fromUtf8(
      ReadFile(out + QStringLiteral("/only.ddsrpc.client.h")));
  EXPECT_NE(empty_client.indexOf(QStringLiteral("#include \"only.ddsrpc.types.h\"")), -1);
  EXPECT_EQ(empty_client.indexOf(QStringLiteral("class ")), -1);
  const QString empty_cpp = QString::fromUtf8(
      ReadFile(out + QStringLiteral("/only.ddsrpc.client.cpp")));
  EXPECT_NE(empty_cpp.indexOf(QStringLiteral("#include \"only.ddsrpc.client.h\"")), -1);

  const QString depfile =
      QString::fromUtf8(ReadFile(out + QStringLiteral("/ddsrpc.d")));
  EXPECT_NE(depfile.indexOf(QStringLiteral("calc.ddsrpc.service.cpp")), -1);
  EXPECT_NE(depfile.indexOf(QStringLiteral("calc.ddsrpc.client.cpp")), -1);

  const QString service_only = tmp.filePath("service_only");
  ASSERT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("service"), QStringLiteral("--out-dir"),
                               service_only, typed_idl}),
            0);
  EXPECT_TRUE(QFile::exists(service_only + QStringLiteral("/calc.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(service_only + QStringLiteral("/calc.ddsrpc.service.h")));
  EXPECT_TRUE(QFile::exists(service_only + QStringLiteral("/calc.ddsrpc.service.cpp")));
  EXPECT_FALSE(QFile::exists(service_only + QStringLiteral("/calc.ddsrpc.client.h")));
  EXPECT_FALSE(QFile::exists(service_only + QStringLiteral("/calc.ddsrpc.client.cpp")));

  const QString client_only = tmp.filePath("client_only");
  ASSERT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("client"), QStringLiteral("--out-dir"),
                               client_only, typed_idl}),
            0);
  EXPECT_TRUE(QFile::exists(client_only + QStringLiteral("/calc.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(client_only + QStringLiteral("/calc.ddsrpc.client.h")));
  EXPECT_TRUE(QFile::exists(client_only + QStringLiteral("/calc.ddsrpc.client.cpp")));
  EXPECT_FALSE(QFile::exists(client_only + QStringLiteral("/calc.ddsrpc.service.h")));
  EXPECT_FALSE(QFile::exists(client_only + QStringLiteral("/calc.ddsrpc.service.cpp")));
}

TEST(CodegenTest, CliWritesDepfileAndReplacesOutputsDeterministically) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString idl = tmp.filePath("wire.idl");
  WriteFile(idl, "POD Pair(int a)\n");
  const QString out = tmp.filePath("generated");
  const QString header = out + QStringLiteral("/wire.ddsrpc.types.h");
  QStringList args{
      QStringLiteral("ddsrpcgen"), QStringLiteral("--role"), QStringLiteral("types"),
      QStringLiteral("--out-dir"), out, QStringLiteral("--namespace"), QStringLiteral("demo"),
      idl,
  };
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  const QByteArray first = ReadFile(header);
  EXPECT_NE(first.indexOf("struct Pair"), -1);
  EXPECT_TRUE(first.endsWith('\n'));

  const QString depfile = out + QStringLiteral("/ddsrpc.d");
  ASSERT_TRUE(QFile::exists(depfile));
  const QString dependencies = QString::fromUtf8(ReadFile(depfile));
  EXPECT_NE(dependencies.indexOf(QFileInfo(header).absoluteFilePath()), -1);
  EXPECT_NE(dependencies.indexOf(QFileInfo(idl).canonicalFilePath()), -1);
  EXPECT_NE(dependencies.indexOf(QStringLiteral(": ")), -1);

  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  EXPECT_EQ(ReadFile(header), first);

  WriteFile(idl, "POD Pair(int a, int b)\n");
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  const QByteArray second = ReadFile(header);
  EXPECT_NE(second.indexOf("int b"), -1);
  EXPECT_TRUE(QDir(out).entryList({QStringLiteral("*.tmp")}, QDir::Files).isEmpty());

  const QString custom_depfile = tmp.filePath("deps/custom.d");
  QStringList custom_args = args;
  custom_args.insert(custom_args.size() - 1, QStringLiteral("--depfile"));
  custom_args.insert(custom_args.size() - 1, custom_depfile);
  ASSERT_EQ(ddsrpcgen::RunCli(custom_args), 0);
  EXPECT_TRUE(QFile::exists(custom_depfile));
}

TEST(CodegenTest, EmitsDistinctGuardsForStemsThatSanitizeTheSame) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  WriteFile(tmp.filePath("foo-bar.idl"), "POD Alpha(int a)\n");
  WriteFile(tmp.filePath("foo_bar.idl"), "POD Beta(int b)\n");
  const QString out = tmp.filePath("generated");
  ASSERT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("types"), QStringLiteral("--out-dir"), out,
                               QStringLiteral("--namespace"), QStringLiteral("demo"),
                               tmp.filePath("foo-bar.idl"), tmp.filePath("foo_bar.idl")}),
            0);
  const QString dashed =
      QString::fromUtf8(ReadFile(out + QStringLiteral("/foo-bar.ddsrpc.types.h")));
  const QString underscored =
      QString::fromUtf8(ReadFile(out + QStringLiteral("/foo_bar.ddsrpc.types.h")));
  EXPECT_NE(dashed.indexOf(QStringLiteral("#ifndef DDSRPC_GEN_TYPES_FOO_x2D_BAR_H")), -1);
  EXPECT_NE(underscored.indexOf(QStringLiteral("#ifndef DDSRPC_GEN_TYPES_FOO_BAR_H")), -1);
  EXPECT_NE(dashed.indexOf(QStringLiteral("struct Alpha")), -1);
  EXPECT_NE(underscored.indexOf(QStringLiteral("struct Beta")), -1);
}

TEST(CodegenTest, CliHonorsIncludeDirsAndAnalyzeRejectsDuplicateStems) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  WriteFile(tmp.filePath("include/dep.idl"), "POD Dep(int value)\n");
  WriteFile(tmp.filePath("root.idl"), "IMPORT \"dep.idl\";\nPOD Root(Dep dep)\n");
  const QString out = tmp.filePath("generated");
  ASSERT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("types"), QStringLiteral("--out-dir"), out,
                               QStringLiteral("-I"), tmp.filePath("include"),
                               tmp.filePath("root.idl")}),
            0);
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/root.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/dep.ddsrpc.types.h")));

  WriteFile(tmp.filePath("first/x.idl"), "POD X(int a)\n");
  WriteFile(tmp.filePath("second/x.idl"), "POD X(int b)\n");
  const QString duplicate_out = tmp.filePath("duplicate");
  EXPECT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("types"), QStringLiteral("--out-dir"),
                               duplicate_out, tmp.filePath("first/x.idl"),
                               tmp.filePath("second/x.idl")}),
            1);
  EXPECT_FALSE(QFile::exists(duplicate_out));
}
