#include "schema.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace ddsrpcgen {
namespace {

// Length-prefixed fields keep the serialization unambiguous no matter what
// names, defaults or directives contain.
void AppendValue(QByteArray* out, const QByteArray& value) {
  out->append(QByteArray::number(value.size()));
  out->append(':');
  out->append(value);
  out->append(';');
}

void AppendField(QByteArray* out, const char* tag, const QString& value) {
  out->append(tag);
  out->append('=');
  AppendValue(out, value.toUtf8());
}

void AppendField(QByteArray* out, const char* tag, qint64 value) {
  out->append(tag);
  out->append('=');
  AppendValue(out, QByteArray::number(value));
}

void AppendFlag(QByteArray* out, const char* tag, bool value) {
  AppendField(out, tag, static_cast<qint64>(value ? 1 : 0));
}

[[nodiscard]] QString NormalizeBuiltinSpelling(const QString& spelling) {
  QStringList words;
  QString current;
  for (const QChar c : spelling) {
    if (c.isSpace() || c == QLatin1Char('&')) {
      if (!current.isEmpty()) {
        words.append(current);
        current.clear();
      }
    } else {
      current.append(c);
    }
  }
  if (!current.isEmpty()) words.append(current);
  words.removeAll(QStringLiteral("const"));
  return words.join(QLatin1Char(' '));
}

[[nodiscard]] bool IsBuiltinCore(const QString& spelling) {
  static const QSet<QString> kWords = {
      QStringLiteral("signed"),   QStringLiteral("unsigned"), QStringLiteral("short"),
      QStringLiteral("long"),     QStringLiteral("int"),      QStringLiteral("char"),
      QStringLiteral("bool"),     QStringLiteral("float"),    QStringLiteral("double"),
      QStringLiteral("void"),     QStringLiteral("wchar_t"),  QStringLiteral("char16_t"),
      QStringLiteral("char32_t"),
  };
  if (spelling.isEmpty()) return false;
  const QStringList words = spelling.split(QLatin1Char(' '), QString::SkipEmptyParts);
  if (words.isEmpty()) return false;
  for (const QString& word : words) {
    if (!kWords.contains(word)) return false;
  }
  return true;
}

[[nodiscard]] QString Qualify(const QString& name, const QString& name_space) {
  if (name_space.isEmpty()) return name;
  return name_space + QStringLiteral("::") + name;
}

struct TypeSymbols {
  QHash<QString, QString> names;    // IDL spelling -> unqualified generated name
  QHash<QString, QString> aliases;  // "Class::Enum" -> "Class_Enum"
};

void CollectSymbols(const FileModel& file, TypeSymbols* symbols) {
  for (const PodModel& pod : file.pods) symbols->names.insert(pod.name, pod.name);
  for (const EnumModel& e : file.enums) symbols->names.insert(e.idl_name, e.cpp_name);
  for (const ClassModel& cls : file.classes) {
    for (const EnumModel& e : cls.class_enums) {
      symbols->names.insert(e.cpp_name, e.cpp_name);
      if (!e.class_owner.isEmpty()) {
        symbols->aliases.insert(e.class_owner + QStringLiteral("::") + e.idl_name, e.cpp_name);
      }
    }
  }
}

[[nodiscard]] QString CanonicalType(const TypeRef& type, const TypeSymbols& symbols,
                                    const QString& name_space) {
  const QString builtin = NormalizeBuiltinSpelling(type.spelling);
  QString base;
  if (type.args.isEmpty() && IsBuiltinCore(builtin)) {
    base = builtin;
  } else {
    const auto alias = symbols.aliases.constFind(type.base);
    if (alias != symbols.aliases.constEnd()) {
      base = Qualify(alias.value(), name_space);
    } else {
      const auto name = symbols.names.constFind(type.base);
      base = name != symbols.names.constEnd() ? Qualify(name.value(), name_space)
                                              : type.base;
    }
  }
  if (!type.args.isEmpty()) {
    QStringList args;
    for (const TypeRef& arg : type.args) {
      args.append(CanonicalType(arg, symbols, name_space));
    }
    base += QLatin1Char('<') + args.join(QStringLiteral(", ")) + QLatin1Char('>');
  }
  return base;
}

void AppendEnum(QByteArray* out, const EnumModel& e, const char* tag) {
  AppendField(out, tag, e.idl_name);
  AppendField(out, "enum_cpp", e.cpp_name);
  if (!e.class_owner.isEmpty()) AppendField(out, "enum_owner", e.class_owner);
  AppendField(out, "enum_value_count", e.values.size());
  for (const EnumValue& value : e.values) {
    AppendField(out, "enum_value", value.name);
    AppendField(out, "enum_number", static_cast<qint64>(value.value));
  }
}

void AppendPod(QByteArray* out, const PodModel& pod, const TypeSymbols& symbols,
               const QString& name_space, const char* tag) {
  AppendField(out, tag, pod.name);
  AppendField(out, "pod_field_count", pod.fields.size());
  for (const FieldDecl& field : pod.fields) {
    AppendField(out, "pod_field", field.name);
    AppendField(out, "pod_field_type", CanonicalType(field.type, symbols, name_space));
  }
}

void AppendParams(QByteArray* out, const QVector<ParamModel>& params) {
  AppendField(out, "param_count", params.size());
  for (const ParamModel& param : params) {
    AppendField(out, "param", param.name);
    AppendField(out, "param_type", param.cpp_type);
    AppendFlag(out, "param_named", param.named);
  }
}

void AppendClass(QByteArray* out, const ClassModel& cls) {
  AppendField(out, "class", cls.idl_name);
  AppendField(out, "property_count", cls.properties.size());
  for (const PropertyModel& property : cls.properties) {
    AppendField(out, "property", property.name);
    AppendField(out, "property_type", property.cpp_type);
    AppendFlag(out, "property_has_default", property.has_default);
    AppendField(out, "property_default", property.default_value);
    AppendField(out, "property_modifier", static_cast<qint64>(property.modifier));
    AppendField(out, "property_id", property.property_id);
    AppendField(out, "property_setter_op", property.remote_setter_op);
  }
  AppendField(out, "signal_count", cls.signals.size());
  for (const SignalModel& signal : cls.signals) {
    AppendField(out, "signal", signal.name);
    AppendField(out, "signal_event_id", signal.event_id);
    AppendParams(out, signal.params);
  }
  AppendField(out, "slot_count", cls.slots.size());
  for (const SlotModel& slot : cls.slots) {
    AppendField(out, "slot", slot.name);
    AppendField(out, "slot_return", slot.return_cpp_type);
    AppendField(out, "slot_op", slot.op);
    AppendParams(out, slot.params);
  }
  AppendField(out, "class_enum_count", cls.class_enums.size());
  for (const EnumModel& e : cls.class_enums) AppendEnum(out, e, "class_enum");
}

struct ImportedType {
  QString idl_name;
  QString cpp_name;
  int kind = 0;  // 0 = ENUM, 1 = POD
  EnumModel enum_model;
  PodModel pod_model;
};

}  // namespace

