#!/bin/sh
# End-to-end tests for parametertool. Drives the real binary against a throwaway tree.
#
# Run directly, or via `ctest`, which sets PARAMTOOL to the freshly built binary.
set -u

PT="${PARAMTOOL:-./build/parametertool}"
if [ ! -x "$PT" ]; then
    echo "cannot find parametertool at $PT (set PARAMTOOL=/path/to/parametertool)" >&2
    exit 1
fi
PT=$(cd "$(dirname "$PT")" && pwd)/$(basename "$PT")

WORK=$(mktemp -d "${TMPDIR:-/tmp}/paramtool-e2e.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

FAILURES=0
CASE=""
CASE_BASE=0

start() { CASE="$1"; CASE_BASE=$FAILURES; }
bad()   { printf '  FAIL  %s: %s\n' "$CASE" "$1" >&2; FAILURES=$((FAILURES + 1)); }

# Reports the current case as passed only if it added no failures of its own. Gating on the global
# count instead would silence every later case once anything failed.
ok() {
    if [ "$FAILURES" -eq "$CASE_BASE" ]; then
        printf '  ok    %s\n' "$CASE"
    fi
}

# Asserts the exit status of a command.
expect_status() {
    want=$1; shift
    "$@" >/dev/null 2>&1
    got=$?
    [ "$got" -eq "$want" ] || bad "expected exit $want from '$*', got $got"
}

# Builds a fresh config tree with `n` parameter files plus decoys.
make_tree() {
    dir=$1; n=$2
    rm -rf "$dir"; mkdir -p "$dir/subsys"
    i=1
    while [ "$i" -le "$n" ]; do
        printf '<?xml version="1.0"?>\n<parameters><param name="p%s" value="%s"/></parameters>\n' \
            "$i" "$((i * 7))" > "$dir/parameter_p$i.xml"
        i=$((i + 1))
    done
    printf '<?xml version="1.0"?>\n<parameters><param name="c" value="1"/></parameters>\n' \
        > "$dir/subsys/parameter_c.xml"
    echo "field notes" > "$dir/notes.txt"
    echo "editor backup" > "$dir/parameter_p1.xml.bak"
}

# Compares two trees ignoring the store itself.
trees_match() {
    diff -r -x .paramsnap "$1" "$2" >/dev/null 2>&1
}

echo "e2e: $PT"

# --- 1. round trip -----------------------------------------------------------
start "round trip restores a mutated tree byte for byte"
D=$WORK/rt
make_tree "$D" 20
cp -R "$D" "$WORK/rt-pristine"
"$PT" snap "$D" --name "baseline" >/dev/null
echo '<clobbered/>' > "$D/parameter_p3.xml"
echo '<clobbered/>' > "$D/subsys/parameter_c.xml"
rm "$D/parameter_p7.xml"
"$PT" restore "$D" --version 1 --yes >/dev/null
if trees_match "$WORK/rt-pristine" "$D"; then ok; else bad "tree differs from the original"; fi

# --- 2. worker count does not change the result ------------------------------
start "snapshots are identical at 1 and 8 workers"
D=$WORK/det
make_tree "$D" 30
"$PT" snap "$D" --workers 1 >/dev/null
"$PT" snap "$D" --workers 8 >/dev/null
A=$(grep -E '"(path|hash)"' "$D/.paramsnap/snapshots/0001.json")
B=$(grep -E '"(path|hash)"' "$D/.paramsnap/snapshots/0002.json")
if [ "$A" = "$B" ]; then ok; else bad "manifests differ between worker counts"; fi

# --- 3. deduplication --------------------------------------------------------
start "re-snapshotting one changed file stores exactly one new object"
D=$WORK/dedup
make_tree "$D" 50
"$PT" snap "$D" >/dev/null
BEFORE=$(find "$D/.paramsnap/objects" -type f | wc -l | tr -d ' ')
echo '<changed/>' > "$D/parameter_p9.xml"
"$PT" snap "$D" >/dev/null
AFTER=$(find "$D/.paramsnap/objects" -type f | wc -l | tr -d ' ')
if [ "$((AFTER - BEFORE))" -eq 1 ]; then ok; else bad "expected 1 new object, got $((AFTER - BEFORE))"; fi

# --- 4. exact restore is scoped to the snapshot's own pattern -----------------
start "exact restore removes new parameter files but spares unrelated ones"
D=$WORK/exact
make_tree "$D" 10
"$PT" snap "$D" >/dev/null
echo '<new/>' > "$D/parameter_new.xml"
echo "later note" > "$D/notes2.txt"
"$PT" restore "$D" --version 1 --yes >/dev/null
[ ! -e "$D/parameter_new.xml" ] || bad "parameter_new.xml should have been deleted"
[ -e "$D/notes2.txt" ]          || bad "notes2.txt must not be touched"
[ -e "$D/notes.txt" ]           || bad "notes.txt must not be touched"
[ -e "$D/parameter_p1.xml.bak" ]|| bad "the .bak file must not be touched"
ok

# --- 5. the safety snapshot can undo a restore -------------------------------
start "auto-snapshot before restore can be restored to undo it"
D=$WORK/undo
make_tree "$D" 5
"$PT" snap "$D" >/dev/null
echo '<new/>' > "$D/parameter_new.xml"
"$PT" restore "$D" --version 1 --yes >/dev/null     # deletes parameter_new.xml, saves auto as 2
[ ! -e "$D/parameter_new.xml" ] || bad "setup: file should be gone after the exact restore"
"$PT" restore "$D" --version 2 --yes >/dev/null     # undo
if [ -e "$D/parameter_new.xml" ]; then ok; else bad "undo did not bring the file back"; fi

# --- 6. single-file scope never reaches past its one file --------------------
start "single-file snapshot restores one file and deletes nothing"
D=$WORK/single
make_tree "$D" 8
"$PT" snap "$D" >/dev/null
"$PT" snap "$D/subsys/parameter_c.xml" --name "just c" >/dev/null
[ ! -d "$D/subsys/.paramsnap" ] || bad "a single-file snapshot must not start a second store"
echo '<clobbered/>' > "$D/subsys/parameter_c.xml"
echo '<clobbered/>' > "$D/parameter_p2.xml"
echo '<extra/>'     > "$D/parameter_extra.xml"
"$PT" restore "$D" --version 2 --yes >/dev/null
grep -q 'value="1"' "$D/subsys/parameter_c.xml" || bad "the single file was not restored"
grep -q 'clobbered' "$D/parameter_p2.xml"       || bad "an unrelated file was modified"
[ -e "$D/parameter_extra.xml" ]                 || bad "single-file restore must never delete"
ok

# --- 7. nested paths ---------------------------------------------------------
start "files in subdirectories survive a snapshot/restore cycle"
D=$WORK/nested
make_tree "$D" 3
mkdir -p "$D/a/b/c"
echo '<deep/>' > "$D/a/b/c/parameter_deep.xml"
"$PT" snap "$D" >/dev/null
rm -rf "$D/a"
"$PT" restore "$D" --version 1 --yes >/dev/null
if [ -f "$D/a/b/c/parameter_deep.xml" ]; then ok; else bad "the nested file was not restored"; fi

# --- 8. restore --file -------------------------------------------------------
start "restore --file touches only the named file"
D=$WORK/onefile
make_tree "$D" 6
"$PT" snap "$D" >/dev/null
echo '<clobbered/>' > "$D/parameter_p1.xml"
echo '<clobbered/>' > "$D/parameter_p2.xml"
"$PT" restore "$D" --version 1 --file parameter_p1.xml --yes >/dev/null
grep -q 'value="7"' "$D/parameter_p1.xml" || bad "the named file was not restored"
grep -q 'clobbered' "$D/parameter_p2.xml" || bad "another file was restored too"
expect_status 1 "$PT" restore "$D" --version 1 --file ../escape.xml
expect_status 1 "$PT" restore "$D" --version 1 --file no_such_file.xml
ok

# --- 9. delete and gc --------------------------------------------------------
start "delete leaves objects behind and gc reclaims exactly the orphans"
D=$WORK/gc
make_tree "$D" 5
"$PT" snap "$D" >/dev/null
echo '<only-in-two/>' > "$D/parameter_unique.xml"
"$PT" snap "$D" >/dev/null
"$PT" delete "$D" 2 --yes >/dev/null
BEFORE=$(find "$D/.paramsnap/objects" -type f | wc -l | tr -d ' ')
"$PT" gc "$D" --dry-run >/dev/null
MID=$(find "$D/.paramsnap/objects" -type f | wc -l | tr -d ' ')
[ "$BEFORE" -eq "$MID" ] || bad "gc --dry-run removed something"
"$PT" gc "$D" >/dev/null
AFTER=$(find "$D/.paramsnap/objects" -type f | wc -l | tr -d ' ')
[ "$AFTER" -lt "$BEFORE" ] || bad "gc reclaimed nothing"
expect_status 0 "$PT" verify "$D"
"$PT" restore "$D" --version 1 --yes >/dev/null 2>&1 || bad "snapshot 1 broke after gc"
ok

# --- 10. corruption ----------------------------------------------------------
start "verify reports a corrupted object and restore refuses to use it"
D=$WORK/corrupt
make_tree "$D" 4
"$PT" snap "$D" >/dev/null
OBJ=$(find "$D/.paramsnap/objects" -type f | head -1)
chmod u+w "$OBJ"
printf 'XXXX' >> "$OBJ"
expect_status 4 "$PT" verify "$D"
# Force restore to actually need that object.
for f in "$D"/parameter_*.xml "$D"/subsys/*.xml; do echo '<clobbered/>' > "$f"; done
expect_status 1 "$PT" restore "$D" --version 1 --yes --no-auto-snapshot
ok

# --- 11. locking -------------------------------------------------------------
start "a lock held by a live process blocks writers, a stale one is reclaimed"
D=$WORK/lock
make_tree "$D" 3
"$PT" snap "$D" >/dev/null
sleep 30 &
LIVE=$!
echo "$LIVE" > "$D/.paramsnap/lock"
expect_status 3 "$PT" snap "$D"
expect_status 3 "$PT" gc "$D"
expect_status 0 "$PT" list "$D"        # read-only commands are unaffected
kill "$LIVE" 2>/dev/null
wait "$LIVE" 2>/dev/null
echo "999999" > "$D/.paramsnap/lock"   # a pid that cannot be running
expect_status 0 "$PT" snap "$D"
ok

# --- 12. dry runs change nothing ---------------------------------------------
start "every --dry-run leaves the tree and the store untouched"
D=$WORK/dry
make_tree "$D" 6
"$PT" snap "$D" >/dev/null
echo '<changed/>' > "$D/parameter_p2.xml"
SIG_BEFORE=$(find "$D" -type f | sort | xargs cksum | cksum)
"$PT" restore "$D" --version 1 --dry-run >/dev/null
"$PT" cleanup "$D" --dry-run >/dev/null
"$PT" delete "$D" 1 --dry-run >/dev/null
"$PT" gc "$D" --dry-run >/dev/null
"$PT" snap "$D" --dry-run >/dev/null
SIG_AFTER=$(find "$D" -type f | sort | xargs cksum | cksum)
if [ "$SIG_BEFORE" = "$SIG_AFTER" ]; then ok; else bad "a dry run modified something"; fi

# --- 13. cleanup -------------------------------------------------------------
start "cleanup honors --keep-last and then clears everything"
D=$WORK/clean
make_tree "$D" 3
for n in 1 2 3 4 5; do
    echo "<v$n/>" > "$D/parameter_p1.xml"
    "$PT" snap "$D" --name "v$n" >/dev/null
done
"$PT" cleanup "$D" --keep-last 2 --yes >/dev/null
LEFT=$(find "$D/.paramsnap/snapshots" -name '*.json' | wc -l | tr -d ' ')
[ "$LEFT" -eq 2 ] || bad "expected 2 snapshots after --keep-last 2, got $LEFT"
"$PT" cleanup "$D" --yes >/dev/null
LEFT=$(find "$D/.paramsnap/snapshots" -name '*.json' | wc -l | tr -d ' ')
[ "$LEFT" -eq 0 ] || bad "expected 0 snapshots after cleanup, got $LEFT"
expect_status 0 "$PT" list "$D"
ok

# --- 14. refusing without a terminal -----------------------------------------
start "destructive commands refuse rather than assume yes when piped"
D=$WORK/noconfirm
make_tree "$D" 3
"$PT" snap "$D" >/dev/null
"$PT" cleanup "$D" < /dev/null >/dev/null 2>&1
LEFT=$(find "$D/.paramsnap/snapshots" -name '*.json' | wc -l | tr -d ' ')
if [ "$LEFT" -eq 1 ]; then ok; else bad "cleanup deleted snapshots without confirmation"; fi

# --- 15. usage errors --------------------------------------------------------
start "bad command lines exit 2 with a usage error"
D=$WORK/usage
make_tree "$D" 2
"$PT" snap "$D" >/dev/null
expect_status 2 "$PT" bogus-command
expect_status 2 "$PT" snap "$D" --bogus-flag
expect_status 2 "$PT" snap "$D" --name version 1     # unquoted name
expect_status 2 "$PT" restore "$D"                   # missing --version
expect_status 2 "$PT" show "$D"                      # missing id
expect_status 2 "$PT" list "$D" --file x             # option on the wrong command
expect_status 0 "$PT" --help
expect_status 0 "$PT" --version
ok

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "e2e: all passed"
    exit 0
fi
echo "e2e: $FAILURES failure(s)" >&2
exit 1
