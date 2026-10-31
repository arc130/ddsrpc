#pragma once
#include <QVector>

#include "model.h"

namespace ddsrpcgen {

// Computes the wire schema hash for one IDL file. Only the declarations in
// file and the type declarations (POD/ENUM/class ENUM models) of
// imported_files participate; file paths, absolute paths and the schemas of
// other files never do. The 64-bit value is the big-endian first eight bytes
// of the SHA-256 digest of a normalized, length-prefixed serialization.
[[nodiscard]] quint64 ComputeSchema(const FileModel& file,
                                    const QVector<FileModel>& imported_files);

}  // namespace ddsrpcgen
