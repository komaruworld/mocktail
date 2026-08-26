#include "runtime/runtime_config_writer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uintmax_t kMaximumConfigBytes = 1U * 1024U * 1024U;
constexpr std::size_t kIndentWidth = 2;
std::atomic<std::uint64_t> g_temporary_sequence{0};

struct Line {
  std::string text;
  // Set only for `key:` lines that are not commented out.
  std::optional<std::string> key;
  std::size_t indent = 0;
  bool blank = false;
  bool comment = false;
  // A `# key: value` template line, used to place a missing optional key.
  std::optional<std::string> commented_key;
  // Indentation the key would have once the `#` and one following space are
  // removed, so it compares directly against a block indent.
  std::size_t commented_indent = 0;
};

std::size_t CountIndent(std::string_view text) {
  std::size_t indent = 0;
  while (indent < text.size() && text[indent] == ' ') {
    ++indent;
  }
  return indent;
}

bool IsKeyCharacter(char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '_' ||
         character == '-';
}

// Returns the key when `text` starts, at `offset`, with `key:` followed by a
// value separator. List items and inline flow maps are deliberately not keys.
std::optional<std::string> ParseKey(std::string_view text, std::size_t offset) {
  std::size_t end = offset;
  while (end < text.size() && IsKeyCharacter(text[end])) {
    ++end;
  }
  if (end == offset || end >= text.size() || text[end] != ':') {
    return std::nullopt;
  }
  const std::size_t after = end + 1;
  if (after != text.size() && text[after] != ' ' && text[after] != '\t') {
    return std::nullopt;
  }
  return std::string(text.substr(offset, end - offset));
}

Line ClassifyLine(std::string text) {
  Line line;
  line.indent = CountIndent(text);
  const std::string_view view(text);
  if (line.indent == view.size()) {
    line.blank = true;
    line.text = std::move(text);
    return line;
  }
  if (view[line.indent] == '#') {
    line.comment = true;
    // `  # proxy_host: 127.0.0.1` and `#   type: mobile` both document a key
    // that would sit at indent 2 once uncommented.
    std::size_t offset = line.indent + 1;
    std::size_t spaces = 0;
    while (offset < view.size() && view[offset] == ' ') {
      ++offset;
      ++spaces;
    }
    if (auto key = ParseKey(view, offset)) {
      line.commented_key = std::move(key);
      line.commented_indent = offset - 1 - (spaces > 0 ? 1 : 0);
    }
    line.text = std::move(text);
    return line;
  }
  line.key = ParseKey(view, line.indent);
  line.text = std::move(text);
  return line;
}

std::vector<Line> SplitLines(std::string_view yaml, bool* trailing_newline) {
  std::vector<Line> lines;
  *trailing_newline = !yaml.empty() && yaml.back() == '\n';
  std::size_t start = 0;
  while (start <= yaml.size()) {
    const std::size_t end = yaml.find('\n', start);
    if (end == std::string_view::npos) {
      if (start < yaml.size()) {
        lines.push_back(ClassifyLine(std::string(yaml.substr(start))));
      }
      break;
    }
    lines.push_back(ClassifyLine(std::string(yaml.substr(start, end - start))));
    start = end + 1;
  }
  return lines;
}

std::string JoinLines(const std::vector<Line>& lines, bool trailing_newline) {
  std::string output;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    output += lines[index].text;
    if (index + 1 < lines.size() || trailing_newline) {
      output += '\n';
    }
  }
  return output;
}

// Half-open range of the block owned by the key line at `parent`, which is the
// whole document when `parent` is npos.
struct Block {
  std::size_t begin = 0;
  // Past the block's last real key line: where a plain append belongs.
  std::size_t end = 0;
  // Past the block's trailing comments too. Comments document the key below
  // them, so a section's `# key: value` templates sit after its last real key
  // and would otherwise fall outside the block entirely.
  std::size_t comment_end = 0;
  std::size_t indent = 0;
};

Block DocumentBlock(const std::vector<Line>& lines) {
  return {0, lines.size(), lines.size(), 0};
}

