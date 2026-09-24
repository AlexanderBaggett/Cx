# Where C Holds the Optimizer Back: Code Paths and Measurements in Clang/LLVM 23.1.2

This is a fact-first analysis of the C compiler we vendored (`llvm-project` `llvmorg-23.1.2`, commit `85ac5602`). It covers two questions:

1. What makes compiling C complex?
2. Which C rules stop the compiler from producing faster code, and by how much?

Every claim rests on at least one of these kinds of evidence:

- **Code paths.** `path:line` references into this repository. Each was opened and checked; 17 were re-checked by hand for this document.
- **Micro-kernels.** Small C functions that each isolate one language rule. We compiled them with the official LLVM 23.1.2 release binary, which was built from the same commit as our vendored source. We then looked at the IR, the assembly, the optimizer's own remarks, and timings.
- **A census of real code.** We compiled SQLite, Lua and zstd with optimization records turned on. We counted every place the optimizer reported that it gave up, and why.
- **Ablation benchmarks.** We rebuilt the same three projects with compiler flags that switch off one C rule at a time, then benchmarked each build.

Where a statement is a reading of the evidence rather than something measured or quoted, it is marked **(inference)**.

---

## 0. Summary

**Roughly ranked by measured impact.**

| # | Barrier (C rule) | Where the compiler gives up | Evidence | Measured cost | What a stricter language could guarantee |
|---|---|---|---|---|---|
| 1 | **Calls have unknown memory effects, and pointers escape freely** | `BasicAliasAnalysis.cpp:980-996`, `:1076-1077`; `Attributes.cpp:1458-1461`; `CaptureTracking.cpp:304`, `:323-327` | In SQLite, 71% of the 237,932 loads GVN failed to eliminate were blocked by a call. 93% of those callees are defined in the same file, yet LLVM inferred *nothing* about their memory effects. In Lua (split into separate files), 59% of blocking calls go to another file or libc. | No flag can switch this rule off. The part we can measure is file visibility (row 5). | Checked effect summaries on every function. Pointers and locals don't escape unless marked. Module-private by default. |
| 2 | **Aliasing rules: `char` aliases everything, unions have no type info, `restrict` is parameter-only, `const` promises nothing** | `CodeGenTBAA.cpp:166-174`; `CGExpr.cpp:5856-5858`; `CGCall.cpp:3602-3604` | A byte fill through a struct field is **34× slower** than the same loop with hoisted locals (0.474 vs 0.014 ns/byte). Turning off type-based alias analysis in SQLite gives 60% more store-clobbered loads, 9% fewer vectorized loops and 10% fewer scalar promotions. | Whole program: within noise (SQLite −0.2%, Lua +1.5%, zstd +1.3%), even though the flag changes the code of 7–17% of functions | A distinct `byte` type holds the aliasing power. Exclusive references or `restrict` by default. Tagged unions. |
| 3 | **Strict IEEE floating point, and `errno` in libm** | `Builtins.cpp:285-289`; `Linux.cpp:962-966`; `IVDescriptors.cpp:1019-1022` | Float sum: **8.1× faster** when reassociation is allowed. `sqrt` loop: **2.0× faster** without `errno` (SSE2). | Whole program: no effect on these integer-heavy programs. `-fno-math-errno` produces byte-identical code for SQLite and zstd. | Pure math functions. Reductions that are explicitly reassociable. |
| 4 | **Unsigned arithmetic wraps; `int` indexes get sign-extended** | `CGExprScalar.cpp:4763-4766`; `SimplifyIndVar.cpp:1502-1505` | `unsigned` index plus offset: the vectorizer has to add a runtime wrap check, and the scalar loop re-computes the index with trunc, add and zext each iteration. `-fwrapv` does the same to `int`. | Whole program: within noise (+1.4%, +2.0%, −0.5%) | Overflow traps or is UB for every fixed-width integer, with explicit wrapping operators. Pointer-width index type. |
| 5 | **External linkage by default; separate compilation** | `GlobalOpt.cpp:1671-1673`, `:1983-1988`; `ArgumentPromotion.cpp:816-818`; `DeadArgumentElimination.cpp:463-466` | Lua: 3,086 "definition unavailable" inlining misses. The same kind of miss occurs 262 times in the one-file SQLite amalgamation. | **SQLite split into 100 files: −3.3%, slower in 6 of 6 rounds; LTO recovers it (+2.2%). Lua as one file: +4.7%, faster in 6 of 6 rounds.** | Module system. Exports are explicit and hidden or protected. Interface files carry effect summaries. |
| 6 | **`setjmp`, varargs, function pointers** | `InlineCost.cpp:3189-3191`, `:3352-3356`; `CGCall.cpp:2914-2921` | Never inlined (`cost=never`). SQLite has 1,541 varargs inlining refusals. 16.5% (SQLite) and 12.9% (Lua) of blocking calls are indirect. | — | No `setjmp`. Typed variadics. Closed-world function values. |
| 7 | **ABI and layout: structs over 16 bytes go through memory; fields stay in declaration order** | `X86.cpp:2111-2114`, `:2190-2195`; `RecordLayoutBuilder.cpp:1439-1447` | `struct {double x,y,z}` travels on the stack (6 memory operands in `dot3`), while `{double x,y}` travels in registers. A badly ordered struct is 40 bytes; sorted, it is 24. | — | Layout unspecified by default, `repr(C)` for FFI, a private calling convention between Cx functions. |
| 8 | **Frontend complexity: preprocessor, context-sensitive grammar, 13 C dialects shared with C++** | `Parser.cpp:1988-2040`; `SemaInit.cpp:324-354` | Sema is 331,629 lines. 5 libc headers expand 6 lines to 1,648. The front end takes 54% of a `-O0` Lua build. | Compile time, not run time | Modules instead of `#include`. Context-free syntax. One dialect. |

**Three conclusions shape the Cx roadmap.**

- **The biggest lever is memory-effect and escape information, not micro-rules.**
  - In SQLite and Lua, about 70% of the loads the optimizer failed to eliminate were blocked by a call. That holds even when every callee is visible, as in SQLite's single-file amalgamation.
  - zstd keeps hot state in locals and is the exception (33%).
  - The only whole-program speed effect that beat the noise floor in every round was cross-file visibility (§5). LLVM's summary of what a function touches only distinguishes argument memory, inaccessible memory, errno and "everything else". A C function that writes through a pointer it loaded from a struct falls into "everything else".
- **Relaxing the famous UB-based rules changed no whole-program result beyond the noise floor.** This covers strict aliasing, signed overflow, null-check deletion, forward progress, errno and FP strictness. Each measured within ±2–3% on SQLite, Lua and zstd, even though some of them rewrite 15–36% of functions. They matter as *cliffs* in specific kernels (2×–34× above), not as general speed-ups for integer-heavy systems code. The language should still fix them, because the fixes are cheap and remove the cliffs.
- **Most frontend complexity costs engineering, not runtime.** In an optimized build of the SQLite amalgamation, the front end is 2.7% of compile time. The optimizer is 51% and code generation is 45%.

---

## 1. Method and setup

