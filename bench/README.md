# Cx vs C: head-to-head benchmarks

This suite compares two toolchains on the same algorithms:

| Toolchain | Compiler | Sources |
|---|---|---|
| **C** (reference) | The unchanged official LLVM/Clang **23.1.2** release | `src/*.c` |
| **Cx** | Clang built **from this repository** (the Cx compiler), plus `CX_FLAGS` | `src/*.c`, or a `cx/NAME.cx` override where one exists |

Until the repository's compiler diverges from upstream, both toolchains compile identical source with identical compiler code. Results should then match within run-to-run noise, and the suite starts out as an **A/A test**. As Cx features land in the compiler (see `docs/design/cx-language-spec.md` §20), and as Cx-specific versions are added under `cx/`, the differences show up here.

## Layout

| Path | What |
|---|---|
| `harness/bench.h` | The benchmark API (`struct bench`), deterministic PRNG, overflow-free checksum mixing |
| `harness/main.c` | Driver: `bench --list`, `bench NAME [--iters K]`, `bench --all` |
| `harness/list_{ops,ds,alg,kern}.h` | Registry, one file per category |
| `src/ops_*.c` | Core operations: integer, floating point, branches, calls, memory, strings |
| `src/ds_*.c` | Data structures |
| `src/alg_*.c` | Algorithms: sorting, searching, graphs, dynamic programming, backtracking |
| `src/kern_*.c` | Compute kernels: linear algebra, simulation, codecs, hashing |
| `cx/NAME.cx` | Optional Cx-specific replacement for `src/NAME.c` (Cx toolchain only) |
| `tools/run.py` | Runs every benchmark × toolchain × round, pinned to one CPU |
| `tools/report.py` | Markdown summary with a verdict per benchmark |
| `tools/codegen_compare.sh` | Checks whether both compilers emit identical object code |
| `tools/check_subset.sh` | Lint: sources stay in the C23 ∩ Cx subset |
| `results/` | Result files (`.jsonl` raw data, `.md` report) |

## The two compilers

```sh
# Reference: the official release (or set CC_C to any clang 23.1.2 release binary)
curl -LO https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.2/LLVM-23.1.2-Linux-X64.tar.xz
tar xf LLVM-23.1.2-Linux-X64.tar.xz && ln -s "$PWD/LLVM-23.1.2-Linux-X64" ~/llvm-23.1.2-release

# Cx: build this repository's clang (Release, X86 only), next to the checkout in ../cx-build
cmake -G Ninja -S llvm -B ../cx-build -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DCLANG_INCLUDE_TESTS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBXML2=OFF
ninja -C ../cx-build clang
```

Override the paths with `make CC_C=… CC_CX=…`. Both compilers get the same flags: `-std=c23 -O2 -march=x86-64 -ffp-contract=off` (baseline ISA). With `-ffp-contract=off`, floating-point checksums cannot depend on whether an expression was constant-folded with fused rounding.

## Running

```sh
cd bench
make check             # subset lint + strict-warning build + sanitizer run (reference compiler)
make                   # build/c/bench, build/cx/bench, build/ctrl/bench
make codegen-compare   # are the two compilers' objects identical?
make run ROUNDS=5      # ~minutes per round; writes results/<timestamp>.jsonl
make report            # writes results/<timestamp>.md
```

The protocol, in `tools/run.py`:

- Every run is a fresh process pinned to one CPU (`--cpu`, default 3).
- A process does `setup()`, one untimed warm-up `run()`, then `ITERS` timed `run()` calls.
- In each round the benchmark order is shuffled. For each benchmark, the toolchains run back to back in shuffled order.
- `ctrl` is a byte-identical copy of the C binary. Its spread against C is the **noise floor**. A Cx result counts as faster or slower only if every round agrees and the median lies outside the largest control deviation (at least 2%).
- Every toolchain must produce the same checksum for every benchmark. The report flags any mismatch. A mismatch means a miscompile, or a benchmark that depends on unspecified behaviour.

Keep the machine otherwise idle while running. On a shared VM, single rounds commonly vary by ±5% even for identical binaries, so the medians over rounds matter.

## Source rules (the C23 ∩ Cx subset)

Each benchmark is written once and must be valid in both languages. The build enforces most rules: `make check` runs the lint, a `-Werror -Wconversion -Wsign-conversion …` build, and ASan + UBSan with `unsigned-integer-overflow` and `implicit-conversion`. It then checks that the sanitizer build (`-O1`) and the `-O2` build produce identical checksums (`tools/compare_checksums.py`).

1. **No removed keywords or features:** no `restrict`, `volatile`, `_Atomic`, `register`, `auto`, `long double`, `_Complex`, `_BitInt`, VLAs, `alloca`, `setjmp`/`longjmp`, variadic definitions, `goto`, or union punning (spec §3, §19).
2. **No integer overflow**, signed *or unsigned*, because Cx treats unsigned wraparound as a violation (spec §7.1). Values are bounded, or computed in 64-bit with explicit masks. The PRNG is xorshift (shifts and xors only).
3. **No implicit narrowing or sign-changing conversions**, and no implicit `void *` → `T *` (spec §7.2).
4. **Linkage:** exported definitions are written `extern` and private ones `static`, so they mean the same in both languages (spec §4.1).
5. **Cx intent is written as attributes**, which plain C compilers ignore:
   - `[[cx::escapes]]` on parameters that are stored or freed;
   - `[[cx::nullable]]` on parameters that may be NULL;
   - `[[cx::alias]]` on parameters that may overlap;
   - `[[cx::owned]]` on pointer members that own their pointee.

   The attributes matter once the Cx compiler implements them. The spec (§5) defines what a missing or wrong annotation means.
6. **No dependence** on plain-`char` signedness, struct layout, or evaluation order.

## Adding a benchmark

1. Add `static` setup/run/teardown functions and `extern const struct bench bench_NAME = { … };` to a `src/<category>_*.c` file, and `B(NAME)` to `harness/list_<category>.h`.
2. `run()` must be idempotent: it returns the same checksum on every call. Target 30–100 ms per call.
3. `make ONLY=<category> lint strict sanitize` must pass. `ONLY` builds one category in isolation, in `build-<category>/`.
4. To give the Cx toolchain its own version, copy `src/X.c` to `cx/X.cx` and change only that copy. The checksum must stay the same.
