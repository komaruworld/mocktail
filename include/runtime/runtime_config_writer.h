#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_WRITER_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_WRITER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// One `a.b.c` assignment. `value` is written verbatim; use EncodeConfigScalar
// for free-form text.
struct ConfigAssignment {
  std::vector<std::string> path;
  std::string value;
};

// Quotes only when a bare scalar would not round-trip.
std::string EncodeConfigScalar(std::string_view value);

// Rewrites the config line by line so comments, key order, and untouched
// values survive; a YAML round-trip would discard every comment in it. A
// missing key is inserted after its commented-out template when one exists,
// otherwise at the end of its parent block.
//
// Fails instead of overwriting a key that opens a nested block: `device` may
// be a preset string or a mapping, and flattening it would drop a custom
// device profile.
bool ApplyConfigAssignments(std::string_view yaml,
                            const std::vector<ConfigAssignment>& assignments,
                            std::string* updated, std::string* error);

// Reads, rewrites, and atomically replaces the file at mode 0600. Refuses a
// symlinked target.
bool WriteConfigAssignments(const std::filesystem::path& config_file,
                            const std::vector<ConfigAssignment>& assignments,
                            std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_WRITER_H_