| Item | Value |
|---|---|
| Compiler | Official `LLVM-23.1.2-Linux-X64` release. `clang --version` reports `85ac560262434c9ccfc0c183ec22d4138ed647fb`, the same commit as our import. |
| Target | `x86_64-unknown-linux-gnu`, baseline ISA (SSE2 only; the distribution default) unless a row says `-march=native` |
| Machine | Intel Xeon @ 2.80 GHz (KVM guest), 4 cores, AVX-512 available, glibc 2.39 |
| SQLite | 3.54.0 (`2503dc6`). Built as the amalgamation `sqlite3.c` (9.5 MB, 270,822 lines) and as 100 separate files. Benchmark: `speedtest1 --memdb --size 60 --singlethread --nomemstat`. |
| Lua | 5.5 development tree (`0b29f40`). Built as 32 separate files and as `onelua.c`. Benchmark: 6 scripts (`fib`, `nbody`, `spectral`, `strings`, `tables`, `closures`). |
| zstd | 1.6.0-dev (`01b7154`), `-O3`, with `ZSTD_DISABLE_ASM` so the hot loops are C. Benchmark: `zstd -b{1,3} -i1 -T1` on a 75 MB mix of source text and machine code. |
| Optimization level | Each project's own default: SQLite and Lua `-O2`, zstd `-O3` |
| Benchmark protocol | 6 rounds. Variant order shuffled every round. Each run pinned to one core (`taskset -c 3`), with nothing else running. Every variant is compared with its project's baseline **in the same round**. We report the median, the range, and how many rounds were faster. Byte-identical copies of each baseline serve as noise controls. |

Everything needed to reproduce this is in [`c-optimization-barriers/`](c-optimization-barriers/) (see §7).

**Limits of the method:**

- Flags can only *relax* C rules; they cannot make C stricter than it is. The effect of stronger guarantees therefore comes from micro-kernels, `restrict`, LTO and the census.
- Remark counts are instances, not costs. A remark in a cold function counts the same as one in a hot loop.
- Some clobbers are real dependences that no language rule could remove.
- One VM, one CPU model. Effects under about ±2% are within noise; §5 shows the ranges.

---

## 2. Size and time: where the compiler spends itself

### 2.1 Lines of code (vendored tree)

| Component | Lines (.cpp/.h) | Notes |
|---|---|---|
| `clang/lib/Lex` | 32,214 | Preprocessor and lexer |
| `clang/lib/Parse` | 49,811 | |
| `clang/lib/Sema` | 331,629 | About 94k in C++-named files and about 72k for ObjC, OpenMP, CUDA, HLSL, OpenCL and targets. That leaves about 146k of core code that C shares with C++. |
| `clang/lib/AST` | 196,472 | Includes `ExprConstant.cpp` (23,072) and the bytecode constant interpreter (38,274) |
| `clang/lib/CodeGen` | 211,671 | |

- **Diagnostics:** 5,240 `err_`/`warn_`/`ext_` definitions in the Lex, Parse and Sema diagnostic files. 396 of them are "accepted as an extension".
- **Dialects:** `LangStandards.def` defines **13 C dialects** (c89 … gnu2y).
- **Shared code:** Sema contains 971 C++ dialect checks spread over 47 of 87 files, and 102 explicit C-version checks. C is not compiled by a C compiler; it is compiled by the C++ compiler's shared code.

### 2.2 Where compile time goes

| Build | Front end | IR generation | Optimizer | Machine code |
|---|---|---|---|---|
| SQLite amalgamation, `-O2` (22.3 s) | 2.7% | 1.5% | 51.2% | 44.6% |
| Lua, 32 files, `-O2` (5.03 s total) | 16% | 3% | 39% | 42% |
| Lua, 32 files, `-O0` (1.35 s total) | **54%** | 9% | 1% | 36% |

**Textual inclusion:**

| Source | Lines before preprocessing | Lines after preprocessing |
|---|---|---|
| `hello.c` with `stdio.h`, `stdlib.h`, `string.h`, `math.h`, `pthread.h` (opens 118 header files) | 6 | 1,648 |
| Lua, 32 files | 34,031 unique | 55,148 across all files (1.6×) |
| `zstd_compress.c` | 8,375 | 11,538 (607 kB) |

**(inference)** C's frontend design costs compile time mainly in debug and incremental builds. In optimized builds its cost is engineering complexity. Its larger effect is on the optimizer, through the information it cannot express (§4).

---

## 3. Frontend: complexity that C forces on the compiler

### 3.1 Textual `#include` and macros

- **Every identifier is checked for a macro.** `Preprocessor::HandleIdentifier` (`clang/lib/Lex/Preprocessor.cpp:876-884`) implements C99 6.10.3p10 and marks tokens that must never be expanded again (`Token::DisableExpand`).
- **Macro arguments can be lexed twice.** `MacroArgs::getPreExpArgument` (`MacroArgs.cpp:176-181`) does this, and `ArgNeedsPreexpansion` conservatively says yes whenever an argument token names any macro (`:145-155`).
- **Token pasting re-lexes.** `TokenLexer::pasteTokens` (`TokenLexer.cpp:800-812`) builds a new spelling and lexes it again.
- **Repeated headers need their own state machine.** Include-guard detection is a separate state machine (`MultipleIncludeOpt.h:22-28`), and it is defeated by any macro expansion on the guard line (`PPMacroExpansion.cpp:439-442`).
- **Source-location space is a 32-bit budget** (`SourceLocation.h:102`). Very large translation units can run out of it (`DiagnosticCommonKinds.td:400-401`).
- **Information lost:** Sema only ever sees expanded tokens. Clang has to recover macro origin after the fact, for example to decide whether an array bound "came from a macro" (`DeclBase.cpp:505-522`).
- **Cx lever:** modules with declaration-only interfaces; typed constants instead of `#define`; declaration-level conditional compilation.

### 3.2 The "lexer hack": parsing needs the symbol table

`T * x;` is a declaration or a multiplication depending on whether `T` is a typedef in scope.

- The parser calls Sema in the middle of parsing: `Parser::TryAnnotateTypeOrScopeTokenAfterScopeSpec` → `Actions.getTypeName(...)` (`Parser.cpp:1993-1998`, `:2022-2025`).
- The same applies to cast versus parenthesized expression (`Parser.h:5085-5089`).
- **Cost (inference):** parsing cannot be separated from name lookup, so a function body cannot be parsed without every prior declaration, including all headers. That rules out parallel or lazy parsing.
- **Cx lever:** a context-free grammar, for example `let x: T` and a distinct cast syntax.

### 3.3 Declarators ("declaration mirrors use")

- Types are assembled from the identifier outwards (`DeclSpec.h:1966-1969`).
- `GetFullTypeForDeclarator` is about 1,437 lines (`SemaType.cpp:4332` onwards). It walks the chunks in reverse; the comment at `:4702-4712` says "opposite of what we want :)".
- Declaration specifiers may appear in any order and are validated afterwards (`DeclSpec::Finish`, `DeclSpec.cpp:1159`).
- **Cx lever:** left-to-right type syntax with a fixed specifier order.

### 3.4 Implicit conversions

