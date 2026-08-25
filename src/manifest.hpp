#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pt {

// What a snapshot covers. This drives how far a restore is allowed to reach.
enum class Scope {
    Tree,  // a directory: exact restore may delete, but only files matching `pattern`
    File,  // a single file: restore writes that one file and never deletes anything
};

struct FileEntry {
    std::string path;        // relative to the store root, '/'-separated
    std::string hash;        // sha256 hex of the contents
    std::uint64_t size = 0;
    std::int64_t mtime = 0;  // unix seconds
    std::uint32_t mode = 0;  // permission bits (0644 etc.)
};

struct Snapshot {
    int id = 0;
    std::string name;             // user label, may be empty
    std::string created_utc;      // ISO-8601, for humans
    std::int64_t created_epoch = 0;
    Scope scope = Scope::Tree;
    std::string pattern;          // the glob this snapshot was taken with
    bool recursive = true;
    bool automatic = false;       // taken by the tool before a restore
    std::vector<FileEntry> files; // sorted by path

    const FileEntry* Find(const std::string& path) const;
    std::uint64_t TotalSize() const;
};

std::string SerializeSnapshot(const Snapshot& s);

// Parses a manifest. Throws pt::Error with a message naming the problem if the document is not a
// well-formed manifest, so a hand-edited or truncated file fails loudly.
Snapshot ParseSnapshot(const std::string& text, const std::string& origin);

// "2026-08-25T09:50:12Z" for a unix timestamp.
std::string FormatUtc(std::int64_t epoch);

// Human-friendly byte count: "1.2 MB".
std::string FormatSize(std::uint64_t bytes);

}  // namespace pt
