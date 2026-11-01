#include "emit_types.h"

#include <QFileInfo>
#include <QStringList>

#include "code_writer.h"
#include "emit_common.h"

namespace ddsrpcgen {
namespace {

[[nodiscard]] QString SanitizeIdentifier(const QString& text, bool to_upper) {
  QString sanitized;
  sanitized.reserve(text.size());
  for (const QChar c : text) {
    if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
        (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
        (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
        c == QLatin1Char('_')) {
      sanitized.append(to_upper ? c.toUpper() : c);
    } else {
      sanitized.append(QLatin1Char('_'));
    }
  }
  return sanitized;
}

[[nodiscard]] QString StemTag(const QString& stem) {
  const QString escaped = EscapeStem(stem);
  return escaped.isEmpty() ? QStringLiteral("TYPES") : escaped;
}

[[nodiscard]] QString IncludeGuard(const FileModel& file) {
  QString stem = file.stem;
  if (stem.isEmpty()) stem = QFileInfo(file.path).completeBaseName();
  return QStringLiteral("DDSRPC_GEN_TYPES_") + StemTag(stem) + QStringLiteral("_H");
}

[[nodiscard]] QString Qualified(const QString& name, const QStringList& parts) {
  if (parts.isEmpty()) return name;
  return parts.join(QStringLiteral("::")) + QStringLiteral("::") + name;
}

// Case-preserving namespace fragment used in generated helper names. The
// length prefixes keep distinct namespaces from producing the same tag.
[[nodiscard]] QString NamespaceTag(const QStringList& parts) {
  if (parts.isEmpty()) return QStringLiteral("GLOBAL");
  QString tag;
  for (const QString& part : parts) {
    const QString sanitized = SanitizeIdentifier(part, false);
    tag += QString::number(sanitized.size()) + sanitized;
  }
  return tag;
}

[[nodiscard]] QVector<EnumModel> CollectEnums(const FileModel& file) {
  QVector<EnumModel> enums;
  enums.reserve(file.enums.size());
  for (const EnumModel& model : file.enums) enums.append(model);
  for (const ClassModel& cls : file.classes) {
    for (const EnumModel& model : cls.class_enums) enums.append(model);
  }
  return enums;
}

void EmitEnumDefinition(CodeWriter& writer, const EnumModel& model) {
  writer.Line(QStringLiteral("enum class ") + model.cpp_name + QStringLiteral(" : qint32 {"));
  writer.Indent();
  for (const EnumValue& value : model.values) {
    writer.Line(value.name + QStringLiteral(" = ") + QString::number(value.value) +
                QStringLiteral(","));
  }
  writer.Dedent();
  writer.Line(QStringLiteral("};"));
}

void EmitEnumStreamOperators(CodeWriter& writer, const EnumModel& model) {
  writer.Line(QStringLiteral("inline QDataStream& operator<<(QDataStream& stream, ") +
              model.cpp_name + QStringLiteral(" value) {"));
  writer.Indent();
  writer.Line(QStringLiteral("return stream << static_cast<qint32>(value);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));

  writer.Line();
  writer.Line(QStringLiteral("inline QDataStream& operator>>(QDataStream& stream, ") +
              model.cpp_name + QStringLiteral("& value) {"));
  writer.Indent();
  writer.Line(QStringLiteral("qint32 raw = 0;"));
  writer.Line(QStringLiteral("stream >> raw;"));
  writer.Line(QStringLiteral("value = static_cast<") + model.cpp_name + QStringLiteral(">(raw);"));
  writer.Line(QStringLiteral("return stream;"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitPodDefinition(CodeWriter& writer, const PodModel& pod) {
  writer.Line(QStringLiteral("struct ") + pod.name + QStringLiteral(" {"));
  writer.Indent();
  for (const PodFieldModel& field : pod.fields) {
    writer.Line(field.cpp_type + QLatin1Char(' ') + field.name + QStringLiteral("{};"));
  }
  if (!pod.fields.isEmpty()) writer.Line();
  writer.Line(pod.name + QStringLiteral("() = default;"));
  if (!pod.fields.isEmpty()) {
    QStringList parameters;
    QStringList initializers;
    for (const PodFieldModel& field : pod.fields) {
      parameters.append(field.cpp_type + QLatin1Char(' ') + field.name);
      initializers.append(field.name + QLatin1Char('(') + field.name + QLatin1Char(')'));
    }
    writer.Line(pod.name + QLatin1Char('(') + parameters.join(QStringLiteral(", ")) +
                QStringLiteral(") : ") + initializers.join(QStringLiteral(", ")) +
                QStringLiteral(" {}"));
  }
  writer.Dedent();
  writer.Line(QStringLiteral("};"));

  writer.Line();
  if (pod.fields.isEmpty()) {
    writer.Line(QStringLiteral("inline bool operator==(const ") + pod.name +
                QStringLiteral("&, const ") + pod.name + QStringLiteral("&) {"));
    writer.Indent();
    writer.Line(QStringLiteral("return true;"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line();
    writer.Line(QStringLiteral("inline bool operator!=(const ") + pod.name +
                QStringLiteral("&, const ") + pod.name + QStringLiteral("&) {"));
    writer.Indent();
    writer.Line(QStringLiteral("return false;"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    return;
  }

  QStringList comparisons;
  comparisons.reserve(pod.fields.size());
  for (const PodFieldModel& field : pod.fields) {
    comparisons.append(QStringLiteral("lhs.") + field.name + QStringLiteral(" == rhs.") +
                       field.name);
  }
  writer.Line(QStringLiteral("inline bool operator==(const ") + pod.name +
              QStringLiteral("& lhs, const ") + pod.name + QStringLiteral("& rhs) {"));
  writer.Indent();
  writer.Line(QStringLiteral("return ") + comparisons.join(QStringLiteral(" && ")) +
              QLatin1Char(';'));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Line();
  writer.Line(QStringLiteral("inline bool operator!=(const ") + pod.name +
              QStringLiteral("& lhs, const ") + pod.name + QStringLiteral("& rhs) {"));
  writer.Indent();
  writer.Line(QStringLiteral("return !(lhs == rhs);"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitPodStreamOperators(CodeWriter& writer, const PodModel& pod) {
  if (pod.fields.isEmpty()) {
    writer.Line(QStringLiteral("inline QDataStream& operator<<(QDataStream& stream, const ") +
                pod.name + QStringLiteral("&) {"));
    writer.Indent();
    writer.Line(QStringLiteral("return stream;"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    writer.Line();
    writer.Line(QStringLiteral("inline QDataStream& operator>>(QDataStream& stream, ") +
                pod.name + QStringLiteral("&) {"));
    writer.Indent();
    writer.Line(QStringLiteral("return stream;"));
    writer.Dedent();
    writer.Line(QStringLiteral("}"));
    return;
  }

  writer.Line(QStringLiteral("inline QDataStream& operator<<(QDataStream& stream, const ") +
              pod.name + QStringLiteral("& value) {"));
  writer.Indent();
  for (const PodFieldModel& field : pod.fields) {
    writer.Line(QStringLiteral("stream << value.") + field.name + QLatin1Char(';'));
  }
  writer.Line(QStringLiteral("return stream;"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));

  writer.Line();
  writer.Line(QStringLiteral("inline QDataStream& operator>>(QDataStream& stream, ") +
              pod.name + QStringLiteral("& value) {"));
  writer.Indent();
  for (const PodFieldModel& field : pod.fields) {
    writer.Line(QStringLiteral("stream >> value.") + field.name + QLatin1Char(';'));
  }
  writer.Line(QStringLiteral("return stream;"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

// Every types header included in a translation unit contributes one hook that
// registers that file's metatypes. The registry and registerTypes() are guarded
// per namespace so that several generated headers can be included together
// without redefining the function: Qt requires Q_DECLARE_METATYPE to be emitted
// at global scope, and that declaration must be visible before qRegisterMetaType
// is instantiated, so the hook itself is defined after the declarations.
void EmitRegistrationSupport(CodeWriter& writer, const QString& detail_namespace) {
  writer.Line(QStringLiteral("namespace ") + detail_namespace + QStringLiteral(" {"));
  writer.Indent();
  writer.Line(QStringLiteral("using RegistrationHook = void (*)();"));
  writer.Line();
  writer.Line(QStringLiteral("inline QVector<RegistrationHook>& RegistrationHooks() {"));
  writer.Indent();
  writer.Line(QStringLiteral("static QVector<RegistrationHook> hooks;"));
  writer.Line(QStringLiteral("return hooks;"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Dedent();
  writer.Line(QStringLiteral("}  // namespace ") + detail_namespace);
  writer.Line();
  writer.Line(QStringLiteral("inline void registerTypes() {"));
  writer.Indent();
  writer.Line(QStringLiteral("static std::once_flag once;"));
  writer.Line(QStringLiteral("std::call_once(once, [] {"));
  writer.Indent();
  writer.Line(QStringLiteral("for (const ") + detail_namespace +
              QStringLiteral("::RegistrationHook hook :"));
  writer.Line(QStringLiteral("     ") + detail_namespace +
              QStringLiteral("::RegistrationHooks()) {"));
  writer.Indent();
  writer.Line(QStringLiteral("hook();"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Dedent();
  writer.Line(QStringLiteral("});"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
}

void EmitRegistrationHook(CodeWriter& writer, const QString& type_name,
                          const QString& namespace_tag, const QString& detail_namespace,
                          const QStringList& namespaces, const QVector<EnumModel>& enums,
                          const QVector<PodModel>& pods) {
  const QString registrar =
      QStringLiteral("DdsrpcGeneratedTypesRegistrar_") + namespace_tag +
      QLatin1Char('_') + type_name;
  const QString variable =
      QStringLiteral("ddsrpc_generated_types_registrar_") + namespace_tag +
      QLatin1Char('_') + type_name;
  const QString detail = Qualified(detail_namespace, namespaces);

  writer.Line(QStringLiteral("namespace {"));
  writer.Indent();
  writer.Line(QStringLiteral("struct ") + registrar + QStringLiteral(" {"));
  writer.Indent();
  writer.Line(registrar + QStringLiteral("() {"));
  writer.Indent();
  writer.Line(detail + QStringLiteral("::RegistrationHooks().append([] {"));
  writer.Indent();
  for (const EnumModel& model : enums) {
    writer.Line(QStringLiteral("qRegisterMetaType<") +
                Qualified(model.cpp_name, namespaces) + QStringLiteral(">();"));
  }
  for (const PodModel& pod : pods) {
    writer.Line(QStringLiteral("qRegisterMetaType<") + Qualified(pod.name, namespaces) +
                QStringLiteral(">();"));
  }
  writer.Dedent();
  writer.Line(QStringLiteral("});"));
  writer.Dedent();
  writer.Line(QStringLiteral("}"));
  writer.Dedent();
  writer.Line(QStringLiteral("};"));
  writer.Line();
  writer.Line(QStringLiteral("const ") + registrar + QLatin1Char(' ') + variable +
              QLatin1Char(';'));
  writer.Dedent();
  writer.Line(QStringLiteral("}  // namespace"));
}

}  // namespace

QString EmitTypesHeader(const FileModel& file, const QString& name_space) {
  const QString guard = IncludeGuard(file);
  const QStringList namespaces = NamespaceParts(name_space);
  const QVector<EnumModel> enums = CollectEnums(file);

  CodeWriter writer;
  writer.Line(QStringLiteral("#ifndef ") + guard);
  writer.Line(QStringLiteral("#define ") + guard);
  writer.Line();

  for (const Directive& directive : file.directives) writer.Line(directive.text);
  if (!file.directives.isEmpty()) writer.Line();

  writer.Line(QStringLiteral("#include <QtCore/QDataStream>"));
  writer.Line(QStringLiteral("#include <QtCore/QMetaType>"));
  writer.Line(QStringLiteral("#include <QtCore/QString>"));
  writer.Line(QStringLiteral("#include <QtCore/QVector>"));
  writer.Line(QStringLiteral("#include <mutex>"));
  writer.Line();

  QStringList imported_stems;
  for (const QString& import : file.imports) {
    const QString stem = QFileInfo(import).completeBaseName();
    if (!stem.isEmpty() && !imported_stems.contains(stem)) imported_stems.append(stem);
  }
  for (const QString& stem : imported_stems) {
    writer.Line(QStringLiteral("#include \"") + stem + QStringLiteral(".ddsrpc.types.h\""));
  }
  if (!imported_stems.isEmpty()) writer.Line();

  writer.OpenNamespace(name_space);
  if (!namespaces.isEmpty()) writer.Line();

  bool first_declaration = true;
  for (const EnumModel& model : enums) {
    if (!first_declaration) writer.Line();
    EmitEnumDefinition(writer, model);
    first_declaration = false;
  }
  for (const PodModel& pod : file.pods) {
    if (!first_declaration) writer.Line();
    EmitPodDefinition(writer, pod);
    first_declaration = false;
  }

  if (!first_declaration) writer.Line();
  bool first_operator = true;
  for (const EnumModel& model : enums) {
    if (!first_operator) writer.Line();
    EmitEnumStreamOperators(writer, model);
    first_operator = false;
  }
  for (const PodModel& pod : file.pods) {
    if (!first_operator) writer.Line();
    EmitPodStreamOperators(writer, pod);
    first_operator = false;
  }
  if (!first_operator) writer.Line();

  const QString namespace_tag = NamespaceTag(namespaces);
  const QString registration_guard =
      QStringLiteral("DDSRPC_GEN_TYPES_REGISTRATION_") + namespace_tag;
  const QString detail_namespace =
      QStringLiteral("DdsrpcGeneratedTypesDetail_") + namespace_tag;
  writer.Line(QStringLiteral("#ifndef ") + registration_guard);
  writer.Line(QStringLiteral("#define ") + registration_guard);
  EmitRegistrationSupport(writer, detail_namespace);
  writer.Line(QStringLiteral("#endif  // ") + registration_guard);

  if (!namespaces.isEmpty()) {
    writer.Line();
    writer.CloseNamespace();
  }

  if (!enums.isEmpty() || !file.pods.isEmpty()) {
    writer.Line();
    for (const EnumModel& model : enums) {
      writer.Line(QStringLiteral("Q_DECLARE_METATYPE(") +
                  Qualified(model.cpp_name, namespaces) + QLatin1Char(')'));
    }
    for (const PodModel& pod : file.pods) {
      writer.Line(QStringLiteral("Q_DECLARE_METATYPE(") + Qualified(pod.name, namespaces) +
                  QLatin1Char(')'));
    }
    writer.Line();

    const QString file_tag = StemTag(file.stem);
    EmitRegistrationHook(writer, file_tag, namespace_tag, detail_namespace, namespaces,
                         enums, file.pods);
  }

  writer.Line();
  writer.Line(QStringLiteral("#endif  // ") + guard);
  return writer.Text();
}

}  // namespace ddsrpcgen