- **Every operation goes through the promotion machinery:** integer promotions and the usual arithmetic conversions (`SemaExpr.cpp:842`, `:1707`).
- **Target-dependent choice:** when a signed type outranks an unsigned one but isn't wider, Clang picks the unsigned counterpart (`SemaExpr.cpp:1369-1375`).
- **Two `char`s:** separate `Char_S`/`Char_U` ranks, because `char` signedness is implementation-defined (`ASTContext.cpp:8293-8297`).
- **int → pointer** is only a default-error *extension* in C (`DiagnosticSemaKinds.td:9262-9264`).
- **Information lost (inference):** narrow arithmetic is widened to `int` in the AST and IR, and the optimizer has to prove narrowness again later, for example to vectorize `uint8_t` math.
- **Cx lever:** arithmetic stays in the operand type; mixed sign or width requires explicit casts; no implicit int↔pointer conversion.

### 3.5 Unprototyped (K&R) functions and implicit declarations

- **`f()` means "unknown parameters"** before C23 (`SemaType.cpp:5209-5212`).
- **Arity is only checked if the definition's body is visible** (`SemaExpr.cpp:7305-7314`). Arguments get default promotions: `float` → `double` (`:892`, `:905-915`).
- **Code generation has to guess the calling convention:** `isNoProtoCallVariadic` (`TargetInfo.cpp:96-102`).
- **Implicit `int foo();` declarations** are still synthesized in older modes (`SemaDecl.cpp:17337-17343`).
- **C23 fixes this, but all 13 dialects must still be supported:** `FunctionNoProtoType` appears 98 times across Sema, AST and CodeGen.
- **Cx lever:** C23 prototype semantics only; declare before use.

### 3.6 Tentative definitions, compatible and composite types

- **Storage is decided late:** tentative definitions are completed at the end of the translation unit (`Sema.cpp:1488-1535`).
- **Compatibility is a type merge:** type compatibility in C *is* `mergeTypes` (`ASTContext.cpp:11528-11533`), a function of about 456 lines. It contains a `// FIXME: This isn't correct!` for array composites (`:12183`).
- **Redeclarations are merged:** `MergeFunctionDecl` is about 787 lines (`SemaDecl.cpp:3730-4516`).
- **Cx lever:** one declaration per entity, identical types, nominal struct identity.

### 3.7 Variably modified types, array decay, flexible arrays

- **VLA types contain runtime expressions.** They cannot be uniqued (`ASTContext.cpp:4446-4450`), and their `sizeof` is evaluated at run time (`SemaExpr.cpp:4880-4885`).
- **Arrays decay to pointers at every call boundary** (`SemaExpr.cpp:554-556`; parameters in `ASTContext.cpp:4045-4051`). Only the rarely used `T a[static N]` survives into IR as `dereferenceable` (`CGCall.cpp:3539-3557`).
- **Any trailing array member is treated as a flexible array by default** (`LangOptions.h:390-392`, `DeclBase.cpp:467-468`), so the compiler cannot trust bounds on trailing struct arrays.
- **Information lost:** array extents. That is why the vectorizer says "cannot identify array bounds" (§4.2).
- **Cx lever:** slices that carry a length; arrays as values that never decay; explicit flexible-array syntax.

### 3.8 Initializers, bit-fields, enums, `_Generic`, unsequenced evaluation

- **Initializer lists:** `InitListChecker` is about 3,346 lines (`SemaInit.cpp:324-354` describes it). It walks the syntactic and semantic lists in parallel, handles designators that jump around and brace elision, and runs twice (verify, then build; `:5269-5270`, `:8422-8424`).
- **Enums:** the underlying type depends on the target and on flags (`ASTContext.cpp:5596-5619`, `SemaDecl.cpp:21089-21140`).
- **`_Generic`:** checks every pair of associations for compatibility (`SemaExpr.cpp:1975-1991`).
- **Unspecified evaluation order** needs its own ~870-line checker (`SequenceChecker`, `SemaChecking.cpp:14526`).
- **printf-style varargs** are checked by a format-string engine of about 3,000 lines plus 3,444 lines in AST (`SemaChecking.cpp:7225-10209`, `clang/lib/AST/*FormatString.cpp`). This exists because varargs carry no types.

---

## 4. Optimization barriers

Each subsection gives:

1. the C rule;
2. the code path where the compiler has to be conservative;
3. a micro-kernel demonstration;
4. real-code data;
5. the Cx lever.

### 4.1 Calls with unknown memory effects, and escaping pointers (the dominant barrier)

**C rule.** Any function may read or write any memory reachable from any pointer that has escaped. Pointers escape by being stored, passed, or converted to integers. `const` does not make the pointee immutable.

**Code paths.**

- **No attribute means unknown effects.** A call with no `memory(...)` attribute has `MemoryEffects::unknown()` (`llvm/lib/IR/Attributes.cpp:1458-1461`; `ModRef.h:122-124`: "can read and write any memory").
- **The one exemption is non-escaping locals.** `BasicAAResult::getModRefInfo(Call, Loc)` (`BasicAliasAnalysis.cpp:980-996`) only exempts identified function-local objects that have not escaped. Otherwise: `// Be conservative. return ModRefInfo::ModRef;` (`:1076-1077`).
- **Escape is easy.** Passing a pointer to a call without a `captures` attribute captures it fully (`CaptureTracking.cpp:304`; `Attributes.cpp:1464-1467`). So does storing it (`CaptureTracking.cpp:323-327`) or `ptrtoint` (`:391-393`). Past 100 uses (`capture-tracking-max-uses-to-explore`, `:46-48`), a pointer is simply assumed captured.
- **Plain pointer parameters are never "identified objects".** Only `noalias` and `byval` arguments are (`AliasAnalysis.cpp:903-905`, `:921-922`).
- **GlobalsAA only tracks internal globals** (`GlobalsModRef.cpp:276-278`). It gives up on any external call that isn't `nosync` + `nocallback` (`:547-553`).
- **Attribute inference needs exact definitions.** FunctionAttrs infers memory effects only for exact definitions (`FunctionAttrs.cpp:281-285`, `:977-981`). Its effect lattice has four locations: argument memory, inaccessible memory, errno, and other (`ModRef.h:60-68`).

**Micro-kernels** (`kernels/e03_escape.c`, `e04_const.c`):

```c
static int static_counter;                       // internal; address never taken
int count_static(int n) {
  for (int i = 0; i < n; i++) { static_counter++; opaque(); }
  return static_counter;
}
int sum_twice(const int *p) { int a = *p; opaque(); return a + *p; }
```

- **`static_counter` is loaded and stored on every iteration.** It has internal linkage and its address is never taken, but `opaque()` could call back into the externally visible `count_static`, and C gives no way to say it won't.
- **`sum_twice` loads `*p` twice.** The parameter gets `readonly` (for the callee only), but `const` gives no invariance across the call.

**Real code: why GVN could not remove a load.**

