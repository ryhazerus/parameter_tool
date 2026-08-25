#include "commands.hpp"

#include "error.hpp"

#include <algorithm>
#include <cstdio>

namespace pt {

int CmdCleanup(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);

    if (!Store::Exists(target.root)) {
        std::printf("nothing to clean up: no snapshot store in %s\n", target.root.string().c_str());
        return kExitOk;
    }

    const Store store = Store::Open(target.root, /*create=*/false);
    std::vector<int> ids = store.AllSnapshotIds();

    if (ids.empty()) {
        std::printf("nothing to clean up: no snapshots in %s\n", target.root.string().c_str());
        return kExitOk;
    }

    // --keep-last counts real checkpoints. Safety snapshots are the tool's own bookkeeping, so
    // keeping "the last 5" should not be satisfied by five automatic ones.
    std::vector<int> doomed;
    if (opt.keep_last > 0) {
        std::vector<int> manual;
        for (int id : ids) {
            if (!store.GetSnapshot(id).automatic) manual.push_back(id);
        }
        const std::size_t keep = static_cast<std::size_t>(opt.keep_last);

        if (manual.size() > keep) {
            // Everything older than the oldest checkpoint we are keeping goes, safety snapshots
            // included; anything newer than it stays.
            const int cutoff = manual[manual.size() - keep];
            for (int id : ids) {
                if (id < cutoff) doomed.push_back(id);
            }
        }
        // Fewer checkpoints than requested: keep them all, and their safety snapshots with them.
    } else {
        doomed = ids;  // plain cleanup, or --keep-last 0: everything goes
    }

    if (doomed.empty()) {
        std::printf("nothing to remove: %zu snapshot%s, keeping the last %ld\n", ids.size(),
                    ids.size() == 1 ? "" : "s", opt.keep_last);
        return kExitOk;
    }

    std::printf("would remove %zu of %zu snapshot%s:\n", doomed.size(), ids.size(),
                ids.size() == 1 ? "" : "s");
    for (int id : doomed) {
        std::printf("%s\n", FormatSnapshotLine(store.GetSnapshot(id), /*verbose=*/false).c_str());
    }

    if (opt.dry_run) {
        std::printf("(dry run: nothing was removed)\n");
        return kExitOk;
    }

    const std::string question =
        opt.keep_last > 0
            ? "Delete these " + std::to_string(doomed.size()) + " snapshots?"
            : "Delete ALL " + std::to_string(doomed.size()) +
                  " snapshots? The checkpoint history cannot be recovered.";
    if (!opt.assume_yes && !Confirm(question)) {
        std::fprintf(stderr, "aborted\n");
        return kExitError;
    }

    const StoreLock lock = store.AcquireLock();
    for (int id : doomed) store.RemoveSnapshot(id);

    std::printf("removed %zu snapshot%s\n", doomed.size(), doomed.size() == 1 ? "" : "s");
    std::printf("  run `parametertool gc` to reclaim the disk space they used\n");
    return kExitOk;
}

}  // namespace pt
