#!/usr/bin/env bash
# Prepare all generated dependencies inside this project, without sudo.
set -euo pipefail
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$project"
check_sources=false
if [[ "${1:-}" == --check-sources ]]; then check_sources=true; shift; fi
[[ "$#" -le 2 ]] || { echo 'Usage: prepare-local.sh [--check-sources] [LIGHTNING_SOURCE] [FIRECRACKER_SOURCE]' >&2; exit 2; }
lightning_source=${1:-${LIGHTNING_SOURCE:-"$HOME/lightning"}}
firecracker_source=${2:-${FIRECRACKER_SOURCE:-}}
[[ -f "$lightning_source/src/lightning/Cargo.toml" ]] || { echo "Invalid Lightning source: $lightning_source" >&2; exit 2; }
lightning_source=$(cd -- "$lightning_source" && pwd -P)
if [[ -n "$firecracker_source" ]]; then
    [[ -f "$firecracker_source/src/firecracker/Cargo.toml" ]] || { echo "Invalid Firecracker source: $firecracker_source" >&2; exit 2; }
    firecracker_source=$(cd -- "$firecracker_source" && pwd -P)
    [[ "$firecracker_source" != "$lightning_source" && ! -d "$firecracker_source/src/lightning" ]] || {
        echo 'Use an independent upstream Firecracker source tree, not the Lightning checkout.' >&2; exit 2;
    }
fi
if "$check_sources"; then
    printf 'Lightning source: %s\nFirecracker source: %s\n' "$lightning_source" "${firecracker_source:-pinned upstream download}"
    exit 0
fi
mkdir -p build/tmp build/deps/downloads build/deps/lightning build/deps/firecracker build/guest results/full-suite
export TMPDIR="$project/build/tmp" CARGO_HOME="$project/build/deps/cargo" RUSTUP_HOME="$project/build/deps/rustup"
export RUSTUP_TOOLCHAIN=1.97.0 CCACHE_DISABLE=1
for tool in g++ gcc make curl tar xz gzip cpio cmake clang ld.lld llvm-ar llvm-nm llvm-objcopy llvm-strip flex bison bc pkg-config git jq flock; do
    command -v "$tool" >/dev/null || { echo "Install prerequisite: $tool" >&2; exit 2; }
done
download() {
    local url=$1 destination=$2
    if [[ ! -s "$destination" ]]; then
        curl --fail --location --silent --show-error --retry 2 "$url" -o "$destination.part"
        mv "$destination.part" "$destination"
    fi
}
if [[ ! -x "$CARGO_HOME/bin/rustup" ]]; then
    download https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init build/deps/downloads/rustup-init
    chmod +x build/deps/downloads/rustup-init
    RUSTUP_INIT_SKIP_PATH_CHECK=yes build/deps/downloads/rustup-init -y --no-modify-path --profile minimal \
        --default-toolchain 1.97.0 > results/full-suite/rust-install.log 2>&1
fi
export PATH="$CARGO_HOME/bin:$PATH"
rustup toolchain install 1.97.0 --profile minimal >> results/full-suite/rust-install.log 2>&1
copy_source() {
    local source=$1 backend=$2 staging item
    [[ "$source" != "$project/build/deps/$backend" ]] || return 0
    staging=$(mktemp -d "$project/build/deps/$backend-source-XXXXXX")
    local files=()
    for item in Cargo.toml Cargo.lock rust-toolchain.toml .cargo src resources tools LICENSE NOTICE README.md; do
        if [[ -e "$source/$item" ]]; then files+=("$item"); fi
    done
    tar -C "$source" -cf - "${files[@]}" | tar -C "$staging" -xmf -
    rm -rf -- "$project/build/deps/$backend"
    mv -- "$staging" "$project/build/deps/$backend"
}
copy_source "$lightning_source" lightning
if [[ -n "$firecracker_source" ]]; then
    copy_source "$firecracker_source" firecracker
