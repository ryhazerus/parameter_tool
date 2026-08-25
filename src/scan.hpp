#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pt {

inline constexpr const char* kStoreDirName = ".paramsnap";
inline constexpr const char* kDefaultPattern = "parameter_*.xml";

// Wildcard match of `name` against `pattern`, supporting `*` (any run, including empty) and `?`
// (exactly one character). Case-sensitive. Matching is against a bare filename, not a path, so
// `*` never has to reason about separators.
bool GlobMatch(std::string_view pattern, std::string_view name);

struct ScanResult {
    // Paths relative to the scan root, '/'-separated, sorted for determinism.
    std::vector<std::string> files;
    // Non-fatal problems (unreadable subdirectory, skipped symlink). Reported, never silent.
    std::vector<std::string> warnings;
};

// Walks `root` collecting files whose *filename* matches `pattern`.
//
// Always skipped: the `.paramsnap` store, any other dot-directory (`.git`), dotfiles, and symlinks
// (following them could leave the tree or loop; a restore would replace the link with a regular
// file, which is never what someone snapshotting a config tree wants).
ScanResult ScanTree(const std::filesystem::path& root, std::string_view pattern, bool recursive);

// Converts a path to the '/'-separated relative form used in manifests.
std::string ToRelPosix(const std::filesystem::path& p);

}  // namespace pt
