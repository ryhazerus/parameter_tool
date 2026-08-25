#include "commands.hpp"

#include "error.hpp"
#include "pool.hpp"
#include "scan.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace pt {
namespace {

// Normalizes a user-supplied --file value to the '/'-separated form stored in manifests, and
// refuses anything that would reach outside the tree.
std::string NormalizeRelPath(const std::string& raw) {
    fs::path p(raw);
    if (p.is_absolute()) Fail("--file takes a path relative to the configuration directory");

    fs::path clean;
    for (const auto& part : p) {
        if (part == "." || part.empty()) continue;
        if (part == "..") Fail("--file must not contain \"..\"");
        clean /= part;
    }
    if (clean.empty()) Fail("--file needs a filename");
    return ToRelPosix(clean);
}

}  // namespace

int CmdRestore(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);

    const int id = ResolveSnapshotRef(store, opt.id);
    const Snapshot snap = store.GetSnapshot(id);

    // Which entries to write.
    std::vector<FileEntry> wanted = snap.files;
    if (!opt.file.empty()) {
        const std::string rel = NormalizeRelPath(opt.file);
        const FileEntry* e = snap.Find(rel);
        if (!e) {
            Fail("snapshot " + std::to_string(id) + " does not contain \"" + rel +
                 "\"\n  run `parametertool show " + std::to_string(id) + "` to see its contents");
        }
        wanted = {*e};
    }

    if (snap.scope == Scope::File && snap.files.empty()) {
        Fail("snapshot " + std::to_string(id) +
             " is a single-file snapshot but records no file; the manifest is damaged");
    }

    // Deleting is only ever on the table for a whole-tree restore of a tree-scope snapshot.
    const bool exact = !opt.merge && opt.file.empty() && snap.scope == Scope::Tree;

    // Current state, so unchanged files are left completely untouched.
    std::vector<std::string> warnings;
    std::vector<FileEntry> live;
    if (snap.scope == Scope::Tree) {
        live = ScanLive(target.root, snap.pattern, snap.recursive, opt.workers, &warnings);
    } else {
        // Single-file scope: the only path that matters is the recorded one.
        std::error_code ec;
        const fs::path abs = target.root / snap.files.front().path;
        if (fs::is_regular_file(abs, ec)) {
            std::string contents;
            FileEntry e;
            e.path = snap.files.front().path;
            e.hash = HashFile(abs, &contents);
            live.push_back(std::move(e));
        }
    }
    for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());

    std::set<std::string> live_paths;
    std::vector<FileEntry> to_write;
    for (const auto& e : live) live_paths.insert(e.path);

    for (const auto& e : wanted) {
        const auto it = std::lower_bound(
            live.begin(), live.end(), e.path,
            [](const FileEntry& l, const std::string& k) { return l.path < k; });
        if (it != live.end() && it->path == e.path && it->hash == e.hash) {
            continue;  // already byte-identical; leave its mtime alone
        }
        to_write.push_back(e);
    }

    std::vector<std::string> to_delete;
    if (exact) {
        for (const auto& l : live) {
            if (!snap.Find(l.path)) to_delete.push_back(l.path);
        }
        std::sort(to_delete.begin(), to_delete.end());
    }

    // Fail before touching anything if the store cannot supply a needed object.
    for (const auto& e : to_write) {
        if (!store.HasObject(e.hash)) {
            Fail("snapshot " + std::to_string(id) + " needs object " + e.hash.substr(0, 12) +
                 " for \"" + e.path + "\", but it is missing from the store\n  run `parametertool "
                 "verify` to assess the damage");
        }
    }

    if (to_write.empty() && to_delete.empty()) {
        std::printf("nothing to do: %s already matches snapshot %d\n",
                    opt.file.empty() ? "the working tree" : opt.file.c_str(), id);
        return kExitOk;
    }

    // Show the plan for both --dry-run and the confirmation prompt.
    const std::string label = snap.name.empty() ? std::string() : " \"" + snap.name + "\"";
    std::printf("restore snapshot %d%s into %s\n", id, label.c_str(),
                target.root.string().c_str());
    for (const auto& e : to_write) {
        std::printf("  %s  %s\n", live_paths.count(e.path) ? "update" : "create", e.path.c_str());
    }
    for (const auto& p : to_delete) std::printf("  delete  %s\n", p.c_str());

    if (opt.dry_run) {
        std::printf("(dry run: nothing was changed)\n");
        return kExitOk;
    }

    if (!to_delete.empty() && !opt.assume_yes) {
        const std::string q = "This deletes " + std::to_string(to_delete.size()) + " file" +
                              (to_delete.size() == 1 ? "" : "s") +
                              " not present in snapshot " + std::to_string(id) +
                              ". Continue?";
        if (!Confirm(q)) {
            std::fprintf(stderr, "aborted\n");
            return kExitError;
        }
    }

    const StoreLock lock = store.AcquireLock();

    // Safety net: checkpoint the current state so a wrong restore is one command away from undone.
    if (!opt.no_auto_snapshot) {
        if (snap.scope == Scope::File && live.empty()) {
            std::printf("note: no safety snapshot taken — \"%s\" does not exist yet, so this "
                        "restore only creates it\n", snap.files.front().path.c_str());
        } else {
            Options auto_opt = opt;
            Target auto_target = target;
            if (snap.scope == Scope::File) {
                auto_target.single_file = snap.files.front().path;
            } else {
                auto_opt.all = false;
                auto_opt.pattern = snap.pattern;  // exactly the domain this restore can change
            }
            const SnapshotResult saved =
                TakeSnapshot(store, auto_target, auto_opt,
                             "auto-before-restore-" + std::to_string(id), /*automatic=*/true);
            std::printf("saved current state as snapshot %d (undo with `parametertool restore "
                        "--version %d`)\n", saved.snapshot.id, saved.snapshot.id);
        }
    }

    // Writes first, then deletions: if something fails midway the tree still holds the snapshot's
    // files rather than having lost the extras for nothing.
    const unsigned workers = opt.workers ? opt.workers : DefaultWorkers();
    ParallelFor(to_write.size(), workers, [&](std::size_t i) {
        const FileEntry& e = to_write[i];
        const fs::path abs = target.root / e.path;

        std::error_code ec;
        fs::create_directories(abs.parent_path(), ec);

        const std::string data = store.GetObjectVerified(e.hash);
        WriteFileAtomic(abs, data, e.mode ? e.mode : 0644u, store.tmp_dir());
        SetMtime(abs, e.mtime);
    });

    std::set<fs::path> emptied;
    for (const auto& rel : to_delete) {
        const fs::path abs = target.root / rel;
        std::error_code ec;
        if (!fs::remove(abs, ec)) {
            std::fprintf(stderr, "warning: could not delete %s: %s\n", rel.c_str(),
                         ec.message().c_str());
            continue;
        }
        emptied.insert(abs.parent_path());
    }

    // Prune directories our own deletions emptied. fs::remove refuses a non-empty directory, so
    // this can never take out a folder that still holds anything.
    const std::string root_prefix = target.root.string() + "/";
    for (const auto& dir : emptied) {
        for (fs::path d = dir; d != target.root; d = d.parent_path()) {
            // Only ever climb inside the tree we were pointed at.
            if (d.string().rfind(root_prefix, 0) != 0) break;
            std::error_code ec;
            if (!fs::remove(d, ec)) break;  // non-empty (or in use): stop climbing
        }
    }

    std::printf("restored %zu file%s", to_write.size(), to_write.size() == 1 ? "" : "s");
    if (!to_delete.empty()) {
        std::printf(", deleted %zu", to_delete.size());
    }
    std::printf("\n");
    return kExitOk;
}

}  // namespace pt
