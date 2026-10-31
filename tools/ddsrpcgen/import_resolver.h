#pragma once
#include <optional>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ast.h"
#include "diagnostic.h"

namespace ddsrpcgen {

struct ImportedFile {
  QString canonical_path;
  AstFile ast;
  // Canonical paths of the files named by this file's top-level IMPORT
  // declarations, in declaration order. Repeated imports of the same resolved
  // file are kept once. Semantic analysis derives the exact import graph from
  // these edges instead of re-resolving raw IMPORT spellings.
  QVector<QString> resolved_imports;
};

struct LoadedProgram {
  QVector<ImportedFile> files;
  QString root_path;
};

// Recursively loads an IDL file and every file it IMPORTs. Dependencies are
// appended to LoadedProgram::files before the file that imports them
// (post-order), so the root file is always last. A canonical path is loaded
// and stored at most once, which deduplicates diamond imports. All errors are
// appended to the caller-owned Diagnostics and make Load() return nullopt;
// a failed Load() never exposes a partially populated program.
class ImportResolver {
 public:
  ImportResolver(QStringList include_dirs, Diagnostics& diagnostics);

  [[nodiscard]] std::optional<LoadedProgram> Load(const QString& root_file);

 private:
  enum class Visit { kVisiting, kDone };

  bool LoadFile(const QString& canonical_path, LoadedProgram& program);
  [[nodiscard]] std::optional<QString> ResolveImport(const QString& importer,
                                                     const ImportDecl& import);
  [[nodiscard]] bool ReadSource(const QString& path, QString* source);
  void ReportCycle(const QString& importer, const SourceLocation& loc, const QString& target);

  QStringList include_dirs_;
  Diagnostics& diagnostics_;
  QHash<QString, Visit> states_;
  QVector<QString> path_stack_;
};

}  // namespace ddsrpcgen
