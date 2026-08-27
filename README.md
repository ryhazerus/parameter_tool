# parametertool

Checkpoint and restore trees of `parameter_<name>.xml` configuration files.

Take a snapshot before changing a working configuration, see exactly what changed since, and roll
back to a known-good state. C++20, CMake, no external dependencies.

```
$ parametertool snap ./my_dir --name "known good"
created snapshot 1 "known good"
  152 files, 604.1 KB
  152 new objects stored (0 already present)

$ parametertool diff ./my_dir 1
  M  parameter_motor.xml
  M  parameter_pid.xml
  A  parameter_encoder.xml
2 modified, 1 added, 0 removed

$ parametertool restore ./my_dir --version 1
saved current state as snapshot 4 (undo with `parametertool restore --version 4`)
restored 2 files, deleted 1
```

## Installing

### RHEL 8 (x86_64)

Download the tarball from the [latest release](../../releases/latest):

```sh
tar xzf parametertool-<version>-linux-x86_64-rhel8.tar.gz
sudo install -m 0755 parametertool-<version>-linux-x86_64-rhel8/bin/parametertool /usr/local/bin/
parametertool --help
```

The release binary is built inside a RHEL 8.10 image against glibc 2.28 and links the C++ runtime
statically, so it runs on a stock RHEL 8 host with no GCC Toolset installed. Verify the download
against the published `.sha256` file if you like.

## Building from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # optional
sudo cmake --install build                     # optional; installs to /usr/local/bin
```

Requires a C++20 compiler and a POSIX system (Linux or macOS). No third-party libraries: SHA-256,
JSON, glob matching, and the thread pool are all implemented against the standard library.

Tested against Clang 21 (macOS), GCC 13 (Ubuntu), and GCC 14 (RHEL 8) — warning-free under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, and clean under ASan/UBSan/LeakSanitizer.

RHEL 8's system compiler is GCC 8.5 and cannot build C++20. Install a GCC Toolset first:

```sh
sudo dnf install -y gcc-toolset-14-gcc-c++ cmake make
source /opt/rh/gcc-toolset-14/enable
```

`--version` reports the CMake project version by default; pass `-DPARAMTOOL_VERSION=1.2.3` to
override it.

## Commands

`<path>` is the configuration directory and defaults to the current directory, so from inside the
tree you can just run `parametertool list`.

| Command | What it does |
| --- | --- |
| `snap [<path>]` | Take a snapshot of the current state. |
| `restore [<path>] --version <id>` | Restore a snapshot. Exact by default — see below. |
| `list [<path>]` | List snapshots, oldest first. |
| `show [<path>] <id>` | Show one snapshot's file list and hashes. |
| `diff [<path>] <id> [<id2>]` | Compare two snapshots, or a snapshot to the live tree. |
| `extract [<path>] --version <id>` | Copy out only the files that changed since the first snapshot. |
| `delete [<path>] <id>` | Delete one snapshot. |
| `cleanup [<path>]` | Delete all snapshots, or all but `--keep-last N`. |
| `verify [<path>]` | Re-hash every stored object and check every manifest. |
| `gc [<path>]` | Remove objects no snapshot references. |

A snapshot is referred to by number (`3`, `0003`), by `--name` label, or as `latest`.

### Options

```
-n, --name <label>     Label for a new snapshot. Quote it if it contains spaces.
    --pattern <glob>   Filenames to include (default: parameter_*.xml). Supports * and ?.
    --all              Include every file, not just the default pattern.
-w, --workers <n>      Hashing threads (default: min(cores, 8), max 64).
    --version <id>     Snapshot to restore.
    --file <relpath>   Restore just one file out of the snapshot.
    --out <dir>        extract: where to write (default: ./extract-<id>).
    --since <id>       extract: baseline to compare against (default: the oldest snapshot).
    --force            extract: write into an output directory that is not empty.
    --merge            Restore without deleting files the snapshot lacks.
    --keep-last <n>    cleanup: keep the n most recent checkpoints.
    --no-auto-snapshot Skip the safety snapshot taken before a restore.
    --dry-run          Print what would happen and change nothing.
-y, --yes              Do not prompt before destructive actions.
    --verbose          More detail in list/show/diff/verify output.
