#include "cli.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QVector>

#include <cstddef>
#include <cstdio>
#include <utility>

#include "emit_client.h"
#include "emit_service.h"
#include "emit_types.h"
#include "import_resolver.h"
#include "semantic.h"

namespace ddsrpcgen {
namespace {

const char kUsage[] =
    "usage: ddsrpcgen --role types|service|client|all --out-dir DIR [--namespace NS] "
    "[-I DIR]... [--depfile FILE] <root.idl>...\n";

struct GeneratedFile {
  QString target;
  QByteArray contents;
};

[[nodiscard]] bool IsKnownRole(const QString& role) {
  return role == QStringLiteral("types") || role == QStringLiteral("service") ||
         role == QStringLiteral("client") || role == QStringLiteral("all");
}

void UsageError(Diagnostics& diagnostics, const QString& message) {
  diagnostics.Error(QStringLiteral("ddsrpcgen"), SourceLocation{}, message);
}

void PrintDiagnostics(const Diagnostics& diagnostics) {
  if (!diagnostics.HasErrors()) return;
  const QByteArray text = diagnostics.Format().toUtf8();
  std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void PrintUsage() {
  std::fwrite(kUsage, 1, sizeof(kUsage) - 1, stderr);
  std::fflush(stderr);
}

[[nodiscard]] std::optional<CliOptions> ParseArgs(const QStringList& args,
                                                  Diagnostics& diagnostics) {
  CliOptions options;
  bool saw_out_dir = false;

  for (int index = 1; index < args.size(); ++index) {
    const QString arg = args.at(index);
    const auto take_value = [&](QString* target) {
      if (index + 1 >= args.size()) {
        UsageError(diagnostics,
                   QStringLiteral("option '%1' requires a value").arg(arg));
        return false;
      }
      *target = args.at(++index);
      return true;
    };

    if (arg == QStringLiteral("--role")) {
      if (!take_value(&options.role)) return std::nullopt;
    } else if (arg == QStringLiteral("--out-dir")) {
      if (!take_value(&options.out_dir)) return std::nullopt;
      saw_out_dir = true;
    } else if (arg == QStringLiteral("--namespace")) {
      if (!take_value(&options.name_space)) return std::nullopt;
    } else if (arg == QStringLiteral("-I") || arg == QStringLiteral("--include-dir")) {
      QString include_dir;
      if (!take_value(&include_dir)) return std::nullopt;
      options.include_dirs.append(include_dir);
    } else if (arg == QStringLiteral("--depfile")) {
      if (!take_value(&options.depfile)) return std::nullopt;
    } else if (arg.startsWith(QLatin1Char('-'))) {
      UsageError(diagnostics, QStringLiteral("unknown option '%1'").arg(arg));
      return std::nullopt;
    } else {
      options.roots.append(arg);
    }
  }

  if (!saw_out_dir || options.out_dir.isEmpty()) {
    UsageError(diagnostics, QStringLiteral("missing required option --out-dir"));
    return std::nullopt;
  }
  if (options.roots.isEmpty()) {
    UsageError(diagnostics, QStringLiteral("at least one root .idl file is required"));
    return std::nullopt;
  }
  for (const QString& root : options.roots) {
    if (!root.isEmpty()) continue;
    UsageError(diagnostics, QStringLiteral("root .idl path must not be empty"));
    return std::nullopt;
  }
  if (!IsKnownRole(options.role)) {
    UsageError(diagnostics,
               QStringLiteral("invalid role '%1' (expected types, service, client, or all)")
                   .arg(options.role));
    return std::nullopt;
  }
  return options;
}

[[nodiscard]] bool WriteTemporary(const GeneratedFile& file, Diagnostics& diagnostics) {
  const QString temporary = file.target + QStringLiteral(".tmp");
  QFile output(temporary);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    diagnostics.Error(file.target, SourceLocation{},
                      QStringLiteral("cannot open temporary output '%1': %2")
                          .arg(temporary, output.errorString()));
    return false;
  }
  const qint64 written = output.write(file.contents);
  const bool write_ok = written == file.contents.size() && output.flush();
  const QString error = output.errorString();
  const QFileDevice::FileError error_code = output.error();
  output.close();
  if (!write_ok || error_code != QFileDevice::NoError) {
    diagnostics.Error(file.target, SourceLocation{},
                      QStringLiteral("cannot write temporary output '%1': %2")
                          .arg(temporary, error));
    return false;
  }
  return true;
}

void RemoveTemporaries(const QVector<GeneratedFile>& files, int from) {
  for (int index = from; index < files.size(); ++index) {
    QFile::remove(files.at(index).target + QStringLiteral(".tmp"));
  }
}

[[nodiscard]] bool ReplaceWithTemporary(const QString& target,
                                        Diagnostics& diagnostics) {
  const QString temporary = target + QStringLiteral(".tmp");
  // POSIX rename replaces the target atomically. On platforms where QFile
  // refuses to overwrite, fall back to removing the old file first.
  if (QFile::rename(temporary, target)) return true;
  if (!QFile::exists(target) ||
      (!QFile::remove(target) || !QFile::rename(temporary, target))) {
    diagnostics.Error(target, SourceLocation{},
                      QStringLiteral("cannot replace output '%1' with temporary file '%2'")
                          .arg(target, temporary));
    return false;
  }
  return true;
}

[[nodiscard]] QString MakeToken(const QString& path) {
  QString token = QFileInfo(path).absoluteFilePath();
  token.replace(QLatin1Char(' '), QStringLiteral("\\ "));
  token.replace(QLatin1Char('#'), QStringLiteral("\\#"));
  token.replace(QLatin1Char('$'), QStringLiteral("$$"));
  return token;
}

[[nodiscard]] QString DepfileContents(const QVector<GeneratedFile>& generated,
                                      const LoadedProgram& program) {
  QStringList targets;
  targets.reserve(generated.size());
  QStringList inputs;
  inputs.reserve(program.files.size());
  for (const GeneratedFile& file : generated) targets.append(MakeToken(file.target));
  for (const ImportedFile& file : program.files) inputs.append(MakeToken(file.canonical_path));
  return targets.join(QLatin1Char(' ')) + QStringLiteral(": ") +
         inputs.join(QLatin1Char(' ')) + QLatin1Char('\n');
}

}  // namespace

std::optional<CliOptions> ParseCli(int argc, char** argv, Diagnostics& diagnostics) {
  QStringList args;
  args.reserve(argc);
  for (int index = 0; index < argc; ++index) {
    args.append(QString::fromLocal8Bit(argv[index]));
  }
  return ParseArgs(args, diagnostics);
}

int RunCli(const QStringList& args) {
  Diagnostics diagnostics;
  const std::optional<CliOptions> options = ParseArgs(args, diagnostics);
  if (!options.has_value()) {
    PrintDiagnostics(diagnostics);
    PrintUsage();
    return 2;
  }

  // Merge every root's loaded program by canonical path. A shared import is
  // analyzed once even when several roots reach it.
  LoadedProgram program;
  QSet<QString> seen;
  for (const QString& root : options->roots) {
    ImportResolver resolver(options->include_dirs, diagnostics);
    auto loaded = resolver.Load(root);
    if (!loaded.has_value()) {
      PrintDiagnostics(diagnostics);
      return 1;
    }
    for (ImportedFile& file : loaded->files) {
      if (seen.contains(file.canonical_path)) continue;
      seen.insert(file.canonical_path);
      program.files.append(std::move(file));
    }
  }

  auto model = Analyze(program, options->name_space, diagnostics);
  if (!model.has_value()) {
    PrintDiagnostics(diagnostics);
    return 1;
  }

  QDir output_dir(options->out_dir);
  if (!QDir().mkpath(options->out_dir)) {
    diagnostics.Error(options->out_dir, SourceLocation{},
                      QStringLiteral("cannot create output directory '%1'")
                          .arg(options->out_dir));
    PrintDiagnostics(diagnostics);
    return 1;
  }

  const bool emit_service = options->role == QStringLiteral("service") ||
                            options->role == QStringLiteral("all");
  const bool emit_client = options->role == QStringLiteral("client") ||
                           options->role == QStringLiteral("all");

  QVector<GeneratedFile> generated;
  generated.reserve(model->files.size() * 5);
  const auto add_output = [&generated, &output_dir](const QString& name,
                                                    const QString& contents) {
    GeneratedFile output;
    output.target = output_dir.filePath(name);
    output.contents = contents.toUtf8();
    generated.append(std::move(output));
  };
  for (const FileModel& file : model->files) {
    // Types headers are always emitted: service/client translation units
    // include them, and CMake predicts them as outputs for every role.
    add_output(file.stem + QStringLiteral(".ddsrpc.types.h"),
               EmitTypesHeader(file, model->name_space));
    if (emit_service) {
      add_output(file.stem + QStringLiteral(".ddsrpc.service.h"),
                 EmitServiceHeader(file, model->name_space));
      add_output(file.stem + QStringLiteral(".ddsrpc.service.cpp"),
                 EmitServiceSource(file, model->name_space));
    }
    if (emit_client) {
      add_output(file.stem + QStringLiteral(".ddsrpc.client.h"),
                 EmitClientHeader(file, model->name_space));
      add_output(file.stem + QStringLiteral(".ddsrpc.client.cpp"),
                 EmitClientSource(file, model->name_space));
    }
  }

  QString depfile = options->depfile;
  if (depfile.isEmpty()) depfile = output_dir.filePath(QStringLiteral("ddsrpc.d"));
  const QString depfile_target = QFileInfo(depfile).absoluteFilePath();
  if (!QDir().mkpath(QFileInfo(depfile_target).absolutePath())) {
    diagnostics.Error(depfile_target, SourceLocation{},
                      QStringLiteral("cannot create depfile directory for '%1'")
                          .arg(depfile_target));
    PrintDiagnostics(diagnostics);
    return 1;
  }
  for (const GeneratedFile& file : generated) {
    if (QFileInfo(file.target).absoluteFilePath() != depfile_target) continue;
    diagnostics.Error(depfile_target, SourceLocation{},
                      QStringLiteral("depfile path collides with a generated output"));
    PrintDiagnostics(diagnostics);
    return 1;
  }
  GeneratedFile depfile_output{depfile_target, DepfileContents(generated, program).toUtf8()};
  generated.append(std::move(depfile_output));

  for (int index = 0; index < generated.size(); ++index) {
    if (WriteTemporary(generated.at(index), diagnostics)) continue;
    RemoveTemporaries(generated, 0);
    PrintDiagnostics(diagnostics);
    return 1;
  }
  for (int index = 0; index < generated.size(); ++index) {
    if (ReplaceWithTemporary(generated.at(index).target, diagnostics)) continue;
    RemoveTemporaries(generated, index);
    PrintDiagnostics(diagnostics);
    return 1;
  }
  return 0;
}

}  // namespace ddsrpcgen
