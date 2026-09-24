# Reproduction kit for `../c-optimization-barriers.md`

These are the inputs and scripts behind the measurements in the analysis. The toolchain used was the official LLVM 23.1.2 release (`LLVM-23.1.2-Linux-X64`), which was built from the same commit as the vendored source in this repo.

## Micro-kernels (`kernels/`)

Each `eNN_*.c` file isolates one C rule. Compile a kernel with remarks and IR, for example:

```sh
clang -O2 -S -emit-llvm -fno-discard-value-names -g0 kernels/e02_char_alias.c -o -
clang -O2 -c kernels/e05_index.c -Rpass=loop-vectorize -Rpass-analysis=loop-vectorize
```

Timings: compile `bench_kern.c` with the flags under test and `bench_driver.c` separately, link them, then run `./a.out <kernel>` where `<kernel>` is one of `fill_c`, `fill_c_local`, `vsqrt`, `fsum`, `scale` or `scale_r`.

## Census (`census/`)

1. Compile each translation unit with:

   ```sh
   -fsave-optimization-record -foptimization-record-file=<tu>.yaml
   ```

2. Run `remarks.py <name> "<glob of yaml>" out.json` to aggregate by pass, remark and message.

3. To classify the calls that block GVN load elimination:
   1. Emit optimized IR with line info for each TU: `-O2 -gline-tables-only -S -emit-llvm`.
   2. Run `pertu.py callkinds.py <ll-dir> <yaml-dir>`, or `pertu.py calleemem.py <ll-dir> <yaml-dir>`.

## Ablation (`ablation/`)

Setup:
- `WORK` must contain `src/{sqlite,sqlite-bld,lua,zstd}`.
  - `sqlite-bld` is a build directory where `make sqlite3.c` was run. It must contain `sqlite3.c`, and `tsrc/` for the split build.
- `LLVM` must point at the LLVM install.

Steps:

1. Build the variants:

   ```sh
   ./build.sh <sqlite|sqlite_sep|lua|lua_one|zstd> <variant> "<flags>" [lto]
   ```

   The flags per variant are listed in `variants.txt`.

2. Copy `luabench/` into `$WORK/abl/`, and put a `corpus.bin` there for zstd.

3. Run `python3 bench.py <rounds>`, then `python3 analyze.py` for per-metric results and `python3 summarize.py` for per-round composites. `results.jsonl` holds the raw data behind §5.

Every run is pinned to CPU 3. Each variant is compared with its baseline from the same round.
