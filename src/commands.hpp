#pragma once

#include "cli.hpp"
#include "manifest.hpp"
#include "store.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace pt {

// What the user pointed at: either a directory to snapshot, or one file inside a directory.
struct Target {
    std::filesystem::path root;  // the directory holding (or to hold) .paramsnap
    std::string single_file;     // relative path of the one file, empty for a whole-tree target
    bool is_file() const { return !single_file.empty(); }
};

// Resolves opt.path, rejecting a path inside a .paramsnap store.
Target ResolveTarget(const Options& opt, bool allow_file);

// Resolves a user-supplied snapshot reference: a number ("3", "0003"), "latest", or a snapshot
// name. Numbers win over names when both could match. Throws with the available ids listed if
// nothing matches.
int ResolveSnapshotRef(const Store& store, const std::string& ref);

// The glob a command should use, honoring --pattern and --all.
std::string EffectivePattern(const Options& opt);

// One difference between two path-sorted entry lists.
struct FileChange {
    char kind = 'M';                 // 'M' modified, 'A' added, 'R' removed
    std::string path;
    const FileEntry* from = nullptr;  // the "before" side; null when kind == 'A'
    const FileEntry* to = nullptr;    // the "after" side; null when kind == 'R'
};

// Ordered merge of two lists that are already sorted by path. The returned pointers alias `from`
// and `to`, which must outlive the result.
std::vector<FileChange> CompareEntries(const std::vector<FileEntry>& from,
                                       const std::vector<FileEntry>& to);

struct SnapshotResult {
    Snapshot snapshot;
    std::size_t new_objects = 0;   // objects actually written; the rest were already stored
    std::vector<std::string> warnings;
};

// Builds a snapshot of the current state of `target` and writes it to the store.
// Caller must hold the store lock.
SnapshotResult TakeSnapshot(const Store& store, const Target& target, const Options& opt,
                            const std::string& name, bool automatic);

// Hashes the current on-disk state matching `pattern`, writing nothing to the store. Used by
// `diff` and by `restore` to skip files that already have the right contents. Sorted by path.
std::vector<FileEntry> ScanLive(const std::filesystem::path& root, const std::string& pattern,
                                bool recursive, unsigned workers,
                                std::vector<std::string>* warnings);

// One line per snapshot, as used by `list` and after `snap`.
std::string FormatSnapshotLine(const Snapshot& s, bool verbose);

int CmdSnap(const Options& opt);
int CmdRestore(const Options& opt);
int CmdList(const Options& opt);
int CmdShow(const Options& opt);
int CmdDiff(const Options& opt);
int CmdExtract(const Options& opt);
int CmdDelete(const Options& opt);
int CmdCleanup(const Options& opt);
int CmdVerify(const Options& opt);
int CmdGc(const Options& opt);

}  // namespace pt
