#pragma once
#include <QString>

#include "model.h"

namespace ddsrpcgen {

// Renders the QObject client facade for one analyzed IDL file. Files without
// classes still produce a self-contained header/source pair so that CMake can
// predict the output set for every role.
[[nodiscard]] QString EmitClientHeader(const FileModel& file,
                                       const QString& name_space);
[[nodiscard]] QString EmitClientSource(const FileModel& file,
                                       const QString& name_space);

}  // namespace ddsrpcgen
