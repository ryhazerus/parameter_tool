# Builds parametertool for RHEL 8 and emits the binary to your local directory.
#
#   podman build -o type=local,dest=./dist .
#
# That writes ./dist/bin/parametertool plus the release tarball. There is no `podman run` step and
# no bind mount, so nothing has to negotiate SELinux labels or file ownership.
#
# Older podman (before 4.0) has no --output; see packaging/podman-build.sh, which detects that and
# falls back to `podman create` + `podman cp`.
#
# Useful arguments:
#   --build-arg PARAMTOOL_VERSION=1.2.3   version reported by `parametertool --version`
#   --build-arg GCC_TOOLSET=12            use a different GCC Toolset
#   --build-arg UBI_TAG=8.6               pin an older RHEL 8 minor version
#   --target build                        stop at the build image instead of exporting files
#   --platform linux/amd64                cross-build for x86_64 from an ARM host (Apple Silicon)
#
# On an ARM host this produces an aarch64 binary by default, which will NOT run on an x86_64 RHEL 8
# server. Pass --platform linux/amd64 when the target is an ordinary Intel/AMD RHEL box.

ARG UBI_TAG=8.10
FROM registry.access.redhat.com/ubi8/ubi:${UBI_TAG} AS build

# Toolchain lives in its own layer so editing source doesn't re-run dnf on every build.
# RHEL 8's system GCC is 8.5 and cannot compile C++20; the GCC Toolset supplies a modern compiler.
ARG GCC_TOOLSET=14
RUN dnf -y --setopt=install_weak_deps=False install \
        cmake make tar gzip findutils diffutils binutils \
        "gcc-toolset-${GCC_TOOLSET}-gcc-c++" \
    && dnf clean all \
    && rm -rf /var/cache/dnf

ARG PARAMTOOL_VERSION=0.0.0-dev
ENV PARAMTOOL_VERSION=${PARAMTOOL_VERSION}

COPY . /src

# One source of truth: the same script CI runs. It builds, runs the full test suite, strips the
# binary, and refuses to package anything that would not load on a stock RHEL 8 host.
RUN SKIP_DEPS=1 SRC_DIR=/src OUT_DIR=/out BUILD_DIR=/tmp/build /src/packaging/build-rhel8.sh \
    && install -D -m 0755 /tmp/build/parametertool /out/bin/parametertool

# Final stage is nothing but the artifacts, so `--output type=local` drops exactly these files
# into your directory and not a whole root filesystem.
FROM scratch AS artifact
COPY --from=build /out/ /
