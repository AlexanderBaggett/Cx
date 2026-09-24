#!/bin/sh
# Build the two compilers the benchmarks compare, from this repository:
#
#   upstream/  unmodified LLVM/Clang 23.1.2  ->  $REF_BUILD/bin/clang   (reference "C" compiler)
#   cx/        the Cx compiler sources      ->  $CX_BUILD/bin/clang    ("Cx" compiler)
#
# Both use exactly the same CMake configuration, so any difference between the
# two compilers comes from the sources under cx/.
#
#   tools/build-compilers.sh [ref|cx|both]      (default: both)
#
# Environment:
#   REF_BUILD  build directory for upstream/  (default: ../ref-build next to the repo)
#   CX_BUILD   build directory for cx/        (default: ../cx-build  next to the repo)
#   HOST_CC / HOST_CXX  compilers used to build clang (default: clang / clang++ on PATH)
#   HOST_LD    linker passed with --ld-path (default: ld.lld if found, else the compiler default)
#   JOBS       parallel jobs (default: nproc)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REF_BUILD=${REF_BUILD:-$(dirname "$ROOT")/ref-build}
CX_BUILD=${CX_BUILD:-$(dirname "$ROOT")/cx-build}
HOST_CC=${HOST_CC:-clang}
HOST_CXX=${HOST_CXX:-clang++}
JOBS=${JOBS:-$(nproc)}
LDFLAGS_OPT=""
HOST_LD=${HOST_LD:-$(command -v ld.lld || true)}
[ -n "$HOST_LD" ] && LDFLAGS_OPT="--ld-path=$HOST_LD"

build() {  # build <source root> <build dir>
    src=$1; out=$2
    mkdir -p "$out"
    cmake -G Ninja -S "$src/llvm" -B "$out" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 \
        -DCMAKE_C_COMPILER="$HOST_CC" -DCMAKE_CXX_COMPILER="$HOST_CXX" \
        -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS_OPT" -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_OPT" -DCMAKE_MODULE_LINKER_FLAGS="$LDFLAGS_OPT" \
        -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_INCLUDE_EXAMPLES=OFF -DCLANG_INCLUDE_TESTS=OFF \
        -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBXML2=OFF \
        -DLLVM_PARALLEL_LINK_JOBS=1 > "$out/cmake.log"
    ninja -C "$out" -j "$JOBS" clang
    "$out/bin/clang" --version | head -1
}

what=${1:-both}
case $what in
    ref)  "$ROOT/tools/check-upstream.sh"; build "$ROOT/upstream" "$REF_BUILD" ;;
    cx)   build "$ROOT/cx" "$CX_BUILD" ;;
    both) "$ROOT/tools/check-upstream.sh"; build "$ROOT/upstream" "$REF_BUILD"; build "$ROOT/cx" "$CX_BUILD" ;;
    *)    echo "usage: $0 [ref|cx|both]" >&2; exit 2 ;;
esac
