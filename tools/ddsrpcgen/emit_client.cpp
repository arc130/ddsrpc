#include "emit_client.h"

#include <QStringList>

#include "code_writer.h"
#include "emit_common.h"

namespace ddsrpcgen {
namespace {

[[nodiscard]] QString ReturnType(const SlotModel& slot) {
  return slot.return_cpp_type.isEmpty() ? QStringLiteral("void")
                                        : slot.return_cpp_type;
}

[[nodiscard]] QString Parameters(const QVector<ParamModel>& params) {
  QStringList parts;
  parts.reserve(params.size());
  for (const ParamModel& param : params) {
    parts.append(param.cpp_type + QLatin1Char(' ') + param.name);
  }
  return parts.join(QStringLiteral(", "));
}

[[nodiscard]] QString UniqueLocal(const QString& base, QStringList* taken) {
  QString name = base;
  while (taken->contains(name)) name += QLatin1Char('_');
  taken->append(name);
  return name;
}

[[nodiscard]] QString HashLiteral(const FileModel& file) {
  return SchemaHashLiteral(file.schema_hash);
}

void EmitClassEnums(CodeWriter& writer, const ClassModel& cls) {
  for (const EnumModel& model : cls.class_enums) {
    writer.Line(QStringLiteral("using ") + model.idl_name + QStringLiteral(" = ") +
                model.cpp_name + QLatin1Char(';'));
  }
  if (!cls.class_enums.isEmpty()) writer.Line();
}

void EmitClassHeader(CodeWriter& writer, const ClassModel& cls) {
  writer.Line(QStringLiteral("class ") + cls.idl_name +
              QStringLiteral("Client : public QObject, public ddsrpc::ClientBase {"));
  writer.Line(QStringLiteral("  Q_OBJECT"));
  writer.Line();
  writer.Line(QStringLiteral("public:"));
  writer.Indent();
  writer.Line(QStringLiteral("struct Options {"));
  writer.Indent();
  writer.Line(QStringLiteral("std::string service_name = \"") + cls.idl_name +
              QStringLiteral("\";"));
  writer.Line(QStringLiteral(
      "transport::RetryPolicy request_retry{std::chrono::milliseconds(2000), 3};"));
  writer.Line(QStringLiteral("std::chrono::milliseconds handshake_interval{1000};"));
  writer.Line(QStringLiteral("std::chrono::milliseconds link_poll_interval{100};"));
  writer.Line();
  // GCC rejects a default argument inside the enclosing class when a nested
  // aggregate's default member initializers have not been parsed yet. A
  // user-declared default constructor (defined out of line) avoids that while
  // keeping the defaults next to the fields.
  writer.Line(QStringLiteral("Options();"));
  writer.Dedent();
  writer.Line(QStringLiteral("};"));
  writer.Line();
  writer.Line(QStringLiteral("explicit ") + cls.idl_name +
              QStringLiteral("Client(transport::DdsNode& node, "
                             "transport::DdsTransport& transport,"));
  writer.Line(QStringLiteral("    Options options = {}, QObject* parent = nullptr);"));
  writer.Line(QStringLiteral("~") + cls.idl_name + QStringLiteral("Client() override;"));
  writer.Line();
  writer.Line(cls.idl_name + QStringLiteral("Client(const ") + cls.idl_name +
              QStringLiteral("Client&) = delete;"));
  writer.Line(cls.idl_name + QStringLiteral("Client& operator=(const ") + cls.idl_name +
              QStringLiteral("Client&) = delete;"));
  writer.Line();
  EmitClassEnums(writer, cls);
  writer.Line(QStringLiteral("// Lifecycle: call stop() before destruction. The destructor"));
  writer.Line(QStringLiteral("// neither joins fibers nor calls virtual hooks; destroying a"));
  writer.Line(QStringLiteral("// running client is a usage error."));
  writer.Line(QStringLiteral("//"));
  writer.Line(QStringLiteral("// Do not call stop() from readyChanged, syncFailed, or any event"));
  writer.Line(QStringLiteral("// handler: those run on the event fiber that stop() joins, so the"));
  writer.Line(QStringLiteral("// call would self-join. Defer it to the next Qt event-loop turn."));
  writer.Line(QStringLiteral("// Those callbacks may safely call refreshProperties() or slots."));
  // ClientBase exposes Prepare (capitalized); the facade adds the lowercase
  // lifecycle name used by the public design. start()/stop() are generated.
  writer.Line(QStringLiteral("Coro::Result<void> prepare();"));
  writer.Line(QStringLiteral("Coro::Result<void> start();"));
  writer.Line(QStringLiteral("Coro::Result<void> stop();"));
  writer.Line();
  for (const SlotModel& slot : cls.slots) {
    writer.Line(QStringLiteral("Coro::Result<") + ReturnType(slot) + QStringLiteral("> ") +
                slot.name + QLatin1Char('(') + Parameters(slot.params) +
                QStringLiteral(");"));
  }
  if (!cls.slots.isEmpty()) writer.Line();
  writer.Dedent();
  writer.Line(QStringLiteral("Q_SIGNALS:"));
  writer.Indent();
  for (const SignalModel& signal : cls.signals) {
    writer.Line(QStringLiteral("void ") + signal.name + QLatin1Char('(') +
                Parameters(signal.params) + QStringLiteral(");"));
  }
  writer.Line(QStringLiteral("void readyChanged(bool ready);"));
  writer.Line(QStringLiteral("void syncFailed(QString message);"));
  writer.Dedent();
  writer.Line();
  writer.Line(QStringLiteral("protected:"));
  writer.Indent();
  writer.Line(QStringLiteral("void ResetPropertiesForResync() override;"));
  writer.Line(QStringLiteral(
      "Coro::Result<void> ApplyPropertySnapshot(QDataStream& in) override;"));
  writer.Line(QStringLiteral(
      "void DispatchSignal(quint32 event_id, QDataStream& in) override;"));
  writer.Line(QStringLiteral(
      "void DispatchProperty(quint32 event_id, quint64 revision, QDataStream& in) override;"));
  writer.Line(QStringLiteral("void EmitReadyChanged(bool ready) override;"));
  writer.Line(QStringLiteral("void EmitSyncFailed(const QString& message) override;"));
  writer.Dedent();
  writer.Line();
  writer.Line(QStringLiteral("private:"));
  writer.Indent();
  writer.Line(QStringLiteral("void LinkMonitorLoop();"));
  writer.Line();
  writer.Line(QStringLiteral("transport::DdsTransport& transport_;"));
  writer.Line(QStringLiteral("Options options_;"));
  writer.Line(QStringLiteral("std::shared_ptr<Coro::FiberTask<void>> link_monitor_;"));
  writer.Line(QStringLiteral("boost::fibers::fiber::id link_monitor_fiber_{};"));
  writer.Line(QStringLiteral("bool running_ = false;"));
  writer.Dedent();
  writer.Line(QStringLiteral("};"));
}

void EmitConstructor(CodeWriter& writer, const FileModel& file, const ClassModel& cls) {
  const QString name = cls.idl_name + QStringLiteral("Client");
  writer.Line(name + QLatin1String("::") + name +
              QStringLiteral("(transport::DdsNode& node, "
                             "transport::DdsTransport& transport,"));
  writer.Line(QStringLiteral("    Options options, QObject* parent)"));
  writer.Line(QStringLiteral("    : QObject(parent),"));
  writer.Line(QStringLiteral("      ddsrpc::ClientBase(node,"));
  writer.Line(QStringLiteral("                         ddsrpc::ClientConfig{options.service_name,"));
  writer.Line(QStringLiteral(
      "                                              \"ddsrpc.\" + options.service_name + "
      "\".events\","));
  writer.Line(QStringLiteral("                                              ") + HashLiteral(file) +
              QStringLiteral(","));
  writer.Line(QStringLiteral("                                              options.request_retry,"));
  writer.Line(QStringLiteral(
      "                                              options.handshake_interval}),"));
  writer.Line(QStringLiteral("      transport_(transport),"));
  writer.Line(QStringLiteral("      options_(std::move(options)) {"));
  writer.Indent();
  writer.Line(QStringLiteral("registerTypes();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(name + QStringLiteral("::Options::Options() = default;"));
  writer.Line();
  writer.Line(QStringLiteral("Coro::Result<void> ") + name +
              QStringLiteral("::prepare() {"));
  writer.Indent();
  writer.Line(QStringLiteral("return Prepare();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitDestructor(CodeWriter& writer, const ClassModel& cls) {
  writer.Line(cls.idl_name + QStringLiteral("Client::~") + cls.idl_name +
              QStringLiteral("Client() = default;"));
}

void EmitStartStop(CodeWriter& writer, const ClassModel& cls) {
  const QString name = cls.idl_name + QStringLiteral("Client");
  writer.Line(QStringLiteral("Coro::Result<void> ") + name + QStringLiteral("::start() {"));
  writer.Indent();
  writer.Line(QStringLiteral("if (running_) {"));
  writer.Indent();
  writer.Line(QStringLiteral("return ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidState);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("auto started = StartEvents();"));
  writer.Line(QStringLiteral("if (!started) return started;"));
  writer.Line(QStringLiteral("running_ = true;"));
  writer.Line(QStringLiteral("link_monitor_ = std::make_shared<Coro::FiberTask<void>>("));
  writer.Indent();
  writer.Line(QStringLiteral("Coro::makeTask([this] { LinkMonitorLoop(); }, "
                             "Coro::Priority::Normal,"));
  writer.Line(QStringLiteral("                   "
                             "Coro::Affinity::fixed(std::this_thread::get_id())));"));
  writer.Dedent();
  writer.Line(QStringLiteral("return {};"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(QStringLiteral("Coro::Result<void> ") + name + QStringLiteral("::stop() {"));
  writer.Indent();
  writer.Line(QStringLiteral("if (!running_) return {};"));
  writer.Line(QStringLiteral("if (link_monitor_ &&"));
  writer.Line(QStringLiteral("    link_monitor_fiber_ == boost::this_fiber::get_id()) {"));
  writer.Indent();
  // readyChanged/syncFailed are emitted from the link monitor fiber; joining
  // that fiber from its own callback would deadlock.
  writer.Line(QStringLiteral("return ddsrpc::make_error_code(ddsrpc::RpcErrc::kInvalidState);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("if (link_monitor_) {"));
  writer.Indent();
  writer.Line(QStringLiteral("link_monitor_->cancel();"));
  writer.Line(QStringLiteral("(void)link_monitor_->get();"));
  writer.Line(QStringLiteral("link_monitor_.reset();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("auto stopped = StopEndpoint();"));
  writer.Line(QStringLiteral("if (!stopped) return stopped;"));
  // Keep running_ set on failure: StopEndpoint refuses a self-join from an
  // event callback, and a later stop() outside the callback must retry.
  writer.Line(QStringLiteral("running_ = false;"));
  writer.Line(QStringLiteral("return {};"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitSlotMethod(CodeWriter& writer, const ClassModel& cls, const SlotModel& slot) {
  const QString name = cls.idl_name + QStringLiteral("Client");
  QStringList taken;
  for (const ParamModel& param : slot.params) taken.append(param.name);
  const QString args_name = UniqueLocal(QStringLiteral("args"), &taken);
  const QString out_name = UniqueLocal(QStringLiteral("out"), &taken);
  const QString reply_name = UniqueLocal(QStringLiteral("reply"), &taken);
  const QString in_name = UniqueLocal(QStringLiteral("in"), &taken);
  const QString value_name = UniqueLocal(QStringLiteral("value"), &taken);

  writer.Line(QStringLiteral("Coro::Result<") + ReturnType(slot) + QStringLiteral("> ") +
              name + QLatin1String("::") + slot.name + QLatin1Char('(') +
              Parameters(slot.params) + QStringLiteral(") {"));
  writer.Indent();
  writer.Line(QStringLiteral("QByteArray ") + args_name + QLatin1Char(';'));
  if (!slot.params.isEmpty()) {
    writer.Line(QStringLiteral("QDataStream ") + out_name + QStringLiteral("(&") +
                args_name + QStringLiteral(", QIODevice::WriteOnly);"));
    writer.Line(QStringLiteral("ddsrpc::wire::Begin(") + out_name + QStringLiteral(");"));
    for (const ParamModel& param : slot.params) {
      writer.Line(out_name + QStringLiteral(" << ") + param.name + QLatin1Char(';'));
    }
    writer.Line(QStringLiteral("if (") + out_name +
                QStringLiteral(".status() != QDataStream::Ok) {"));
    writer.Indent();
    writer.Line(QStringLiteral(
        "return ddsrpc::make_error_code(ddsrpc::RpcErrc::kEncode);"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
  }
  writer.Line(QStringLiteral("auto ") + reply_name + QStringLiteral(" = Call(") +
              QString::number(slot.op) + QStringLiteral(", ") + args_name +
              QStringLiteral(");"));
  writer.Line(QStringLiteral("if (!") + reply_name + QStringLiteral(") return ") +
              reply_name + QStringLiteral(".error();"));

  if (ReturnType(slot) != QStringLiteral("void")) {
    writer.Line(QStringLiteral("QDataStream ") + in_name + QStringLiteral("(") +
                reply_name + QStringLiteral(".value());"));
    writer.Line(QStringLiteral("ddsrpc::wire::Begin(") + in_name + QStringLiteral(");"));
    writer.Line(ReturnType(slot) + QLatin1Char(' ') + value_name + QStringLiteral("{};"));
    writer.Line(in_name + QStringLiteral(" >> ") + value_name + QLatin1Char(';'));
    writer.Line(QStringLiteral("if (") + in_name +
                QStringLiteral(".status() != QDataStream::Ok) {"));
    writer.Indent();
    writer.Line(QStringLiteral(
        "return ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode);"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line(QStringLiteral("return ") + value_name + QLatin1Char(';'));
  } else {
    writer.Line(QStringLiteral("return {};"));
  }

  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitPropertyAndEventHooks(CodeWriter& writer, const ClassModel& cls) {
  const QString name = cls.idl_name + QStringLiteral("Client");
  writer.Line(QStringLiteral("void ") + name +
              QStringLiteral("::ResetPropertiesForResync() {}"));
  writer.Line();
  writer.Line(QStringLiteral("Coro::Result<void> ") + name +
              QStringLiteral("::ApplyPropertySnapshot(QDataStream& in) {"));
  writer.Indent();
  writer.Line(QStringLiteral("quint32 count = 0;"));
  writer.Line(QStringLiteral("in >> count;"));
  writer.Line(QStringLiteral("if (in.status() != QDataStream::Ok) {"));
  writer.Indent();
  writer.Line(QStringLiteral(
      "return ddsrpc::make_error_code(ddsrpc::RpcErrc::kDecode);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("// Task 14 replaces this with the generated property table."));
  writer.Line(QStringLiteral("if (count != 0) {"));
  writer.Indent();
  writer.Line(QStringLiteral(
      "return ddsrpc::make_error_code(ddsrpc::RpcErrc::kProtocol);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("return {};"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(QStringLiteral("void ") + name +
              QStringLiteral("::DispatchSignal(quint32, QDataStream&) {}"));
  writer.Line();
  writer.Line(QStringLiteral("void ") + name +
              QStringLiteral("::DispatchProperty(quint32, quint64, QDataStream&) {}"));
  writer.Line();
  writer.Line(QStringLiteral("void ") + name +
              QStringLiteral("::EmitReadyChanged(bool ready) {"));
  writer.Indent();
  writer.Line(QStringLiteral("Q_EMIT readyChanged(ready);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(QStringLiteral("void ") + name +
              QStringLiteral("::EmitSyncFailed(const QString& message) {"));
  writer.Indent();
  writer.Line(QStringLiteral("Q_EMIT syncFailed(message);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitLinkMonitor(CodeWriter& writer, const ClassModel& cls) {
  const QString name = cls.idl_name + QStringLiteral("Client");
  writer.Line(QStringLiteral("void ") + name + QStringLiteral("::LinkMonitorLoop() {"));
  writer.Indent();
  writer.Line(QStringLiteral("link_monitor_fiber_ = boost::this_fiber::get_id();"));
  writer.Line(QStringLiteral("for (;;) {"));
  writer.Indent();
  writer.Line(QStringLiteral("const auto state = transport_.CurrentLinkState();"));
  writer.Line(QStringLiteral("(void)HandleLinkState(state);"));
  writer.Line(QStringLiteral("const auto interval = (!ready() && "
                             "state == transport::LinkState::kUp)"));
  writer.Indent();
  writer.Line(QStringLiteral("? options_.handshake_interval"));
  writer.Line(QStringLiteral(": options_.link_poll_interval;"));
  writer.Dedent();
  writer.Line(QStringLiteral("const auto delayed = Coro::delay_for(interval);"));
  writer.Line(QStringLiteral("if (!delayed) break;"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("link_monitor_fiber_ = boost::fibers::fiber::id();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

}  // namespace

QString EmitClientHeader(const FileModel& file, const QString& name_space) {
  const QString stem = StemOf(file);
  const QString guard = MakeIncludeGuard(QStringLiteral("CLIENT"), stem);

  CodeWriter writer;
  writer.Line(QStringLiteral("#ifndef ") + guard);
  writer.Line(QStringLiteral("#define ") + guard);
  writer.Line();
  writer.Line(QStringLiteral("#include \"") + stem + QStringLiteral(".ddsrpc.types.h\""));

  if (file.classes.isEmpty()) {
    writer.Line();
    writer.Line(QStringLiteral("#endif  // ") + guard);
    return writer.Text();
  }

  writer.Line();
  writer.Line(QStringLiteral("#include <QDataStream>"));
  writer.Line(QStringLiteral("#include <QObject>"));
  writer.Line(QStringLiteral("#include <QString>"));
  writer.Line(QStringLiteral("#include <chrono>"));
  writer.Line(QStringLiteral("#include <memory>"));
  writer.Line(QStringLiteral("#include <string>"));
  writer.Line();
  writer.Line(QStringLiteral("#include <boost/fiber/fiber.hpp>"));
  writer.Line(QStringLiteral("#include <ddsrpc/client_base.h>"));
  writer.Line(QStringLiteral("#include <transport/io/dds/DdsTransport.hpp>"));
  writer.Line();

  writer.OpenNamespace(name_space);
  if (!NamespaceParts(name_space).isEmpty()) writer.Line();
  bool first = true;
  for (const ClassModel& cls : file.classes) {
    if (!first) writer.Line();
    EmitClassHeader(writer, cls);
    first = false;
  }
  writer.Line();
  writer.CloseNamespace();
  writer.Line();
  writer.Line(QStringLiteral("#endif  // ") + guard);
  return writer.Text();
}

QString EmitClientSource(const FileModel& file, const QString& name_space) {
  const QString stem = StemOf(file);
  CodeWriter writer;
  writer.Line(QStringLiteral("#include \"") + stem + QStringLiteral(".ddsrpc.client.h\""));
  if (file.classes.isEmpty()) return writer.Text();

  writer.Line();
  writer.Line(QStringLiteral("#include <QIODevice>"));
  writer.Line(QStringLiteral("#include <thread>"));
  writer.Line(QStringLiteral("#include <utility>"));
  writer.Line();
  writer.Line(QStringLiteral("#include <await/delay.hpp>"));
  writer.Line(QStringLiteral("#include <boost/fiber/operations.hpp>"));
  writer.Line();
  writer.OpenNamespace(name_space);
  if (!NamespaceParts(name_space).isEmpty()) writer.Line();

  bool first = true;
  for (const ClassModel& cls : file.classes) {
    if (!first) writer.Line();
    EmitConstructor(writer, file, cls);
    writer.Line();
    EmitDestructor(writer, cls);
    writer.Line();
    EmitStartStop(writer, cls);
    for (const SlotModel& slot : cls.slots) {
      writer.Line();
      EmitSlotMethod(writer, cls, slot);
    }
    writer.Line();
    EmitPropertyAndEventHooks(writer, cls);
    writer.Line();
    EmitLinkMonitor(writer, cls);
    first = false;
  }
  writer.Line();
  writer.CloseNamespace();
  return writer.Text();
}

}  // namespace ddsrpcgen
