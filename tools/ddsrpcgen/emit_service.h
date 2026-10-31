#pragma once
#include <QString>

#include "model.h"

namespace ddsrpcgen {

// Renders the service facade for one analyzed IDL file. Files without classes
// still produce a self-contained header/source pair so that CMake can predict
// the output set for every role.
[[nodiscard]] QString EmitServiceHeader(const FileModel& file,
                                        const QString& name_space);
[[nodiscard]] QString EmitServiceSource(const FileModel& file,
                                        const QString& name_space);

}  // namespace ddsrpcgen
