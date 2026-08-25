#pragma once

#include <cstddef>
#include <functional>

namespace pt {

// min(hardware_concurrency, 8) — beyond that, hashing many small XML files is bound by the
// filesystem, not the CPU.
unsigned DefaultWorkers();

// Clamps a user-supplied --workers value into [1, 64].
unsigned ClampWorkers(long requested);

// Runs fn(i) for every i in [0, n) across `workers` threads.
//
// If fn throws, remaining indices are abandoned and the first exception is rethrown on the calling
// thread once every worker has joined — so a failure surfaces as the same clean error it would in
// the single-threaded path, with no thread still touching the store.
void ParallelFor(std::size_t n, unsigned workers, const std::function<void(std::size_t)>& fn);

}  // namespace pt