```

Exit codes: `0` ok, `1` error, `2` usage, `3` store locked, `4` verify found damage.

## What gets captured

By default: files named `parameter_*.xml`, searched recursively from `<path>`.

Never captured: the `.paramsnap` store itself, dot-directories such as `.git`, dotfiles, and
symlinks (following them could leave the tree, and restoring one would replace the link with a
regular file). Skipped symlinks are reported, not silently dropped.

Pattern matching is case-sensitive and applies to the **filename only**, not the path — so
`parameter_a.xml.bak` and `PARAMETER_A.XML` are both excluded by the default pattern.

## How restore behaves

**Exact by default.** Restoring makes the tree match the snapshot: files the snapshot contains are
rewritten, and files it does *not* contain are deleted. Deletion is scoped to the snapshot's own
pattern, so with the default `parameter_*.xml` a stray `notes.txt` or `README` in the config
directory is never touched. Pass `--merge` to restore without deleting anything.

**Every restore takes a safety snapshot first.** Before changing anything, the current state is
checkpointed and its id is printed, so a wrong restore is one command away from being undone. These
appear as `[auto]` in `list`. Disable with `--no-auto-snapshot`.

**Files that already match are left alone**, including their modification times. Only files whose
contents actually differ are rewritten.

**A single-file snapshot never deletes.** `parametertool snap ./my_dir/parameter_motor.xml` records
just that file; restoring it writes that one file and nothing else. If `my_dir` already has a store,
the file joins that history rather than starting a second store in a subdirectory.

**Corrupt data is never restored.** Each object is re-hashed as it is read; a mismatch aborts the
restore with an error rather than writing damaged bytes into a live configuration.

Individual file writes are atomic (write to a temporary, then rename), so no reader ever sees a
half-written file. A restore as a whole is *not* transactional — if it is killed halfway you get a
partially restored tree. The safety snapshot is the recovery path for that case.

## Extracting a delta

`restore` puts a snapshot back into the configuration tree. `extract` instead copies the changed
files *out*, into a directory of your choosing, and never touches the tree or the store.

The workflow it exists for: snapshot the machine before anyone touches it, test and tune, snapshot
again once the parameters are right — then take just those files to the next machine, attach them
to a ticket, or hand them to someone for review.

```
$ parametertool extract ./my_dir --version 5
snapshot 1 "baseline" -> snapshot 5 "pid tuned"
  M  parameter_motor.xml
  R  parameter_old.xml   (removed; nothing extracted)
  A  subsys/parameter_encoder.xml
extracted 2 files into ./extract-5 (1 removed file not extracted)
```

**The baseline is the oldest snapshot in the store** — the starting snapshot taken before testing
began. The `[auto]` safety snapshots that pile up during a testing session never become the
baseline, because `restore` cannot run before a real snapshot exists, so the oldest is always one
somebody took on purpose. Pass `--since <id>` to measure from somewhere else.

**Only added and modified files are written**, with their subdirectory layout, mode, and
modification time preserved, so the output can be copied straight over another tree. Files
identical in both snapshots are skipped. A file the baseline had and the target does not is listed
as `R` and nothing is written for it — the output directory holds only real files, so applying a
delta never means picking the leftovers out by hand.

**The output directory defaults to `./extract-<id>`.** If it already exists and is not empty the
command stops, so a stale delta from an earlier run is never silently mixed into a new one; pass
`--force` to write anyway. Extracting into the configuration directory itself is refused — that is
what `restore` is for, and it takes a safety snapshot first.

Writes are atomic through a staging directory created inside the output directory, so extracting
onto a USB stick or a network share works even though it is a different filesystem from the store.

## The store

Snapshots live in `<path>/.paramsnap`, so history travels with a copy, move, or backup of the
configuration directory. Add `.paramsnap/` to your `.gitignore` if the config tree is version
controlled.

```
.paramsnap/
  version              store format
  next_id              snapshot counter
  lock                 present only while a command is writing
  tmp/                 staging for atomic renames
  objects/ab/cd12ef…   file contents, addressed by SHA-256
  snapshots/0001.json  manifest: which paths, which hashes, when, under what name