Block ChildBlock(const std::vector<Line>& lines, std::size_t parent) {
  Block block;
  block.begin = parent + 1;
  block.indent = lines[parent].indent + kIndentWidth;
  std::size_t end = block.begin;
  std::size_t comment_end = lines.size();
  for (std::size_t index = block.begin; index < lines.size(); ++index) {
    const Line& line = lines[index];
    if (line.blank || line.comment) {
      continue;
    }
    if (line.indent <= lines[parent].indent) {
      comment_end = index;
      break;
    }
    end = index + 1;
  }
  block.end = end;
  block.comment_end = comment_end;
  // An empty block still indents one level below its parent.
  if (block.end < block.begin) {
    block.end = block.begin;
  }
  if (block.comment_end < block.end) {
    block.comment_end = block.end;
  }
  return block;
}

std::optional<std::size_t> FindKey(const std::vector<Line>& lines,
                                   const Block& block,
                                   std::string_view key) {
  for (std::size_t index = block.begin; index < block.end; ++index) {
    const Line& line = lines[index];
    if (!line.key.has_value() || line.indent != block.indent) {
      continue;
    }
    if (*line.key == key) {
      return index;
    }
  }
  return std::nullopt;
}

// True when the key line carries no inline value, so the key owns a block.
bool OpensBlock(const std::vector<Line>& lines, std::size_t index) {
  const Line& line = lines[index];
  const std::size_t colon = line.text.find(':', line.indent);
  if (colon == std::string::npos) {
    return false;
  }
  for (std::size_t offset = colon + 1; offset < line.text.size(); ++offset) {
    const char character = line.text[offset];
    if (character == ' ' || character == '\t') {
      continue;
    }
    return character == '#';
  }
  return true;
}

// Rewrites the value while keeping indentation, the key, and any trailing
// comment that follows the value.
void ReplaceValue(Line* line, std::string_view value) {
  const std::size_t colon = line->text.find(':', line->indent);
  std::string trailing;
  std::size_t offset = colon + 1;
  while (offset < line->text.size() &&
         (line->text[offset] == ' ' || line->text[offset] == '\t')) {
    ++offset;
  }
  const std::size_t comment = line->text.find(" #", offset);
  if (comment != std::string::npos) {
    trailing = line->text.substr(comment);
  }
  line->text = line->text.substr(0, colon + 1) + " " + std::string(value) +
               trailing;
}

Line MakeKeyLine(std::size_t indent, std::string_view key,
                 std::string_view value) {
  return ClassifyLine(std::string(indent, ' ') + std::string(key) + ": " +
                      std::string(value));
}

// Prefers the documented slot: a `# key: ...` template at the block's indent.
std::size_t InsertionPoint(const std::vector<Line>& lines, const Block& block,
                           std::string_view key) {
  for (std::size_t index = block.begin; index < block.comment_end; ++index) {
    const Line& line = lines[index];
    if (line.commented_key.has_value() && *line.commented_key == key &&
        line.commented_indent == block.indent) {
      return index + 1;
    }
  }
  return block.end;
}

bool ApplyOne(std::vector<Line>* lines, const ConfigAssignment& assignment,
              std::string* error) {
  if (assignment.path.empty()) {
    *error = "config assignment needs a key path";
    return false;
  }
  Block block = DocumentBlock(*lines);
  for (std::size_t depth = 0; depth + 1 < assignment.path.size(); ++depth) {
    const std::string& segment = assignment.path[depth];
    const std::optional<std::size_t> parent = FindKey(*lines, block, segment);
    if (!parent.has_value()) {
      // Append the missing section, then continue descending into it.
      Line section = ClassifyLine(std::string(block.indent, ' ') + segment +
                                  ":");
      lines->insert(lines->begin() +
                        static_cast<std::ptrdiff_t>(block.end),
                    std::move(section));
      block = ChildBlock(*lines, block.end);
      continue;
    }
    if (!OpensBlock(*lines, *parent)) {
      *error = "config key '" + segment + "' holds a value, not a section";
      return false;
    }
    block = ChildBlock(*lines, *parent);
  }

  const std::string& leaf = assignment.path.back();
  const std::optional<std::size_t> existing = FindKey(*lines, block, leaf);
  if (existing.has_value()) {
    if (OpensBlock(*lines, *existing)) {
      *error = "config key '" + leaf +
               "' opens a section; refusing to replace it with a value";
      return false;
    }
    ReplaceValue(&(*lines)[*existing], assignment.value);
    return true;
  }
  const std::size_t insertion = InsertionPoint(*lines, block, leaf);
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(insertion),
                MakeKeyLine(block.indent, leaf, assignment.value));
  return true;
}