quint64 ComputeSchema(const FileModel& file, const QVector<FileModel>& imported_files) {
  QByteArray feed;
  AppendField(&feed, "schema_version", QStringLiteral("ddsrpc-1"));
  AppendField(&feed, "namespace", file.name_space);
  AppendField(&feed, "stem", file.stem);

  AppendField(&feed, "directive_count", file.directives.size());
  for (const Directive& directive : file.directives) {
    AppendField(&feed, "directive", directive.text);
  }

  // Import relations participate by stem only. The declaration spelling (for
  // example an absolute IMPORT path) must never leak into the schema, because
  // it differs between otherwise identical checkouts.
  AppendField(&feed, "import_count", file.imports.size());
  for (const QString& import : file.imports) {
    AppendField(&feed, "import", QFileInfo(import).completeBaseName());
  }

  TypeSymbols symbols;
  QSet<QString> seen_paths;
  if (!file.path.isEmpty()) seen_paths.insert(file.path);
  CollectSymbols(file, &symbols);
  for (const FileModel& imported : imported_files) {
    if (!imported.path.isEmpty()) {
      if (seen_paths.contains(imported.path)) continue;
      seen_paths.insert(imported.path);
    }
    CollectSymbols(imported, &symbols);
  }

  AppendField(&feed, "pod_count", file.pods.size());
  for (const PodModel& pod : file.pods) AppendPod(&feed, pod, symbols, file.name_space, "pod");

  AppendField(&feed, "enum_count", file.enums.size());
  for (const EnumModel& e : file.enums) AppendEnum(&feed, e, "enum");

  AppendField(&feed, "class_count", file.classes.size());
  for (const ClassModel& cls : file.classes) AppendClass(&feed, cls);

  // Imported file interfaces are intentionally excluded: only the type
  // declarations from the IMPORT graph are part of this file's schema.
  QVector<ImportedType> types;
  QSet<QString> type_paths;
  if (!file.path.isEmpty()) type_paths.insert(file.path);
  for (const FileModel& imported : imported_files) {
    if (!imported.path.isEmpty()) {
      if (type_paths.contains(imported.path)) continue;
      type_paths.insert(imported.path);
    }
    for (const PodModel& pod : imported.pods) {
      ImportedType type;
      type.idl_name = pod.name;
      type.cpp_name = pod.name;
      type.kind = 1;
      type.pod_model = pod;
      types.append(type);
    }
    QVector<EnumModel> enums = imported.enums;
    for (const ClassModel& cls : imported.classes) {
      enums += cls.class_enums;
    }
    for (const EnumModel& e : enums) {
      ImportedType type;
      type.idl_name = e.idl_name;
      type.cpp_name = e.cpp_name;
      type.kind = 0;
      type.enum_model = e;
      types.append(type);
    }
  }
  std::sort(types.begin(), types.end(), [](const ImportedType& a, const ImportedType& b) {
    if (a.idl_name != b.idl_name) return a.idl_name < b.idl_name;
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.cpp_name < b.cpp_name;
  });

  AppendField(&feed, "imported_type_count", types.size());
  for (const ImportedType& type : types) {
    if (type.kind == 0) {
      AppendEnum(&feed, type.enum_model, "imported_enum");
    } else {
      AppendPod(&feed, type.pod_model, symbols, file.name_space, "imported_pod");
    }
  }

  const QByteArray digest = QCryptographicHash::hash(feed, QCryptographicHash::Sha256);
  quint64 hash = 0;
  for (int i = 0; i < 8; ++i) {
    hash = (hash << 8) | static_cast<quint8>(digest.at(i));
  }
  return hash;
}

}  // namespace ddsrpcgen