```

Contents are stored once and shared by every snapshot that references them, so re-snapshotting a
tree where three files changed costs three new objects, not the whole tree. That is also why there
is no compression option — deduplication does the same job without a decompress step on restore.

Manifests are plain JSON and safe to read with other tools. Deleting a snapshot removes its
manifest but not its objects; run `gc` to reclaim the space.

Only one process may write to a store at a time; a second one exits with code 3 rather than
interleaving writes. A lock whose owning process has died is reclaimed automatically. Read-only
commands (`list`, `show`, `diff`, `verify`) ignore the lock, so `verify` can inspect a store even
while something else is writing — at the cost of possibly reporting an object that is mid-write.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Unit tests cover SHA-256 (against the published NIST vectors, including the one-million-character
streaming case), the JSON reader/writer (escaping round-trips and malformed-input rejection), and
the glob matcher. `tests/e2e.sh` drives the real binary through eighteen scenarios: byte-exact round
trips, worker-count determinism, deduplication, pattern-scoped deletion, undoing a restore via its
safety snapshot, single-file isolation, `gc` correctness, corruption detection, lock contention,
extracting a delta and the guards on where it may be written, and that every `--dry-run` leaves both
tree and store untouched.

Run it standalone against any build with:

```sh
PARAMTOOL=./build/parametertool sh tests/e2e.sh
```

## Continuous integration and releases

`.github/workflows/ci.yml` runs on every push and pull request:

- build and test on Linux (GCC Release, GCC Debug, Clang Release) and macOS (Clang Release);
- the full RHEL 8 release build, so a packaging or toolchain break surfaces on a normal push rather
  than when someone cuts a tag;
- an ASan/UBSan/LeakSanitizer build, which runs the end-to-end suite and so covers the threaded
  snapshot path, not just the unit-tested pure code.

`.github/workflows/release.yml` runs on a `v*` tag and publishes a GitHub release with the RHEL 8
x86_64 tarball and its checksum attached. Cut one with:

```sh
git tag v1.0.0 && git push origin v1.0.0
```

The tag drives the version baked into the binary (`v1.0.0` → `parametertool 1.0.0`). Re-running the
workflow on an existing tag refreshes the assets instead of failing. It can also be run manually
from the Actions tab, where `publish: false` builds and uploads the artifact without creating a
release — useful for testing the pipeline.

### Building the RHEL 8 binary locally with Podman

You don't need a RHEL 8 machine, or even a C++20 compiler, to produce a RHEL 8 binary. The
`Containerfile` builds one and writes it straight into your working directory:

```sh
podman build -o type=local,dest=./dist .
```

That leaves you with:

```
dist/bin/parametertool                                  ready to scp to a RHEL 8 host
dist/parametertool-<version>-linux-x86_64-rhel8.tar.gz  the same thing, packaged
dist/…​.tar.gz.sha256
```

There is no `podman run` step and no bind mount, so nothing has to negotiate SELinux labels or
end up owned by root. Docker works with the identical command.

Or use the wrapper, which picks an engine, and falls back to `podman create` + `podman cp` on
podman older than 4.0 (RHEL 8.4 and earlier), where `--output` does not exist:

```sh
./packaging/podman-build.sh              # writes ./dist
./packaging/podman-build.sh /tmp/out     # or somewhere else
```

Useful knobs, on either the wrapper (as environment variables) or `podman build` (as
`--build-arg`):

| | |
| --- | --- |
| `PARAMTOOL_VERSION=1.2.3` | version reported by `--version` |
| `GCC_TOOLSET=12` | build with a different GCC Toolset |
| `UBI_TAG=8.6` | pin an older RHEL 8 minor version |
| `PLATFORM=linux/amd64` | see below |

**On an Apple Silicon Mac or another ARM host**, the default build produces an *aarch64* binary,
which will not run on an ordinary x86_64 RHEL 8 server. Build for the right architecture with:

```sh
PLATFORM=linux/amd64 ./packaging/podman-build.sh
# or: podman build --platform linux/amd64 -o type=local,dest=./dist .
```

This runs the compiler under emulation, so expect it to take a few minutes rather than seconds.
The wrapper inspects the finished binary and warns you if it isn't x86_64.

### The build script

Both the Containerfile and CI call `packaging/build-rhel8.sh`, so there is one source of truth for
how a release is produced. It installs a GCC Toolset (RHEL 8's own GCC 8.5 cannot compile C++20),
builds, runs the full test suite, strips the binary, and then refuses to package it if it still
links `libstdc++` dynamically or needs a glibc symbol newer than RHEL 8's 2.28. You can run it
directly against any RHEL 8 image:

```sh
podman run --rm -v "$PWD:/src:Z" -e PARAMTOOL_VERSION=1.0.0 \
    registry.access.redhat.com/ubi8/ubi:8.10 /src/packaging/build-rhel8.sh
```

The `:Z` is what relabels the bind mount for SELinux; without it, a RHEL host gives the container
permission denied on your source tree. The Containerfile route avoids the issue entirely.