bool WriteAll(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

class ScopedFileDescriptor final {
 public:
  explicit ScopedFileDescriptor(int descriptor = -1)
      : descriptor_(descriptor) {}
  ~ScopedFileDescriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  int get() const { return descriptor_; }

 private:
  int descriptor_ = -1;
};

}  // namespace

std::string EncodeConfigScalar(std::string_view value) {
  const bool needs_quotes =
      value.empty() || value.front() == ' ' || value.back() == ' ' ||
      value.find(": ") != std::string_view::npos ||
      value.find(" #") != std::string_view::npos ||
      value.find('\n') != std::string_view::npos ||
      value.find('"') != std::string_view::npos ||
      std::string_view("#&*!|>%@`{}[],'\"").find(value.front()) !=
          std::string_view::npos;
  if (!needs_quotes) {
    return std::string(value);
  }
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      quoted += '\\';
    }
    if (character == '\n') {
      quoted += "\\n";
      continue;
    }
    quoted += character;
  }
  quoted += '"';
  return quoted;
}

bool ApplyConfigAssignments(std::string_view yaml,
                            const std::vector<ConfigAssignment>& assignments,
                            std::string* updated, std::string* error) {
  if (updated == nullptr || error == nullptr) {
    if (error != nullptr) {
      *error = "config writer output is required";
    }
    return false;
  }
  bool trailing_newline = true;
  std::vector<Line> lines = SplitLines(yaml, &trailing_newline);
  if (lines.empty()) {
    trailing_newline = true;
  }
  for (const ConfigAssignment& assignment : assignments) {
    if (!ApplyOne(&lines, assignment, error)) {
      return false;
    }
  }
  *updated = JoinLines(lines, trailing_newline);
  return true;
}

bool WriteConfigAssignments(const std::filesystem::path& config_file,
                            const std::vector<ConfigAssignment>& assignments,
                            std::string* error) {
  if (error == nullptr) {
    return false;
  }
  if (!config_file.is_absolute()) {
    *error = "config path must be absolute";
    return false;
  }
  std::error_code filesystem_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(config_file, filesystem_error);
  if (!filesystem_error && std::filesystem::is_symlink(status)) {
    *error = "config target is a symlink";
    return false;
  }

  std::string original;
  const int descriptor =
      open(config_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor >= 0) {
    const ScopedFileDescriptor file(descriptor);
    struct stat metadata{};
    if (fstat(file.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > kMaximumConfigBytes) {
      *error = "config is not a bounded regular file";
      return false;
    }
    original.resize(static_cast<std::size_t>(metadata.st_size));
    std::size_t offset = 0;
    while (offset < original.size()) {
      const ssize_t count =
          read(file.get(), original.data() + offset, original.size() - offset);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        *error = "cannot read config";
        return false;
      }
      if (count == 0) {
        *error = "config changed while being read";
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
  } else if (errno != ENOENT) {
    *error = "cannot safely open config";
    return false;
  }

  std::string updated;
  if (!ApplyConfigAssignments(original, assignments, &updated, error)) {
    return false;
  }
  if (updated == original) {
    return true;
  }

  if (!RuntimePaths::EnsureDirectory(config_file.parent_path(),
                                     &filesystem_error)) {
    *error = "cannot create config directory";
    return false;
  }
  const std::filesystem::path temporary =
      config_file.parent_path() /
      (".mocktail-config.tmp." + std::to_string(getpid()) + "." +
       std::to_string(g_temporary_sequence.fetch_add(1)));
  const int target =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (target < 0) {
    *error = "cannot create temporary config";
    return false;
  }
  bool stored = false;
  {
    const ScopedFileDescriptor file(target);
    stored = fchmod(file.get(), S_IRUSR | S_IWUSR) == 0 &&
             WriteAll(file.get(), updated) && fsync(file.get()) == 0;
  }
  if (!stored || rename(temporary.c_str(), config_file.c_str()) != 0) {
    const int saved_errno = errno;
    (void)unlink(temporary.c_str());
    errno = saved_errno;
    *error = "cannot atomically store config";
    return false;
  }
  const int directory =
      open(config_file.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory < 0) {
    *error = "cannot open config directory for sync";
    return false;
  }
  const ScopedFileDescriptor directory_file(directory);
  if (fsync(directory_file.get()) != 0) {
    *error = "cannot sync config directory";
    return false;
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
