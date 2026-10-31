#pragma once
#include <optional>

#include "diagnostic.h"
#include "import_resolver.h"
#include "model.h"

namespace ddsrpcgen {

// Validates all loaded IDL files, resolves types, assigns operation/event IDs,
// orders PODs by dependency and computes each file's schema hash. Every file in
// the loaded program shares name_space. On any new diagnostic the function
// returns nullopt; diagnostics already present before the call are ignored.
[[nodiscard]] std::optional<Model> Analyze(const LoadedProgram& program,
                                           const QString& name_space,
                                           Diagnostics& diagnostics);

}  // namespace ddsrpcgen