| | SQLite (1 file) | Lua (32 files) | zstd (26 files) |
|---|---|---|---|
| Loads GVN could not eliminate (`LoadClobbered`) | 237,932 | 26,079 | 62,323 |
| … blocked by a call | **168,766 (71%)** | **18,176 (70%)** | 20,548 (33%) |
| … blocked by `memcpy`/`memset`/`memmove`/lifetime intrinsics | 15,322 (6%) | 3,054 (12%) | 3,780 (6%) |
| … blocked by a store | 53,336 (22%) | 4,846 (19%) | 37,854 (61%) |
| Loads GVN *did* eliminate (`LoadElim` + `LoadPRE`) | 1,725 | 445 | 1,044 |
| LICM: "loop may invalidate its value" | 24,989 | 4,430 | 8,498 |
| LICM: loop accesses promoted to registers | 117 | 16 | 123 |

**What those blocking calls are.** We matched each remark's `ClobberedBy` location to the call in the optimized IR (`census/callkinds.py`, `calleemem.py`):

| Blocking call | SQLite | Lua (per file) |
|---|---|---|
| Function defined in the same file, not inlined | 83.2% | 27.8% |
| Function in another file, or libc (declaration only) | 0.0% | **58.7%** |
| Indirect call through a function pointer | 16.5% | 12.9% |
| *Of the same-file callees: LLVM inferred no memory restriction at all* | **93.4%** | **99.8%** |

**Reading the numbers (inference).**

- **Seeing the code is not enough.** SQLite's amalgamation gives the optimizer every function body, and 93% of the callees still summarize as "may read or write anything". The effect lattice can't express "writes only the object that `p->pager` points to". C pointer graphs carry no ownership or region information, so the summary collapses to *other memory*.
- **Separate compilation adds to it.** In Lua, most blocking calls are simply invisible.
- **zstd is the counter-example that proves the rule.** It is written in a style that keeps hot state in locals and passes small structs by value, and it has the lowest call-blocked share (33%).

**Cx levers, in priority order:**

1. **Compiler-checked effect summaries on every function**, exported in module interfaces: reads, writes, which parameters are written, no callbacks, no synchronization.
2. **Non-escaping references by default.** Taking an address that may outlive the call must be explicit. This turns most pointer parameters into `noalias` + `captures(none)`.
3. **Module-private by default** (§4.6), so that GlobalsAA and interprocedural passes apply without LTO.
4. **An ownership or region model for pointer fields** (inference: the largest single change, and the only one that addresses the "other memory" collapse).

### 4.2 Aliasing rules

**C rules.**

- A character type may access any object.
- Union members may be used for type punning.
- `restrict` is the only way to state non-aliasing, and it can be written anywhere.
- `const` pointers may be cast back to mutable.

**Code paths.**

- **All three char types are the "omnipotent char" type-based alias node.** `CodeGenTBAA.cpp:166-174` says "In C, it includes all three", and `:78` creates the node. `std::byte` is treated the same (`:227-228`). `int8_t` and `uint8_t` are typedefs of these, so **every byte buffer aliases every object**.
- **Unions have no TBAA:** `CGExpr.cpp:5856-5858`, `// TODO: Support TBAA for unions.` → `getMayAliasInfo()`.
- **Enums alias `int`:** C enums use their underlying integer type (`CodeGenTBAA.cpp:343-344`), and signed and unsigned collapse (`:176-180`).
- **`void*` aliases any pointer.** `void*` and `void**` fall back to the "any pointer" node (`CodeGenTBAA.cpp:275-276`).
- **Some fields lose type info.** Bit-fields get no TBAA (`CGExpr.cpp:5839-5842`), and neither do structs with a flexible array member (`CodeGenTBAA.cpp:149-150`).
- **`restrict` is only honored on parameters.** It becomes `noalias` in exactly one place, parameters in `EmitFunctionProlog` (`CGCall.cpp:3602-3604`). Restrict on locals, struct fields, globals and return values emits nothing. Clang emits no scoped `noalias` metadata for C; that metadata only appears when LLVM inlines a `noalias` parameter (`InlineFunction.cpp:1226-1228`).
- **`const` emits nothing.** `const T*` parameters get no `readonly`/`noalias` from the front end.
- **`-fno-strict-aliasing` drops every type tag** (`CodeGenTBAA.cpp:376-378`). It is the default for MSVC-compatible and UEFI targets (`clang/lib/Driver/ToolChains/Clang.cpp:6120-6125`).

**Micro-kernels** (`kernels/e02_char_alias.c`, `e01_restrict.c`):

```c
struct cbuf { char *data; size_t len; };
void fill_c(struct cbuf *b, char c) { for (size_t i = 0; i < b->len; i++) b->data[i] = c; }
```

- **`fill_c`:** the byte store may overwrite `b->data` and `b->len`, so both are reloaded on every iteration, one byte is stored per iteration, and the vectorizer reports `cannot identify array bounds`.
- **The same loop with `int *data`:** vectorized as `<4 x i32>` × 2, with the loads hoisted.
- **Measured:** `fill_c` 0.474 ns/byte; the same loop with `b->data`/`b->len` copied into locals, 0.014 ns/byte (**34×**).
- **`restrict` on a simple `dst[i] = src[i]*k`:** both versions vectorize. Without `restrict`, the vectorizer adds a pointer-difference runtime check and a scalar prologue (69 vs 36 lines of assembly). There was no measurable runtime difference when the arrays don't overlap. Runtime checks recover simple cases. They fail when bounds are unknown (above), or when more than 128 checks would be needed (`vectorize-memory-check-threshold`, `LoopVectorize.cpp:201-203`).

**Real code: how much type-based aliasing buys SQLite.** SQLite compiled with and without `-fno-strict-aliasing`:

| Remark | Strict aliasing (default) | `-fno-strict-aliasing` | Change |
|---|---|---|---|
| GVN loads blocked by a store | 53,336 | 85,390 | **+60%** |
| GVN loads eliminated | 1,725 | 1,641 | −4.9% |
| LICM "loop may invalidate its value" | 24,989 | 25,727 | +3.0% |
| LICM scalar promotions | 117 | 105 | −10% |
| Loops vectorized | 44 | 40 | −9% |

**Cx levers:**

- A single `byte` type with aliasing power; `u8`/`i8` are ordinary integers.
- Tagged unions, with reinterpretation only through an explicit `bit_cast`.
- Exclusive (`noalias`) references by default, lowered to `noalias` on parameters and scoped metadata for locals and fields.
- Transitively read-only references, lowered to `readonly` + `!invariant.load`.
- Nominal enums.

### 4.3 Integer semantics

**C rules.**

- Signed overflow is UB.
- Unsigned arithmetic wraps modulo 2ⁿ.
- Indexes are usually `int`, which is 32-bit on a 64-bit target.

**Code paths.**

- **Only signed arithmetic gets `nsw`; unsigned gets no flags.** Signed `+`, `-`, `*` and `++` get `nsw` (`CGExprScalar.cpp:4763-4766`, `:846-849`, `:4925`, `:3293-3298`). `-fwrapv` removes it (`:217-228`).
- **Signed `<<` never gets `nsw`,** even though shifting into the sign bit is UB (`CGExprScalar.cpp:5139`).
- **Array indexes are widened with `sext` or `zext`** (`CGExpr.cpp:5016`, `:5024`). A GEP with a signed index gets `inbounds` only; `nuw` needs an unsigned index (`CGExprScalar.cpp:6412-6414`).
- **Widening a 32-bit induction variable needs a no-wrap flag.** IndVarSimplify needs `nsw` for sign extension or `nuw` for zero extension (`SimplifyIndVar.cpp:1502-1505`).
- **Trip counts also need no-wrap.** SCEV trip-count computation needs no-wrap flags or a finite-loop assumption (`ScalarEvolution.cpp:13526-13527`, `:13568-13570`).

