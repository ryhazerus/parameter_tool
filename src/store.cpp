#include "store.hpp"

#include "error.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace pt {
namespace {

constexpr int kStoreVersion = 1;

std::string Errno(const char* what, const fs::path& p) {
    return std::string(what) + " " + p.string() + ": " + std::strerror(errno);
}

// Unique enough across processes and threads without needing randomness.
fs::path MakeTempPath(const fs::path& tmp_dir) {
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "tmp-%ld-%llu", static_cast<long>(::getpid()), n);
    return tmp_dir / buf;
}

void EnsureDir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec && !fs::is_directory(p)) {
        Fail("cannot create directory " + p.string() + ": " + ec.message());
    }
}

bool IsHexHash(std::string_view h) {
    if (h.size() != 64) return false;
    for (char c : h) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

}  // namespace

// --- StoreLock ---

StoreLock::~StoreLock() {
    if (held_) {
        std::error_code ec;
        fs::remove(path_, ec);  // best effort: a failure here only leaves a stale lock behind
    }
}

StoreLock& StoreLock::operator=(StoreLock&& o) noexcept {
    if (this != &o) {
        if (held_) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
        path_ = std::move(o.path_);
        held_ = o.held_;
        o.held_ = false;
    }
    return *this;
}

// --- Store ---

bool Store::Exists(const fs::path& root) {
    std::error_code ec;
    return fs::is_directory(root / kStoreDirName / "objects", ec);
}

Store Store::Open(const fs::path& root, bool create) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        Fail("not a directory: " + root.string());
    }

    Store s(fs::absolute(root, ec));
    if (ec) s = Store(root);

    const fs::path store = s.dir();
    const fs::path version_file = store / "version";

    if (!fs::exists(store, ec)) {
        if (!create) {
            Fail("no snapshot store in " + root.string() +
                 "\n  run `parametertool snap` there first to create one");
        }
        EnsureDir(store / "objects");
        EnsureDir(store / "snapshots");
        EnsureDir(store / "tmp");
        WriteFileAtomic(version_file, std::to_string(kStoreVersion) + "\n", 0644, store / "tmp");
        return s;
    }

    // Existing store: make sure it is one we understand before touching anything.
    if (fs::exists(version_file, ec)) {
        const std::string text = ReadFileFully(version_file);
        const int v = std::atoi(text.c_str());
        if (v > kStoreVersion) {
            Fail("snapshot store in " + root.string() + " is format version " + std::to_string(v) +
                 ", but this parametertool only understands version " +
                 std::to_string(kStoreVersion));
        }
    }
    // Recreate incidental subdirectories (an empty tmp/ can be pruned by cleanup tools).
    EnsureDir(store / "objects");
    EnsureDir(store / "snapshots");
    EnsureDir(store / "tmp");
    return s;
}

StoreLock Store::AcquireLock() const {
    const fs::path lock_path = dir() / "lock";

    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = ::open(lock_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            const std::string pid = std::to_string(static_cast<long>(::getpid())) + "\n";
            const ssize_t n = ::write(fd, pid.data(), pid.size());
            (void)n;
            ::close(fd);
            return StoreLock(lock_path);
        }
        if (errno != EEXIST) {
            Fail(Errno("cannot create lock file", lock_path));
        }

        // Someone holds it — or did, and died. Reclaim only if the owner is provably gone.
        std::string owner;
        try {
            owner = ReadFileFully(lock_path);
        } catch (const Error&) {
            owner.clear();
        }
        const long owner_pid = std::atol(owner.c_str());
        const bool stale = owner_pid > 0 && ::kill(static_cast<pid_t>(owner_pid), 0) != 0 &&
                           errno == ESRCH;
        if (stale && attempt == 0) {
            std::error_code ec;
            fs::remove(lock_path, ec);
            continue;  // retry once
        }

        throw Error(kExitLocked,
                    "another parametertool is using this store (lock held by pid " +
                        std::to_string(owner_pid) + ")\n  if that process is gone, remove " +
                        lock_path.string());
    }
    Fail("could not acquire the store lock");
}

