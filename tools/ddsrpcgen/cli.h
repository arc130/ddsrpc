#pragma once
#include <optional>

#include <QString>
#include <QStringList>

#include "diagnostic.h"

namespace ddsrpcgen {

struct CliOptions {
  QString role = QStringLiteral("all");
  QString out_dir;
  QString name_space;
  QStringList include_dirs;
  QString depfile;
  QStringList roots;
};

// Parses argv[1..argc-1]. Usage errors are appended to diagnostics and make
// the function return nullopt; the caller maps that to exit code 2.
[[nodiscard]] std::optional<CliOptions> ParseCli(int argc, char** argv,
                                                 Diagnostics& diagnostics);

// Runs the generator with ddsrpcgen-style arguments (args[0] is the program
// name). Returns 0 on success, 1 on a diagnostic, and 2 on a usage error.
int RunCli(const QStringList& args);

}  // namespace ddsrpcgen
