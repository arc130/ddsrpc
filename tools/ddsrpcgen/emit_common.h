#pragma once
#include <QString>
#include <QStringList>

#include "model.h"

namespace ddsrpcgen {

// Shared helpers for the service/client emitters. Output names and include
// guards must match the types emitter so CMake can predict the file set.
[[nodiscard]] QString StemOf(const FileModel& file);
[[nodiscard]] QString SanitizeStem(const QString& stem);
[[nodiscard]] QString MakeIncludeGuard(const QString& kind, const QString& stem);
[[nodiscard]] QStringList NamespaceParts(const QString& name_space);
[[nodiscard]] QString SchemaHashLiteral(quint64 hash);

}  // namespace ddsrpcgen
