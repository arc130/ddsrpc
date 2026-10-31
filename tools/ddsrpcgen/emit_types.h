#pragma once
#include <QString>

#include "model.h"

namespace ddsrpcgen {

// Renders the self-contained types header for one analyzed IDL file. The
// output is deterministic, UTF-8/LF friendly and ends with a newline.
[[nodiscard]] QString EmitTypesHeader(const FileModel& file, const QString& name_space);

}  // namespace ddsrpcgen
