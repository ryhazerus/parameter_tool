#!/usr/bin/env bash
#
# Builds a release tarball of parametertool for RHEL 8.
#
# Runs *inside* a RHEL 8 / UBI 8 container so the binary links against RHEL 8's glibc (2.28) and
# will therefore run on any RHEL 8 host. Building on a newer distro and shipping that binary here
# would fail at load time with a GLIBC version error, which is the whole reason this script exists.
#
# The C++20 standard library comes from a GCC Toolset compiler and is linked statically, so the
# result does not need the toolset installed on the target machine either.
#
# Usage (from a checkout, with a container runtime running):
#
#   docker run --rm -v "$PWD:/src" registry.access.redhat.com/ubi8/ubi:8.10 \
#       /src/packaging/build-rhel8.sh
#
# Environment:
#   PARAMTOOL_VERSION  version string baked into the binary (default: 0.0.0-dev)
#   SRC_DIR            source checkout inside the container (default: /src)
#   OUT_DIR            where the tarball is written  (default: $SRC_DIR/dist)
#   JOBS               parallel build jobs (default: nproc)

set -euo pipefail

VERSION="${PARAMTOOL_VERSION:-0.0.0-dev}"
SRC_DIR="${SRC_DIR:-/src}"
OUT_DIR="${OUT_DIR:-$SRC_DIR/dist}"
BUILD_DIR="${BUILD_DIR:-/tmp/paramtool-build}"
JOBS="${JOBS:-$(nproc)}"

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

say "host"
if [ -r /etc/redhat-release ]; then
    cat /etc/redhat-release
else
    echo "WARNING: /etc/redhat-release is missing — this does not look like RHEL/UBI." >&2
    echo "         The resulting binary may not run on RHEL 8." >&2
fi
echo "glibc: $(ldd --version | head -1)"
echo "arch:  $(uname -m)"

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
# RHEL 8's system GCC is 8.5, which does not accept -std=c++20. A GCC Toolset provides a modern
# compiler alongside it. Try newest first so we get the best available, rather than pinning a
# version that may vanish from the repos later.
#
# SKIP_DEPS=1 means the caller already installed the toolchain — the Containerfile does this in a
# cached layer so repeat builds don't re-run dnf.
if [ "${SKIP_DEPS:-0}" = "1" ]; then
    say "using preinstalled build tools"
else
    say "installing build tools"
    # Note: do NOT add `coreutils` here. UBI 8 ships `coreutils-single` (a multi-call binary that
    # already provides sha256sum, install, stat and friends), and the full package conflicts with it.
    dnf -y --setopt=install_weak_deps=False install \
        cmake make tar gzip findutils diffutils >/dev/null

    for v in 14 13 12 11 10; do
        if dnf -y --setopt=install_weak_deps=False install "gcc-toolset-$v-gcc-c++" >/dev/null 2>&1; then
            break
        fi
    done
fi

# Detect what is actually present, whether this script installed it or the image already had it.
TOOLSET=""
for v in 14 13 12 11 10; do
    if [ -r "/opt/rh/gcc-toolset-$v/enable" ]; then
        TOOLSET="gcc-toolset-$v"
        break
    fi
done

if [ -n "$TOOLSET" ]; then
    echo "using $TOOLSET"
    # shellcheck disable=SC1090
    source "/opt/rh/$TOOLSET/enable"
else
    echo "WARNING: no GCC Toolset available; falling back to the system compiler." >&2
    TOOLSET="system-gcc"
fi

echo "compiler: $(g++ --version | head -1)"
echo "cmake:    $(cmake --version | head -1)"

if ! g++ -std=c++20 -x c++ -fsyntax-only - <<<'#include <filesystem>
int main() { return 0; }' 2>/dev/null; then
    echo "ERROR: the available compiler cannot build C++20 with <filesystem>." >&2
    echo "       Install a GCC Toolset (gcc-toolset-12 or newer) in the build image." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
# Static libstdc++/libgcc: the toolset's C++ runtime is newer than the one RHEL 8 ships, so a
# dynamically linked binary would need the toolset present on every target host.
say "building parametertool $VERSION"
rm -rf "$BUILD_DIR"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPARAMTOOL_VERSION="$VERSION" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build "$BUILD_DIR" -j "$JOBS"

say "running the test suite"
ctest --test-dir "$BUILD_DIR" --output-on-failure

# ---------------------------------------------------------------------------
# Confirm the binary is actually portable before we ship it
# ---------------------------------------------------------------------------
say "stripping and checking runtime dependencies"
BIN="$BUILD_DIR/parametertool"
BEFORE=$(stat -c %s "$BIN")
strip --strip-unneeded "$BIN"
echo "stripped: $BEFORE -> $(stat -c %s "$BIN") bytes"
ldd "$BIN" || true

if ldd "$BIN" | grep -q libstdc++; then
    echo "ERROR: the binary still links libstdc++ dynamically; it will not run on a stock" >&2
    echo "       RHEL 8 host without the GCC Toolset runtime installed." >&2
    exit 1
fi

# Highest glibc symbol version required. RHEL 8 provides 2.28, so anything above that would fail
# to load on the very platform we are targeting.
MAXGLIBC=$(objdump -T "$BIN" 2>/dev/null | grep -o 'GLIBC_[0-9]\+\.[0-9]\+' | sort -u -t. -k2,2n | tail -1 || true)
echo "highest glibc symbol required: ${MAXGLIBC:-none}"
case "$MAXGLIBC" in
    ""|GLIBC_2.[0-9]|GLIBC_2.1[0-9]|GLIBC_2.2[0-8]) ;;
    *)
        echo "ERROR: $BIN requires $MAXGLIBC, newer than RHEL 8's 2.28." >&2
        exit 1
        ;;
esac

"$BIN" --version

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------
ARCH=$(uname -m)
NAME="parametertool-${VERSION}-linux-${ARCH}-rhel8"
STAGE="$BUILD_DIR/stage/$NAME"

say "packaging $NAME.tar.gz"
rm -rf "$BUILD_DIR/stage"
mkdir -p "$STAGE/bin"
install -m 0755 "$BIN" "$STAGE/bin/parametertool"
[ -f "$SRC_DIR/README.md" ] && install -m 0644 "$SRC_DIR/README.md" "$STAGE/README.md"
[ -f "$SRC_DIR/LICENSE" ] && install -m 0644 "$SRC_DIR/LICENSE" "$STAGE/LICENSE"

cat > "$STAGE/BUILD-INFO" <<INFO
parametertool $VERSION
built    $(date -u +%Y-%m-%dT%H:%M:%SZ)
platform $(cat /etc/redhat-release 2>/dev/null || uname -sr)
arch     $ARCH
compiler $(g++ --version | head -1) ($TOOLSET)
glibc    $(ldd --version | head -1 | grep -o '[0-9]\+\.[0-9]\+$')

Install by copying bin/parametertool anywhere on your PATH, for example:
  sudo install -m 0755 bin/parametertool /usr/local/bin/parametertool
INFO

mkdir -p "$OUT_DIR"
tar -czf "$OUT_DIR/$NAME.tar.gz" -C "$BUILD_DIR/stage" "$NAME"
( cd "$OUT_DIR" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )

# Don't leave root-owned artifacts behind when this is run locally against a bind mount.
if [ -d "$SRC_DIR" ]; then
    OWNER=$(stat -c '%u:%g' "$SRC_DIR")
    chown -R "$OWNER" "$OUT_DIR" 2>/dev/null || true
fi

say "done"
ls -l "$OUT_DIR"
