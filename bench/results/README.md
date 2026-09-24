# Benchmark results

Each run has a `.jsonl` file (raw per-process timings and checksums, with build metadata on the first line) and a `.md` file (the `tools/report.py` summary).

## Baseline: A/A runs, 2026-09-24

Both toolchains were Clang 23.1.2 compiled from the same source:

- **C:** the official release binary (`llvm-project` `85ac560`).
- **Cx:** built from this repository (`AlexanderBaggett/Cx` `5cbbe15`) with no changes to the compiler yet.

Both used the same flags, `-std=c23 -O2 -march=x86-64 -ffp-contract=off`, on the same sources. `make codegen-compare` confirmed identical code: all 24 object files, and the linked executables, are byte-identical apart from the `.comment` version string. So any timing difference below is measurement noise, and these runs calibrate how large a real effect must be before the suite can see it.

| Run | Binary paths | Checksums identical | Cx vs C geomean (compiler-bound) | Control geomean | Cx verdicts | Benchmarks flagged by a naive ±2% rule, Cx / control |
|---|---|---:|---:|---:|---|---:|
| [`20260924-210813-unequal-paths`](20260924-210813-unequal-paths.md) | `build/c/bench`, `build/cx/bench`, `build/ctrl/bench` | 78/78 | +0.1% | +0.3% | 78 same | 4 / 3 |
| [`20260924-211944`](20260924-211944.md) | equal length (`build/run/t0..t2`) | 78/78 | −0.5% | −0.2% | 78 same | 3 / 3 |

5 rounds × 78 benchmarks × 3 binaries, 7 timed iterations per process, pinned to one CPU of a 4-vCPU KVM guest (Xeon @ 2.8 GHz).

### What the baseline tells us

- **Correctness gate works.** Every benchmark produced the same checksum with both compilers, in every run.
- **Noise floor.** For byte-identical binaries, the per-benchmark median speed ratio spans about **±2.5–3%** between the 10th and 90th percentile. Single rounds range much wider, occasionally ±10–15%, because other tenants share the host. The suite-wide geomean is stable to within **±0.5%**.
- **False positives.**
  - A naive rule flags about 4% of benchmarks for identical binaries: every round agrees and the median moves by more than 2%.
  - The flagged benchmarks change from run to run (`sort_quick`, `string_builder` and `fp_libm` in run 1; `trie_words`, `index_u32` and `nbody` in run 2), so they are not systematic.
  - The report's rule widens the band to the control's own spread, and gave **0 false Cx verdicts** in both runs.
- **Binary paths matter.**
  - In run 1, `sort_quick` was 2–2.5% "faster" in *both* the Cx build and the byte-identical control.
  - The only difference between those two binaries and the C binary was a longer program path.
  - The path is copied onto the initial stack and shifts stack alignment; this is a known source of measurement bias.
  - With equal-length paths (run 2) the offset disappeared. `run.py` now always runs binaries from `build/run/t<i>`.

### How to read future Cx results against this baseline

- **One benchmark:** a change is credible only if the report calls it `faster` or `slower`, and it reproduces in a second run. As a rule of thumb, it should exceed ±3%.
- **The suite-wide geomean:** a shift beyond about ±1% is meaningful.
- **Checksums:** a mismatch is always a finding. It means either a miscompile, or a benchmark that depends on behaviour that Cx changes on purpose.
