#include "import_resolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextCodec>

#include <utility>

#include "lexer.h"
#include "parser.h"

namespace ddsrpcgen {
namespace {

[[nodiscard]] bool HasIdlSuffix(const QString& path) {
  return path.endsWith(QLatin1String(".idl"), Qt::CaseInsensitive);
}

}  // namespace

ImportResolver::ImportResolver(QStringList include_dirs, Diagnostics& diagnostics)
    : include_dirs_(std::move(include_dirs)), diagnostics_(diagnostics) {}

std::optional<LoadedProgram> ImportResolver::Load(const QString& root_file) {
  if (!HasIdlSuffix(root_file)) {
    diagnostics_.Error(root_file, SourceLocation{},
                       QStringLiteral("root file must have a .idl extension: '%1'").arg(root_file));
    return std::nullopt;
  }

  const QFileInfo root_info(root_file);
  const QString root_path = root_info.canonicalFilePath();
  if (root_path.isEmpty() || !root_info.isFile()) {
    diagnostics_.Error(root_file, SourceLocation{},
                       QStringLiteral("cannot open root file '%1'").arg(root_file));
    return std::nullopt;
  }

  states_.clear();
  path_stack_.clear();
  LoadedProgram program;
  program.root_path = root_path;
  if (!LoadFile(root_path, program)) return std::nullopt;
  return program;
}

bool ImportResolver::LoadFile(const QString& canonical_path, LoadedProgram& program) {
  const auto existing = states_.constFind(canonical_path);
  if (existing != states_.constEnd()) {
    if (*existing == Visit::kDone) return true;
    // Imports reach this case through the kVisiting check in the import loop,
    // which can attach the IMPORT location; keep a defensive fallback.
    ReportCycle(canonical_path, SourceLocation{}, canonical_path);
    return false;
  }

  states_.insert(canonical_path, Visit::kVisiting);
  path_stack_.push_back(canonical_path);

  QString source;
  std::optional<AstFile> ast;
  bool ok = ReadSource(canonical_path, &source);
  if (ok) {
    // The parser deliberately ignores errors accumulated for earlier files, so
    // the only reliable per-file test is a diagnostic-count snapshot.
    const int diagnostics_before = diagnostics_.All().size();
    Lexer lexer(canonical_path, source, diagnostics_);
    QVector<Token> tokens = lexer.Scan();
    if (diagnostics_.All().size() != diagnostics_before) {
      ok = false;
    } else {
      Parser parser(canonical_path, std::move(tokens), source, diagnostics_);
      ast = parser.Parse();
      if (diagnostics_.All().size() != diagnostics_before) {
        ok = false;
      } else if (!ast.has_value()) {
        diagnostics_.Error(canonical_path, SourceLocation{},
                           QStringLiteral("internal error: parser produced no AST"));
        ok = false;
      }
    }
  }

  QVector<QString> resolved_imports;
  if (ok) {
    for (const ImportDecl& import : ast->imports) {
      const auto resolved = ResolveImport(canonical_path, import);
      if (!resolved.has_value()) {
        ok = false;
        break;
      }
      const QString imported_path = QFileInfo(*resolved).canonicalFilePath();
      if (imported_path.isEmpty()) {
        diagnostics_.Error(
            canonical_path, import.loc,
            QStringLiteral("cannot resolve imported file '%1'").arg(import.path));
        ok = false;
        break;
      }
      const auto imported_state = states_.constFind(imported_path);
      if (imported_state != states_.constEnd() && *imported_state == Visit::kVisiting) {
        ReportCycle(canonical_path, import.loc, imported_path);
        ok = false;
        break;
      }
      if (!LoadFile(imported_path, program)) {
        ok = false;
        break;
      }
      if (!resolved_imports.contains(imported_path)) {
        resolved_imports.append(imported_path);
      }
    }
  }

  path_stack_.removeLast();
  if (!ok) {
    states_.remove(canonical_path);
    return false;
  }
  states_[canonical_path] = Visit::kDone;
  program.files.push_back(
      ImportedFile{canonical_path, std::move(*ast), std::move(resolved_imports)});
  return true;
}

std::optional<QString> ImportResolver::ResolveImport(const QString& importer,
                                                     const ImportDecl& import) {
  if (!HasIdlSuffix(import.path)) {
    diagnostics_.Error(
        importer, import.loc,
        QStringLiteral("IMPORT target must have a .idl extension: '%1'").arg(import.path));
    return std::nullopt;
  }

  const QFileInfo target(import.path);
  if (target.isAbsolute()) {
    if (target.isFile()) return import.path;
    diagnostics_.Error(importer, import.loc,
                       QStringLiteral("imported file not found: '%1'").arg(import.path));
    return std::nullopt;
  }

  const QFileInfo relative(QDir(QFileInfo(importer).absolutePath()).filePath(import.path));
  if (relative.isFile()) return relative.absoluteFilePath();

  for (const QString& include_dir : include_dirs_) {
    const QFileInfo candidate(QDir(include_dir).filePath(import.path));
    if (candidate.isFile()) return candidate.absoluteFilePath();
  }

  diagnostics_.Error(importer, import.loc,
                     QStringLiteral("imported file not found: '%1'").arg(import.path));
  return std::nullopt;
}

bool ImportResolver::ReadSource(const QString& path, QString* source) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    diagnostics_.Error(path, SourceLocation{},
                       QStringLiteral("cannot open '%1' for reading: %2")
                           .arg(path, file.errorString()));
    return false;
  }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFileDevice::NoError) {
    diagnostics_.Error(path, SourceLocation{},
                       QStringLiteral("cannot read '%1': %2").arg(path, file.errorString()));
    return false;
  }

  QTextCodec::ConverterState state;
  *source = QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(), bytes.size(), &state);
  if (state.invalidChars > 0) {
    diagnostics_.Error(path, SourceLocation{},
                       QStringLiteral("'%1' is not valid UTF-8").arg(path));
    return false;
  }
  return true;
}

void ImportResolver::ReportCycle(const QString& importer, const SourceLocation& loc,
                                 const QString& target) {
  const int begin = path_stack_.indexOf(target);
  QStringList chain;
  for (int i = begin < 0 ? 0 : begin; i < path_stack_.size(); ++i) {
    chain.append(QFileInfo(path_stack_.at(i)).fileName());
  }
  chain.append(QFileInfo(target).fileName());
  diagnostics_.Error(importer, loc,
                     QStringLiteral("IMPORT cycle: %1").arg(chain.join(QStringLiteral(" -> "))));
}

}  // namespace ddsrpcgen