else
    download https://codeload.github.com/firecracker-microvm/firecracker/tar.gz/fd9fc7a9f164b73002d22065d17c150d8c262d91 \
        build/deps/downloads/firecracker.tar.gz
    # Re-extract so a previous custom checkout cannot become the pinned baseline.
    rm -rf -- "$project/build/deps/firecracker"
    mkdir -p build/deps/firecracker
    tar -xzmf build/deps/downloads/firecracker.tar.gz -C build/deps/firecracker --strip-components=1
fi
download https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.128.tar.xz build/deps/downloads/linux-6.1.128.tar.xz
if [[ ! -f build/deps/linux-6.1.128/Makefile ]]; then
    tar -xJf build/deps/downloads/linux-6.1.128.tar.xz -C build/deps
fi
for backend in lightning firecracker; do
    cargo build --locked --release --jobs "${VM_BENCH_JOBS:-4}" \
        --manifest-path "build/deps/$backend/Cargo.toml" --target-dir "build/vmm/$backend" \
        --config 'build.rustflags=["-Ccodegen-units=1"]' \
        --config 'env.AWS_LC_SYS_NO_JITTER_ENTROPY="1"' \
        --config 'env.AWS_LC_SYS_CFLAGS="-DMY_ASSEMBLER_IS_TOO_OLD_FOR_512AVX"' \
        -p "$backend" > "results/full-suite/$backend-build.log" 2>&1
done
cp build/deps/lightning/resources/guest_configs/microvm-kernel-ci-x86_64-6.1.config build/deps/linux-6.1.128/.config
# This minimal guest does not need a certificate keyring. Avoid the old ENGINE API.
build/deps/linux-6.1.128/scripts/config --file build/deps/linux-6.1.128/.config --disable SYSTEM_TRUSTED_KEYRING
make -C build/deps/linux-6.1.128 LLVM=1 olddefconfig > results/full-suite/kernel-build-clang.log 2>&1
make -C build/deps/linux-6.1.128 LLVM=1 -j"${VM_BENCH_JOBS:-4}" vmlinux >> results/full-suite/kernel-build-clang.log 2>&1
cp build/deps/linux-6.1.128/vmlinux build/guest/vmlinux
cp build/deps/linux-6.1.128/.config build/guest/kernel.config
build/deps/lightning/tools/lightning/build-guest.sh "$project/build/guest" > results/full-suite/guest-build.log 2>&1
make -B check > results/full-suite/controller-build-test.log 2>&1
jq -n --arg rust "$(rustc --version)" --arg cxx "$(g++ --version | head -n 1)" \
    --arg clang "$(clang --version | head -n 1)" --arg root "$project" \
    --arg lightning_source "$lightning_source" --arg firecracker_source "${firecracker_source:-pinned upstream download}" \
    --arg lightning_commit "$(git -C "$lightning_source" rev-parse HEAD 2>/dev/null || true)" \
    --arg firecracker_commit "$(if [[ -n "$firecracker_source" ]]; then git -C "$firecracker_source" rev-parse HEAD 2>/dev/null || true; else printf fd9fc7a9f164b73002d22065d17c150d8c262d91; fi)" \
    '{rust:$rust,cxx:$cxx,kernel_compiler:$clang,kernel_version:"6.1.128",rust_codegen_units:1,
      kernel_configuration:"Lightning CI x86_64 Linux 6.1 config; SYSTEM_TRUSTED_KEYRING disabled; LLVM=1",
      lightning_source:$lightning_source,firecracker_source:$firecracker_source,
      lightning_commit:$lightning_commit,firecracker_commit:$firecracker_commit,generated_files_root:$root}' \
    > results/full-suite/dependencies.json
find build/deps/lightning build/deps/firecracker -type f -print0 | sort -z | xargs -0 sha256sum > results/full-suite/sources.sha256
sha256sum build/deps/downloads/linux-6.1.128.tar.xz >> results/full-suite/sources.sha256
printf 'Prepared both VMMs, kernel, minimal guest and controller. Run scripts/run-full-suite.sh from a KVM-enabled terminal.\n'
