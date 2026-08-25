#include "commands.hpp"

#include "error.hpp"

#include <cstdio>
#include <system_error>

namespace pt {

int CmdList(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);

    if (!Store::Exists(target.root)) {
        std::printf("no snapshots in %s\n  run `parametertool snap` there to create the first one\n",
                    target.root.string().c_str());
        return kExitOk;
    }

    const Store store = Store::Open(target.root, /*create=*/false);
    const std::vector<Snapshot> all = store.AllSnapshots();

    if (all.empty()) {
        std::printf("no snapshots in %s\n", target.root.string().c_str());
        return kExitOk;
    }

    // Field widths here must match FormatSnapshotLine's, or the header drifts off its columns.
    std::printf("%5s  %-19s  %10s  %9s  %s\n", "ID", "CREATED (UTC)", "FILES", "SIZE", "NAME");
    for (const auto& s : all) {
        std::printf("%s\n", FormatSnapshotLine(s, opt.verbose).c_str());
    }

    if (opt.verbose) {
        // Physical footprint: unique objects, which is what the store actually costs on disk.
        std::uint64_t on_disk = 0;
        for (const auto& hash : store.AllObjectHashes()) {
            std::error_code ec;
            on_disk += std::filesystem::file_size(store.ObjectPath(hash), ec);
        }
        std::uint64_t logical = 0;
        for (const auto& s : all) logical += s.TotalSize();

        std::printf("\n%zu snapshot%s, %s on disk (%s before deduplication)\n", all.size(),
                    all.size() == 1 ? "" : "s", FormatSize(on_disk).c_str(),
                    FormatSize(logical).c_str());
    }

    return kExitOk;
}

}  // namespace pt
