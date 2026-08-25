#include "cli.hpp"
#include "commands.hpp"
#include "error.hpp"

#include <cstdio>
#include <exception>
#include <string>

namespace {

int Dispatch(const pt::Options& opt) {
    if (opt.command == "snap")    return pt::CmdSnap(opt);
    if (opt.command == "restore") return pt::CmdRestore(opt);
    if (opt.command == "list")    return pt::CmdList(opt);
    if (opt.command == "show")    return pt::CmdShow(opt);
    if (opt.command == "diff")    return pt::CmdDiff(opt);
    if (opt.command == "delete")  return pt::CmdDelete(opt);
    if (opt.command == "cleanup") return pt::CmdCleanup(opt);
    if (opt.command == "verify")  return pt::CmdVerify(opt);
    if (opt.command == "gc")      return pt::CmdGc(opt);

    pt::FailUsage("unknown command \"" + opt.command + "\"");
}

}  // namespace

int main(int argc, char** argv) {
    // Line-buffer stdout so progress and error messages stay in order when output is redirected.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    try {
        pt::Options opt;
        if (!pt::ParseArgs(argc, argv, opt)) return pt::kExitOk;
        return Dispatch(opt);
    } catch (const pt::Error& e) {
        std::fprintf(stderr, "parametertool: %s\n", e.what());
        return e.code();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parametertool: %s\n", e.what());
        return pt::kExitError;
    }
}
