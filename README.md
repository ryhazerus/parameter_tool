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

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # optional
sudo cmake --install build                     # optional; installs to /usr/local/bin
```

Requires a C++20 compiler and a POSIX system (Linux or macOS). No third-party libraries: SHA-256,
JSON, glob matching, and the thread pool are all implemented against the standard library.

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
the glob matcher. `tests/e2e.sh` drives the real binary through fifteen scenarios: byte-exact round
trips, worker-count determinism, deduplication, pattern-scoped deletion, undoing a restore via its
safety snapshot, single-file isolation, `gc` correctness, corruption detection, lock contention, and
that every `--dry-run` leaves both tree and store untouched.

Run it standalone against any build with:

```sh
PARAMTOOL=./build/parametertool sh tests/e2e.sh
```
