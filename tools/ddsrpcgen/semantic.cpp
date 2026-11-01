#include "semantic.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <utility>

#include "schema.h"

namespace ddsrpcgen {
namespace {

[[nodiscard]] bool IsCppKeyword(const QString& name) {
  static const QSet<QString> kKeywords = {
      QStringLiteral("alignas"),      QStringLiteral("alignof"),
      QStringLiteral("and"),          QStringLiteral("and_eq"),
      QStringLiteral("asm"),          QStringLiteral("auto"),
      QStringLiteral("bitand"),       QStringLiteral("bitor"),
      QStringLiteral("bool"),         QStringLiteral("break"),
      QStringLiteral("case"),         QStringLiteral("catch"),
      QStringLiteral("char"),         QStringLiteral("char16_t"),
      QStringLiteral("char32_t"),     QStringLiteral("class"),
      QStringLiteral("compl"),        QStringLiteral("const"),
      QStringLiteral("constexpr"),    QStringLiteral("const_cast"),
      QStringLiteral("continue"),     QStringLiteral("decltype"),
      QStringLiteral("default"),      QStringLiteral("delete"),
      QStringLiteral("do"),           QStringLiteral("double"),
      QStringLiteral("dynamic_cast"), QStringLiteral("else"),
      QStringLiteral("enum"),         QStringLiteral("explicit"),
      QStringLiteral("export"),       QStringLiteral("extern"),
      QStringLiteral("false"),        QStringLiteral("float"),
      QStringLiteral("for"),          QStringLiteral("friend"),
      QStringLiteral("goto"),         QStringLiteral("if"),
      QStringLiteral("inline"),       QStringLiteral("int"),
      QStringLiteral("long"),         QStringLiteral("mutable"),
      QStringLiteral("namespace"),    QStringLiteral("new"),
      QStringLiteral("noexcept"),     QStringLiteral("not"),
      QStringLiteral("not_eq"),       QStringLiteral("nullptr"),
      QStringLiteral("operator"),     QStringLiteral("or"),
      QStringLiteral("or_eq"),        QStringLiteral("private"),
      QStringLiteral("protected"),    QStringLiteral("public"),
      QStringLiteral("register"),     QStringLiteral("reinterpret_cast"),
      QStringLiteral("return"),       QStringLiteral("short"),
      QStringLiteral("signed"),       QStringLiteral("sizeof"),
      QStringLiteral("static"),       QStringLiteral("static_assert"),
      QStringLiteral("static_cast"),  QStringLiteral("struct"),
      QStringLiteral("switch"),       QStringLiteral("template"),
      QStringLiteral("this"),         QStringLiteral("thread_local"),
      QStringLiteral("throw"),        QStringLiteral("true"),
      QStringLiteral("try"),          QStringLiteral("typedef"),
      QStringLiteral("typeid"),       QStringLiteral("typename"),
      QStringLiteral("union"),        QStringLiteral("unsigned"),
      QStringLiteral("using"),        QStringLiteral("virtual"),
      QStringLiteral("void"),         QStringLiteral("volatile"),
      QStringLiteral("wchar_t"),      QStringLiteral("while"),
      QStringLiteral("xor"),          QStringLiteral("xor_eq"),
  };
  return kKeywords.contains(name);
}

[[nodiscard]] bool IsReservedIdentifier(const QString& name) {
  if (name.contains(QStringLiteral("__"))) return true;
  return name.size() >= 2 && name.at(0) == QLatin1Char('_') && name.at(1).isUpper();
}

// Generated code includes Qt headers, where signals/slots/emit are macros
// unless the consuming project defines QT_NO_KEYWORDS. Qt owns the whole Q_
// macro namespace as well (Q_OBJECT, Q_PROPERTY, Q_SIGNALS, ...), so any such
// identifier would break the generated C++ regardless of where it appears.
[[nodiscard]] bool IsQtReservedName(const QString& name) {
  static const QSet<QString> kKeywords = {
      QStringLiteral("signals"),
      QStringLiteral("slots"),
      QStringLiteral("emit"),
  };
  return kKeywords.contains(name) || name.startsWith(QStringLiteral("Q_"));
}

// Names emitted by the service/client facades. A member with one of these
// names would be hidden by (or collide with) generated state, lifecycle
// methods, or virtual hooks, so they are rejected before emission.
[[nodiscard]] bool IsReservedMemberName(const QString& name) {
  if (name.startsWith(QStringLiteral("ddsrpc_"))) return true;
  static const QSet<QString> kReserved = {
      // Base lifecycle, generated lifecycle wrappers, and registration.
      QStringLiteral("prepare"),
      QStringLiteral("start"),
      QStringLiteral("stop"),
      QStringLiteral("Prepare"),
      QStringLiteral("Start"),
      QStringLiteral("Stop"),
      QStringLiteral("registerTypes"),
      QStringLiteral("ready"),
      QStringLiteral("readyChanged"),
      QStringLiteral("syncFailed"),
      QStringLiteral("lastErrorText"),
      QStringLiteral("refreshProperties"),
      QStringLiteral("prepared"),
      QStringLiteral("running"),
      QStringLiteral("Options"),
      // ServiceBase surface.
      QStringLiteral("HandleOperation"),
      QStringLiteral("BuildPropertySnapshot"),
      QStringLiteral("PublishEvent"),
      QStringLiteral("NextRevision"),
      QStringLiteral("schema_hash"),
      QStringLiteral("node"),
      // ClientBase surface and generated hooks.
      QStringLiteral("StartEvents"),
      QStringLiteral("StopEvents"),
      QStringLiteral("StopEndpoint"),
      QStringLiteral("HandleLinkState"),
      QStringLiteral("Call"),
      QStringLiteral("CallWithRetry"),
      QStringLiteral("ResetPropertiesForResync"),
      QStringLiteral("ApplyPropertySnapshot"),
      QStringLiteral("DispatchSignal"),
      QStringLiteral("DispatchProperty"),
      QStringLiteral("EmitReadyChanged"),
      QStringLiteral("EmitSyncFailed"),
      // Generated helpers and state.
      QStringLiteral("LinkMonitorLoop"),
      QStringLiteral("ResetPropertyCacheToDefaults"),
      QStringLiteral("EmitPendingPropertyChanges"),
      QStringLiteral("transport_"),
      QStringLiteral("options_"),
      QStringLiteral("link_monitor_"),
      QStringLiteral("link_monitor_fiber_"),
      QStringLiteral("running_"),
      QStringLiteral("event_dispatch_fiber_"),
  };
  return kReserved.contains(name);
}

[[nodiscard]] QString Capitalize(const QString& name) {
  if (name.isEmpty()) return name;
  QString capitalized = name;
  capitalized[0] = capitalized.at(0).toUpper();
  return capitalized;
}

[[nodiscard]] QString NormalizeTypeSpelling(const QString& spelling) {
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

// QDataStream only has overloads for the fixed-width Qt integer types. Bare
// char is ambiguous between qint8/quint8 and platform-dependent, long does not
// match qint64 on LP64, and wchar_t/char16_t/char32_t/long double have no
// overload at all. Accepting them would produce generated code that cannot
// compile, so the semantic layer rejects them with a suggested replacement.
// Duplicate or contradictory specifier sequences ("unsigned unsigned int",
// "signed signed char", "double double", "long short int", ...) are rejected
// as malformed before they can be emitted. The empty string means the builtin
// is supported.
[[nodiscard]] QString UnsupportedBuiltinReason(const QString& spelling) {
  const QStringList words = spelling.split(QLatin1Char(' '), QString::SkipEmptyParts);
  int sign_words = 0;
  int longs = 0;
  int shorts = 0;
  QHash<QString, int> base_words;
  for (const QString& word : words) {
    if (word == QStringLiteral("unsigned")) {
      ++sign_words;
    } else if (word == QStringLiteral("signed")) {
      ++sign_words;
    } else if (word == QStringLiteral("long")) {
      ++longs;
    } else if (word == QStringLiteral("short")) {
      ++shorts;
    } else {
      ++base_words[word];
    }
  }

  const auto invalid = [&spelling] {
    return QStringLiteral("invalid builtin type spelling '%1'").arg(spelling);
  };
  // Repeated sign/base specifiers and multiple base type names never denote a
  // valid C++ builtin (and never a QDataStream overload).
  if (sign_words > 1) return invalid();
  if (base_words.size() > 1) return invalid();
  for (auto it = base_words.constBegin(); it != base_words.constEnd(); ++it) {
    if (it.value() > 1) return invalid();
  }

  const QString base = base_words.isEmpty() ? QStringLiteral("int")
                                            : base_words.constBegin().key();
  const bool has_sign = sign_words > 0;
  if (base == QStringLiteral("bool") || base == QStringLiteral("float") ||
      base == QStringLiteral("void")) {
    if (has_sign || longs > 0 || shorts > 0) return invalid();
    return {};
  }
  if (base == QStringLiteral("double")) {
    if (longs > 1 || shorts > 0 || has_sign) return invalid();
    if (longs == 1) {
      return QStringLiteral("builtin type '%1' is not supported by QDataStream; use 'double'")
          .arg(spelling);
    }
    return {};
  }
  if (base == QStringLiteral("wchar_t") || base == QStringLiteral("char16_t") ||
      base == QStringLiteral("char32_t")) {
    if (has_sign || longs > 0 || shorts > 0) return invalid();
    return QStringLiteral(
               "builtin type '%1' is not supported by QDataStream; use an integer type "
               "or QString")
        .arg(spelling);
  }
  if (base == QStringLiteral("char")) {
    if (longs > 0 || shorts > 0) return invalid();
    if (!has_sign) {
      return QStringLiteral(
                 "builtin type 'char' is not supported by QDataStream because its "
                 "signedness is platform-dependent; use 'signed char' or 'unsigned char'");
    }
    return {};
  }
  // Remaining valid base: int (explicit or implied by signed/short/long).
  if (longs > 2 || shorts > 1 || (longs > 0 && shorts > 0)) return invalid();
  if (longs == 1) {
    return QStringLiteral(
               "builtin type '%1' is not supported by QDataStream; use 'long long' or "
               "'unsigned long long'")
        .arg(spelling);
  }
  return {};
}

[[nodiscard]] bool IsBareIdentifier(const QString& text) {
  if (text.isEmpty()) return false;
  for (int i = 0; i < text.size(); ++i) {
    const QChar c = text.at(i);
    const bool valid = i == 0 ? (c.isLetter() || c == QLatin1Char('_'))
                              : (c.isLetterOrNumber() || c == QLatin1Char('_'));
    if (!valid) return false;
  }
  return true;
}

[[nodiscard]] QString Qualify(const QString& name, const QString& name_space) {
  if (name_space.isEmpty()) return name;
  return name_space + QStringLiteral("::") + name;
}

struct DeclRecord {
  QString file;
  SourceLocation loc;
  QString kind;
};

// Generated type symbols declared by one file. Resolution uses these per-file
// tables; only the file itself and its transitive imports are visible.
struct FileSymbols {
  QHash<QString, EnumModel> enums;
  QHash<QString, EnumModel> enum_aliases;
  QSet<QString> pods;
};

struct ResolvedType {
  QString cpp_type;
  bool is_reference = false;
  bool is_void = false;
  bool is_generated_enum = false;
  EnumModel enum_model;
};

class SemanticAnalyzer {
 public:
  SemanticAnalyzer(const LoadedProgram& program, QString name_space, Diagnostics& diagnostics)
      : program_(program),
        name_space_(std::move(name_space)),
        diagnostics_(diagnostics),
        diagnostic_base_(diagnostics.All().size()) {}

  [[nodiscard]] std::optional<Model> Run() {
    BuildSkeletons();
    BuildImportEdges();
    BuildImportClosures();
    BuildVisibleSymbols();
    // Ambiguous output names make downstream schema/emitter work meaningless;
    // stop before resolving declarations or computing schemas.
    const int before_stem_check = diagnostics_.All().size();
    RejectDuplicateOutputStems();
    if (diagnostics_.All().size() != before_stem_check) return std::nullopt;
    ResolveDeclarations();
    SortPods();
    ComputeSchemas();
    if (diagnostics_.All().size() != diagnostic_base_) return std::nullopt;
    return std::optional<Model>(std::move(model_));
  }

 private:
  [[nodiscard]] EnumModel MakeEnum(const QString& idl_name, const QString& cpp_name,
                                   const QString& owner, const QVector<EnumValue>& values) const {
    EnumModel model;
    model.idl_name = idl_name;
    model.cpp_name = cpp_name;
    model.class_owner = owner;
    model.values = values;
    return model;
  }

  void ValidateIdentifier(const QString& name, const QString& what, const QString& file,
                          const SourceLocation& loc) {
    if (IsCppKeyword(name)) {
      diagnostics_.Error(file, loc,
                         QStringLiteral("'%1' is a C++ keyword and cannot be used as %2")
                             .arg(name, what));
    } else if (IsReservedIdentifier(name)) {
      diagnostics_.Error(
          file, loc,
          QStringLiteral("'%1' is a reserved C++ identifier and cannot be used as %2")
              .arg(name, what));
    } else if (IsQtReservedName(name)) {
      diagnostics_.Error(
          file, loc,
          QStringLiteral("'%1' is reserved by Qt and cannot be used as %2").arg(name, what));
    }
  }

  void ValidateEnum(const EnumModel& model, const QString& file, const SourceLocation& loc) {
    QSet<QString> values;
    for (const EnumValue& value : model.values) {
      ValidateIdentifier(value.name, QStringLiteral("an ENUM value"), file, loc);
      if (values.contains(value.name)) {
        diagnostics_.Error(file, loc,
                           QStringLiteral("duplicate value '%1' in ENUM '%2'")
                               .arg(value.name, model.idl_name));
      } else {
        values.insert(value.name);
      }
    }
  }

  void RegisterName(QHash<QString, DeclRecord>* table, const QString& name, const QString& file,
                    const SourceLocation& loc, const QString& kind) {
    const auto existing = table->constFind(name);
    if (existing == table->constEnd()) {
      table->insert(name, DeclRecord{file, loc, kind});
      return;
    }
    diagnostics_.Error(
        file, loc,
        QStringLiteral("duplicate declaration of '%1' (%2; previously declared as %3 in %4:%5:%6)")
            .arg(name)
            .arg(kind)
            .arg(existing->kind)
            .arg(QFileInfo(existing->file).fileName())
            .arg(existing->loc.line)
            .arg(existing->loc.column));
  }

  void BuildSkeletons() {
    model_.name_space = name_space_;
    const QVector<ImportedFile>& files = program_.files;
    model_.files.reserve(files.size());
    file_symbols_.reserve(files.size());
    for (int index = 0; index < files.size(); ++index) {
      const AstFile& ast = files[index].ast;
      FileModel file;
      file.path = files[index].canonical_path;
      file.stem = QFileInfo(file.path).completeBaseName();
      file.name_space = name_space_;
      file.directives = ast.directives;
      file.imports = files[index].resolved_imports;

      FileSymbols symbols;
      for (const EnumDecl& decl : ast.enums) {
        ValidateIdentifier(decl.name, QStringLiteral("an ENUM name"), file.path, decl.loc);
        EnumModel model = MakeEnum(decl.name, decl.name, QString(), decl.values);
        ValidateEnum(model, file.path, decl.loc);
        RegisterName(&declared_names_, model.idl_name, file.path, decl.loc,
                     QStringLiteral("ENUM"));
        RegisterName(&emitted_names_, model.idl_name, file.path, decl.loc,
                     QStringLiteral("ENUM"));
        symbols.enums.insert(model.idl_name, model);
        type_records_.insert(model.idl_name,
                             DeclRecord{file.path, decl.loc, QStringLiteral("ENUM")});
        file.enums.append(model);
      }

      for (const PodDecl& decl : ast.pods) {
        ValidateIdentifier(decl.name, QStringLiteral("a POD name"), file.path, decl.loc);
        RegisterName(&declared_names_, decl.name, file.path, decl.loc, QStringLiteral("POD"));
        RegisterName(&emitted_names_, decl.name, file.path, decl.loc, QStringLiteral("POD"));
        PodModel model;
        model.name = decl.name;
        symbols.pods.insert(model.name);
        type_records_.insert(model.name, DeclRecord{file.path, decl.loc, QStringLiteral("POD")});
        file.pods.append(model);
      }

      for (const ClassDecl& decl : ast.classes) {
        ValidateIdentifier(decl.name, QStringLiteral("a class name"), file.path, decl.loc);
        RegisterName(&declared_names_, decl.name, file.path, decl.loc,
                     QStringLiteral("class"));
        RegisterName(&emitted_names_, decl.name + QStringLiteral("Service"), file.path,
                     decl.loc, QStringLiteral("generated Service class"));
        RegisterName(&emitted_names_, decl.name + QStringLiteral("Client"), file.path,
                     decl.loc, QStringLiteral("generated Client class"));

        ClassModel cls;
        cls.idl_name = decl.name;
        for (const EnumDecl& enum_decl : decl.enums) {
          ValidateIdentifier(enum_decl.name, QStringLiteral("a class ENUM name"), file.path,
                             enum_decl.loc);
          if (IsReservedMemberName(enum_decl.name)) {
            diagnostics_.Error(
                file.path, enum_decl.loc,
                QStringLiteral("'%1' is a reserved member name and cannot be used as a class "
                               "ENUM name")
                    .arg(enum_decl.name));
          }
          const QString canonical = decl.name + QStringLiteral("_") + enum_decl.name;
          EnumModel model = MakeEnum(enum_decl.name, canonical, decl.name, enum_decl.values);
          ValidateEnum(model, file.path, enum_decl.loc);
          RegisterName(&emitted_names_, model.cpp_name, file.path, enum_decl.loc,
                       QStringLiteral("class ENUM"));
          const QString alias = decl.name + QStringLiteral("::") + model.idl_name;
          symbols.enums.insert(model.cpp_name, model);
          symbols.enum_aliases.insert(alias, model);
          type_records_.insert(
              model.cpp_name,
              DeclRecord{file.path, enum_decl.loc, QStringLiteral("class ENUM")});
          type_records_.insert(alias, DeclRecord{file.path, enum_decl.loc,
                                                 QStringLiteral("class ENUM alias")});
          cls.class_enums.append(model);
        }
        file.classes.append(cls);
      }

      index_by_path_.insert(file.path, index);
      file_symbols_.append(std::move(symbols));
      model_.files.append(std::move(file));
    }
  }

  void BuildImportEdges() {
    direct_imports_.resize(program_.files.size());
    for (int index = 0; index < program_.files.size(); ++index) {
      const ImportedFile& imported = program_.files[index];
      QSet<int> edges;
      for (const QString& resolved_import : imported.resolved_imports) {
        const auto found = index_by_path_.constFind(resolved_import);
        if (found == index_by_path_.constEnd()) {
          diagnostics_.Error(
              imported.canonical_path, SourceLocation{},
              QStringLiteral("internal error: resolved import '%1' was not loaded")
                  .arg(resolved_import));
          continue;
        }
        if (found.value() != index) edges.insert(found.value());
      }
      QVector<int> edge_list;
      for (const int edge : edges) edge_list.append(edge);
      std::sort(edge_list.begin(), edge_list.end());
      direct_imports_[index] = edge_list;
    }
  }

  void BuildImportClosures() {
    closures_.resize(direct_imports_.size());
    for (int start = 0; start < direct_imports_.size(); ++start) {
      QSet<int> seen;
      seen.insert(start);
      std::function<void(int)> visit = [&](int node) {
        for (const int dependency : direct_imports_.at(node)) {
          if (seen.contains(dependency)) continue;
          seen.insert(dependency);
          visit(dependency);
        }
      };
      visit(start);
      QVector<int> dependencies;
      for (const int dependency : seen) {
        if (dependency != start) dependencies.append(dependency);
      }
      std::sort(dependencies.begin(), dependencies.end());
      closures_[start] = dependencies;
    }
  }

  void BuildVisibleSymbols() {
    visible_symbols_.resize(file_symbols_.size());
    for (int index = 0; index < file_symbols_.size(); ++index) {
      FileSymbols visible;
      const auto merge = [&visible](const FileSymbols& source) {
        for (auto it = source.enums.constBegin(); it != source.enums.constEnd(); ++it) {
          if (!visible.enums.contains(it.key())) visible.enums.insert(it.key(), it.value());
        }
        for (auto it = source.enum_aliases.constBegin();
             it != source.enum_aliases.constEnd(); ++it) {
          if (!visible.enum_aliases.contains(it.key())) {
            visible.enum_aliases.insert(it.key(), it.value());
          }
        }
        visible.pods.unite(source.pods);
      };
      merge(file_symbols_.at(index));
      for (const int dependency : closures_.at(index)) {
        merge(file_symbols_.at(dependency));
      }
      visible_symbols_[index] = std::move(visible);
    }
  }

  void RejectDuplicateOutputStems() {
    QHash<QString, QString> first_path;
    for (const FileModel& file : model_.files) {
      const auto existing = first_path.constFind(file.stem);
      if (existing == first_path.constEnd()) {
        first_path.insert(file.stem, file.path);
        continue;
      }
      diagnostics_.Error(
          file.path, SourceLocation{},
          QStringLiteral("duplicate output stem '%1': '%2' and '%3' would generate the same "
                         "output files")
              .arg(file.stem, existing.value(), file.path));
    }
  }

  void ResolveDeclarations() {
    for (int index = 0; index < model_.files.size(); ++index) {
      FileModel& file = model_.files[index];
      const AstFile& ast = program_.files.at(index).ast;
      for (int pod = 0; pod < file.pods.size(); ++pod) {
        BuildPod(file.pods[pod], ast.pods.at(pod), index, file.path);
      }
      for (int cls = 0; cls < file.classes.size(); ++cls) {
        BuildClass(file.classes[cls], ast.classes.at(cls), index, file.path);
      }
    }
  }

  void BuildPod(PodModel& pod, const PodDecl& decl, int file_index, const QString& file) {
    QSet<QString> field_names;
    for (const FieldDecl& field : decl.fields) {
      ValidateIdentifier(field.name, QStringLiteral("a POD field name"), file, field.loc);
      if (field.name == pod.name) {
        diagnostics_.Error(
            file, field.loc,
            QStringLiteral("field '%1' in POD '%2' cannot have the same name as the POD")
                .arg(field.name, pod.name));
      }
      if (field_names.contains(field.name)) {
        diagnostics_.Error(
            file, field.loc,
            QStringLiteral("duplicate field '%1' in POD '%2'").arg(field.name, pod.name));
      } else {
        field_names.insert(field.name);
      }
      const ResolvedType type =
          ResolveType(field.type, nullptr, file_index, file, field.loc);
      if (type.is_void) {
        diagnostics_.Error(file, field.loc,
                           QStringLiteral("field '%1' in POD '%2' cannot have type 'void'")
                               .arg(field.name, pod.name));
      }
      PodFieldModel model;
      model.name = field.name;
      model.cpp_type = type.cpp_type;
      pod.fields.append(model);
    }
  }

  [[nodiscard]] ResolvedType ResolveType(const TypeRef& type,
                                         const QHash<QString, EnumModel>* class_enums,
                                         int file_index, const QString& file,
                                         const SourceLocation& loc) {
    ResolvedType result;
    result.is_reference = type.is_reference;
    if (type.spelling.contains(QLatin1Char('*'))) {
      diagnostics_.Error(file, loc, QStringLiteral("pointer types are not supported"));
    }

    const FileSymbols& visible = visible_symbols_.at(file_index);
    bool has_enum = false;
    EnumModel enum_model;
    if (type.args.isEmpty()) {
      if (class_enums != nullptr) {
        const auto current = class_enums->constFind(type.base);
        if (current != class_enums->constEnd()) {
          enum_model = current.value();
          has_enum = true;
        }
      }
      if (!has_enum) {
        const auto alias = visible.enum_aliases.constFind(type.base);
        if (alias != visible.enum_aliases.constEnd()) {
          enum_model = alias.value();
          has_enum = true;
        }
      }
      if (!has_enum) {
        const auto known = visible.enums.constFind(type.base);
        if (known != visible.enums.constEnd()) {
          enum_model = known.value();
          has_enum = true;
        }
      }
    }

    const QString spelling = NormalizeTypeSpelling(type.spelling);
    const bool builtin = type.args.isEmpty() && IsBuiltinCore(spelling);
    QString core;
    if (builtin) {
      core = spelling;
      result.is_void = spelling == QStringLiteral("void");
      const QString reason = UnsupportedBuiltinReason(spelling);
      if (!reason.isEmpty()) diagnostics_.Error(file, loc, reason);
    } else if (has_enum) {
      core = Qualify(enum_model.cpp_name, name_space_);
    } else if (visible.pods.contains(type.base)) {
      core = Qualify(type.base, name_space_);
    } else {
      if (type.args.isEmpty()) {
        const auto declared = type_records_.constFind(type.base);
        if (declared != type_records_.constEnd()) {
          diagnostics_.Error(
              file, loc,
              QStringLiteral("type '%1' is declared in '%2' but is not visible here; add an "
                             "IMPORT for that file")
                  .arg(type.base, declared->file));
        }
      }
      core = type.base;
    }

    if (!type.args.isEmpty()) {
      QStringList arguments;
      for (const TypeRef& argument : type.args) {
        arguments.append(
            ResolveType(argument, class_enums, file_index, file, loc).cpp_type);
      }
      core += QLatin1Char('<') + arguments.join(QStringLiteral(", ")) + QLatin1Char('>');
    }

    result.cpp_type = core;
    result.is_generated_enum = has_enum && type.args.isEmpty();
    if (result.is_generated_enum) result.enum_model = enum_model;
    return result;
  }

  [[nodiscard]] QVector<ParamModel> BuildParams(
      const QVector<ParamDecl>& params, const QHash<QString, EnumModel>* class_enums,
      int file_index, const QString& file) {
    QVector<ParamModel> result;
    QSet<QString> names;
    int unnamed_index = 0;
    for (const ParamDecl& decl : params) {
      ParamModel model;
      model.named = decl.named;
      model.name = decl.named ? decl.name
                              : QStringLiteral("arg%1").arg(unnamed_index++);
      if (decl.named) {
        ValidateIdentifier(model.name, QStringLiteral("a parameter name"), file, decl.loc);
      }
      const ResolvedType type =
          ResolveType(decl.type, class_enums, file_index, file, decl.loc);
      if (type.is_void) {
        diagnostics_.Error(file, decl.loc,
                           QStringLiteral("parameter '%1' cannot have type 'void'")
                               .arg(model.name));
      }
      model.cpp_type = type.cpp_type;
      if (names.contains(model.name)) {
        diagnostics_.Error(file, decl.loc,
                           QStringLiteral("duplicate parameter '%1'").arg(model.name));
      } else {
        names.insert(model.name);
      }
      result.append(model);
    }
    return result;
  }

  void BuildClass(ClassModel& cls, const ClassDecl& decl, int file_index,
                  const QString& file) {
    QHash<QString, EnumModel> class_enums;
    for (const EnumModel& model : cls.class_enums) {
      class_enums.insert(model.idl_name, model);
    }

    QHash<QString, QString> declared;
    const auto note_member = [&](const QString& name, const QString& what,
                                 const SourceLocation& loc) {
      ValidateIdentifier(name, what, file, loc);
      if (IsReservedMemberName(name)) {
        diagnostics_.Error(
            file, loc,
            QStringLiteral("'%1' is a reserved member name and cannot be used as %2")
                .arg(name, what));
      }
      if (declared.contains(name)) {
        diagnostics_.Error(file, loc,
                           QStringLiteral("duplicate member '%1' in class '%2'")
                               .arg(name, cls.idl_name));
      } else {
        declared.insert(name, what);
      }
    };
    for (const PropDecl& property : decl.properties) {
      note_member(property.name, QStringLiteral("a property name"), property.loc);
    }
    for (const SignalDecl& signal : decl.signals) {
      note_member(signal.name, QStringLiteral("a signal name"), signal.loc);
    }
    for (const SlotDecl& slot : decl.slots) {
      note_member(slot.name, QStringLiteral("a slot name"), slot.loc);
    }

    int property_id = 0;
    int op = 0;
    for (const PropDecl& property : decl.properties) {
      const ResolvedType type =
          ResolveType(property.type, &class_enums, file_index, file, property.loc);
      if (property.persisted) {
        diagnostics_.Error(file, property.loc, QStringLiteral("PERSISTED is not supported"));
      }
      if (type.is_void) {
        diagnostics_.Error(file, property.loc,
                           QStringLiteral("property '%1' cannot have type 'void'")
                               .arg(property.name));
      }
      PropertyModel model;
      model.name = property.name;
      model.cpp_type = type.cpp_type;
      model.default_value = property.default_value;
      model.has_default = property.has_default;
      model.modifier = property.modifier;
      model.property_id = ++property_id;
      if (property.modifier == PropModifier::kReadPush ||
          property.modifier == PropModifier::kReadWrite) {
        model.remote_setter_op = ++op;
      }
      if (property.has_default && type.is_generated_enum) {
        const QString expression = property.default_value.trimmed();
        if (IsBareIdentifier(expression)) {
          for (const EnumValue& value : type.enum_model.values) {
            if (value.name == expression) {
              model.default_value = type.enum_model.cpp_name + QStringLiteral("::") + expression;
              break;
            }
          }
        }
      }
      cls.properties.append(model);
    }

    int event_id = 0;
    for (const SignalDecl& signal : decl.signals) {
      SignalModel model;
      model.name = signal.name;
      model.params = BuildParams(signal.params, &class_enums, file_index, file);
      model.event_id = ++event_id;
      cls.signals.append(model);
    }

    for (const SlotDecl& slot : decl.slots) {
      const ResolvedType return_type =
          ResolveType(slot.return_type, &class_enums, file_index, file, slot.loc);
      if (return_type.is_reference) {
        diagnostics_.Error(
            file, slot.loc,
            QStringLiteral("SLOT '%1' cannot return a reference").arg(slot.name));
      }
      SlotModel model;
      model.name = slot.name;
      model.return_cpp_type = return_type.cpp_type;
      model.params = BuildParams(slot.params, &class_enums, file_index, file);
      model.op = ++op;
      cls.slots.append(model);
    }

    ValidateGeneratedNames(cls, decl, file);
  }

  void ValidateGeneratedNames(const ClassModel& cls, const ClassDecl& decl,
                              const QString& file) {
    QHash<QString, QString> service;
    QHash<QString, QString> client;
    // A member or alias with the enclosing class name would be parsed as a
    // (malformed) constructor, so seed each generated class with its own name.
    const QString service_class = cls.idl_name + QStringLiteral("Service");
    const QString client_class = cls.idl_name + QStringLiteral("Client");
    service.insert(service_class,
                   QStringLiteral("generated class '%1'").arg(service_class));
    client.insert(client_class, QStringLiteral("generated class '%1'").arg(client_class));
    const auto add = [&](QHash<QString, QString>* names, const QString& name,
                         const QString& origin, const SourceLocation& loc) {
      const auto existing = names->constFind(name);
      if (existing != names->constEnd()) {
        diagnostics_.Error(
            file, loc,
            QStringLiteral("generated member '%1' for %2 conflicts with %3 in class '%4'")
                .arg(name, origin, existing.value(), cls.idl_name));
        return;
      }
      names->insert(name, origin);
    };

    for (int index = 0; index < cls.properties.size(); ++index) {
      const PropertyModel& property = cls.properties.at(index);
      const SourceLocation loc = decl.properties.at(index).loc;
      const QString property_origin =
          QStringLiteral("property '%1'").arg(property.name);
      add(&service, property.name, property_origin + QStringLiteral(" getter"), loc);
      add(&client, property.name, property_origin + QStringLiteral(" getter"), loc);

      const QString capitalized = Capitalize(property.name);
      // The change signal is reserved for every property, including CONSTANT:
      // future emitters may generate it unconditionally, and the model must
      // never allow a colliding SIGNAL to reach them.
      add(&client, property.name + QStringLiteral("Changed"),
          property_origin + QStringLiteral(" change signal"), loc);
      if (property.modifier != PropModifier::kConstant) {
        add(&service, QStringLiteral("set") + capitalized,
            property_origin + QStringLiteral(" setter"), loc);
      }
      if (property.modifier == PropModifier::kReadWrite) {
        add(&client, QStringLiteral("set") + capitalized,
            property_origin + QStringLiteral(" setter"), loc);
      } else if (property.modifier == PropModifier::kReadPush) {
        add(&client, QStringLiteral("push") + capitalized,
            property_origin + QStringLiteral(" push method"), loc);
      }
    }

    for (int index = 0; index < cls.signals.size(); ++index) {
      const SignalModel& signal = cls.signals.at(index);
      const SourceLocation loc = decl.signals.at(index).loc;
      const QString origin = QStringLiteral("signal '%1'").arg(signal.name);
      add(&service, QStringLiteral("publish") + Capitalize(signal.name),
          origin + QStringLiteral(" publisher"), loc);
      add(&client, signal.name, origin, loc);
    }

    for (int index = 0; index < cls.slots.size(); ++index) {
      const SlotModel& slot = cls.slots.at(index);
      const SourceLocation loc = decl.slots.at(index).loc;
      const QString origin = QStringLiteral("slot '%1'").arg(slot.name);
      add(&service, slot.name, origin, loc);
      add(&client, slot.name, origin, loc);
    }

    for (int index = 0; index < cls.class_enums.size(); ++index) {
      const EnumModel& model = cls.class_enums.at(index);
      const SourceLocation loc = decl.enums.at(index).loc;
      const QString origin = QStringLiteral("class enum '%1::%2' alias")
                                 .arg(cls.idl_name, model.idl_name);
      add(&service, model.idl_name, origin, loc);
      add(&client, model.idl_name, origin, loc);
    }
  }

  void CollectPodDependencies(const TypeRef& type, const QHash<QString, int>& index,
                              QVector<int>* dependencies) const {
    const auto found = index.constFind(type.base);
    if (found != index.constEnd() && !dependencies->contains(found.value())) {
      dependencies->append(found.value());
    }
    for (const TypeRef& argument : type.args) {
      CollectPodDependencies(argument, index, dependencies);
    }
  }

  void SortPods() {
    for (int file_index = 0; file_index < model_.files.size(); ++file_index) {
      FileModel& file = model_.files[file_index];
      const QVector<PodDecl>& declarations = program_.files.at(file_index).ast.pods;
      QHash<QString, int> index;
      for (int i = 0; i < file.pods.size(); ++i) {
        if (!index.contains(file.pods.at(i).name)) index.insert(file.pods.at(i).name, i);
      }

      QVector<int> state(file.pods.size(), 0);
      QVector<int> stack;
      QVector<int> order;
      QSet<QString> reported;
      std::function<void(int)> visit = [&](int current) {
        if (state.at(current) != 0) return;
        state[current] = 1;
        stack.append(current);
        QVector<int> dependencies;
        // file.pods is still in AST order here; dependency discovery uses the
        // raw AST types because PodModel fields are already resolved C++.
        for (const FieldDecl& field : declarations.at(current).fields) {
          CollectPodDependencies(field.type, index, &dependencies);
        }
        for (const int dependency : dependencies) {
          if (state.at(dependency) == 1) {
            const int begin = stack.indexOf(dependency);
            QStringList chain;
            for (int i = begin; i < stack.size(); ++i) {
              chain.append(file.pods.at(stack.at(i)).name);
            }
            chain.append(file.pods.at(dependency).name);
            const QString message = chain.join(QStringLiteral(" -> "));
            if (!reported.contains(message)) {
              reported.insert(message);
              const SourceLocation loc =
                  current < declarations.size() ? declarations.at(current).loc
                                                : SourceLocation{};
              diagnostics_.Error(file.path, loc,
                                 QStringLiteral("POD dependency cycle: %1").arg(message));
            }
          } else {
            visit(dependency);
          }
        }
        stack.removeLast();
        state[current] = 2;
        order.append(current);
      };
      for (int i = 0; i < file.pods.size(); ++i) visit(i);

      QVector<PodModel> sorted;
      sorted.reserve(order.size());
      for (const int i : order) sorted.append(file.pods.at(i));
      file.pods = std::move(sorted);
    }
  }

  void ComputeSchemas() {
    for (int index = 0; index < model_.files.size(); ++index) {
      QVector<FileModel> imported;
      for (const int dependency : closures_.at(index)) {
        imported.append(model_.files.at(dependency));
      }
      const quint64 hash = ComputeSchema(model_.files.at(index), imported);
      model_.files[index].schema_hash = hash;
      for (ClassModel& cls : model_.files[index].classes) cls.schema_hash = hash;
    }
  }

  const LoadedProgram& program_;
  const QString name_space_;
  Diagnostics& diagnostics_;
  const int diagnostic_base_;
  Model model_;
  QHash<QString, DeclRecord> declared_names_;
  QHash<QString, DeclRecord> emitted_names_;
  // Every globally known generated type spelling (POD/ENUM/class ENUM canonical
  // names and Class::Enum aliases) with its declaration location. A reference
  // to one of these from outside its import closure is an error.
  QHash<QString, DeclRecord> type_records_;
  QHash<QString, int> index_by_path_;
  QVector<FileSymbols> file_symbols_;
  QVector<FileSymbols> visible_symbols_;
  QVector<QVector<int>> direct_imports_;
  QVector<QVector<int>> closures_;
};

}  // namespace

std::optional<Model> Analyze(const LoadedProgram& program, const QString& name_space,
                             Diagnostics& diagnostics) {
  SemanticAnalyzer analyzer(program, name_space, diagnostics);
  return analyzer.Run();
}

}  // namespace ddsrpcgen