fs::path Store::ObjectPath(std::string_view hash) const {
    if (!IsHexHash(hash)) Fail("invalid object id: " + std::string(hash));
    return dir() / "objects" / std::string(hash.substr(0, 2)) / std::string(hash.substr(2));
}

bool Store::HasObject(std::string_view hash) const {
    std::error_code ec;
    return fs::is_regular_file(ObjectPath(hash), ec);
}

bool Store::PutObject(std::string_view hash, std::string_view data) const {
    const fs::path target = ObjectPath(hash);
    std::error_code ec;
    if (fs::is_regular_file(target, ec)) return false;  // dedup: identical content already stored

    EnsureDir(target.parent_path());
    // Two threads may race here with the same new hash. Each writes its own temp file and renames;
    // the contents are identical by construction, so whichever lands last is still correct.
    WriteFileAtomic(target, data, 0444, tmp_dir());
    return true;
}

std::string Store::GetObject(std::string_view hash) const {
    const fs::path p = ObjectPath(hash);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        Fail("object " + std::string(hash) + " is missing from the store\n  run `parametertool "
             "verify` to check for damage");
    }
    return ReadFileFully(p);
}

std::string Store::GetObjectVerified(std::string_view hash) const {
    std::string data = GetObject(hash);
    const std::string actual = Sha256Hex(data);
    if (actual != hash) {
        Fail("stored object " + std::string(hash).substr(0, 12) + " is corrupt (its contents hash "
             "to " + actual.substr(0, 12) + ")\n  refusing to restore damaged data; run "
             "`parametertool verify` to see the full extent");
    }
    return data;
}

