#include "emit_service.h"

#include <QSet>
#include <QStringList>

#include "code_writer.h"
#include "emit_common.h"

namespace ddsrpcgen {
namespace {

[[nodiscard]] QString ReturnType(const SlotModel& slot) {
  return slot.return_cpp_type.isEmpty()
             ? QStringLiteral("void")
             : QualifiedCppType(slot.return_cpp_type);
}

[[nodiscard]] QString Parameters(const QVector<ParamModel>& params) {
  QStringList parts;
  parts.reserve(params.size());
  for (const ParamModel& param : params) {
    parts.append(QualifiedCppType(param.cpp_type) + QLatin1Char(' ') + param.name);
  }
  return parts.join(QStringLiteral(", "));
}

[[nodiscard]] QString SlotSignature(const SlotModel& slot) {
  return QStringLiteral("::Coro::Result<") + ReturnType(slot) + QStringLiteral("> ") +
         slot.name + QLatin1Char('(') + Parameters(slot.params) + QLatin1Char(')');
}

[[nodiscard]] QString Capitalize(const QString& name) {
  if (name.isEmpty()) return name;
  return name.left(1).toUpper() + name.mid(1);
}

[[nodiscard]] QString PublishName(const SignalModel& signal) {
  return QStringLiteral("publish") + Capitalize(signal.name);
}

[[nodiscard]] QString UniqueLocal(const QString& base, QSet<QString>* taken) {
  QString name = base;
  while (taken->contains(name)) name += QLatin1Char('_');
  taken->insert(name);
  return name;
}

[[nodiscard]] QSet<QString> ParamNames(const SlotModel& slot) {
  QSet<QString> names;
  for (const ParamModel& param : slot.params) names.insert(param.name);
  return names;
}

[[nodiscard]] QSet<QString> AllParamNames(const ClassModel& cls) {
  QSet<QString> names;
  for (const SlotModel& slot : cls.slots) {
    for (const ParamModel& param : slot.params) names.insert(param.name);
  }
  return names;
}

[[nodiscard]] QString HashLiteral(const FileModel& file) {
  return SchemaHashLiteral(file.schema_hash);
}

void EmitFailureReturn(CodeWriter& writer, const QString& error,
                       const QString& message) {
  writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Failure("));
  writer.Indent();
  writer.Line(error + QLatin1Char(','));
  writer.Line(QStringLiteral("::QString::fromLatin1(\"") + message +
              QStringLiteral("\"));"));
  writer.Dedent();
}

void EmitClassEnums(CodeWriter& writer, const ClassModel& cls) {
  for (const EnumModel& model : cls.class_enums) {
    writer.Line(QStringLiteral("using ") + model.idl_name + QStringLiteral(" = ") +
                model.cpp_name + QLatin1Char(';'));
  }
  if (!cls.class_enums.isEmpty()) writer.Line();
}

void EmitClassHeader(CodeWriter& writer, const ClassModel& cls,
                     const QString& name_space) {
  writer.Line(QStringLiteral("class ") + cls.idl_name +
              QStringLiteral("Service : public ::ddsrpc::ServiceBase {"));
  writer.Line(QStringLiteral("public:"));
  writer.Indent();
  writer.Line(QStringLiteral("struct Options {"));
  writer.Indent();
  writer.Line(QStringLiteral("::std::string service_name = \"") + cls.idl_name +
              QStringLiteral("\";"));
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
              QStringLiteral("Service(::transport::DdsNode& node, Options options = {});"));
  writer.Line();
  // ServiceBase exposes Prepare/Start/Stop (capitalized). The generated facade
  // provides the lowercase lifecycle names used by the public design and
  // calls the base explicitly so IDL members cannot hide them.
  writer.Line(QStringLiteral("::Coro::Result<void> prepare();"));
  writer.Line(QStringLiteral("::Coro::Result<void> start();"));
  writer.Line(QStringLiteral("::Coro::Result<void> stop();"));
  writer.Line();
  writer.Line(cls.idl_name + QStringLiteral("Service(const ") + cls.idl_name +
              QStringLiteral("Service&) = delete;"));
  writer.Line(cls.idl_name + QStringLiteral("Service& operator=(const ") + cls.idl_name +
              QStringLiteral("Service&) = delete;"));
  writer.Line();
  EmitClassEnums(writer, cls);
  for (const SlotModel& slot : cls.slots) {
    writer.Line(QStringLiteral("virtual ") + SlotSignature(slot) + QStringLiteral(" = 0;"));
  }
  if (!cls.slots.isEmpty()) writer.Line();
  for (const SignalModel& signal : cls.signals) {
    writer.Line(QStringLiteral("::Coro::Result<void> ") + PublishName(signal) +
                QLatin1Char('(') + Parameters(signal.params) + QStringLiteral(");"));
  }
  if (!cls.signals.isEmpty()) writer.Line();
  for (const PropertyModel& property : cls.properties) {
    const QString type = QualifiedCppType(property.cpp_type);
    writer.Line(type + QLatin1Char(' ') + property.name +
                QStringLiteral("() const;"));
    if (property.modifier != PropModifier::kConstant) {
      writer.Line(QStringLiteral("::Coro::Result<void> set") + Capitalize(property.name) +
                  QLatin1Char('(') + type + QStringLiteral(" value);"));
    }
  }
  if (!cls.properties.isEmpty()) writer.Line();
  writer.Dedent();
  writer.Line(QStringLiteral("protected:"));
  writer.Indent();
  writer.Line(QStringLiteral(
      "::ddsrpc::OperationResult HandleOperation(::quint32 op, ::QDataStream& args) override;"));
  writer.Line(QStringLiteral(
      "::ddsrpc::OperationResult BuildPropertySnapshot() override;"));
  writer.Dedent();
  if (!cls.properties.isEmpty()) {
    writer.Line(QStringLiteral("private:"));
    writer.Indent();
    for (const PropertyModel& property : cls.properties) {
      const QString type = QualifiedCppType(property.cpp_type);
      const QString initial =
          property.has_default ? property.default_value : QStringLiteral("{}");
      writer.Line(type + QLatin1Char(' ') + property.name + QLatin1Char('_') +
                  QStringLiteral(" = ") + initial + QLatin1Char(';'));
      writer.Line(QStringLiteral("::quint64 ") + property.name +
                  QStringLiteral("_revision_ = 0;"));
      if (property.modifier != PropModifier::kConstant) {
        writer.Line(QStringLiteral("::Coro::Result<::quint64> ") +
                    QStringLiteral("ddsrpc_set_") + property.name + QLatin1Char('(') +
                    type + QStringLiteral(" value);"));
      }
    }
    writer.Dedent();
  }
  writer.Line(QStringLiteral("};"));
}

void EmitDecode(CodeWriter& writer, const QString& stream, const SlotModel& slot,
                const QString& context) {
  for (const ParamModel& param : slot.params) {
    writer.Line(QualifiedCppType(param.cpp_type) + QLatin1Char(' ') + param.name +
                QStringLiteral("{};"));
  }
  for (const ParamModel& param : slot.params) {
    writer.Line(stream + QStringLiteral(" >> ") + param.name + QLatin1Char(';'));
  }
  if (slot.params.isEmpty()) return;
  writer.Line(QStringLiteral("if (") + stream +
              QStringLiteral(".status() != ::QDataStream::Ok) {"));
  writer.Indent();
  EmitFailureReturn(writer,
                    QStringLiteral("::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode)"),
                    context + QStringLiteral(": failed to decode arguments"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitSlotCase(CodeWriter& writer, const ClassModel& cls, const SlotModel& slot,
                  const QString& stream) {
  const QString context = cls.idl_name + QLatin1String("::") + slot.name;
  writer.Line(QStringLiteral("case ") + QString::number(slot.op) + QStringLiteral(": {"));
  writer.Indent();

  QSet<QString> taken = ParamNames(slot);
  const QString result_name = UniqueLocal(QStringLiteral("result"), &taken);
  const QString payload_name = UniqueLocal(QStringLiteral("payload"), &taken);
  const QString out_name = UniqueLocal(QStringLiteral("out"), &taken);

  EmitDecode(writer, stream, slot, context);

  QStringList arguments;
  arguments.reserve(slot.params.size());
  for (const ParamModel& param : slot.params) arguments.append(param.name);
  // Qualify the virtual call: a parameter may share the slot's name, which
  // would otherwise shadow the member function in the decoded-argument scope.
  writer.Line(QStringLiteral("const auto ") + result_name + QStringLiteral(" = this->") +
              slot.name + QLatin1Char('(') + arguments.join(QStringLiteral(", ")) +
              QStringLiteral(");"));
  writer.Line(QStringLiteral("if (!") + result_name + QStringLiteral(") {"));
  writer.Indent();
  writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Failure("));
  writer.Indent();
  writer.Line(result_name + QStringLiteral(".error(),"));
  writer.Line(QStringLiteral("::QString::fromStdString(") + result_name +
              QStringLiteral(".error().message()));"));
  writer.Dedent();
  writer.Dedent();
  writer.Line(QStringLiteral("}"));

  if (ReturnType(slot) == QStringLiteral("void")) {
    writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Success();"));
  } else {
    writer.Line(QStringLiteral("::QByteArray ") + payload_name + QLatin1Char(';'));
    writer.Line(QStringLiteral("::QDataStream ") + out_name + QStringLiteral("(&") +
                payload_name + QStringLiteral(", ::QIODevice::WriteOnly);"));
    writer.Line(QStringLiteral("::ddsrpc::wire::Begin(") + out_name + QStringLiteral(");"));
    writer.Line(out_name + QStringLiteral(" << ") + result_name + QStringLiteral(".value();"));
    writer.Line(QStringLiteral("if (") + out_name +
                QStringLiteral(".status() != ::QDataStream::Ok) {"));
    writer.Indent();
    EmitFailureReturn(
        writer, QStringLiteral("::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode)"),
        context + QStringLiteral(": failed to encode the reply"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Success(") + payload_name +
                QStringLiteral(");"));
  }

  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitConstructor(CodeWriter& writer, const FileModel& file, const ClassModel& cls) {
  const QString name = cls.idl_name + QStringLiteral("Service");
  writer.Line(name + QLatin1String("::") + name +
              QStringLiteral("(::transport::DdsNode& node,"));
  writer.Line(QStringLiteral("    Options options)"));
  writer.Line(QStringLiteral("    : ::ddsrpc::ServiceBase(node,"));
  writer.Line(QStringLiteral(
      "                            ::ddsrpc::ServiceConfig{options.service_name,"));
  writer.Line(QStringLiteral(
      "                                                  \"ddsrpc.\" + "
      "options.service_name + \".events\","));
  writer.Line(QStringLiteral("                                                  ") +
              HashLiteral(file) + QStringLiteral("}) {"));
  writer.Indent();
  writer.Line(QStringLiteral("registerTypes();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(name + QStringLiteral("::Options::Options() = default;"));
  writer.Line();
  writer.Line(QStringLiteral("::Coro::Result<void> ") + name +
              QStringLiteral("::prepare() {"));
  writer.Indent();
  writer.Line(QStringLiteral("return this->Prepare();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(QStringLiteral("::Coro::Result<void> ") + name + QStringLiteral("::start() {"));
  writer.Indent();
  writer.Line(QStringLiteral("return this->Start();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(QStringLiteral("::Coro::Result<void> ") + name + QStringLiteral("::stop() {"));
  writer.Indent();
  writer.Line(QStringLiteral("return this->Stop();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitPropertySetCase(CodeWriter& writer, const ClassModel& cls,
                         const PropertyModel& property, const QString& stream);

void EmitHandleOperation(CodeWriter& writer, const ClassModel& cls) {
  QSet<QString> used = AllParamNames(cls);
  QString stream = QStringLiteral("args");
  while (used.contains(stream)) stream += QLatin1Char('_');

  writer.Line(QStringLiteral("::ddsrpc::OperationResult ") + cls.idl_name +
              QStringLiteral("Service::HandleOperation(::quint32 op, ::QDataStream& ") +
              stream + QStringLiteral(") {"));
  writer.Indent();
  if (used.isEmpty()) writer.Line(QStringLiteral("(void)") + stream + QLatin1Char(';'));
  writer.Line(QStringLiteral("switch (op) {"));
  writer.Indent();
  for (const SlotModel& slot : cls.slots) EmitSlotCase(writer, cls, slot, stream);
  for (const PropertyModel& property : cls.properties) {
    if (property.remote_setter_op > 0) {
      EmitPropertySetCase(writer, cls, property, stream);
    }
  }
  writer.Line(QStringLiteral("default:"));
  writer.Indent();
  writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Failure("));
  writer.Indent();
  writer.Line(
      QStringLiteral("::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kInvalidArgument),"));
  writer.Line(
      QStringLiteral("::QString::fromLatin1(\"unknown operation %1\").arg(op));"));
  writer.Dedent();
  writer.Dedent();
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitSignalPublish(CodeWriter& writer, const ClassModel& cls,
                       const SignalModel& signal) {
  QSet<QString> taken;
  for (const ParamModel& param : signal.params) taken.insert(param.name);
  const QString payload_name = UniqueLocal(QStringLiteral("payload"), &taken);
  const QString out_name = UniqueLocal(QStringLiteral("out"), &taken);

  writer.Line(QStringLiteral("::Coro::Result<void> ") + cls.idl_name +
              QStringLiteral("Service::") + PublishName(signal) + QLatin1Char('(') +
              Parameters(signal.params) + QStringLiteral(") {"));
  writer.Indent();
  writer.Line(QStringLiteral("::QByteArray ") + payload_name + QLatin1Char(';'));
  if (!signal.params.isEmpty()) {
    writer.Line(QStringLiteral("::QDataStream ") + out_name + QStringLiteral("(&") +
                payload_name + QStringLiteral(", ::QIODevice::WriteOnly);"));
    writer.Line(QStringLiteral("::ddsrpc::wire::Begin(") + out_name + QStringLiteral(");"));
    for (const ParamModel& param : signal.params) {
      writer.Line(out_name + QStringLiteral(" << ") + param.name + QLatin1Char(';'));
    }
    writer.Line(QStringLiteral("if (") + out_name +
                QStringLiteral(".status() != ::QDataStream::Ok) {"));
    writer.Indent();
    writer.Line(
        QStringLiteral("return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
  }
  writer.Line(QStringLiteral("return this->PublishEvent(::ddsrpc::wire::EventKind::kSignal, ") +
              QString::number(signal.event_id) +
              QStringLiteral(", this->NextRevision(), ") + payload_name + QStringLiteral(");"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitPropertySetCase(CodeWriter& writer, const ClassModel& cls,
                         const PropertyModel& property, const QString& stream) {
  const QString type = QualifiedCppType(property.cpp_type);
  const QString context = cls.idl_name + QStringLiteral("::") + property.name;
  writer.Line(QStringLiteral("case ") + QString::number(property.remote_setter_op) +
              QStringLiteral(": {"));
  writer.Indent();
  writer.Line(type + QStringLiteral(" ddsrpc_value{};"));
  writer.Line(stream + QStringLiteral(" >> ddsrpc_value;"));
  writer.Line(QStringLiteral("if (") + stream +
              QStringLiteral(".status() != ::QDataStream::Ok) {"));
  writer.Indent();
  EmitFailureReturn(
      writer, QStringLiteral("::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kDecode)"),
      context + QStringLiteral(": failed to decode the property value"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("const auto ddsrpc_revision = this->ddsrpc_set_") +
              property.name + QStringLiteral("(ddsrpc_value);"));
  writer.Line(QStringLiteral("if (!ddsrpc_revision) {"));
  writer.Indent();
  writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Failure("));
  writer.Indent();
  writer.Line(QStringLiteral("ddsrpc_revision.error(),"));
  writer.Line(QStringLiteral("::QString::fromStdString(ddsrpc_revision.error().message()));"));
  writer.Dedent();
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("::QByteArray ddsrpc_reply_payload;"));
  writer.Line(QStringLiteral("::QDataStream ddsrpc_reply_out(&ddsrpc_reply_payload, "
                             "::QIODevice::WriteOnly);"));
  writer.Line(QStringLiteral("::ddsrpc::wire::Begin(ddsrpc_reply_out);"));
  writer.Line(QStringLiteral("ddsrpc_reply_out << ddsrpc_revision.value() << ddsrpc_value;"));
  writer.Line(QStringLiteral("if (ddsrpc_reply_out.status() != ::QDataStream::Ok) {"));
  writer.Indent();
  EmitFailureReturn(
      writer, QStringLiteral("::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode)"),
      context + QStringLiteral(": failed to encode the property reply"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Success(ddsrpc_reply_payload);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitPropertyAccessors(CodeWriter& writer, const ClassModel& cls) {
  for (const PropertyModel& property : cls.properties) {
    const QString type = QualifiedCppType(property.cpp_type);
    writer.Line(type + QLatin1Char(' ') + cls.idl_name + QStringLiteral("Service::") +
                property.name + QStringLiteral("() const { return ") + property.name +
                QStringLiteral("_; }"));
    writer.Line();
    if (property.modifier == PropModifier::kConstant) continue;
    writer.Line(QStringLiteral("::Coro::Result<void> ") + cls.idl_name +
                QStringLiteral("Service::set") + Capitalize(property.name) +
                QLatin1Char('(') + type + QStringLiteral(" value) {"));
    writer.Indent();
    writer.Line(QStringLiteral("const auto revision = this->ddsrpc_set_") +
                property.name + QStringLiteral("(value);"));
    writer.Line(QStringLiteral("if (!revision) return revision.error();"));
    writer.Line(QStringLiteral("return {};"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line();
    writer.Line(QStringLiteral("::Coro::Result<::quint64> ") + cls.idl_name +
                QStringLiteral("Service::ddsrpc_set_") + property.name +
                QLatin1Char('(') + type + QStringLiteral(" value) {"));
    writer.Indent();
    writer.Line(property.name + QStringLiteral("_ = value;"));
    writer.Line(QStringLiteral("const auto revision = this->NextRevision();"));
    writer.Line(property.name + QStringLiteral("_revision_ = revision;"));
    writer.Line(QStringLiteral("::QByteArray ddsrpc_payload;"));
    writer.Line(QStringLiteral("::QDataStream ddsrpc_out(&ddsrpc_payload, "
                               "::QIODevice::WriteOnly);"));
    writer.Line(QStringLiteral("::ddsrpc::wire::Begin(ddsrpc_out);"));
    writer.Line(QStringLiteral("ddsrpc_out << value;"));
    writer.Line(QStringLiteral("if (ddsrpc_out.status() != ::QDataStream::Ok) {"));
    writer.Indent();
    writer.Line(QStringLiteral("return ::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode);"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line(QStringLiteral("const auto published = this->PublishEvent("
                               "::ddsrpc::wire::EventKind::kProperty, ") +
                QString::number(property.property_id) + QStringLiteral(", revision, "
                "ddsrpc_payload);"));
    writer.Line(QStringLiteral("if (!published) return published.error();"));
    writer.Line(QStringLiteral("return revision;"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line();
  }
}

void EmitBuildPropertySnapshot(CodeWriter& writer, const ClassModel& cls) {
  writer.Line(QStringLiteral("::ddsrpc::OperationResult ") + cls.idl_name +
              QStringLiteral("Service::BuildPropertySnapshot() {"));
  writer.Indent();
  writer.Line(QStringLiteral("::QByteArray payload;"));
  writer.Line(QStringLiteral("::QDataStream out(&payload, ::QIODevice::WriteOnly);"));
  writer.Line(QStringLiteral("::ddsrpc::wire::Begin(out);"));
  writer.Line(QStringLiteral("out << ::quint32(") +
              QString::number(cls.properties.size()) + QStringLiteral(");"));
  for (const PropertyModel& property : cls.properties) {
    writer.Line(QStringLiteral("out << ::quint32(") +
                QString::number(property.property_id) + QStringLiteral(") << ") +
                property.name + QStringLiteral("_revision_ << ") + property.name +
                QStringLiteral("_;"));
  }
  writer.Line(QStringLiteral("if (out.status() != ::QDataStream::Ok) {"));
  writer.Indent();
  EmitFailureReturn(
      writer, QStringLiteral("::ddsrpc::make_error_code(::ddsrpc::RpcErrc::kEncode)"),
      QStringLiteral("failed to encode the property snapshot"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line(QStringLiteral("return ::ddsrpc::OperationResult::Success(payload);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

}  // namespace

QString EmitServiceHeader(const FileModel& file, const QString& name_space) {
  const QString stem = StemOf(file);
  const QString guard = MakeIncludeGuard(QStringLiteral("SERVICE"), stem);

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
  writer.Line(QStringLiteral("#include <string>"));
  writer.Line();
  writer.Line(QStringLiteral("#include <ddsrpc/service_base.h>"));
  writer.Line();

  writer.OpenNamespace(name_space);
  if (!NamespaceParts(name_space).isEmpty()) writer.Line();
  bool first = true;
  for (const ClassModel& cls : file.classes) {
    if (!first) writer.Line();
    EmitClassHeader(writer, cls, name_space);
    first = false;
  }
  writer.Line();
  writer.CloseNamespace();
  writer.Line();
  writer.Line(QStringLiteral("#endif  // ") + guard);
  return writer.Text();
}

QString EmitServiceSource(const FileModel& file, const QString& name_space) {
  const QString stem = StemOf(file);
  CodeWriter writer;
  writer.Line(QStringLiteral("#include \"") + stem + QStringLiteral(".ddsrpc.service.h\""));
  if (file.classes.isEmpty()) return writer.Text();

  writer.Line();
  writer.Line(QStringLiteral("#include <QIODevice>"));
  writer.Line(QStringLiteral("#include <QString>"));
  writer.Line();
  writer.OpenNamespace(name_space);
  if (!NamespaceParts(name_space).isEmpty()) writer.Line();

  bool first = true;
  for (const ClassModel& cls : file.classes) {
    if (!first) writer.Line();
    EmitConstructor(writer, file, cls);
    writer.Line();
    EmitPropertyAccessors(writer, cls);
    EmitHandleOperation(writer, cls);
    for (const SignalModel& signal : cls.signals) {
      writer.Line();
      EmitSignalPublish(writer, cls, signal);
    }
    writer.Line();
    EmitBuildPropertySnapshot(writer, cls);
    first = false;
  }
  writer.Line();
  writer.CloseNamespace();
  return writer.Text();
}

}  // namespace ddsrpcgen
