#pragma once

#include "manifest.hpp"
#include "scan.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pt {

// Holds the store's exclusive lock for its lifetime. Move-only; releases on destruction, so an
// exception unwinding out of a command still frees the lock.
class StoreLock {
public:
    StoreLock() = default;
    explicit StoreLock(std::filesystem::path path) : path_(std::move(path)), held_(true) {}
    ~StoreLock();

    StoreLock(StoreLock&& o) noexcept : path_(std::move(o.path_)), held_(o.held_) { o.held_ = false; }
    StoreLock& operator=(StoreLock&& o) noexcept;
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;

private:
    std::filesystem::path path_;
    bool held_ = false;
};

struct FileStat {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::uint32_t mode = 0;
};

// The `.paramsnap` directory inside a config tree: a content-addressed object database plus a set
// of snapshot manifests.
class Store {
public:
    // Opens the store under `root`. Creates it when `create` is set; otherwise throws if absent.
    static Store Open(const std::filesystem::path& root, bool create);

    // True if `root` already has a store, without creating one.
    static bool Exists(const std::filesystem::path& root);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path dir() const { return root_ / kStoreDirName; }
    std::filesystem::path tmp_dir() const { return dir() / "tmp"; }

    // Blocks nothing: either takes the lock or throws Error(kExitLocked). A lock whose owning
    // process is gone is reclaimed automatically.
    StoreLock AcquireLock() const;

    std::filesystem::path ObjectPath(std::string_view hash) const;
    bool HasObject(std::string_view hash) const;

    // Writes `data` under `hash` unless already present. Atomic and safe to call concurrently
    // from worker threads. Returns true if a new object was created.
    bool PutObject(std::string_view hash, std::string_view data) const;

    // Reads an object back without checking its contents. Throws if missing.
    std::string GetObject(std::string_view hash) const;

    // Reads an object and re-hashes it, throwing if the bytes no longer match the name. Used on
    // every restore: writing silently-corrupted data into a live configuration tree is the one
    // failure this tool must never produce.
    std::string GetObjectVerified(std::string_view hash) const;

    std::vector<std::string> AllObjectHashes() const;

    // Next snapshot id, persisted. Must be called while holding the lock.
    int AllocateId() const;

    void PutSnapshot(const Snapshot& s) const;
    Snapshot GetSnapshot(int id) const;
    bool HasSnapshot(int id) const;
    void RemoveSnapshot(int id) const;

    // Ascending snapshot ids.
    std::vector<int> AllSnapshotIds() const;

    // Every snapshot, ascending by id.
    std::vector<Snapshot> AllSnapshots() const;

    std::filesystem::path SnapshotPath(int id) const;

private:
    explicit Store(std::filesystem::path root) : root_(std::move(root)) {}

    std::filesystem::path root_;
};

// --- filesystem helpers shared by the commands ---

std::string ReadFileFully(const std::filesystem::path& p);

// Writes via a temporary in `tmp_dir` then renames into place, so a reader never sees a partial
// file and a crash leaves either the old contents or the new. `tmp_dir` must be on the same
// filesystem as `final_path`.
void WriteFileAtomic(const std::filesystem::path& final_path, std::string_view data,
                     std::uint32_t mode, const std::filesystem::path& tmp_dir);

FileStat StatFile(const std::filesystem::path& p);
void SetMtime(const std::filesystem::path& p, std::int64_t epoch);

// Hashes a file's contents. Files at or under kSlurpLimit are read once into `contents` (which the
// caller can then hand to PutObject); larger ones stream and leave `contents` empty, signalling
// that the data must be re-read.
inline constexpr std::uint64_t kSlurpLimit = 8ull * 1024 * 1024;
std::string HashFile(const std::filesystem::path& p, std::string* contents);

}  // namespace pt
