#include "commands.hpp"

#include "error.hpp"
#include "pool.hpp"
#include "scan.hpp"

#include <cstdio>
#include <system_error>

namespace fs = std::filesystem;

namespace pt {

int CmdSnap(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/true);

    if (opt.dry_run) {
        // Report what would be captured without creating a store or writing anything.
        std::vector<std::string> files;
        if (target.is_file()) {
            files.push_back(target.single_file);
        } else {
            ScanResult scan = ScanTree(target.root, EffectivePattern(opt), true);
            for (const auto& w : scan.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
            files = std::move(scan.files);
        }

        if (files.empty()) {
            std::printf("would capture nothing: no files match %s in %s\n",
                        EffectivePattern(opt).c_str(), target.root.string().c_str());
            return kExitOk;
        }
        std::printf("would capture %zu file%s from %s:\n", files.size(),
                    files.size() == 1 ? "" : "s", target.root.string().c_str());
        for (const auto& f : files) std::printf("  %s\n", f.c_str());
        return kExitOk;
    }

    const Store store = Store::Open(target.root, /*create=*/true);
    const StoreLock lock = store.AcquireLock();

    // Remember the newest existing snapshot so we can point out an unchanged tree afterwards.
    const std::vector<int> before = store.AllSnapshotIds();

    SnapshotResult r = TakeSnapshot(store, target, opt, opt.name, /*automatic=*/false);
    for (const auto& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());

    if (r.snapshot.files.empty()) {
        std::fprintf(stderr,
                     "warning: snapshot %d is empty — nothing matched %s in %s\n",
                     r.snapshot.id, r.snapshot.pattern.c_str(), target.root.string().c_str());
    }

    const std::string label = opt.name.empty() ? std::string() : " \"" + opt.name + "\"";
    std::printf("created snapshot %d%s\n", r.snapshot.id, label.c_str());
    std::printf("  %zu file%s, %s\n", r.snapshot.files.size(),
                r.snapshot.files.size() == 1 ? "" : "s",
                FormatSize(r.snapshot.TotalSize()).c_str());
    std::printf("  %zu new object%s stored (%zu already present)\n", r.new_objects,
                r.new_objects == 1 ? "" : "s", r.snapshot.files.size() - r.new_objects);

    // A checkpoint identical to the previous one is worth calling out; it usually means the user
    // expected a change that did not land.
    if (!before.empty()) {
        const Snapshot prev = store.GetSnapshot(before.back());
        if (prev.files.size() == r.snapshot.files.size()) {
            bool same = true;
            for (std::size_t i = 0; i < prev.files.size(); ++i) {
                if (prev.files[i].path != r.snapshot.files[i].path ||
                    prev.files[i].hash != r.snapshot.files[i].hash) {
                    same = false;
                    break;
                }
            }
            if (same) {
                std::printf("  contents are identical to snapshot %d\n", prev.id);
            }
        }
    }

    return kExitOk;
}

}  // namespace pt
