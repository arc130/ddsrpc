#pragma once
#include <QString>
#include <QStringList>

#include "model.h"

namespace ddsrpcgen {

// Shared helpers for the service/client emitters. Output names and include
// guards must match the types emitter so CMake can predict the file set.
[[nodiscard]] QString StemOf(const FileModel& file);
// Escapes a stem for identifiers/include guards. Distinct stems must stay
// distinct ("foo-bar" vs "foo_bar"); the result may be empty for an empty
// stem, so callers choose their own fallback.
[[nodiscard]] QString EscapeStem(const QString& stem);
[[nodiscard]] QString SanitizeStem(const QString& stem);
[[nodiscard]] QString MakeIncludeGuard(const QString& kind, const QString& stem);
[[nodiscard]] QStringList NamespaceParts(const QString& name_space);
[[nodiscard]] QString SchemaHashLiteral(quint64 hash);
// Prints a resolved model type with global qualification. Builtin spellings
// (including multiword "unsigned long long") are used verbatim; everything
// else gets a leading "::" so an IDL member/parameter named like a type or
// namespace cannot shadow it in generated member functions. Template
// arguments are qualified recursively.
[[nodiscard]] QString QualifiedCppType(const QString& cpp_type);

}  // namespace ddsrpcgen
