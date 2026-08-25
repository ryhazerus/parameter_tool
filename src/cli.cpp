#include "cli.hpp"

#include "error.hpp"
#include "pool.hpp"
#include "scan.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace pt {
namespace {

struct CommandSpec {
    const char* name;
    int max_ids;      // how many snapshot refs may follow the optional path
    bool needs_id;    // an id is mandatory
};

const CommandSpec kCommands[] = {
    {"snap", 0, false},   {"restore", 1, true},  {"list", 0, false},
    {"show", 1, true},    {"diff", 2, true},     {"delete", 1, true},
    {"cleanup", 0, false},{"verify", 0, false},  {"gc", 0, false},
};

const CommandSpec* FindCommand(std::string_view name) {
    for (const auto& c : kCommands) {
        if (name == c.name) return &c;
    }
    return nullptr;
}

long ParseLong(const std::string& s, const char* flag) {
    if (s.empty()) FailUsage(std::string(flag) + " needs a number");
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') {
        FailUsage(std::string(flag) + " needs a number, got \"" + s + "\"");
    }
    return v;
}

// A positional that names an existing directory is the target path; anything else is a snapshot
// reference. This is what makes both `parametertool show 3` (from inside the dir) and
// `parametertool show ./my_dir 3` work.
bool LooksLikePath(const std::string& s) {
    std::error_code ec;
    return fs::is_directory(s, ec) || fs::is_regular_file(s, ec);
}

}  // namespace

void PrintUsage() {
    std::printf(
        "parametertool %s — snapshot and restore parameter_*.xml configuration trees\n"
        "\n"
        "USAGE\n"
        "  parametertool <command> [<path>] [options]\n"
        "\n"
        "  <path> is the configuration directory and defaults to the current directory.\n"
        "  Snapshots live in <path>/.paramsnap, so history travels with a copy of the tree.\n"
        "\n"
        "COMMANDS\n"
        "  snap [<path>]               Take a snapshot of the current state.\n"
        "  restore [<path>] --version <id>\n"
        "                              Restore a snapshot. Exact by default: files matching the\n"
        "                              snapshot's pattern that it does not contain are removed.\n"
        "  list [<path>]               List snapshots, newest last.\n"
        "  show [<path>] <id>          Show one snapshot's contents.\n"
        "  diff [<path>] <id> [<id2>]  Compare two snapshots, or a snapshot to the live tree.\n"
        "  delete [<path>] <id>        Delete one snapshot.\n"
        "  cleanup [<path>]            Delete all snapshots (or all but --keep-last N).\n"
        "  verify [<path>]             Re-hash stored objects and check every manifest.\n"
        "  gc [<path>]                 Remove objects no snapshot references.\n"
        "\n"
        "OPTIONS\n"
        "  -n, --name <label>          Label for a new snapshot. Quote it if it has spaces.\n"
        "      --pattern <glob>        Filenames to include (default: %s).\n"
        "      --all                   Include every file, not just the default pattern.\n"
        "  -w, --workers <n>           Hashing threads (default: %u here, max 64).\n"
        "      --version <id>          Snapshot to restore: a number, a name, or \"latest\".\n"
        "      --file <relpath>        Restore just this one file out of the snapshot.\n"
        "      --merge                 Restore without deleting files the snapshot lacks.\n"
        "      --keep-last <n>         cleanup: keep the n most recent snapshots.\n"
        "      --no-auto-snapshot      Skip the safety snapshot taken before a restore.\n"
        "      --dry-run               Print what would happen and change nothing.\n"
        "  -y, --yes                   Do not prompt before destructive actions.\n"
        "      --verbose               More detail in list/verify output.\n"
        "  -h, --help                  This message.\n"
        "\n"
        "EXAMPLES\n"
        "  parametertool snap ./my_dir --workers 4 --name \"version 1\"\n"
        "  parametertool snap ./my_dir/parameter_motor.xml --name \"motor tuned\"\n"
        "  parametertool list ./my_dir\n"
        "  parametertool diff ./my_dir 3            # snapshot 3 vs the live tree\n"
        "  parametertool restore ./my_dir --version 3\n"
        "  parametertool restore ./my_dir --version 3 --file parameter_motor.xml\n"
        "  parametertool delete ./my_dir 3\n"
        "  parametertool cleanup ./my_dir --keep-last 5\n"
        "\n"
        "EXIT CODES\n"
        "  0 ok   1 error   2 usage   3 store locked   4 verify found damage\n",
        kToolVersion, kDefaultPattern, DefaultWorkers());
}

