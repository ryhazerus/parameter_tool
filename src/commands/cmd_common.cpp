#include "commands.hpp"

#include "error.hpp"
#include "pool.hpp"
#include "scan.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;

namespace pt {

Target ResolveTarget(const Options& opt, bool allow_file) {
    std::error_code ec;
    const fs::path given = opt.path;

    if (!fs::exists(given, ec)) {
        Fail("no such file or directory: " + given.string());
    }

    Target t;
    if (fs::is_directory(given, ec)) {
        t.root = fs::weakly_canonical(given, ec);
        if (ec) t.root = fs::absolute(given);
    } else if (fs::is_regular_file(given, ec)) {
        if (!allow_file) {
            Fail(given.string() + " is a file, but `" + opt.command +
                 "` works on the configuration directory\n  try: parametertool " + opt.command +
                 " " + (given.parent_path().empty() ? "." : given.parent_path().string()));
        }
        fs::path parent = given.parent_path();
        if (parent.empty()) parent = ".";
        parent = fs::weakly_canonical(parent, ec);
        if (ec) parent = fs::absolute(parent);

        // Walk up looking for a store already covering this file, so snapshotting
        // my_dir/subsys/parameter_c.xml joins my_dir's history instead of starting a second store
        // down in subsys/. Only file targets search upward; a directory target is taken literally.
        fs::path owner = parent;
        for (fs::path p = parent; !p.empty(); p = p.parent_path()) {
            if (Store::Exists(p)) {
                owner = p;
                break;
            }
            if (p == p.parent_path()) break;  // reached the filesystem root
        }

        t.root = owner;
        std::error_code rel_ec;
        const fs::path rel = fs::relative(parent / given.filename(), owner, rel_ec);
        t.single_file = rel_ec ? given.filename().string() : ToRelPosix(rel);
    } else {
        Fail("not a regular file or directory: " + given.string());
    }

    // Snapshotting the store into itself would recurse; catch the mistake early.
    for (const auto& part : t.root) {
        if (part == kStoreDirName) {
            Fail(given.string() + " is inside the snapshot store\n  point parametertool at the "
                 "configuration directory instead");
        }
    }
    return t;
}

std::string EffectivePattern(const Options& opt) {
    if (opt.all) return "*";
    if (!opt.pattern.empty()) return opt.pattern;
    return kDefaultPattern;
}

int ResolveSnapshotRef(const Store& store, const std::string& ref) {
    const std::vector<int> ids = store.AllSnapshotIds();
    if (ids.empty()) {
        Fail("this store has no snapshots yet\n  run `parametertool snap` to create one");
    }

    if (ref == "latest") return ids.back();

    // A pure number is an id. Leading zeros are fine, so both `3` and `0003` work.
    const bool numeric = !ref.empty() && std::all_of(ref.begin(), ref.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
    if (numeric) {
        const int id = std::atoi(ref.c_str());
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) return id;
        Fail("no snapshot with id " + std::to_string(id) +
             "\n  run `parametertool list` to see the available ids");
    }

    // Otherwise match a snapshot name; the most recent wins if a label was reused.
    int found = -1;
    int matches = 0;
    for (int id : ids) {
        if (store.GetSnapshot(id).name == ref) {
            found = id;
            ++matches;
        }
    }
    if (matches == 1) return found;
    if (matches > 1) {
        std::fprintf(stderr, "note: %d snapshots are named \"%s\"; using the newest (id %d)\n",
                     matches, ref.c_str(), found);
        return found;
    }

    Fail("no snapshot with id or name \"" + ref +
         "\"\n  run `parametertool list` to see what is available");
}

SnapshotResult TakeSnapshot(const Store& store, const Target& target, const Options& opt,
                            const std::string& name, bool automatic) {
    SnapshotResult result;
    Snapshot& snap = result.snapshot;
    snap.name = name;
    snap.automatic = automatic;
    snap.created_epoch = static_cast<std::int64_t>(std::time(nullptr));
    snap.created_utc = FormatUtc(snap.created_epoch);

    std::vector<std::string> rel_paths;

    if (target.is_file()) {
        // A single-file snapshot records the filename as its pattern, so a later restore can never
        // reach past that one file.
        snap.scope = Scope::File;
        snap.pattern = target.single_file;
        snap.recursive = false;

        std::error_code ec;
        if (!fs::is_regular_file(target.root / target.single_file, ec)) {
            Fail("not a regular file: " + (target.root / target.single_file).string());
        }
        rel_paths.push_back(target.single_file);
    } else {
        snap.scope = Scope::Tree;
        snap.pattern = EffectivePattern(opt);
        snap.recursive = true;

        ScanResult scan = ScanTree(target.root, snap.pattern, snap.recursive);
        rel_paths = std::move(scan.files);
        result.warnings = std::move(scan.warnings);
    }

    // Hash and ingest in parallel. Each worker owns its own index, so the only shared state is the
    // object store, which dedups by content and writes atomically.
    std::vector<FileEntry> entries(rel_paths.size());
    std::atomic<std::size_t> new_objects{0};
    const unsigned workers = opt.workers ? opt.workers : DefaultWorkers();

    ParallelFor(rel_paths.size(), workers, [&](std::size_t i) {
        const fs::path abs = target.root / rel_paths[i];

        std::string contents;
        const std::string hash = HashFile(abs, &contents);
        const FileStat st = StatFile(abs);

        if (contents.empty() && st.size > 0) {
            contents = ReadFileFully(abs);  // streamed because it was large; read it back to store
        }
        if (store.PutObject(hash, contents)) {
            new_objects.fetch_add(1, std::memory_order_relaxed);
        }

        FileEntry e;
        e.path = rel_paths[i];
        e.hash = hash;
        e.size = st.size;
        e.mtime = st.mtime;
        e.mode = st.mode;
        entries[i] = std::move(e);
    });

    // rel_paths came back sorted from ScanTree, so entries are already in manifest order.
    snap.files = std::move(entries);
    snap.id = store.AllocateId();
    store.PutSnapshot(snap);

    result.new_objects = new_objects.load(std::memory_order_relaxed);
    return result;
}

std::vector<FileEntry> ScanLive(const fs::path& root, const std::string& pattern, bool recursive,
                                unsigned workers, std::vector<std::string>* warnings) {
    ScanResult scan = ScanTree(root, pattern, recursive);
    if (warnings) warnings->insert(warnings->end(), scan.warnings.begin(), scan.warnings.end());

    std::vector<FileEntry> entries(scan.files.size());
    ParallelFor(scan.files.size(), workers ? workers : DefaultWorkers(), [&](std::size_t i) {
        const fs::path abs = root / scan.files[i];
        std::string contents;
        FileEntry e;
        e.hash = HashFile(abs, &contents);
        const FileStat st = StatFile(abs);
        e.path = scan.files[i];
        e.size = st.size;
        e.mtime = st.mtime;
        e.mode = st.mode;
        entries[i] = std::move(e);
    });
    return entries;  // ScanTree sorted the paths, so this is sorted too
}

std::vector<FileChange> CompareEntries(const std::vector<FileEntry>& from,
                                       const std::vector<FileEntry>& to) {
    std::vector<FileChange> out;
    std::size_t i = 0, j = 0;

    while (i < from.size() || j < to.size()) {
        if (j == to.size() || (i < from.size() && from[i].path < to[j].path)) {
            out.push_back({'R', from[i].path, &from[i], nullptr});
            ++i;
        } else if (i == from.size() || to[j].path < from[i].path) {
            out.push_back({'A', to[j].path, nullptr, &to[j]});
            ++j;
        } else {
            if (from[i].hash != to[j].hash) {
                out.push_back({'M', from[i].path, &from[i], &to[j]});
            }
            ++i;
            ++j;
        }
    }
    return out;
}

std::string FormatSnapshotLine(const Snapshot& s, bool verbose) {
    char buf[256];
    std::string when = s.created_utc;
    if (when.size() == 20) {  // 2026-08-25T09:50:12Z -> 2026-08-25 09:50:12
        when[10] = ' ';
        when.resize(19);
    }

    std::snprintf(buf, sizeof(buf), "%5d  %s  %4zu file%s  %9s%s", s.id, when.c_str(),
                  s.files.size(), s.files.size() == 1 ? " " : "s",
                  FormatSize(s.TotalSize()).c_str(), s.automatic ? "  [auto]" : "");

    std::string line = buf;
    if (!s.name.empty()) line += "  " + s.name;
    if (s.scope == Scope::File) line += "  (single file: " + s.pattern + ")";
    if (verbose && s.scope == Scope::Tree) line += "  (pattern: " + s.pattern + ")";
    return line;
}

}  // namespace pt
