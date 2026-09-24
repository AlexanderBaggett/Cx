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
| `harness/bench.cxh` | The benchmark API (`struct bench`), deterministic PRNG, overflow-free checksum mixing. Shared headers use the Cx header extension `.cxh`, so the Cx compiler imports them as Cx rather than as foreign C (spec §4.1, §14). C compilers include them unchanged. |
| `harness/main.c` | Driver: `bench --list`, `bench NAME [--iters K]`, `bench --all` |
| `harness/list_{ops,ds,alg,kern}.cxh` | Registry, one file per category |
| `src/ops_*.c` | Core operations: integer, floating point, branches, calls, memory, strings, aliasing and ownership (`ops_alias.c`) |
| `src/ds_*.c` | Data structures |
| `src/alg_*.c` | Algorithms: sorting, searching, graphs, dynamic programming, backtracking |
| `src/kern_*.c` | Compute kernels: linear algebra, simulation, codecs, hashing |
| `cx/NAME.cx` | Optional Cx-specific replacement for `src/NAME.c` (Cx toolchain only) |
| `tools/run.py` | Runs every benchmark × toolchain × round, pinned to one CPU |
| `tools/report.py` | Markdown summary with a verdict per benchmark |
| `tools/codegen_compare.sh` | Checks whether both compilers emit identical object code |
| `tools/check_subset.sh` | Lint: sources stay in the C23 ∩ Cx subset |
| `tools/cx_scan.py` | AST check for Cx conversion rules that warnings miss (mixed signedness after promotion, implicit sign change or narrowing) |
| `tools/compare_checksums.py` | Checksum parity between two `bench --all` outputs |
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

Override the paths with `make CC_C=… CC_CX=…`. The ISA can be changed for both toolchains with `make MARCH=x86-64-v3`. Each build writes `build/<toolchain>/buildinfo.json`: compiler, version, flags and Cx overrides. Objects depend on it, so changing any of these rebuilds, and `run.py` copies it into the results. Both compilers get the same flags: `-std=c23 -O2 -march=x86-64 -ffp-contract=off` (baseline ISA). With `-ffp-contract=off`, floating-point checksums cannot depend on whether an expression was constant-folded with fused rounding.

## Running

```sh
cd bench
make check             # lint, Cx-rule scan, strict build, sanitizers (-O1 vs -O2 checksums), C/Cx checksum parity
make                   # build/c/bench, build/cx/bench, build/ctrl/bench
make codegen-compare   # are the two compilers' objects identical?
make run ROUNDS=5      # ~minutes per round; writes results/<timestamp>.jsonl
make report            # writes results/<timestamp>.md
```

The protocol, in `tools/run.py`:

- Every run is a fresh process pinned to one CPU (`--cpu`, default 3).
- A process does `setup()`, two untimed warm-up `run()` calls (`--warmup`), then `ITERS` timed `run()` calls.
- In each round the benchmark order is shuffled. For each benchmark, the toolchains run back to back in shuffled order.
- `ctrl` is a byte-identical copy of the C binary. Its spread against C is the **noise floor**. A Cx result counts as faster or slower only if every round agrees and the median lies outside the largest control deviation (at least 2%).
- Every toolchain must produce the same checksum for every benchmark. The report flags any mismatch. A mismatch means a miscompile, or a benchmark that depends on unspecified behaviour.
- **The control shows the false-positive rate.** `ctrl` gets the same verdict rule as Cx. The number of control benchmarks judged "faster" or "slower" tells you how many false positives to expect among Cx's verdicts.
- **C-library-bound benchmarks are left out of the compiler-bound geomean.** They are tagged `(libc)`: `str_libc`, `mem_copy`, `mem_set_cmp`, `alloc_churn`, `sort_qsort`, `fp_libm`, `fmt_snprintf` and `slist`. Both toolchains call the same glibc.

Keep the machine otherwise idle while running. On a shared VM, single rounds commonly vary by ±5% even for identical binaries, so the medians over rounds matter.

## What this suite can and cannot show

- **It is well-tuned C.** Helpers are `static`, hot data is passed as parameters, and arithmetic avoids overflow by construction.
- **It cannot show the linkage-default win.** In C that tuning is manual. In Cx it is the default (spec §4.1). Measuring it would need code written in typical C style, where forgetting `static` is common.
- **Cx-specific versions go under `cx/`.** The benchmarks tied to Cx claims are `owned_fields`, `byte_buffer`, `struct_layout`, `index_u32`, `fp_reduce`, `fp_divsqrt`, `fmt_snprintf`, `hash_murmur3`, `dispatch_table` and `inplace_alias`. They are where differences should appear once the Cx compiler implements the spec, or where a `cx/` version uses Cx features such as `wuint32_t`, `<cxfmt.h>` or `#pragma cx fp_reassociate`.
- **The baseline ISA understates vectorization.** It is x86-64 (SSE2), where vectors hold only 2 doubles. Run `MARCH=x86-64-v3` as a second configuration.
- **Floating-point contraction.** `-ffp-contract=off` differs from Cx's default (contraction on within an expression, spec §8.4). Without FMA (baseline x86-64) this makes no difference. Decide explicitly before running x86-64-v3.

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
2. `run()` must be idempotent: it returns the same checksum on every call. The named operation should dominate the time, so keep checksums cheap: sampled elements plus aggregates. Target 30–100 ms per call.
3. `make ONLY=<category> check` must pass. `ONLY` builds one category in isolation, in `build-<category>/`.
4. To give the Cx toolchain its own version, copy `src/X.c` to `cx/X.cx` and change only that copy. The checksum must stay the same.