bool ParseArgs(int argc, char** argv, Options& out) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        PrintUsage();
        return false;
    }
    if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        PrintUsage();
        return false;
    }
    if (args[0] == "--version" || args[0] == "-V") {
        std::printf("parametertool %s\n", kToolVersion);
        return false;
    }

    const CommandSpec* spec = FindCommand(args[0]);
    if (!spec) {
        FailUsage("unknown command \"" + args[0] +
                  "\"\n  run `parametertool --help` for the command list");
    }
    out.command = args[0];

    std::vector<std::string> positional;
    bool saw_help = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string a = args[i];

        // Split --flag=value so both spellings work.
        std::string inline_value;
        bool has_inline = false;
        if (a.rfind("--", 0) == 0) {
            const auto eq = a.find('=');
            if (eq != std::string::npos) {
                inline_value = a.substr(eq + 1);
                a = a.substr(0, eq);
                has_inline = true;
            }
        }

        const auto need_value = [&](const char* flag) -> std::string {
            if (has_inline) return inline_value;
            if (i + 1 >= args.size()) FailUsage(std::string(flag) + " needs a value");
            return args[++i];
        };
        const auto no_value = [&](const char* flag) {
            if (has_inline) FailUsage(std::string(flag) + " does not take a value");
        };

        if (a == "-h" || a == "--help") { saw_help = true; }
        else if (a == "-n" || a == "--name") { out.name = need_value("--name"); }
        else if (a == "--pattern") { out.pattern = need_value("--pattern"); }
        else if (a == "--all") { no_value("--all"); out.all = true; }
        else if (a == "-w" || a == "--workers") {
            out.workers = ClampWorkers(ParseLong(need_value("--workers"), "--workers"));
        }
        else if (a == "--version") { out.id = need_value("--version"); }
        else if (a == "--file") { out.file = need_value("--file"); }
        else if (a == "--merge") { no_value("--merge"); out.merge = true; }
        else if (a == "--keep-last") {
            out.keep_last = ParseLong(need_value("--keep-last"), "--keep-last");
            if (out.keep_last < 0) FailUsage("--keep-last cannot be negative");
        }
        else if (a == "--no-auto-snapshot") { no_value("--no-auto-snapshot"); out.no_auto_snapshot = true; }
        else if (a == "--dry-run") { no_value("--dry-run"); out.dry_run = true; }
        else if (a == "-y" || a == "--yes") { no_value("--yes"); out.assume_yes = true; }
        else if (a == "--verbose") { no_value("--verbose"); out.verbose = true; }
        else if (!a.empty() && a[0] == '-' && a != "-") {
            FailUsage("unknown option \"" + args[i] + "\" for command \"" + out.command +
                      "\"\n  run `parametertool --help` for the option list");
        }
        else { positional.push_back(args[i]); }
    }

    if (saw_help) {
        PrintUsage();
        return false;
    }

    // First positional is the path only if it actually exists; otherwise it is a snapshot ref.
    std::size_t next = 0;
    if (!positional.empty() && LooksLikePath(positional[0])) {
        out.path = positional[0];
        next = 1;
    }

    std::vector<std::string> ids(positional.begin() + static_cast<std::ptrdiff_t>(next),
                                 positional.end());

    if (static_cast<int>(ids.size()) > spec->max_ids) {
        if (spec->max_ids == 0) {
            FailUsage("`" + out.command + "` takes no arguments after the path, but got \"" +
                      ids[0] + "\"\n  if that was part of a --name, quote it: --name \"" +
                      out.name + " " + ids[0] + "\"");
        }
        FailUsage("too many arguments for `" + out.command + "`: unexpected \"" +
                  ids[static_cast<std::size_t>(spec->max_ids)] + "\"");
    }

    if (!ids.empty()) {
        // An explicit --version wins; a positional fills in when it was not given.
        if (out.id.empty()) out.id = ids[0];
        else FailUsage("snapshot given twice: --version " + out.id + " and \"" + ids[0] + "\"");
    }
    if (ids.size() > 1) out.id_b = ids[1];

    if (spec->needs_id && out.id.empty()) {
        if (out.command == "restore") {
            FailUsage("restore needs a snapshot: parametertool restore [<path>] --version <id>\n"
                      "  run `parametertool list` to see the available ids");
        }
        FailUsage(out.command + " needs a snapshot id\n  run `parametertool list` to see them");
    }

    if (!out.file.empty() && out.command != "restore") {
        FailUsage("--file only applies to `restore`");
    }
    if (out.merge && out.command != "restore") {
        FailUsage("--merge only applies to `restore`");
    }
    if (out.keep_last >= 0 && out.command != "cleanup") {
        FailUsage("--keep-last only applies to `cleanup`");
    }
    if (!out.name.empty() && out.command != "snap") {
        FailUsage("--name only applies to `snap`");
    }
    if (out.all && !out.pattern.empty()) {
        FailUsage("--all and --pattern conflict; use one or the other");
    }

    return true;
}

bool Confirm(const std::string& question) {
    if (!::isatty(STDIN_FILENO)) {
        std::fprintf(stderr, "%s\nrefusing to continue: stdin is not a terminal (pass --yes)\n",
                     question.c_str());
        return false;
    }
    std::fprintf(stderr, "%s [y/N] ", question.c_str());
    std::fflush(stderr);

    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line == "y" || line == "Y" || line == "yes" || line == "Yes";
}

}  // namespace pt