**Micro-kernel** (`kernels/e05_index.c`), `a[i] += b[i + off]`:

| Index type | Vectorizer runtime checks | Scalar loop body |
|---|---|---|
| `int` (nsw) | memory check only | loop-invariant base plus one IV, no extends |
| `unsigned` | memory check **+ SCEV wrap check** | `trunc` → `add` → `zext` → GEP each iteration |
| `int` with `-fwrapv` | memory check **+ SCEV wrap check** | same as `unsigned` |
| `size_t` | memory check only | clean |

**Cx levers:**

- Overflow of every fixed-width integer type traps (checked builds) or is UB (release), so both `nsw` and `nuw` can always be emitted.
- Explicit wrapping operators or types (`+%`, or a `wrapping<u32>`).
- The default index type is pointer-width.
- Shifts: shift counts ≥ width are errors; `shl` gets `nsw`/`nuw`.

### 4.4 Floating point and `errno`

**C rules.**

- libm functions report domain errors through `errno` (`math_errhandling & MATH_ERRNO` on glibc).
- FP arithmetic is not reassociated unless the programmer opts in.

**Code paths.**

- **`MathErrno` defaults to on:** true by default (`ToolChain.h:512-513`). Linux turns it off only for Android and musl (`Linux.cpp:962-966`), so glibc keeps it; Darwin turns it off (`Darwin.h:262`).
- **With errno on, `sqrt` stays a libcall.** Builtins become LLVM intrinsics only when errno is off (`clang/lib/Basic/Builtins.cpp:285-289`); `sqrt` and `pow` are `ConstIgnoringErrnoAndExceptions` (`Builtins.td:4471-4473`).
- **LLVM marks libm as writing errno** (`BuildLibCalls.cpp:1353-1366`). A call that writes memory is never mapped to an intrinsic (`ValueTracking.cpp:4685-4687`), and the vectorizer rejects it (`LoopVectorizationLegality.cpp:904-925`).
- **errno is modelled as an `int`,** so any `int`-sized store through an unknown pointer may alias it (`BasicAliasAnalysis.cpp:1907-1918`; `CodeGenModule.cpp:1901-1922`).
- **FP reductions need reassociation.** A reduction without `reassoc` is "exact" (`IVDescriptors.cpp:1019-1022`), and x86 has no ordered-reduction fallback (`TargetTransformInfoImpl.h:436`; AArch64 does, `AArch64TargetTransformInfo.h:442`).

**Micro-kernels** (`kernels/e06_errno.c`, `e07_fpreduce.c`, timed by `bench_driver.c`, n = 4096, median of 9):

| Kernel | Default | Relaxed | Speed-up |
|---|---|---|---|
| `o[i] = sqrt(in[i])` | "library call cannot be vectorized. Try compiling with -fno-math-errno": **1.848 ns/elem** | `-fno-math-errno`: vectorized, **0.920** | **2.0×** (SSE2, 2 lanes) |
| `s += a[i]` (float) | "cannot prove it is safe to reorder floating-point operations": **1.240 ns/elem** | `-fassociative-math -fno-signed-zeros -fno-trapping-math`: VF4×IC2, **0.153** | **8.1×** |
| `s += a[i]` (int) | vectorized | — | — |

**Real code:**

- **Libm is not what blocks these three.** SQLite, Lua and zstd have 1,651, 423 and 824 `CantVectorizeLibcall` remarks, but every one of them is the generic "call instruction cannot be vectorized", not the math-errno variant. These programs are integer-heavy.
- The ablation (§5) shows what `-fno-math-errno` and `-ffast-math` do to them.

**Cx levers:**

- Math functions are pure and return errors as values, or produce NaN. There is no global `errno`; FFI wrappers read it explicitly.
- Reductions are marked reassociable per operation (for example a `sum` intrinsic, or `+` on a `reassoc` float view).

### 4.5 Control-flow features: `setjmp`, varargs, function pointers, loop termination

**Code paths.**

- **`setjmp` is recognized by name.** Clang forces `returns_twice` on `setjmp`, `_setjmp`, `sigsetjmp`, `vfork`, `getcontext` and similar (`CGCall.cpp:2914-2921`).
- **`returns_twice` disables a lot:**
  - inlining (`InlineCost.cpp:2470-2474`, `:3334-3336`)
  - non-escaping objects are treated as clobbered (`BasicAliasAnalysis.cpp:984-989`)
  - tail-call marking (`TailRecursionElimination.cpp:197-199`)
  - **register coalescing for the whole function** (`RegisterCoalescer.cpp:4306-4314`)
  - stack-slot sharing (`StackColoring.cpp:721-724`)
- **Varargs:**
  - Functions that call `va_start` are never inlined (`InlineCost.cpp:3352-3356`).
  - They never get the faster calling convention (`GlobalOpt.cpp:1722-1723`) or argument promotion (`ArgumentPromotion.cpp:824-829`).
  - On x86-64, the caller must set `%al` and the callee spills every argument register into a save area (`X86ISelLoweringCall.cpp:1578-1601`, `:2464-2482`).
- **Indirect calls are never inlined** (`InlineCost.cpp:3189-3191`). Any indirect call also blocks `norecurse` (`FunctionAttrs.cpp:2031-2047`). `norecurse` in turn gates GlobalOpt's localization of globals (`GlobalOpt.cpp:1482-1489`).
- **Loop termination:**
  - C11 lets a loop be assumed to terminate only if its controlling expression is *not* constant (`CGStmt.cpp:1030-1031`).
  - C functions **never** get `mustprogress`; only C++11 functions do (`CodeGenFunction.h:646-651`).
  - `goto` loops get no progress guarantee (inference from `CGStmt.cpp:1114-1117`).
  - FunctionAttrs cannot infer `willreturn` for a function containing a loop without `mustprogress` (`FunctionAttrs.cpp:2161-2174`).

**Micro-kernels:**

- `kernels/e08_setjmp_varargs.c` produces these remarks, verbatim:
  - `'guarded' not inlined into 'caller' because it should never be inlined (cost=never): exposes returns twice`
  - `'sum_va' not inlined into 'caller' because it should never be inlined (cost=never): varargs`
  - `'plain' inlined into 'caller' with (cost=-14995, threshold=337)`
- `kernels/e11_finite.c`: `void spin(unsigned i, unsigned n) { while (i != n) i += 2; }` compiles to a real loop under `-std=c99`, and to just `retq` under `-std=c11`. The language version alone changes the semantics.

**Real code:**

