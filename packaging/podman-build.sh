#!/bin/sh
#
# Builds parametertool for RHEL 8 in a container and drops the result in a local directory.
#
#   ./packaging/podman-build.sh [dest-dir]
#
# Uses `podman build --output` where available (podman 4.0+, and docker with BuildKit), which
# writes the files directly with no container to run or clean up. On older podman it falls back to
# building an image and copying the files out with `podman cp`.
#
# Environment:
#   ENGINE             container command to use (default: podman, else docker)
#   PARAMTOOL_VERSION  version baked into the binary (default: 0.0.0-dev)
#   GCC_TOOLSET        GCC Toolset major version to build with (default: 14)
#   PLATFORM           target architecture, e.g. linux/amd64 (default: the host's)
#
# Building on an Apple Silicon Mac or another ARM host produces an aarch64 binary, which will not
# run on an x86_64 RHEL 8 server. Set PLATFORM=linux/amd64 for that (slower: it runs the compiler
# under emulation).

set -eu

DEST=${1:-./dist}
VERSION=${PARAMTOOL_VERSION:-0.0.0-dev}
TOOLSET=${GCC_TOOLSET:-14}

if [ -n "${ENGINE:-}" ]; then
    :
elif command -v podman >/dev/null 2>&1; then
    ENGINE=podman
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
else
    echo "no container engine found: install podman (or docker)" >&2
    exit 1
fi

REPO_ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

PLATFORM_ARG=""
if [ -n "${PLATFORM:-}" ]; then
    PLATFORM_ARG="--platform $PLATFORM"
fi

echo "engine:   $ENGINE"
echo "version:  $VERSION"
echo "dest:     $DEST"
echo "platform: ${PLATFORM:-host default ($(uname -m))}"

BUILD_ARGS="--build-arg PARAMTOOL_VERSION=$VERSION --build-arg GCC_TOOLSET=$TOOLSET $PLATFORM_ARG"

# `podman build --output` landed in 4.0; docker needs BuildKit, which is the default in any
# currently supported version.
if $ENGINE build --help 2>&1 | grep -q -- '--output'; then
    echo "using --output (no container to run or clean up)"
    # shellcheck disable=SC2086
    $ENGINE build $BUILD_ARGS -o "type=local,dest=$DEST" -f Containerfile .
else
    echo "this $ENGINE has no --output; falling back to build + cp"
    IMAGE=parametertool-rhel8-build
    CONTAINER=parametertool-rhel8-export-$$

    # shellcheck disable=SC2086
    $ENGINE build $BUILD_ARGS --target build -t "$IMAGE" -f Containerfile .

    # `create` makes a container without starting it, purely so its filesystem can be read.
    $ENGINE create --name "$CONTAINER" "$IMAGE" /bin/true >/dev/null
    # Remove it even if the copy fails, so a bad run doesn't block the next one.
    trap '$ENGINE rm -f "$CONTAINER" >/dev/null 2>&1 || true' EXIT

    mkdir -p "$DEST"
    $ENGINE cp "$CONTAINER:/out/." "$DEST/"
fi

echo
echo "built:"
find "$DEST" -type f -exec ls -l {} +

# Report what was actually produced rather than what was hoped for — an ARM host silently yields an
# aarch64 binary, which is the wrong thing for an x86_64 RHEL 8 server.
echo
if command -v file >/dev/null 2>&1; then
    ARCH_DESC=$(file -b "$DEST/bin/parametertool" | cut -d, -f1-2)
    echo "binary: $ARCH_DESC"
    case "$ARCH_DESC" in
        *x86-64*) ;;
        *) echo
           echo "NOTE: this is not an x86_64 binary. If it is destined for an x86_64 RHEL 8"
           echo "      server, rebuild with: PLATFORM=linux/amd64 $0 $DEST" ;;
    esac
fi
echo
echo "This is a Linux/RHEL 8 binary — run it there, not on macOS:"
echo "  scp $DEST/bin/parametertool user@rhel8-host:/tmp/ && ssh user@rhel8-host /tmp/parametertool --help"
