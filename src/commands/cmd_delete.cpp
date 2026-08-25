#include "commands.hpp"

#include "error.hpp"

#include <cstdio>

namespace pt {

int CmdDelete(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);

    const int id = ResolveSnapshotRef(store, opt.id);
    const Snapshot s = store.GetSnapshot(id);

    std::printf("%s\n", FormatSnapshotLine(s, /*verbose=*/true).c_str());

    if (opt.dry_run) {
        std::printf("(dry run: snapshot %d was not deleted)\n", id);
        return kExitOk;
    }

    if (!opt.assume_yes && !Confirm("Delete snapshot " + std::to_string(id) + "?")) {
        std::fprintf(stderr, "aborted\n");
        return kExitError;
    }

    const StoreLock lock = store.AcquireLock();
    store.RemoveSnapshot(id);

    std::printf("deleted snapshot %d\n", id);
    std::printf("  file contents are still on disk; run `parametertool gc` to reclaim the space\n");
    return kExitOk;
}

}  // namespace pt