| Inlining refused as "never" | SQLite | Lua | zstd |
|---|---|---|---|
| varargs | 1,541 | 13 | — |
| exposes `returns_twice` (`setjmp`) | — | 6 | — |
| recursive | 309 | 93 | 10 |
| `noinline` attribute (programmer's choice) | 4,637 | — | 345 |

Indirect calls make up 16.5% (SQLite) and 12.9% (Lua) of the calls that block load elimination (§4.1).

**Cx levers:**

- No `setjmp`/`longjmp`. Errors propagate structurally; `setjmp` lives only in FFI shims.
- Typed variadics: compile-time argument packs or slices.
- Closed sets of function values where possible (enums of functions or interfaces with a known implementation set), so calls can be devirtualized.
- Every loop and function must make progress; there is an explicit `loop forever` construct.

### 4.6 Linkage, interposition and separate compilation

**C rules.**

- Functions and globals have external linkage unless marked `static`.
- ELF allows a shared library's symbols to be interposed.
- Each translation unit is compiled alone.

**Code paths.**

- **External is the default.** A function is internal only if it is not externally visible (`ASTContext.cpp:13082-13083`; `CodeGenModule.cpp:6762-6763`, `:6826-6828`).
- **Most interprocedural optimization requires local linkage:**
  - GlobalOpt: everything beyond `unnamed_addr` (`GlobalOpt.cpp:1671-1673`); switching to the `fastcc` calling convention (`:1983-1988`)
  - ArgumentPromotion (`ArgumentPromotion.cpp:816-818`)
  - DeadArgumentElimination (`DeadArgumentElimination.cpp:463-466`)
  - GlobalsAA (§4.1)
  - The inliner's 15,000 "last call to a static function" bonus (`InlineCost.cpp:1251-1254`; `TargetTransformInfoImpl.h:98-101`)
- **Interposition:**
  - An interposable definition is never inlined (`InlineCost.cpp:3237-3239`).
  - Attributes are inferred only for exact definitions (`GlobalValue.h:473-490`).
  - For `-fPIC` (non-PIE), Clang passes `-fhalf-no-semantic-interposition` by default (`clang/lib/Driver/ToolChains/Clang.cpp:5930-5935`). Inlining is allowed, but references to default-visibility symbols still go through the PLT/GOT (`X86Subtarget.cpp:193-224`).

**Micro-kernel** (`kernels/e10_interpose.c`, `api()` calls `helper()` in the same file):

| Build | Result |
|---|---|
| `clang -O2 -fPIC` | `helper` inlined, no calls |
| `clang -O2 -fPIC -fsemantic-interposition` | 2 `call helper@PLT`, not inlined |
| `gcc 13 -O2 -fPIC` | 2 `call helper@PLT`: GCC honors ELF interposition by default |

**Real code.**

- **Visibility of definitions:** inlining failures because "definition is unavailable": Lua (32 files) **3,086**; SQLite amalgamation 262; zstd 206.
- **Lua's `NoDefinition` failures:** these outnumber its successful inlines (1,453).
- **The amalgamation as a workaround:** SQLite distributes an amalgamation to recover exactly this whole-program view.
- **LTO results:** §5 quantifies both the amalgamation and LTO.

**Cx levers:**

- Module-private by default, with an explicit `export`.
- Exported symbols are hidden or protected unless declared `interposable`.
- Module interfaces carry inferred effect summaries, so callers get LTO-grade information without LTO.

### 4.7 ABI and data layout

**Code paths.**

- **x86-64 SysV:** an aggregate larger than 16 bytes is class MEMORY (`clang/lib/CodeGen/Targets/X86.cpp:2111-2114`, `:2190-2195`, `postMerge` `:1819-1820`).
  - As an argument it becomes `byval`, which is copied to the stack (`X86.cpp:2827-2835`; `CGCall.cpp:3146-3147`).
  - As a return value it becomes `sret`, which also consumes a GPR (`X86.cpp:2690-2693`, `:3069-3072`).
  - The register budget is 6 GPRs and 8 SSE registers (`:3042-3043`).
- **AArch64 for comparison:** homogeneous FP aggregates of up to 4 members travel in registers (`Targets/AArch64.cpp:702-705`). The general limit is also 16 bytes (`:496-497`).
- **Layout follows declaration order.** "Layout each field, for now, just sequentially, respecting alignment" (`RecordLayoutBuilder.cpp:1439-1447`). Padding is only diagnosed (`-Wpadded`, `:2183-2193`).
- **Bit-fields:** access units must not overlap non-bit-field storage (the C11 memory-location rule), so writes are read-modify-write and cannot be widened past neighbours (`CGRecordLayoutBuilder.cpp:456-471`).

**Micro-kernels** (`kernels/e09_abi.c`, `e12_layout.c`):

```text
dot2(struct {double x,y}, …)    -> 4 xmm registers: mulsd, mulsd, addsd, ret
dot3(struct {double x,y,z}, …)  -> both structs on the stack: 6 memory operands
add3(…) returning struct v3     -> result written through hidden sret pointer
sizeof {char; double; char; double; short}  = 40 bytes
sizeof {double; double; short; char; char}  = 24 bytes (same fields, sorted)
```

**Cx levers:**

- Struct layout is unspecified by default: the compiler may sort, pack, split hot and cold fields, or go from array-of-structs to struct-of-arrays. `repr(C)` keeps exact C layout for FFI and I/O.
- A private calling convention between Cx functions keeps aggregates of up to N eightbytes in registers, including multi-register returns.
- SysV only for `extern "C"`.
- By-value parameters are immutable, so large ones can be passed by pointer without a copy.

### 4.8 Runtime model: `volatile`, atomics, libc by name, null pointers

- **`volatile`** accesses are not "simple" (`Instructions.h:276`), so LICM, SROA, mem2reg and GlobalOpt skip them (`LICM.cpp:1169-1170`; `PromoteMemoryToRegister.cpp:75-87`; `GlobalStatus.cpp:95-105`).
- **`_Atomic` plain access is `seq_cst`** (`CGAtomic.cpp:1656-1661`, `:2080-2085`). A non-constant `memory_order` becomes a runtime `switch` (`:1438-1459`). Sizes that aren't a power of two, or are over 16 bytes, become library calls (`:1166-1178`).
- **libc is optimized by name only.**
  - `TargetLibraryInfo.td` lists 528 functions; `inferNonMandatoryLibFuncAttrs` gives recognized names their semantics (`BuildLibCalls.cpp:336-340`).
  - `-fno-builtin` or `-ffreestanding` turns all of it off (`CGCall.cpp:2515-2519`; `TargetLibraryInfo.h:285-286`).
  - A hand-written `memcpy` equivalent gets none of it.
- **Null pointers:** `-fno-delete-null-pointer-checks` adds `null_pointer_is_valid` and suppresses every `nonnull` Clang would emit (`CGCall.cpp:2293-2294`, `:2783`, `:3535`). Plain C pointers never get `nonnull`/`dereferenceable`; C++ references do (`CGCall.cpp:3208-3219`).
- **Cx levers:**
  - Explicit MMIO load/store intrinsics instead of a `volatile` qualifier.
  - Atomic orderings must be compile-time constants, with no implicit seq_cst.
  - Copy, fill, compare, length and math are language operations with defined semantics.
  - Non-null, dereferenceable references by default; nullable pointers are opt-in.

---

## 5. Ablation: switching C rules off in real programs

### 5.1 Protocol

**Setup:**

- Each project was rebuilt once per variant; each variant adds one flag that relaxes one C rule.
- 6 rounds, variant order shuffled each round, every run pinned to one core.

**The per-round composite:**

| Project | Composite |
|---|---|
| SQLite | `speedtest1` total time |
| Lua | geometric mean of the 6 scripts |
| zstd | geometric mean of compression and decompression MB/s at levels 1 and 3 |

**How to read the numbers:**

- Each value is the **median change in speed across the 6 rounds** (positive = faster), then the **[min, max]** round, then **how many of the 6 rounds were faster** than the same round's baseline.
- **Controls** are byte-identical copies of each baseline binary. They measure the noise floor of this VM. The controls drift by −0.9% / +1.8% / +1.0% in the median, and single rounds range from −4% to +9%.
- **Rule of thumb:** a median within about ±2.5%, or a mixed faster count, means *no detectable effect*.

### 5.2 Relaxing individual C rules

| Variant (C rule relaxed) | SQLite | Lua | zstd |
|---|---|---|---|
| **Control** (identical binary) | −0.9% [−4.0, +4.1] 2/6 | +1.8% [−1.5, +4.4] 5/6 | +1.0% [−2.3, +8.6] 4/6 |
| `-fno-strict-aliasing` (no TBAA) | −0.2% [−9.5, +2.3] 2/6 | +1.5% [−2.3, +8.1] 5/6 | +1.3% [−0.9, +7.7] 4/6 |
| `-fwrapv` (signed overflow defined) | +1.4% [−9.5, +5.3] 4/6 | +2.0% [−4.1, +3.9] 5/6 | −0.5% [−2.1, +3.0] 3/6 |
| `-fno-delete-null-pointer-checks` | −0.9% [−7.3, +5.6] 3/6 | −1.7% [−9.0, +5.5] 3/6 | +0.4% [−6.4, +7.3] 3/6 |
| All three above (the Linux-kernel dialect) | +0.9% [−5.6, +2.3] 4/6 | +0.5% [−1.1, +5.2] 4/6 | **+2.5% [+1.5, +4.1] 6/6** |
| `-fno-math-errno` | *byte-identical code* | +2.2% [−0.7, +5.6] 5/6 (1 function changed) | *byte-identical code* |
| `-ffast-math` | −1.9% [−7.3, +8.8] 2/6 | −0.8% [−3.5, +1.7] 2/6 | +2.0% [−3.4, +4.9] 4/6 |
| `-fno-finite-loops` (drop C11 progress) | −1.4% [−5.3, +1.9] 3/6 | *byte-identical code* | +0.6% [−6.1, +5.1] 3/6 |
| `-fno-builtin` (forget libc semantics) | −3.5% [−6.9, +2.7] 2/6 | −1.6% [−6.7, +0.9] 1/6 | +3.2% [−0.7, +7.0] 5/6 |
| *Calibration:* `-march=native` (AVX-512) | +0.2% [−3.9, +4.2] 3/6 | +1.4% [−3.0, +7.7] 4/6 | **−9.4% [−11.6, −0.3] 0/6** |

The calibration row is not a C rule. It shows that the harness *does* detect changes of this size. For zstd, AVX-512 code generation makes decompression 21% slower at level 1.

### 5.3 How much of the program each rule touches (static, exact)

| Variant | SQLite: `.text` / functions whose size changed | Lua | zstd |
|---|---|---|---|
| `-fno-strict-aliasing` | −0.3% / 16.8% | −0.8% / 7.4% | −0.1% / 16.1% |
| `-fwrapv` | −0.4% / 17.6% | −0.2% / 8.2% | +0.1% / 3.6% |
| `-fno-delete-null-pointer-checks` | +0.3% / 15.2% | +0.2% / 1.6% | +0.0% / 3.0% |
| All three | −0.4% / 36.2% | −1.0% / 15.1% | +0.1% / 20.8% |
| `-fno-math-errno` | 0 / 0% | −0.0% / 0.1% | 0 / 0% |
| `-ffast-math` | −0.1% / 2.0% | −0.2% / 3.5% | +0.0% / 1.2% |
| `-fno-builtin` | −0.8% / 20.4% | +0.1% / 5.2% | −0.5% / 17.3% |

### 5.4 Translation-unit visibility: the one effect that shows up every round

| Build | SQLite | Lua |
|---|---|---|
| One translation unit (SQLite amalgamation; Lua `onelua.c`) | *baseline* | **+4.7% [+2.1, +9.0] 6/6** vs. separate files |
| Separate files (SQLite: 100 files; Lua: 32 files) | **−3.3% [−12.1, −0.2] 0/6** | *baseline* |
| Separate files + full LTO | +2.2% [−0.1, +8.3] 5/6 | +2.6% [−4.0, +8.7] 4/6 |
| One translation unit + full LTO | −1.4% [−3.5, +2.9] 2/6 | −0.8% [−3.0, +6.2] 3/6 |

**What else changes with visibility:**

- zstd with full LTO: +1.7% [−6.0, +3.1] 4/6.
- Code size moves a lot with visibility. SQLite's split build has **26.6%** less `.text`: less inlining, 68% of functions changed. Lua built as one unit has 34.7% more.

### 5.5 Reading the results (inference)

1. **Individual rules are small at whole-program scale.** On these three programs, no single UB-based C rule changes speed beyond the ±2.5% noise floor: not strict aliasing, not signed overflow, not null-check deletion, not forward progress. That is true even though each one rewrites up to 18% of functions (36% combined). The rules are real but local. The micro-kernels in §4 show where they bite (up to 34×), and in pointer-chasing integer code those places are not hot enough to move the total.
2. **Visibility is the one effect that shows up every round.** Compiling as one unit is 3–5% faster, and LTO recovers most of the loss for SQLite. This matches the census, where call opacity dominates, and it is exactly what Cx's module-private defaults and interface effect summaries (§6, priority 1) would give without LTO.
3. **FP and errno rules don't affect these programs.** `-fno-math-errno` leaves SQLite and zstd byte-identical, and the census found no math-errno vectorization failures in any of the three. The 2× and 8.1× kernel results apply to numeric code. That is a real but different workload, not measured here at program scale.
4. **Relaxing C rules never caused a consistent slowdown.** zstd was even consistently 2.5% *faster* under the kernel dialect (6/6); the cause is not attributed (possibly code layout or heuristic interaction). zstd's speed does not depend on the UB-derived facts these flags remove.
5. **Implication:** for C-like systems code, Cx's performance case rests on *making information available*: effects, escape, visibility, aliasing guarantees the programmer states. It does not rest on exploiting more undefined behaviour. The numeric rules (§6, priority 3) remove cliffs and should be adopted, but they should not be expected to speed up programs like these on their own.

**Limits:**

- One VM, 6 rounds; effects under about 2–3% are not detectable.
- Baseline SSE2 target.
- Three integer-heavy programs. Numeric, HPC or media code would weight the floating-point rules very differently.
- The raw per-round data and the scripts are in [`c-optimization-barriers/ablation/`](c-optimization-barriers/ablation/): `results.jsonl`, `analyze.py`, `summarize.py`.

---

## 6. Implications for Cx

The priorities below follow the evidence, not tradition. For each one:

- **Unlocks** names the IR facts it lets Clang/LLVM emit or prove.
- **C interop** names what has to stay available at the FFI boundary.

### Priority 1: effects and escape

The census says this matters most (§4.1).

| Language rule | Unlocks | C interop |
|---|---|---|
| Every function has an effect summary (reads, writes, which parameters, may call back, may synchronize). It is inferred inside a module, written in the module interface, and checked. | `memory(argmem: …)`, `nocallback`, `nosync`, `willreturn` on declarations. GlobalsAA and GVN see through calls. | `extern "C"` functions default to "unknown", with optional trusted annotations |
| References don't escape unless declared escaping. Storing an address in the heap or a global requires an explicit escaping type. | `captures(none)` + `noalias` on most pointer parameters; locals stay promotable (SROA, mem2reg) | Raw pointers stay available in `unsafe`/FFI code |
| Module-private by default, with `export` | Internal linkage: GlobalOpt, ArgumentPromotion, DeadArgElim, fastcc, the 15,000 "last call to a static" inline bonus | Exported C-ABI symbols explicitly marked |
| An ownership or region discipline for pointer fields **(the largest design question)** | Summaries more precise than "other memory". This targets the 93% "unknown" callees. | Not needed for FFI types |

### Priority 2: aliasing

The rules are cheap to adopt, and the 34× cliff comes from exactly this.

- `byte` is the only type with universal aliasing power. `u8`/`i8` are plain integers, with their own TBAA nodes.
- Tagged unions; reinterpretation only through `bit_cast`.
- Exclusive references by default. Shared mutable aliasing is opt-in and visible in the type. This gets lowered to `noalias` on parameters and scoped `!noalias` metadata for locals and fields, which Clang cannot do for C today (§4.2).
- Transitively read-only references: `readonly` + `!invariant.load`.
- Arrays carry their length (slices) and never decay. This fixes "cannot identify array bounds" and enables `dereferenceable`.

### Priority 3: numeric semantics

These are 1–2 line changes in CodeGen, and they remove performance cliffs.

- **Integer overflow:** overflow on every fixed-width integer traps in checked builds and is UB in release. Wrapping is explicit (an operator or a type). Emit `nsw`/`nuw` everywhere, `nsw` on `shl`, and `nuw` on index GEPs.
- **Indexes:** the default index type is pointer-width, so no `sext` and no SCEV wrap checks (§4.3).
- **Promotions:** none. Arithmetic stays in the operand type, and mixed widths or signs need explicit casts.
- **Math:** no `errno`. Math functions are pure: `llvm.sqrt` etc., which vectorize and can be hoisted.
- **FP reductions:** reassociation is opt-in *per operation* (for example a `sum` intrinsic or a `reassoc` float view), not global `-ffast-math`. This keeps the 8.1× where it's wanted without changing other code.

### Priority 4: control flow

- **Forward progress:** every loop and function must make progress; `loop {}` is the explicit infinite loop. Emit function-level `mustprogress`, which C never gets.
- **`setjmp`/`longjmp`:** removed from the language. Errors propagate structurally.
- **Variadics:** typed (compile-time packs or slices). C varargs are only for calling C.
- **Function values:** prefer closed sets (enums of functions, or interfaces with a known set of implementations) so the compiler can devirtualize.

### Priority 5: data layout and ABI

- **Struct layout:** unspecified unless `repr(C)`. The compiler may reorder and pack fields (a 40 → 24 byte example in §4.7).
- **Calling convention:** a Cx-internal convention returns multi-register aggregates in registers and passes aggregates of up to N eightbytes in registers. SysV applies only at `export "C"`.
- **By-value parameters:** immutable, so large values can be passed by reference without a copy.
- **Bit-fields:** replaced by explicit flag or bitset types with a declared container.

### Priority 6: frontend

This priority is about compile time and compiler maintenance, not runtime.

- Modules instead of textual `#include`.
- Typed constants and a small hygienic macro facility instead of the C preprocessor.
- A context-free grammar (`let x: T`, a distinct cast syntax), left-to-right types, and one declarator per declaration.
- One dialect: C23 prototype rules, no implicit int, no implicit function declarations, no K&R definitions, no tentative definitions.
- Full braces or named-field initializers; no designator overriding.
- Defined evaluation order (left to right). This also removes `SequenceChecker`.

### Suggested next step: prototype the rules as Clang flags, then measure

Most of priorities 2–4 are *emission* changes in `clang/lib/CodeGen`. Examples:

- `nuw` on unsigned arithmetic (`CGExprScalar.cpp:4763-4766`)
- function-level `mustprogress` (`CodeGenFunction.h:646-651`)
- `noalias` + `captures(none)` on every pointer parameter (`CGCall.cpp:3602-3604`)
- a distinct TBAA node for `uint8_t`/`int8_t` (`CodeGenTBAA.cpp:166-174`)
- math intrinsics without errno

Each can be added to our vendored Clang behind an experimental `-fcx-*` flag. Then we rerun the census and the benchmarks in §5 on the same three programs. That turns the "Unlocks" column into measured numbers before any syntax is designed.

**(inference)** The ablation in §5 can only measure C rules made *weaker*. The flag prototypes are the only way to measure rules that are *stronger* than C.

---

## 7. Reproducing

All inputs are in [`c-optimization-barriers/`](c-optimization-barriers/):

| Path | What |
|---|---|
| `kernels/e01…e12_*.c` | One micro-kernel per rule (§4) |
| `kernels/bench_kern.c`, `bench_driver.c` | Timed kernels. The driver is compiled separately so it can't see into the kernels. |
| `census/remarks.py` | Aggregates `-fsave-optimization-record` YAML by pass, remark and message |
| `census/callkinds.py`, `calleemem.py`, `pertu.py` | Classify the calls that block GVN (direct, indirect, external) and the memory effects of the callees |
| `ablation/build.sh`, `variants.txt` | Build SQLite, Lua and zstd per variant (`WORK=… LLVM=… ./build.sh sqlite nosa -fno-strict-aliasing`). LTO goes through `llvm-lto`, because the release's `ld.lld` needs ICU 70. |
| `ablation/bench.py`, `analyze.py`, `summarize.py`, `luabench/*.lua` | Benchmark harness, per-metric and per-round summaries |
| `ablation/results.jsonl` | Raw per-round results behind §5 (6 rounds × 37 binaries) |
| `kernels/RESULTS.txt` | Raw notes from the micro-kernel runs behind §4 |

**Example commands:**

```sh
# micro-kernel evidence, e.g. the errno case
clang -O2 -c kernels/e06_errno.c -Rpass=loop-vectorize -Rpass-analysis=loop-vectorize
clang -O2 -fno-math-errno -c kernels/e06_errno.c -Rpass=loop-vectorize

# census for one file
clang -O2 -c sqlite3.c -fsave-optimization-record -foptimization-record-file=sqlite3.yaml
python3 census/remarks.py sqlite sqlite3.yaml sqlite.json
```
