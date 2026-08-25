#include "commands.hpp"

#include "error.hpp"
#include "pool.hpp"
#include "sha256.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace pt {

int CmdVerify(const Options& opt) {
    const Target target = ResolveTarget(opt, /*allow_file=*/false);
    const Store store = Store::Open(target.root, /*create=*/false);

    std::vector<std::string> problems;
    std::mutex mu;

    // 1. Every manifest must parse, and every object it names must exist.
    std::set<std::string> referenced;
    const std::vector<int> ids = store.AllSnapshotIds();
    int readable = 0;

    for (int id : ids) {
        Snapshot s;
        try {
            s = store.GetSnapshot(id);
        } catch (const Error& e) {
            problems.push_back("snapshot " + std::to_string(id) + ": " + e.what());
            continue;
        }
        ++readable;
        for (const auto& f : s.files) {
            referenced.insert(f.hash);
            if (!store.HasObject(f.hash)) {
                problems.push_back("snapshot " + std::to_string(id) + " references missing object " +
                                   f.hash.substr(0, 12) + " for \"" + f.path + "\"");
            }
        }
    }

    // 2. Every stored object must still hash to its own name.
    const std::vector<std::string> hashes = store.AllObjectHashes();
    std::atomic<std::size_t> checked{0};
    std::uint64_t bytes = 0;

    ParallelFor(hashes.size(), opt.workers ? opt.workers : DefaultWorkers(), [&](std::size_t i) {
        const std::string& want = hashes[i];
        const fs::path p = store.ObjectPath(want);

        std::string contents;
        std::string got;
        try {
            got = HashFile(p, &contents);
        } catch (const Error& e) {
            std::lock_guard<std::mutex> g(mu);
            problems.push_back(std::string("object ") + want.substr(0, 12) + ": " + e.what());
            return;
        }
        if (got != want) {
            std::lock_guard<std::mutex> g(mu);
            problems.push_back("object " + want.substr(0, 12) + " is corrupt: content hashes to " +
                               got.substr(0, 12) + "\n    " + p.string());
            return;
        }
        checked.fetch_add(1, std::memory_order_relaxed);
    });

    for (const auto& h : hashes) {
        std::error_code ec;
        bytes += fs::file_size(store.ObjectPath(h), ec);
    }

    std::size_t orphans = 0;
    for (const auto& h : hashes) {
        if (!referenced.count(h)) ++orphans;
    }

    std::printf("store %s\n", store.dir().string().c_str());
    std::printf("  %d of %zu manifest%s readable\n", readable, ids.size(),
                ids.size() == 1 ? "" : "s");
    std::printf("  %zu of %zu object%s verified, %s\n", checked.load(), hashes.size(),
                hashes.size() == 1 ? "" : "s", FormatSize(bytes).c_str());
    if (orphans > 0) {
        std::printf("  %zu object%s unreferenced (run `parametertool gc` to reclaim)\n", orphans,
                    orphans == 1 ? " is" : "s are");
    }

    if (problems.empty()) {
        std::printf("ok: no damage found\n");
        return kExitOk;
    }

    std::fprintf(stderr, "\n%zu problem%s found:\n", problems.size(),
                 problems.size() == 1 ? "" : "s");
    for (const auto& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
    return kExitCorrupt;
}

}  // namespace pt
