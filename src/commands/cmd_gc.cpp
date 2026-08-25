#include "commands.hpp"

#include "error.hpp"

#include <cstdio>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace pt {

int CmdGc(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);

    // Held for the whole command, including the survey: the temporary files we are about to judge
    // as leftovers would otherwise be indistinguishable from a concurrent write in progress.
    const StoreLock lock = store.AcquireLock();

    // Anything still named by a manifest stays. A manifest we cannot read is treated as if it
    // referenced everything: refusing to guess is the only safe move when deletion is the outcome.
    std::set<std::string> referenced;
    for (int id : store.AllSnapshotIds()) {
        Snapshot s;
        try {
            s = store.GetSnapshot(id);
        } catch (const Error& e) {
            Fail(std::string("refusing to collect garbage: ") + e.what() +
                 "\n  fix or delete that snapshot first (`parametertool verify` shows the damage)");
        }
        for (const auto& f : s.files) referenced.insert(f.hash);
    }

    std::vector<std::string> orphans;
    std::uint64_t reclaim = 0;
    for (const auto& h : store.AllObjectHashes()) {
        if (referenced.count(h)) continue;
        orphans.push_back(h);
        std::error_code ec;
        reclaim += fs::file_size(store.ObjectPath(h), ec);
    }

    // Temporary files left behind by an interrupted run are also garbage.
    std::vector<fs::path> stale_tmp;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(store.tmp_dir(), ec)) {
        if (e.is_regular_file(ec)) {
            stale_tmp.push_back(e.path());
            std::error_code sec;
            reclaim += fs::file_size(e.path(), sec);
        }
    }

    if (orphans.empty() && stale_tmp.empty()) {
        std::printf("nothing to collect: every stored object is referenced by a snapshot\n");
        return kExitOk;
    }

    std::printf("%zu unreferenced object%s and %zu leftover temporary file%s, %s\n", orphans.size(),
                orphans.size() == 1 ? "" : "s", stale_tmp.size(), stale_tmp.size() == 1 ? "" : "s",
                FormatSize(reclaim).c_str());
    if (opt.verbose) {
        for (const auto& h : orphans) std::printf("  %s\n", h.c_str());
    }

    if (opt.dry_run) {
        std::printf("(dry run: nothing was removed)\n");
        return kExitOk;
    }

    std::size_t removed = 0;
    for (const auto& h : orphans) {
        std::error_code rec;
        if (fs::remove(store.ObjectPath(h), rec)) ++removed;
    }
    for (const auto& p : stale_tmp) {
        std::error_code rec;
        fs::remove(p, rec);
    }

    // Drop object shard directories that are now empty. fs::remove refuses a non-empty directory.
    std::error_code sec;
    for (const auto& shard : fs::directory_iterator(store.dir() / "objects", sec)) {
        if (!shard.is_directory(sec)) continue;
        std::error_code rec;
        fs::remove(shard.path(), rec);
    }

    std::printf("reclaimed %s from %zu object%s\n", FormatSize(reclaim).c_str(), removed,
                removed == 1 ? "" : "s");
    return kExitOk;
}

}  // namespace pt
