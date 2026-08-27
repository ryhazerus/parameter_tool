#include "commands.hpp"

#include "error.hpp"
#include "pool.hpp"
#include "scan.hpp"

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace pt {
namespace {

// Removes the staging directory on the way out, including when an exception is unwinding, so a
// failed extract never leaves a dot-directory sitting in the user's output folder.
struct TempDirGuard {
    fs::path path;
    ~TempDirGuard() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDirGuard() = default;
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
};

std::string Label(const Snapshot& s) {
    std::string out = "snapshot " + std::to_string(s.id);
    if (!s.name.empty()) out += " \"" + s.name + "\"";
    return out;
}

// True if `child` is `ancestor` or sits underneath it. Both must already be normalized.
bool IsAtOrUnder(const fs::path& child, const fs::path& ancestor) {
    auto c = child.begin();
    auto a = ancestor.begin();
    for (; a != ancestor.end(); ++a, ++c) {
        if (c == child.end() || *c != *a) return false;
    }
    return true;
}

}  // namespace

int CmdExtract(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);

    const Snapshot to = store.GetSnapshot(ResolveSnapshotRef(store, opt.id));

    // The baseline is the operator's starting snapshot: the oldest one in the store. A restore's
    // automatic safety snapshot can never be the oldest, because restore needs a snapshot to
    // already exist — so the [auto] checkpoints taken while testing never become the baseline.
    const int base_id = opt.since.empty() ? store.AllSnapshotIds().front()
                                          : ResolveSnapshotRef(store, opt.since);

    if (base_id == to.id) {
        if (opt.since.empty()) {
            std::printf("snapshot %d is the oldest snapshot in this store, so there is nothing to "
                        "compare it against\n  pass --since <id> to use a different baseline\n",
                        to.id);
        } else {
            std::printf("--since and --version both name snapshot %d; nothing to extract\n", to.id);
        }
        return kExitOk;
    }

    const Snapshot from = store.GetSnapshot(base_id);

    // Two snapshots taken over different file sets do not describe the same domain, so their
    // difference lists files that were never really added or removed. Worth saying out loud.
    if (from.scope != to.scope || from.pattern != to.pattern) {
        std::fprintf(stderr,
                     "warning: snapshot %d covers %s but snapshot %d covers %s; the comparison "
                     "spans two different file sets\n",
                     from.id, from.pattern.c_str(), to.id, to.pattern.c_str());
    }

    const std::vector<FileChange> changes = CompareEntries(from.files, to.files);

    std::vector<const FileEntry*> to_write;
    std::size_t removed = 0;
    for (const auto& c : changes) {
        if (c.kind == 'R') ++removed;
        else to_write.push_back(c.to);
    }

    // Held back until the output directory has been vetted, so a bad --out fails as a clean
    // one-line error instead of trailing a full change list.
    const auto print_changes = [&] {
        std::printf("%s -> %s\n", Label(from).c_str(), Label(to).c_str());
        for (const auto& c : changes) {
            if (c.kind == 'R') {
                std::printf("  R  %s   (removed; nothing extracted)\n", c.path.c_str());
            } else if (opt.verbose && c.kind == 'M') {
                std::printf("  M  %s  (%s -> %s)\n", c.path.c_str(),
                            FormatSize(c.from->size).c_str(), FormatSize(c.to->size).c_str());
            } else {
                std::printf("  %c  %s\n", c.kind, c.path.c_str());
            }
        }
    };

    if (to_write.empty()) {
        print_changes();
        std::printf("nothing to extract: no files were added or modified between snapshot %d and "
                    "snapshot %d\n", from.id, to.id);
        return kExitOk;
    }

    // Where to write. Resolved for filesystem work, but reported back the way it was asked for.
    const std::string display = opt.out.empty() ? "./extract-" + std::to_string(to.id) : opt.out;
    std::error_code ec;
    fs::path out_dir = fs::weakly_canonical(fs::path(display), ec);
    if (ec) out_dir = fs::absolute(fs::path(display));

    for (const auto& part : out_dir) {
        if (part == kStoreDirName) {
            Fail("refusing to extract into the snapshot store: " + display);
        }
    }
    if (out_dir == target.root) {
        Fail("--out is the configuration directory itself\n  to write these files back into the "
             "tree use `parametertool restore --version " + std::to_string(to.id) +
             "`, which takes a safety snapshot first");
    }
    if (IsAtOrUnder(out_dir, target.root)) {
        std::fprintf(stderr, "warning: %s is inside the configuration directory, so the next "
                     "snapshot will capture the extracted copies too\n", display.c_str());
    }

    if (fs::exists(out_dir, ec)) {
        if (!fs::is_directory(out_dir, ec)) {
            Fail(display + " exists and is not a directory");
        }
        if (!fs::is_empty(out_dir, ec) && !opt.force) {
            Fail(display + " is not empty\n  pass --force to write into it anyway, or choose "
                 "another --out");
        }
    }

    // Fail before creating anything if the store cannot supply a file this extract needs.
    for (const auto* e : to_write) {
        if (!store.HasObject(e->hash)) {
            Fail("snapshot " + std::to_string(to.id) + " needs object " + e->hash.substr(0, 12) +
                 " for \"" + e->path + "\", but it is missing from the store\n  run `parametertool "
                 "verify` to assess the damage");
        }
    }

    print_changes();

    if (opt.dry_run) {
        std::printf("(dry run: %zu file%s would be written to %s)\n", to_write.size(),
                    to_write.size() == 1 ? "" : "s", display.c_str());
        return kExitOk;
    }

    fs::create_directories(out_dir, ec);
    if (ec) Fail("could not create " + display + ": " + ec.message());

    // WriteFileAtomic renames out of its temp directory, which only works within one filesystem.
    // The store's tmp/ is no good here: an extract commonly targets a USB stick or a share, so
    // stage inside the output directory instead.
    TempDirGuard tmp;
    tmp.path = out_dir / ".paramtool-extract-tmp";
    fs::create_directories(tmp.path, ec);
    if (ec) Fail("could not create a staging directory in " + display + ": " + ec.message());

    const unsigned workers = opt.workers ? opt.workers : DefaultWorkers();
    ParallelFor(to_write.size(), workers, [&](std::size_t i) {
        const FileEntry& e = *to_write[i];
        const fs::path abs = out_dir / e.path;

        std::error_code mk_ec;
        fs::create_directories(abs.parent_path(), mk_ec);

        const std::string data = store.GetObjectVerified(e.hash);
        WriteFileAtomic(abs, data, e.mode ? e.mode : 0644u, tmp.path);
        SetMtime(abs, e.mtime);
    });

    std::printf("extracted %zu file%s into %s", to_write.size(),
                to_write.size() == 1 ? "" : "s", display.c_str());
    if (removed > 0) {
        std::printf(" (%zu removed file%s not extracted)", removed, removed == 1 ? "" : "s");
    }
    std::printf("\n");
    return kExitOk;
}

}  // namespace pt
