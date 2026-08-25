#include "commands.hpp"

#include "error.hpp"

#include <algorithm>
#include <cstdio>

namespace pt {
namespace {

struct Change {
    char kind;  // 'M' modified, 'A' added, 'R' removed
    std::string path;
    std::uint64_t old_size = 0;
    std::uint64_t new_size = 0;
};

// Ordered merge of two path-sorted lists.
std::vector<Change> Compare(const std::vector<FileEntry>& from, const std::vector<FileEntry>& to) {
    std::vector<Change> out;
    std::size_t i = 0, j = 0;

    while (i < from.size() || j < to.size()) {
        if (j == to.size() || (i < from.size() && from[i].path < to[j].path)) {
            out.push_back({'R', from[i].path, from[i].size, 0});
            ++i;
        } else if (i == from.size() || to[j].path < from[i].path) {
            out.push_back({'A', to[j].path, 0, to[j].size});
            ++j;
        } else {
            if (from[i].hash != to[j].hash) {
                out.push_back({'M', from[i].path, from[i].size, to[j].size});
            }
            ++i;
            ++j;
        }
    }
    return out;
}

}  // namespace

int CmdDiff(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);

    const Snapshot from = store.GetSnapshot(ResolveSnapshotRef(store, opt.id));

    std::vector<FileEntry> to_files;
    std::string to_label;

    if (!opt.id_b.empty()) {
        const Snapshot to = store.GetSnapshot(ResolveSnapshotRef(store, opt.id_b));
        to_files = to.files;
        to_label = "snapshot " + std::to_string(to.id);
    } else {
        // Compare against the live tree, using the pattern this snapshot was taken with so the
        // two sides describe the same set of files.
        std::vector<std::string> warnings;
        to_files = ScanLive(target.root, from.pattern, from.recursive, opt.workers, &warnings);
        for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        to_label = "working tree";

        if (from.scope == Scope::File) {
            // A single-file snapshot only ever describes one path; ignore everything else on disk.
            to_files.erase(std::remove_if(to_files.begin(), to_files.end(),
                                          [&](const FileEntry& e) { return e.path != from.pattern; }),
                           to_files.end());
        }
    }

    const std::vector<Change> changes = Compare(from.files, to_files);

    std::printf("snapshot %d -> %s\n", from.id, to_label.c_str());
    if (changes.empty()) {
        std::printf("  no differences\n");
        return kExitOk;
    }

    int mod = 0, add = 0, rem = 0;
    for (const auto& c : changes) {
        switch (c.kind) {
            case 'M': ++mod; break;
            case 'A': ++add; break;
            default:  ++rem; break;
        }
    }

    for (const auto& c : changes) {
        if (opt.verbose && c.kind == 'M') {
            std::printf("  %c  %s  (%s -> %s)\n", c.kind, c.path.c_str(),
                        FormatSize(c.old_size).c_str(), FormatSize(c.new_size).c_str());
        } else {
            std::printf("  %c  %s\n", c.kind, c.path.c_str());
        }
    }
    std::printf("%d modified, %d added, %d removed\n", mod, add, rem);
    return kExitOk;
}

}  // namespace pt
