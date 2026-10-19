#pragma once
#include <filesystem>

namespace dumper {

// Writes every assembly's classes, fields (with offsets) and methods (with RVAs) to `out`.
// Returns the number of classes written.
size_t dump_types(const std::filesystem::path& out);

}  // namespace dumper
