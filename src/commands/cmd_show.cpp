#include "commands.hpp"

#include "error.hpp"

#include <cstdio>

namespace pt {

int CmdShow(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);
    const Snapshot s = store.GetSnapshot(ResolveSnapshotRef(store, opt.id));

    std::printf("snapshot %d%s\n", s.id, s.automatic ? "  [auto]" : "");
    if (!s.name.empty()) std::printf("  name      %s\n", s.name.c_str());
    std::printf("  created   %s\n", s.created_utc.c_str());
    const std::string scope =
        s.scope == Scope::File
            ? "single file: " + s.pattern
            : "tree, pattern " + s.pattern + (s.recursive ? ", recursive" : ", top level only");
    std::printf("  scope     %s\n", scope.c_str());
    std::printf("  files     %zu, %s\n", s.files.size(), FormatSize(s.TotalSize()).c_str());

    if (s.files.empty()) return kExitOk;

    std::printf("\n");
    for (const auto& f : s.files) {
        if (opt.verbose) {
            std::printf("  %s  %10llu  %s\n", f.hash.c_str(),
                        static_cast<unsigned long long>(f.size), f.path.c_str());
        } else {
            std::printf("  %.12s  %10s  %s\n", f.hash.c_str(), FormatSize(f.size).c_str(),
                        f.path.c_str());
        }
    }
    return kExitOk;
}

}  // namespace pt