std::vector<std::string> Store::AllObjectHashes() const {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path objects = dir() / "objects";
    for (const auto& shard : fs::directory_iterator(objects, ec)) {
        if (!shard.is_directory(ec)) continue;
        const std::string prefix = shard.path().filename().string();
        if (prefix.size() != 2) continue;
        std::error_code ec2;
        for (const auto& f : fs::directory_iterator(shard.path(), ec2)) {
            if (!f.is_regular_file(ec2)) continue;
            const std::string hash = prefix + f.path().filename().string();
            if (IsHexHash(hash)) out.push_back(hash);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int Store::AllocateId() const {
    const fs::path counter = dir() / "next_id";
    int next = 1;

    std::error_code ec;
    if (fs::is_regular_file(counter, ec)) {
        next = std::atoi(ReadFileFully(counter).c_str());
        if (next < 1) next = 1;
    }

    // Never hand out an id whose manifest already exists, even if the counter was lost or reset.
    for (int id : AllSnapshotIds()) {
        if (id >= next) next = id + 1;
    }

    WriteFileAtomic(counter, std::to_string(next + 1) + "\n", 0644, tmp_dir());
    return next;
}

fs::path Store::SnapshotPath(int id) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d.json", id);
    return dir() / "snapshots" / buf;
}

void Store::PutSnapshot(const Snapshot& s) const {
    WriteFileAtomic(SnapshotPath(s.id), SerializeSnapshot(s), 0644, tmp_dir());
}

bool Store::HasSnapshot(int id) const {
    std::error_code ec;
    return fs::is_regular_file(SnapshotPath(id), ec);
}

Snapshot Store::GetSnapshot(int id) const {
    const fs::path p = SnapshotPath(id);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) Fail("no snapshot with id " + std::to_string(id));
    return ParseSnapshot(ReadFileFully(p), p.filename().string());
}

void Store::RemoveSnapshot(int id) const {
    std::error_code ec;
    if (!fs::remove(SnapshotPath(id), ec)) {
        Fail("cannot delete snapshot " + std::to_string(id) + ": " + ec.message());
    }
}

std::vector<int> Store::AllSnapshotIds() const {
    std::vector<int> ids;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir() / "snapshots", ec)) {
        if (!e.is_regular_file(ec)) continue;
        const fs::path& p = e.path();
        if (p.extension() != ".json") continue;
        const std::string stem = p.stem().string();
        if (stem.empty() || !std::all_of(stem.begin(), stem.end(),
                                         [](unsigned char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        ids.push_back(std::atoi(stem.c_str()));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<Snapshot> Store::AllSnapshots() const {
    std::vector<Snapshot> out;
    for (int id : AllSnapshotIds()) out.push_back(GetSnapshot(id));
    return out;
}

// --- filesystem helpers ---

std::string ReadFileFully(const fs::path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) Fail(Errno("cannot open", p));

    std::string out;
    char buf[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string msg = Errno("cannot read", p);
            ::close(fd);
            Fail(msg);
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

void WriteFileAtomic(const fs::path& final_path, std::string_view data, std::uint32_t mode,
                     const fs::path& tmp_dir) {
    EnsureDir(tmp_dir);
    const fs::path tmp = MakeTempPath(tmp_dir);

    // 0600 while we write; the requested mode is applied just before the rename.
    const int fd = ::open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) Fail(Errno("cannot create temporary file", tmp));

    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string msg = Errno("cannot write", tmp);
            ::close(fd);
            std::error_code ec;
            fs::remove(tmp, ec);
            Fail(msg);
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }

    // Flush before the rename so a crash can't leave a named file with unwritten contents.
    if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
        const std::string msg = Errno("cannot flush", tmp);
        ::close(fd);
        std::error_code ec;
        fs::remove(tmp, ec);
        Fail(msg);
    }
    if (::fchmod(fd, static_cast<mode_t>(mode)) != 0) {
        // Not fatal: the content is what matters, permissions are cosmetic here.
    }
    if (::close(fd) != 0) {
        const std::string msg = Errno("cannot close", tmp);
        std::error_code ec;
        fs::remove(tmp, ec);
        Fail(msg);
    }

    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        const std::string msg = Errno("cannot install", final_path);
        std::error_code ec;
        fs::remove(tmp, ec);
        Fail(msg);
    }
}

FileStat StatFile(const fs::path& p) {
    struct ::stat st {};
    if (::stat(p.c_str(), &st) != 0) Fail(Errno("cannot stat", p));
    FileStat out;
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.mtime = static_cast<std::int64_t>(st.st_mtime);
    out.mode = static_cast<std::uint32_t>(st.st_mode & 07777);
    return out;
}

void SetMtime(const fs::path& p, std::int64_t epoch) {
    struct ::timespec times[2];
    times[0].tv_sec = static_cast<time_t>(epoch);  // atime
    times[0].tv_nsec = 0;
    times[1].tv_sec = static_cast<time_t>(epoch);  // mtime
    times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, p.c_str(), times, 0);  // best effort
}

std::string HashFile(const fs::path& p, std::string* contents) {
    contents->clear();

    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) Fail(Errno("cannot open", p));

    struct ::stat st {};
    if (::fstat(fd, &st) != 0) {
        const std::string msg = Errno("cannot stat", p);
        ::close(fd);
        Fail(msg);
    }
    const bool slurp = static_cast<std::uint64_t>(st.st_size) <= kSlurpLimit;
    if (slurp) contents->reserve(static_cast<std::size_t>(st.st_size));

    Sha256 h;
    char buf[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string msg = Errno("cannot read", p);
            ::close(fd);
            Fail(msg);
        }
        if (n == 0) break;
        h.Update(buf, static_cast<std::size_t>(n));
        if (slurp) contents->append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);

    // A file that grew past the limit while we read it: drop the partial buffer so the caller
    // re-reads rather than storing something that doesn't match the hash.
    if (slurp && contents->size() > kSlurpLimit) contents->clear();
    return h.FinalHex();
}

}  // namespace pt
