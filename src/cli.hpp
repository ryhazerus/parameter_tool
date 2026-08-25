#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pt {

// Set by CMake from the project version, or from the git tag for a release build.
#ifndef PARAMTOOL_VERSION
#define PARAMTOOL_VERSION "0.0.0-dev"
#endif
inline constexpr const char* kToolVersion = PARAMTOOL_VERSION;

struct Options {
    std::string command;
    std::filesystem::path path = ".";   // config directory, or a single file for `snap`

    std::string name;                   // --name
    std::string pattern;                // --pattern (empty means the default)
    bool all = false;                   // --all
    unsigned workers = 0;               // 0 means DefaultWorkers()

    std::string id;                     // primary snapshot ref (--version, or a positional)
    std::string id_b;                   // second ref, for `diff a b`
    std::string file;                   // --file <relpath>, restore one file out of a snapshot

    bool merge = false;                 // --merge: leave unknown files alone
    bool dry_run = false;
    bool assume_yes = false;            // --yes
    bool verbose = false;
    bool no_auto_snapshot = false;
    long keep_last = -1;                // --keep-last N (-1 = not given)
};

// Parses argv. Throws Error(kExitUsage) with a helpful message on bad input.
// Returns false if the command line was fully handled already (--help / --version).
bool ParseArgs(int argc, char** argv, Options& out);

void PrintUsage();

// Asks the user to confirm a destructive action. When stdin is not a terminal this returns false
// rather than assuming yes, so a script that forgot --yes stops instead of deleting things.
bool Confirm(const std::string& question);

}  // namespace pt
