#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace pt {

// Exit codes, also documented in --help and the README.
enum ExitCode {
    kExitOk = 0,
    kExitError = 1,    // runtime failure
    kExitUsage = 2,    // bad command line
    kExitLocked = 3,   // another parametertool holds the store lock
    kExitCorrupt = 4,  // verify found damage
};

// Thrown for any condition the user should see as a clean one-line error rather than a crash.
class Error : public std::runtime_error {
public:
    Error(int code, std::string msg) : std::runtime_error(std::move(msg)), code_(code) {}
    explicit Error(std::string msg) : Error(kExitError, std::move(msg)) {}
    int code() const { return code_; }

private:
    int code_;
};

[[noreturn]] inline void Fail(std::string msg) { throw Error(kExitError, std::move(msg)); }
[[noreturn]] inline void FailUsage(std::string msg) { throw Error(kExitUsage, std::move(msg)); }

}  // namespace pt
