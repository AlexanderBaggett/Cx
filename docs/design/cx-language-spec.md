# Cx Language Design Specification — Draft 1

| | |
|---|---|
| **Status** | Design draft. Normative in intent, not yet implemented. |
| **Base language** | ISO C23 (ISO/IEC 9899:2024). Anything this document does not change is exactly C23. |
| **Reference compiler** | The Clang/LLVM 23.1.2 tree vendored in this repository |
| **Evidence** | [`docs/analysis/c-optimization-barriers.md`](../analysis/c-optimization-barriers.md), cited as **[A§n]**. Micro-kernels are cited as **[E01]**…**[E13]** from [`docs/analysis/c-optimization-barriers/kernels/`](../analysis/c-optimization-barriers/kernels/). |

---

## 0. How to read this document

**Rule format.** Every rule has four parts:

- **Rule:** normative.
- **Why:** the evidence or the optimization it serves.
- **Lowering:** which LLVM IR facts the Cx compiler emits because of the rule.
- **Enforcement:** how the rule is enforced, which is one of:
  - **compile-time**: violations are diagnosed and the program is rejected;
  - **checked**: violations trap in checked builds and are undefined behaviour in release builds (§16);
  - **contract**: the rule is a promise the programmer makes, like C's `restrict`. Violations are undefined behaviour, and a sanitizer may catch some of them.

**Terms:**

- **"Error"** means a required diagnostic; the translation fails.
- **"UB"** means undefined behaviour in release builds.
- **"Foreign"** means code or declarations that follow C semantics: C headers, and C functions called from Cx.

**Spelling rule.** No keyword is renamed. Cx keeps a subset of C23's keywords, each with its C23 spelling (§3). Cx adds no keywords. New concepts use only:

1. C23 attribute syntax in the `cx::` namespace, e.g. `[[cx::owned]]`;
2. `#pragma cx …`;
3. typedefs, macros and builtins declared in Cx standard headers;
4. file extensions.

---

## 1. Goals, non-goals and principles

### 1.1 Goals

1. **G1 Faster than C for the same algorithm.** Cx achieves this by giving the optimizer facts that C cannot express, not by adding more undefined behaviour.
2. **G2 C syntax, fewer keywords.** A C programmer can read Cx. Cx removes rare, exotic and performance-hostile keywords and features.
3. **G3 Zero-cost C interoperability.** Cx calls C and C calls Cx with no wrappers. Declarations are shared through headers.
4. **G4 A smaller, simpler compiler.** Cx has one dialect and one type qualifier, and the grammar can be parsed without Sema.
5. **G5 Every rule is measurable.** Each rule maps to IR facts, so its effect can be benchmarked (§20).

### 1.2 Non-goals

- A guarantee of memory safety. Cx is safer than C, but it is not Rust.
- Garbage collection, exceptions, generics, operator overloading, a new syntax.
- Source compatibility with arbitrary C. Porting is expected; see Appendix C.

### 1.3 Principles, derived from the evidence

- **P1 Information over UB.** In the analysis, relaxing C's UB-based rules (strict aliasing, signed overflow, null checks, forward progress) changed whole-program speed by no more than the ±2.5% noise floor [A§5.5]. What does matter:
  - **Call opacity.** In SQLite, 71% of the loads GVN failed to remove were blocked by a call. In 93% of those cases the callee had no inferable memory effects [A§4.1].
  - **Cross-file visibility.** Visibility changed whole-program speed by 3.3–4.7% in every round [A§5.4].

  Cx therefore makes *information* the default:
  - pointers don't alias, don't escape and aren't null;
  - functions carry effect summaries;
  - symbols are module-private.
- **P2 Flip the defaults, keep the syntax.** The common case costs no annotation. The rare case, such as aliasing pointers, retained pointers or interposable symbols, is written explicitly with an attribute.
- **P3 Remove cliffs.** Some rules are cheap to change and remove measured performance cliffs:
  - `char` aliasing: 34× [E02]
  - FP reduction order: 8.1× [E07]
  - `errno`: 2.0× [E06]
  - unsigned wrap: runtime checks [E05]
  - `_Complex`: library calls for division, and a NaN fallback for multiplication [E13]
- **P4 Say how each rule is enforced.** Every rule is compile-time, checked, or contract. Contracts are kept to a minimum, and each one is listed in §16.

---

## 2. Cx at a glance

### 2.1 The major changes

| # | Area | C23 | Cx | Main optimization unlocked | Evidence |
|---|---|---|---|---|---|
| 1 | Pointer parameters | May alias, may escape, may be null, extent unknown | **Exclusive, non-escaping, non-null, dereferenceable** by default. `[[cx::alias]]`, `[[cx::escapes]]` and `[[cx::nullable]]` opt out. | `noalias`, `captures(none)`, `nonnull`, `dereferenceable`, `writable` → GVN, LICM (including store promotion), vectorization without runtime checks, SROA of locals passed by address | [A§4.1], [A§4.2], [E01], [E03], [E04] |
| 2 | Pointer fields | Nothing is known | `[[cx::owned]]` fields form an ownership tree. Regions rooted at parameters are disjoint. | Scoped `!noalias` metadata on loads, stores *and calls*. Calls that don't receive a region can't touch it. | [A§4.1] (93% of callees summarize as "unknown") |
| 3 | Effects | Unknown for every call the compiler can't see into | Every function has an effect summary: inferred, exported, checked. Function-pointer types carry effects. | `memory(...)`, `nocallback`, `nosync`, `!callees` | [A§4.1] |
| 4 | Linkage | External by default, interposable | **Module-private by default**; `extern` exports. Protected visibility. Summary files give cross-module facts without LTO. | IPO (GlobalOpt, ArgPromotion, DeadArgElim, fastcc), GlobalsAA, inlining | [A§4.6], [A§5.4] |
| 5 | Type-based aliasing | `char` aliases everything; unions pun | `char` is an ordinary type; universal aliasing only through `byte`. No union punning. Enums and signedness are distinct. | Precise TBAA | [A§4.2], [E02] |
| 6 | Integers | Unsigned wraps; mixed-sign conversions are silent | Overflow is a violation for all standard integer types, with explicit wrapping types. Mixed signedness and implicit narrowing are errors. | `nsw`/`nuw` everywhere, IV widening, trip counts | [A§4.3], [E05] |
| 7 | Floating point | libm sets `errno`; reassociation is all-or-nothing (`-ffast-math`) | No `errno`; math functions are pure. Scoped reassociation pragma. | Math intrinsics and vectorization; vectorized reductions | [A§4.4], [E06], [E07] |
| 8 | Control flow | `setjmp`; varargs; backward `goto`; no forward-progress guarantee for C functions | No `setjmp`. No Cx variadic definitions. `goto` is forward-only. Every function must make progress. | Inlining, reducible CFGs, `mustprogress`/`willreturn` | [A§4.5], [E08], [E11] |
| 9 | Layout and ABI | Declaration-order layout; aggregates over 16 bytes go through memory | Layout is unspecified unless `[[cx::c_layout]]`. `cxcall` convention between Cx functions. | Smaller structs; aggregates in registers | [A§4.7], [E09], [E12] |
| 10 | Concurrency and I/O | `volatile` and `_Atomic` type qualifiers | Both keywords removed. Atomics are explicit operations with constant orderings; MMIO uses intrinsics. | No hidden seq_cst or lock library calls; `const` is the only qualifier | [A§4.8] |
| 11 | Keywords | 54 keywords + 5 alternate spellings | **41 keywords**; 13 keywords and all 5 alternate spellings removed (§3) | Smaller language; no x87 or complex-number library calls | [E13] |
| 12 | Frontend | 13 dialects; textual includes; context-sensitive grammar | One dialect. Header units. Typedef names are known without lookup. Defined evaluation order. | Compile time and compiler size | [A§2], [A§3] |

### 2.2 What does not change

- The expression, statement and declaration syntax of C23.
- The preprocessor, with header units (§13).
- The object model for the types Cx keeps.
- Manual memory management.
- The C standard library, called as foreign code or through Cx-annotated headers (§15).
- The platform C ABI for everything that crosses into or out of C.

---

## 3. Keywords

C23 has 54 keywords plus 5 alternate spellings (`_Alignas`, `_Alignof`, `_Bool`, `_Static_assert`, `_Thread_local`). Cx keeps 41, each with its C23 spelling, and removes 13 keywords and all 5 alternate spellings. A removed keyword stays **reserved**: using it is an error with a message that names the replacement. Foreign C headers are still parsed with the full C23 keyword set (§14).

### 3.1 Kept (41)

| Keyword(s) | Cx semantics |
|---|---|
| `break` `case` `continue` `default` `do` `else` `for` `if` `return` `switch` `while` | As C23, with the restrictions in §11 (no jumps past declarations; `case` labels only at the top level of the `switch` body) |
| `goto` | **Forward jumps only**, never into a block or past a declaration (§11.2) |
| `char` `short` `int` `long` `signed` `unsigned` `float` `double` `void` `bool` | As C23, except: plain `char` is unsigned (§9.1); overflow rules (§7); the type combination `long double` is removed (§3.3) |
| `true` `false` `nullptr` | As C23 |
| `struct` `union` `enum` | Struct layout is unspecified unless `[[cx::c_layout]]` (§9.4). Unions don't allow type punning (§9.3). Enums are closed (§9.2). |
| `typedef` | File scope only, and may not be shadowed (§13.3) |
| `const` | The only type qualifier. It gains meaning for pointer parameters: the pointee is immutable for the whole call (§5.1). |
| `static` | Block scope: static storage duration, as C23. File scope: accepted and redundant, because file scope is private by default (§4.1). The `[static n]` array-parameter form is removed, because extents are implied (§5.6). |
| `extern` | On a definition: **exports** it from the module. On a declaration: declares an entity defined elsewhere (§4.1). |
| `inline` | On an exported function, the body becomes part of the module's interface for cross-module inlining. Otherwise it is a hint. C99's inline-definition and external-definition rules are removed (§6.8). |
| `sizeof` `alignof` `alignas` | As C23. `sizeof` is always a constant expression (no VLAs). |
| `static_assert` `constexpr` | As C23. `constexpr` is the preferred way to write constants (§13.2). |
| `thread_local` | As C23 |
| `typeof` | As C23 |
| `_Generic` | Kept for type-generic macros (`<tgmath.h>`, `<cxfmt.h>`). Compile time only. |

That is 12 control keywords (including `goto`), 10 basic types, 3 literals, 3 aggregate keywords, and 13 others (`typedef`, `const`, `static`, `extern`, `inline`, `sizeof`, `alignof`, `alignas`, `static_assert`, `constexpr`, `thread_local`, `typeof`, `_Generic`): **41**.

### 3.2 Removed keywords (13)

| Removed | Why | Replacement |
|---|---|---|
| `restrict` | Exclusivity is the *default* for parameters, and C's restrict on locals and fields is ignored by Clang anyway (`CGCall.cpp:3602-3604` is its only lowering). | Default parameter semantics (§5.1); `[[cx::owned]]` for fields (§5.3); `[[cx::alias]]` to opt out |
| `volatile` | Performance-hostile: volatile accesses are never "simple", so LICM, SROA, mem2reg and GlobalOpt skip them [A§4.8]. Its real uses are MMIO and `setjmp`, and `setjmp` is gone. | `<cxmmio.h>` intrinsics (§12.3). Signal handlers use atomics (§12.4). |
| `_Atomic` | Plain accesses are hidden seq_cst (`CGAtomic.cpp:1656-1661`). A runtime order becomes a `switch` (`:1438-1459`), and odd sizes silently become lock library calls (`:1166-1178`). | Explicit atomic operations on ordinary objects with constant orderings (§12.1) |
| `register` | No semantics in C23 beyond forbidding `&` | None |
| `auto` | Rare in C. C23 type inference adds Sema complexity for little benefit in a C-sized language. | Write the type, or use `typeof` |
| `typeof_unqual` | Rare. With `const` as the only qualifier it would only strip `const`. | `typeof` plus an explicit type |
| `_Noreturn` | Deprecated by C23 itself | `[[noreturn]]` |
| `_Complex` | Performance-hostile. Complex multiply adds a NaN check that falls back to a `__muldc3` library call, and complex divide is *always* a `__divdc3` library call (Annex G semantics) [E13]. Rare outside numeric code. | `<cxcomplex.h>` struct types with inline arithmetic. The Annex G NaN-recovery rules are opt-in there. |
| `_Imaginary` | Exotic; optional in C and not implemented by Clang | None |
| `_Decimal32` `_Decimal64` `_Decimal128` | Exotic, and software-emulated on mainstream CPUs | A library, if ever needed |
| `_BitInt` | Exotic. Arbitrary widths lower to multi-word code: one `_BitInt(256)` divide is 300 instructions [E13]. | `<stdint.h>` fixed widths; `int128_t`/`uint128_t` where the target supports them |

The alternate spellings `_Alignas`, `_Alignof`, `_Bool`, `_Static_assert` and `_Thread_local` are removed too. Each duplicates a kept lowercase keyword.

### 3.3 Removed type combinations and syntax that aren't keywords

| Removed | Why | Replacement |
|---|---|---|
| `long double` | x87 80-bit on x86-64. Arguments are passed in memory (X87 class, `X86.cpp:2827-2835`), and code uses the x87 stack and is never vectorized [E13]. | `double`. A library type for extended precision. |
| Variable-length arrays and `alloca` | Dynamic `alloca` disables inlining of the containing function (`InlineCost.cpp:1609-1614`). VLA types contain runtime expressions [A§3.7]. | Fixed-size arrays, heap buffers, array-parameter extents (§5.6) |
| `...` in Cx function *definitions* | Varargs functions are never inlined, never get `fastcc`, and spill registers [A§4.5], [E08] | Typed argument arrays via `<cxfmt.h>` macros (§6.5). Calling foreign varargs functions is still allowed. |
| K&R definitions, `f()` meaning "unspecified parameters", implicit `int`, implicit function declarations | Already removed or deprecated by C23; Cx removes the remaining compatibility modes [A§3.5] | Prototypes (C23 rules) |
| Tentative definitions, common symbols | Storage is decided late; common symbols are interposable [A§3.6] | One definition per object (§4.5) |
| Digraphs (`<:` `:>` `<%` `%>` `%:`) | Exotic; lexer complexity | The normal punctuators |
| Computed `goto` (`&&label`, `goto *p`: GNU extension) | `indirectbr` blocks inlining (`InlineCost.cpp:3311-3356`) and makes the CFG highly connected | `[[cx::musttail]]` dispatch (§6.6) |
| GNU nested functions, statement expressions | Exotic extensions | Static functions; `constexpr` |

---

## 4. Program structure: modules, headers, linkage

### 4.1 Modules and linkage

**Rule 4.1.1: files.** A Cx source file (`.cx`) is a **module**. A Cx header (`.cxh`) is a **header unit** (§13.1). Files with any other extension that are `#include`d are **foreign** headers (§14).

**Rule 4.1.2: default linkage.** File-scope declarations without a storage-class specifier have **internal linkage** (module-private).

- `extern` on a *definition* gives external linkage: the definition is **exported**.
- `extern` on a *declaration* refers to an entity exported by another module or by foreign code.
- `static` at file scope is accepted and redundant.

**Why:**
- Most interprocedural passes require local linkage: GlobalOpt (`GlobalOpt.cpp:1671-1673`, `:1983-1988`), ArgumentPromotion (`:816-818`), DeadArgumentElimination (`:463-466`), GlobalsAA (`GlobalsModRef.cpp:276-278`), and the inliner's 15,000 "last call to a static function" bonus (`InlineCost.cpp:1251-1254`) [A§4.6].
- In C, forgetting `static` silently disables all of them.

**Lowering:** `internal` linkage, unless the definition is exported.

**Enforcement:** compile-time.

### 4.2 Interposition

**Rule 4.2.1.** Exported definitions have protected visibility in shared objects, and are `dso_local` in executables. They cannot be interposed unless declared `[[cx::interposable]]`.

**Why:** an interposable definition is never inlined (`InlineCost.cpp:3237-3239`), gets no inferred attributes (`GlobalValue.h:473-490`), and is called through the PLT [A§4.6], [E10].

**Enforcement:** compile-time (visibility is a property of the definition).

### 4.3 Summary files: cross-module facts without LTO

**Rule 4.3.1: what a summary contains.** Compiling module `M` produces `M.cxs` alongside the object file. For every exported function it records:
- the effect summary (§6.1);
- the facts the compiler proved about the parameters (captures, the `returns` relation, nullness of the return value);
- the body, if the function is declared `inline` (§6.8).

For every exported object it records its type, extent and constness.

**Rule 4.3.2: how clients use summaries.** When compiling a client module, the compiler resolves every `extern` Cx declaration to the defining module's summary. The build system supplies the mapping, for example with `-fcx-summary-dir=`. Resolution order:

1. If the summary exists and its hash matches, the client uses it.
2. Otherwise the client uses the declaration's explicit attributes.
3. Otherwise the client assumes unknown effects.

**Rule 4.3.3: cycles.** Modules that depend on each other in a cycle are compiled as a group. The compiler either iterates the summaries to a fixed point (`-fcx-summary-iterate`), or uses only the explicit declared summaries inside the cycle.

**Why:**
- Lua built as one file is 4.7% faster than as 32 files in every round [A§5.4].
- In the 32-file Lua build, 58.7% of the calls that block load elimination go to another file [A§4.1].
- Summaries give clients the facts that matter without whole-program LTO. They are like ThinLTO summaries, but produced at compile time and consumed by the front end.

**Enforcement:** compile-time. A stale summary is detected by its hash, and the build system rebuilds dependents when a summary changes.

### 4.4 Exported mutable globals

**Rule 4.4.1.** Exported non-`const` objects are allowed. Their effect class is `shared` (§6.1), so any function that touches them is summarized as touching shared state. Module-private globals are the norm.

### 4.5 Definitions

**Rule 4.5.1: one definition.** Every object and function has exactly one definition.
- At file scope, `int x;` is a definition (zero-initialized). `extern int x;` is a declaration.
- Tentative definitions and common symbols do not exist.

**Rule 4.5.2: identical redeclarations.** All declarations of an entity must have identical types. C's compatible-type rules and composite types are not used; struct types are nominal, identified by module and tag.

**Why:** this removes `mergeTypes` and redeclaration merging from the semantic core [A§3.6], and object storage is known as soon as the object is defined.

**Enforcement:** compile-time.

---

## 5. Pointers, aliasing and escape

This is the core of Cx's performance model.

### 5.1 The parameter contract

For a parameter `P` of pointer type `T *` (where `T` may be `const`-qualified), and without opt-out attributes, the following hold during every execution `B` of the function body. `B` includes every function it calls.

- **5.1.1 Exclusive.**
  - If an object `X` is accessed through an lvalue whose address is *based on* `P`, and `X` is modified during `B` by any means, then every access to `X` during `B` goes through an lvalue based on `P`.
  - This is C23's `restrict` (6.7.4.2), applied by default. "Based on" has C's meaning: values computed from `P` by arithmetic, conversion, member access or subscripting.
- **5.1.2 Read-only is immutable.** If `T` is `const`-qualified, the objects accessed through `P` are **not modified by anyone** during `B`, and writing through a pointer derived from `P` by casting away `const` is UB.
- **5.1.3 Non-escaping.**
  - No value based on `P` is stored into any object except an automatic variable of `B` whose address is not taken. No such value is exposed as an integer (§5.5), passed to an `[[cx::escapes]]` parameter, or freed.
  - `P`-based values *may* be returned. The caller then treats the result as based on the argument it passed.
- **5.1.4 Non-null.** `P` is not a null pointer.
- **5.1.5 Dereferenceable.** If `T` is a complete object type, `P` points to at least `sizeof(T)` bytes that stay valid (not freed) throughout `B`. Array parameters extend this to `n` elements (§5.6).

**Opt-outs.** These attributes attach to the parameter, for example `void f([[cx::alias, cx::nullable]] int *p)`:

| Attribute | Removes | Typical use |
|---|---|---|
| `[[cx::alias]]` | 5.1.1 and 5.1.2 | `memmove`; in-place operations that take overlapping ranges |
| `[[cx::escapes]]` | 5.1.3: the callee may store, retain or free the pointee | Container insert functions; `free`; registering callbacks |
| `[[cx::nullable]]` | 5.1.4 and 5.1.5 | Optional arguments |

**Why:**
- LLVM only treats `noalias` or `byval` arguments as identified objects (`AliasAnalysis.cpp:903-905`, `:921-922`).
- Passing a pointer to an unannotated function captures it (`CaptureTracking.cpp:304`), after which every call may clobber the pointee (`BasicAliasAnalysis.cpp:980-996`).
- Store promotion in LICM needs a `writable` + `noalias` argument (`AliasAnalysis.cpp:998-1021`). SQLite got only 117 promotions against 24,989 "loop may invalidate" misses [A§4.1].
- The measured symptoms are [E01] (runtime alias checks), [E03] (a local reloaded after a call) and [E04] (a `const` pointee reloaded).

**Lowering (Clang `EmitFunctionProlog`, today `CGCall.cpp:3602-3604`):**

| Parameter | IR attributes |
|---|---|
| `T *` | `noalias nonnull noundef dereferenceable(sizeof T) captures(ret: address, provenance) writable` (plus `nofree`) |
| `const T *` | Same, but `readonly` instead of `writable`. Loads through `P` may be marked `!invariant.load` within `B`. |
| With opt-outs | The corresponding attributes are dropped |

At call sites the same facts let SROA and mem2reg keep a local's value in registers after `f(&local)`, because the local stays uncaptured.

**Enforcement:**
- 5.1.3 is **compile-time**. The compiler tracks based-on values within each function and rejects stores of them to memory. This is local and complete, because every callee declares what it does with its parameters.
- 5.1.4 is **checked**: an entry check in checked builds; call sites that pass a literal `nullptr` are rejected.
- 5.1.1 and 5.1.2 are a **contract**, like `restrict`. The compiler rejects the provable violations: the same object, or overlapping `P`/`P+k` with a known extent, passed to two parameters where one is written. Checked builds check range overlap for parameters with extents.

### 5.2 Local pointers

**Rule 5.2.1.** A local pointer variable initialized from a value based on an exclusive parameter is itself based on that parameter, and inherits its scope for alias metadata (§5.3). Pointers loaded from memory are *shared* unless they are loaded from an owned field.

**Lowering:** none beyond metadata propagation. LLVM already handles `noalias` arguments through inlining (`InlineFunction.cpp:1226-1228`).

### 5.3 Owned fields and regions

**Rule 5.3.1: owned fields.** A pointer member declared `[[cx::owned]] T *f;` in a struct `S` is an **owning** field. For every live object `s` of type `S`, while `s.f` is non-null and points to `Y` (an object or array), the following hold:

- `Y` is reachable only through `s.f`, or through local pointers loaded from it. No other owned field, global, or `[[cx::escapes]]`-retained pointer refers into `Y`.
- Ownership moves only by overwriting: after `t.g = s.f;`, the program must set `s.f` (to null or another pointer) before `s.f` is used again.

**Rule 5.3.2: regions.** The **region** of a pointer `p` is `*p` together with everything transitively owned through owned fields of `*p`. An exclusive parameter extends 5.1.1 to its whole region: objects in `region(P)` that are modified during `B` are accessed only through `P`-based or owned-derived lvalues.

**Lowering:**

- **Scopes.** For each function, the front end creates one alias scope per *access path*, meaning a parameter root followed by a sequence of owned fields: `P`, `P.f`, `P.f.g`, … Paths deeper than *k* (default 3) are cut off at a summary scope `P.*`. Module-private globals each get a scope, and so does a *shared* scope.
- **Tagging loads and stores.** Every load or store whose address derives from a path gets `!alias.scope {path}` and `!noalias {every other root's scopes}`. Different paths under the same root are also mutually `!noalias`, because different objects in an ownership tree are disjoint.
- **Tagging calls.** A call instruction gets `!alias.scope` for:
  - the scopes of the regions passed to it;
  - `shared`, if the callee's summary touches shared memory;
  - the scope of every module-private global the callee may access. That is all of them if its summary is unknown or includes `callback`, or if the global's address has escaped.

  It gets `!noalias` for every other root's scopes. For parameter roots this is sound even when the callee's effects are unknown. By 5.3.1 and 5.1.1, a call can reach an object in `region(Q)` only through a `Q`-derived pointer, and such a pointer must be passed to it.
- **Which roots get scopes.** Only parameters that are exclusive *and* non-escaping, plus module-private globals. `[[cx::alias]]` and `[[cx::escapes]]` parameters get none.
- **LLVM needs no change.** `ScopedNoAliasAA::getModRefInfo(Call, Loc)` already uses scope metadata on calls (`ScopedNoAliasAA.cpp:80-94`).

**Why:** in SQLite, 93.4% of the same-file callees that block GVN summarize as "may read or write any memory". Writing through a pointer loaded from a struct lands in LLVM's catch-all "other memory" location [A§4.1]. Ownership lets the front end give those objects names (scopes) that survive calls.

**Enforcement:** **contract**.
- The compiler diagnoses the obvious violations: the same value stored to two owned fields; an owned field stored to a global; the address of an owned object passed to an `[[cx::escapes]]` parameter without clearing the field.
- Ownership is opt-in per field. Unmarked pointer fields are *shared*, which gives exactly C's (conservative) semantics.

### 5.4 Type-based aliasing

**Rules:**

1. **`char` is ordinary.** `char`, `signed char` and `unsigned char` are ordinary integer types with their own alias classes. They do **not** alias other types.
2. **`byte` is the universal type.** Universal access to object representations is only possible through `byte`, declared in `<cxbyte.h>` as `typedef unsigned char byte [[cx::may_alias]];`, or through `void *` memory functions (`memcpy`, `memset`, `memcmp`, …). `[[cx::may_alias]]` is reserved for standard headers.
3. **Signedness is distinct.** Signed and unsigned variants of a type are distinct for aliasing: an `int` object may not be accessed as `unsigned`.
4. **Enums are distinct.** Each enum type is its own alias class.
5. **No union punning.** Reading a union member other than the one last stored is UB. Reinterpreting bits uses `cx_bit_cast(T, x)` (`<cxbyte.h>`, lowered via `__builtin_bit_cast`) or `memcpy`.
6. **`void *` has no access type.** An access through `void **` to a `T *` object is UB.

**Why:**
- The byte-fill loop [E02] is 34× slower only because the `char` store may overwrite `b->data` and `b->len` (`CodeGenTBAA.cpp:166-174`).
- Type-based alias analysis prevents 60% of SQLite's store-clobbered loads [A§4.2].
- Unions currently get no TBAA at all (`CGExpr.cpp:5856-5858`).

**Lowering:**
- The char types get their own TBAA nodes. `byte` becomes the "omnipotent" node.
- Union members get struct-path TBAA.
- Enum types get their own nodes, and so do unsigned types.

**Enforcement:** contract (type-based aliasing is inherently a contract). A type-aware sanitizer can check it.

### 5.5 Provenance and integer↔pointer conversion

**Rules:**

1. **Pointer to integer.** `(uintptr_t)p` yields the address and **does not expose** `p`. The integer cannot be turned back into a usable pointer.
2. **Exposing.** `cx_expose(p)` (`<cxptr.h>`) returns the address and *exposes* the object. The object is then treated as escaped.
3. **Integer to pointer.** A pointer is made from an integer only by `cx_with_addr(p, a)`, which gives a pointer with `p`'s provenance and address `a`. The other ways are a constant address, which is valid only for MMIO (§12.3), or `cx_from_exposed(a)`, which may only access exposed objects.
4. **Non-escaping parameters can't be exposed.** A non-escaping parameter may not be exposed.

**Why:** `ptrtoint` counts as a capture (`CaptureTracking.cpp:391-393`), and `inttoptr` makes any escaped object a possible target (`AliasAnalysis.cpp:948-957`). Strict provenance keeps address arithmetic, hashing, alignment tricks and tagged pointers from defeating escape analysis.

**Enforcement:** compile-time (the cast forms are restricted); the provenance itself is a contract.

### 5.6 Arrays and extents

**Rule 5.6.1: array parameters carry an extent.** An array parameter `T a[n]` in a prototype, where `n` is an earlier parameter or a constant, means `a` points to at least `n` elements. This is C's `[static n]`, which is now implied, so the `static` form is removed. Such a parameter is exclusive, non-null and non-escaping (5.1). `sizeof a` on an array parameter is an error.

**Rule 5.6.2: flexible array members.** Only `T a[];` is a flexible array member. Trailing `[0]` and `[1]` arrays have their declared bounds, equivalent to `-fstrict-flex-arrays=3` (`LangOptions.h:390-392`).

**Rule 5.6.3: no VLAs.** There are no variable-length arrays. `sizeof` is always a constant.

**Why:** array decay loses extents, which is why the vectorizer reports "cannot identify array bounds" [A§3.7], [A§4.2].

**Lowering:**
- A constant `n` gives `dereferenceable(n*sizeof T)`.
- A runtime `n` gives bounds checks in checked builds. Runtime alias checks aren't needed, because `noalias` already covers them. **(inference)** LLVM's `dereferenceable(N)` takes a constant, and its assume-bundle queries compare against a constant argument (`Loads.cpp:225`), so runtime-size dereferenceability is future LLVM work (§21).

**Enforcement:** compile-time for the syntax; checked for the bounds.

### 5.7 Null pointers

`nullptr` and `NULL` are as C23. Parameters are non-null by default (5.1.4). Returned pointers, locals and fields are nullable, exactly as in C. `[[cx::nullable]]` exists only to opt a parameter out.

---

## 6. Functions and effects

### 6.1 Effect summaries

**Rule 6.1.1: every function has an effect summary.** A summary is a set of the following items:

| Item | Meaning |
|---|---|
| `reads(x…)` / `writes(x…)` | `x` is a parameter name (meaning its region, §5.3), `module` (the defining module's private state), or `shared` (exported globals and memory reachable only through shared pointers) |
| `alloc` | Allocates or frees memory (allocator state) |
| `io` | Interacts with the environment: system calls, foreign state including `errno` |
| `callback` | May call functions supplied from outside its own module: function-pointer arguments, or exported functions of other modules that are not in its summary |
| `sync` | Performs atomic operations or synchronization |
| `none` | No effects: the function is pure |

**Rule 6.1.2: inference and interfaces.**
- The compiler **infers** the summary of every Cx function definition, bottom-up over the module's call graph.
- Exported summaries go into the summary file (§4.3).
- A declaration may state an explicit **upper bound**, for example `[[cx::effects(reads(src), writes(dst))]]`. The definition is checked against it.
- The C23 standard attributes `[[unsequenced]]` and `[[reproducible]]` are accepted and mapped to the corresponding summaries. Clang 23.1.2 does not implement them (no definition in `Attr.td`).

**Rule 6.1.3: foreign functions.** Foreign functions have unknown effects (all items) unless their declaration carries `[[cx::effects(...)]]`. Cx's standard headers annotate the C standard library (§15).

**Lowering, from the caller's view:**

| Summary item | IR |
|---|---|
| Parameter regions | `memory(argmem: …)` for the direct objects, plus scope metadata (§5.3) for owned paths |
| The callee's `module` | `inaccessiblemem`: the callee's private globals cannot be named by the caller |
| `alloc`, `io` | `inaccessiblemem` (and `errnomem` for foreign `io`) |
| `shared` | `other` |
| No `callback` | `nocallback` |
| No `sync` | `nosync` |

GlobalsAA then sees through calls to Cx functions (`GlobalsModRef.cpp:547-553` requires `nocallback` + `nosync` for external calls).

**Why:** without summaries, a call to anything opaque is `ModRef` for all memory (`BasicAliasAnalysis.cpp:1076-1077`). That is the single largest source of missed optimizations measured [A§4.1]. It even keeps an address-never-taken `static` counter in memory [E03], because the callee might call back into the module.

**Enforcement:** compile-time. Inferred summaries are sound by construction, and explicit bounds are checked.

### 6.2 Function pointers

**Rule 6.2.1: effects are part of the type.** A function-pointer type may carry an effect bound: `typedef int (*cmp_fn)(const void *a, const void *b) [[cx::effects(reads(a, b))]];`

- Calls through the pointer assume the bound.
- Assigning a function whose summary exceeds the bound is an error.
- An unannotated function-pointer type has unknown effects.

**Rule 6.2.2: closed sets.** If a function-pointer type is module-private and no value of that type enters the module from outside, the set of possible targets is the set of functions converted to that type in the module.

**Lowering:** the call's effects are the union of the targets' summaries, plus `!callees` metadata, which the Attributor and CalledValuePropagation already understand (`AttributorAttributes.cpp:12384`, `CalledValuePropagation.cpp:400`).

**Why:** indirect calls are 16.5% (SQLite) and 12.9% (Lua) of the calls that block load elimination. They are never inlined (`InlineCost.cpp:3189-3191`) and they block `norecurse` [A§4.1], [A§4.5].

**Enforcement:** compile-time.

### 6.3 Forward progress

**Rules:**

1. **Functions.** Every function must make forward progress: it may not run forever without an observable effect (I/O, an atomic or MMIO operation, or a call with `io`/`sync` effects).
2. **Loops.** Loops fall under the same rule, whatever their controlling expression.
3. **Intentional infinite loops.** A loop with an omitted or constant-true controlling expression may run forever only if its body performs an observable effect. An infinite loop with an empty body is an error.

**Why:** C functions never get `mustprogress` (`CodeGenFunction.h:646-651`), so LLVM cannot infer `willreturn` for C functions with loops (`FunctionAttrs.cpp:2161-2174`). Loops with constant conditions or built from `goto` get no progress guarantee [A§4.5]. [E11] shows the language version alone changing codegen.

**Lowering:** `mustprogress` on every function and loop.

**Enforcement:** contract, with compile-time rejection of empty infinite loops.

### 6.4 `setjmp` and `longjmp`

**Rule 6.4.1.** Cx code may not call `setjmp`, `sigsetjmp`, `longjmp`, `vfork`, `getcontext`, or any function Clang treats as `returns_twice` (`CGCall.cpp:2914-2921`). A foreign `longjmp` that crosses Cx frames is UB. Error handling uses return values; C23 `[[nodiscard]]` is recommended on status-returning functions.

**Why:** `returns_twice` disables the following [A§4.5], [E08]:
- inlining (`cost=never`);
- register coalescing for the whole function (`RegisterCoalescer.cpp:4306-4314`);
- stack coloring (`StackColoring.cpp:721-724`);
- tail-call elimination.

It also makes every non-escaped object look clobbered (`BasicAliasAnalysis.cpp:984-989`).

**Enforcement:** compile-time.

### 6.5 Variadic functions

**Rules:**
- Cx functions cannot be *defined* with `...`. `<stdarg.h>` is not available to Cx code.
- Cx code may call foreign variadic functions such as `printf`. Foreign `...` declarations remain valid in foreign headers.
- Cx code uses typed formatting from `<cxfmt.h>`. Its macros (the preprocessor still has `__VA_ARGS__`) expand `cx_print("%d %s\n", n, s)` into a call with a typed argument array: `(const struct cx_arg[]){ CX_ARG(n), CX_ARG(s) }` plus a count. `CX_ARG` uses `_Generic` to tag each value.

**Why:**
- A function that calls `va_start` is never inlined (`InlineCost.cpp:3352-3356`), never gets `fastcc` (`GlobalOpt.cpp:1722-1723`), and never has its arguments promoted (`ArgumentPromotion.cpp:824-829`). Callers must set `%al`, and the callee spills every argument register.
- SQLite alone has 1,541 varargs inline refusals [A§4.5].

**Enforcement:** compile-time.

### 6.6 Guaranteed tail calls

**Rule 6.6.1.** `[[cx::musttail]] return f(args);` is a guaranteed tail call. The callee's signature must match the caller's, and the arguments must not refer to the caller's locals.

**Why:** threaded interpreters, like Lua's dispatch, otherwise need GNU computed goto, which Cx removes (§3.3).

**Lowering:** `musttail` (Clang's `clang::musttail`, `Attr.td:1966-1970`).

### 6.7 `main`

**Rule 6.7.1.** `main` may not be called or have its address taken.

**Why:** C allows recursive `main`, so Clang adds `norecurse` only for C++ `main` (`CodeGenFunction.cpp:1076-1082`). GlobalOpt localizes globals used only by a non-recursive function (`GlobalOpt.cpp:1482-1489`).

**Lowering:** `norecurse` on `main`.

### 6.8 `inline`

**Rule 6.8.1.**
- `inline` on an exported definition puts its body in the summary file (§4.3), so client modules can inline it without LTO. The exported symbol is still emitted once, in the defining module.
- `inline` on a private function is a hint.
- `static inline` in a header unit behaves as today.
- C99's inline-definition and external-definition distinction (`available_externally`) is removed (`ASTContext.cpp:13129-13130`).

### 6.9 By-value aggregates

**Rule 6.9.1.** Aggregates passed by value keep C's value semantics. Under `cxcall` (§10.1), aggregates larger than the register limit are passed as a pointer to the *caller's* object, and **the callee copies them only if it modifies the parameter** (callee-copy). The parameter contract (§5.1) makes this sound: the object is exclusive for the duration of the call.

**Why:** today, SysV `byval` copies the aggregate to the stack on every call [A§4.7], [E09].

---

## 7. Integer arithmetic and conversions

### 7.1 Overflow

**Rule 7.1.1: overflow is a violation.** Overflow of `+`, `-`, `*`, unary `-`, `++`, `--` and compound assignment is a violation for *all* standard integer types, signed and unsigned: it traps in checked builds and is UB in release. The same applies to division by zero, `INT_MIN / -1`, and the matching `%` cases. The evaluation type follows C's promotions (§7.2).

**Rule 7.1.2: wrapping types.** Modular arithmetic is explicit. `<cxwrap.h>` defines `wuint8_t` … `wuint64_t` and `wint8_t` … `wint64_t`, for example `typedef uint32_t wuint32_t [[cx::wrapping]];`.
- Arithmetic whose operands all have the *same* wrapping type is performed in that type, modulo 2ⁿ, with no integer promotion. Mixing a wrapping type with a different integer type is an error unless the other operand is a constant that fits.
- C23's `<stdckdint.h>` (`ckd_add` and similar) gives checked arithmetic with an overflow flag.
- An explicit cast to a narrower type truncates modulo 2ⁿ, which is defined behaviour, exactly as in C.

**Why:**
- Unsigned wraparound forces SCEV wrap checks in the vectorizer and trunc/add/zext chains in scalar loops [E05].
- IndVarSimplify widens an induction variable only when it has `nsw` or `nuw` (`SimplifyIndVar.cpp:1502-1505`).
- Trip counts need no-wrap facts (`ScalarEvolution.cpp:13526-13570`) [A§4.3].

**Lowering:**
- `nsw` on signed operations, and `nuw` on unsigned ones (today there are none: `CGExprScalar.cpp:4763-4766`).
- `nuw` on GEPs with unsigned indices (`CGExprScalar.cpp:6412-6414`).
- Wrapping types get plain operations.
- Checked builds use the `*.with.overflow` intrinsics followed by a trap.

**Enforcement:** checked.

### 7.2 Conversions

**Rules:**

1. **Promotions stay.** The integer promotions keep their C meaning: narrow types compute in `int`.
2. **No mixed signedness.** A binary operator or comparison whose operands, after promotion, differ in signedness, or mix wrapping and non-wrapping types, is an error. The exception is when one operand is an integer constant expression representable in the other's type.
3. **No implicit narrowing.** Implicit conversion to a narrower type, or to a type that can't represent all source values, is an error unless the source is a constant that fits. Explicit casts are always allowed and truncate modulo 2ⁿ.
4. **No implicit integer↔pointer.** There are no implicit conversions between integers and pointers (C already requires a diagnostic; Cx makes it an error). Explicit forms follow §5.5.
5. **No implicit `void *`.** Implicit conversion from `void *` to another object pointer type is allowed only from the results of allocation functions (`malloc`, `calloc`, `realloc`, `aligned_alloc`, and functions with `alloc` effects that return `void *`). Otherwise a cast is required.

**Why:**
- These rules remove the silent unsigned-conversion traps that make C programmers write `unsigned` loops.
- They make every narrowing visible.
- The target-dependent mixed-rank rule (`SemaExpr.cpp:1369-1375`) disappears from the language.

**Enforcement:** compile-time.

### 7.3 Shifts

**Rule 7.3.1.**
- A shift count that is negative or ≥ the width of the promoted left operand is a violation (checked).
- Left shift of a signed value that changes the sign bit or loses significant bits is a violation (checked). Emit `nsw` on signed `shl`; today there is none (`CGExprScalar.cpp:5139`).
- Unsigned shifts discard shifted-out bits, as in C: bit manipulation is not arithmetic.

### 7.4 `char` and `bool`

- Plain `char` is **unsigned** 8-bit on every target, and is a distinct type from `unsigned char` (§9.1).
- `bool` is as C23. Loads carry range `[0,2)`, as today.

---

## 8. Floating point

**Rule 8.1: representation.**
- `float` and `double` are IEEE-754 binary32 and binary64.
- `FLT_EVAL_METHOD` is 0: no excess precision.
- `long double` does not exist (§3.3).

**Rule 8.2: math functions are pure.**
- The `<math.h>` functions never set `errno`. `math_errhandling` is `MATH_ERREXCEPT`.
- Domain and range errors produce NaN or ±∞ and raise IEEE flags. The flags are only observable inside `#pragma STDC FENV_ACCESS ON` regions.

**Rule 8.3: exceptions and rounding.** Outside `FENV_ACCESS` regions, the program may not depend on FP exception flags or on a non-default rounding mode.

**Rule 8.4: contraction.** Contraction is on within an expression (`#pragma STDC FP_CONTRACT` defaults to `ON`), as Clang does today.

**Rule 8.5: reassociation is opt-in and scoped.**
- `#pragma cx fp_reassociate(on)` in a block allows reassociation of `+` and `*` (and assumes no signed-zero sensitivity) for operations lexically inside the block.
- `<cxmath.h>` also provides reduction functions: `cx_sumf(n, a)`, `cx_sum(n, a)`, `cx_dotf(n, a, b)`, `cx_dot(n, a, b)`, and min/max. They reassociate by definition.
- Nothing else changes. There is no global `-ffast-math` in the language.

**Why:**
- With `errno`, `sqrt` stays a library call, and a loop over it doesn't vectorize (2.0× measured) [E06]. The mechanism: `Builtins.cpp:285-289` (the intrinsic is only generated without math errno) and `BuildLibCalls.cpp:1353-1366` (LLVM marks libm as writing errno).
- A float sum can't vectorize without reassociation (8.1× measured) [E07]; see `IVDescriptors.cpp:1019-1022`.
- Scoping keeps IEEE semantics everywhere else.

**Lowering:**
- Math calls become `llvm.*` intrinsics or `memory(none)` calls.
- Operations inside the pragma get the `reassoc nsz` fast-math flags.
- `FENV_ACCESS` regions use constrained intrinsics, as today.

**Enforcement:** compile-time (pragma scope); the FP environment assumption is a contract.

---

## 9. Types and data layout

### 9.1 Character types

`char`, `signed char` and `unsigned char` are three distinct integer types (C already distinguishes them). Plain `char` is unsigned. None of them has aliasing power (§5.4). The standard string functions take `const char *`.

### 9.2 Enums

**Rule 9.2.1: enums are closed.** An object of enum type holds only the values of its enumerators.
- Conversion from an integer to an enum requires an explicit cast, and is checked.
- The underlying type is `int` unless C23's `enum E : T` syntax is used.
- An enum declared `[[cx::flags]]` is open: bitwise combinations are valid, and its range is the underlying type.

**Why:** C enums get no range metadata (`CGExpr.cpp:2095-2098`) and alias `int` (`CodeGenTBAA.cpp:343-344`).

**Lowering:**
- `!range` on loads of closed enums.
- A distinct TBAA node per enum.
- An exhaustive `switch` over a closed enum gets an `unreachable` default.

**Enforcement:** checked (conversion) and compile-time (the cast requirement).

### 9.3 Unions

Unions keep their C layout: all members at offset 0, size of the largest. Type punning is removed (§5.4). A union used as a tagged variant is written as `struct { enum tag t; union { … } u; }`, as in C.

### 9.4 Struct layout

**Rule 9.4.1: layout is unspecified.** Without `[[cx::c_layout]]`, a struct's layout is unspecified, but the compiler uses a deterministic algorithm, so every module in a build agrees:
1. fields are sorted by decreasing alignment, and stably by declaration order within the same alignment;
2. all bit-fields are grouped into dedicated storage units (§9.5).

Guaranteed:
- members don't overlap;
- `offsetof` is a constant;
- `sizeof` and `alignof` are as computed.

Not guaranteed:
- field order;
- that the first member is at offset 0 (so casting `struct S *` to a pointer to its first member is invalid);
- where the padding goes.

**Rule 9.4.2: `[[cx::c_layout]]`.** A struct or union declared `[[cx::c_layout]]` has exactly C's layout. It is **required**, and is a compile-time error to omit, when the type:
- appears in a foreign declaration or an `[[cx::c_abi]]` function (§14);
- is passed to `cx_bit_cast`;
- is `memcpy`'d to or from a `byte` buffer that is written to a file or socket. The last case is diagnosed as a warning, because it can't be proven.

Types declared in foreign headers are always C layout.

**Why:** declaration-order layout wastes space. The analysis example is 40 bytes instead of 24 [E12], and padding can push a struct over the 16-byte register-passing limit [A§4.7]. See `RecordLayoutBuilder.cpp:1439-1447`.

**Enforcement:** compile-time.

### 9.5 Bit-fields

Bit-field syntax and C11 memory-location rules are unchanged. In non-`c_layout` structs, all bit-fields are placed together in storage units that contain no ordinary members. Accesses can therefore use the full unit width (`CGRecordLayoutBuilder.cpp:456-471`).

### 9.6 Removed types

`long double`, `_Complex`, `_Imaginary`, `_Decimal*`, `_BitInt` and VLAs (§3). Their replacements are listed in §3.2 and §3.3.

---

## 10. Calling convention and ABI

### 10.1 `cxcall`

**Rule 10.1.1.** Calls from Cx to Cx use `cxcall`, including calls to exported functions and through Cx function-pointer types. On x86-64, `cxcall` is the SysV register assignment with these changes:

1. **Aggregate arguments.** Aggregates up to **4 eightbytes (32 bytes)** are passed in registers, classified per eightbyte as INTEGER or SSE. This applies while registers remain; otherwise the aggregate is passed indirectly with callee-copy (§6.9).
2. **Aggregate returns.** Aggregates up to 4 eightbytes are returned in registers: `rax`, `rdx`, `rcx`, `rsi` / `xmm0`–`xmm3`. There is no `sret` below that size.
3. **No `%al`.** Cx has no variadics, so the `%al` vector-count protocol is never used.
4. **Callee-saved registers, stack alignment and unwinding** are unchanged from SysV. This keeps debuggers, profilers and unwinders working.

AArch64 follows the same pattern: aggregates up to 4 registers, homogeneous aggregates as today.

**Rule 10.1.2: types carry their convention.** Cx function types and foreign function types have different calling conventions. Converting between them is an error. Foreign function pointers use the platform C ABI.

**Why:**
- On SysV x86-64, `struct {double x,y,z}` is passed through memory (6 memory operands in `dot3`) and returned via a hidden pointer [E09].
- The 16-byte limit is in `X86.cpp:2111-2114` and `:2190-2195`. The register budget is 6 GPRs and 8 SSE registers (`:3042-3043`).
- Clang already supports a larger-budget convention (`regcall`), which shows the limit is a choice.

**Lowering:** a new LLVM calling convention (`cxcc`) in the X86 and AArch64 backends. Module-private functions keep getting `fastcc` from GlobalOpt.

### 10.2 The C ABI at the boundary

**Rule 10.2.1.**
- Foreign declarations use the platform C ABI.
- A Cx function that C must call is declared `[[cx::c_abi]] extern`. It then uses the C ABI and accepts only `c_layout` aggregates.
- Its pointer parameters still carry Cx semantics, which are a contract for the C caller (§14.2).

---

## 11. Statements, expressions and control flow

### 11.1 Evaluation order

**Rule 11.1.1: left to right.** All evaluation order is defined left to right:
- function designator, then arguments left to right;
- binary operands left, then right;
- for assignment, the left operand's address, then the right operand, then the store.

Unsequenced-modification UB is removed.

**Why:**
- This removes `SequenceChecker` (~870 lines, `SemaChecking.cpp:14526`) and a whole class of UB.
- **(inference)** Unspecified order gives no measurable optimization in practice: operands without side effects can still be reordered, and the aliasing rules above supply the facts needed to reorder operands that do have them.

**Enforcement:** compile-time (it is a definition).

### 11.2 Jumps

**Rules:**

1. **Forward only.** `goto` may only jump **forward**, to a label in the same block or an enclosing block.
2. **No bypassing.** No jump (`goto`, `switch` → `case`) may bypass the declaration of a variable whose scope contains the target.
3. **Top-level `case` labels.** `case` and `default` labels must appear at the top level of the `switch` body, so they can't jump into nested statements. Duff's device is removed.

Forward `goto` for cleanup (`goto fail;`) stays fully supported.

**Why:**
- With only forward jumps, every loop is a `for`/`while`/`do` statement, so the CFG is **reducible** and every loop gets loop metadata and `mustprogress`. Loops built from `goto` get none (inference from `CGStmt.cpp:1114-1117`).
- Because no jump bypasses a declaration, lifetime markers can always be emitted. C mode suppresses them when a label has been seen in scope (`CGDecl.cpp:1630-1640`), which costs stack coloring and dead-store elimination.

**Enforcement:** compile-time.

### 11.3 `switch`

- A `switch` over a closed enum that covers every enumerator needs no `default`. The compiler warns on a missing enumerator.
- `[[fallthrough]]` (C23) is required for fall-through between non-empty cases.

### 11.4 Initializers

**Rules:**
- Nested aggregates must be written with their own braces. Brace elision is removed, except for a string literal initializing a `char` array.
- Designators may appear in any order, but each subobject may be initialized **at most once**. Overriding is an error.

**Why:** the rules simplify `InitListChecker` (~3,300 lines, which today runs twice) [A§3.8], and remove override-order surprises.

**Enforcement:** compile-time.

### 11.5 Everything else

Expressions and statements not mentioned here are unchanged from C23. That includes the comma operator, the conditional operator, compound literals, `sizeof`, `alignof`, `static_assert`, and the C23 attributes `[[nodiscard]]`, `[[maybe_unused]]`, `[[deprecated]]`, `[[fallthrough]]` and `[[noreturn]]`.

---

## 12. Concurrency, atomics, MMIO and signals

### 12.1 Atomics without `_Atomic`

**Rules:**

1. **Operations, not types.** Atomic operations are operations on ordinary objects: `atomic_load_explicit(&x, memory_order_acquire)`, `atomic_store_explicit`, `atomic_fetch_add_explicit`, `atomic_compare_exchange_strong_explicit`, and the rest of `<stdatomic.h>`. The `atomic_int` etc. typedefs name the plain type (e.g. `int`). Objects must be naturally aligned; use `alignas` where a type's alignment is smaller than its size.
2. **Lock-free sizes only.** The object must be 1, 2, 4 or 8 bytes (16 where the target has lock-free 16-byte atomics) and naturally aligned. Other sizes are a compile-time error, so there are no hidden lock library calls.
3. **Constant orderings.** The `memory_order` argument must be an integer constant expression. There are no non-`_explicit` forms and no implicit seq_cst.
4. **Mixed access.** If an object is accessed atomically by one thread and non-atomically by another without happens-before ordering, that is a data race, which is UB. This is the C11 memory model applied to `atomic_ref`-style use.

**Why:** today, plain `_Atomic` accesses are seq_cst (`CGAtomic.cpp:1656-1661`), a runtime order becomes a `switch` (`:1438-1459`), and odd sizes become lock library calls (`:1166-1178`) [A§4.8].

**Lowering:** LLVM `load atomic`, `store atomic`, `atomicrmw` and `cmpxchg` with the stated ordering.

### 12.2 Data races

Unchanged from C11/C23: a data race is UB. With exclusive parameters (§5.1), no other thread can access an exclusive object during the call, so introducing stores (`writable`) is sound.

### 12.3 Memory-mapped I/O

**Rule 12.3.1.** `<cxmmio.h>` provides intrinsics: `mmio_read8/16/32/64(const void *addr)` and `mmio_write8/16/32/64(void *addr, value)`.

Each call performs exactly one access of that width. It is never merged, split or elided, and never reordered with other MMIO intrinsics. MMIO addresses may be integer constants cast to pointers (§5.5).

**Lowering:** LLVM `volatile` loads and stores. The IR concept stays; only the C type qualifier goes.

**Why:** `volatile` as a *qualifier* infects every type rule in Sema. As an object property it blocks LICM, SROA, mem2reg and GlobalOpt [A§4.8]. MMIO only needs the access primitive.

### 12.4 Signals

Signal handlers communicate through atomic operations with `memory_order_relaxed` plus `atomic_signal_fence`. `volatile sig_atomic_t` is replaced by a naturally aligned integer accessed atomically.

---

## 13. Preprocessor and lexical changes

### 13.1 Header units

**Rule 13.1.1.** A `#include` of a `.cxh` file imports a **header unit**:
- It is preprocessed in a clean macro environment: predefined macros plus command-line `-D` only, never the includer's macros.
- It is parsed once per build configuration and cached.
- It exports its declarations and macros to the includer.
- Including it again has no effect, so include guards are unnecessary.

`#include` of foreign headers keeps C's textual semantics (§14).

**Why:**
- Five libc headers expand 6 lines into 1,648 lines and open 118 files.
- The front end is 54% of a `-O0` build of Lua's 32 files [A§2.2].
- Header units are Clang modules without new syntax.

### 13.2 Macros and constants

The preprocessor is unchanged, apart from header-unit isolation. `constexpr` objects are the recommended way to write constants. `#define` for constants is still allowed.

### 13.3 Typedef names

**Rules:**
- `typedef` declarations are only allowed at file scope, including in header units.
- An identifier declared as a typedef name may not be redeclared as an ordinary identifier anywhere in the module.

**Why:** the parser can tell whether an identifier is a type from a pre-scan of the module and its header units. That removes the parser→Sema lookup in `TryAnnotateTypeOrScopeToken` (`Parser.cpp:1993-2025`), known as the "lexer hack" [A§3.2]. This is the only change needed to make Cx's grammar parseable without semantic analysis.

### 13.4 Lexical

**Rules:**
- Digraphs are removed. Trigraphs were already removed by C23.
- Keywords are those of §3.
- Everything else is as C23: identifiers, literals (including C23's digit separators and binary literals), and string prefixes.

### 13.5 Pragmas

**Rule 13.5.1.** Cx recognizes these pragmas:
- `#pragma cx fp_reassociate(on|off)`
- `#pragma STDC FENV_ACCESS`
- `#pragma STDC FP_CONTRACT`
- `#pragma STDC CX_LIMITED_RANGE`: this applies to `<cxcomplex.h>` only.

Unknown `cx` pragmas are errors.

---

## 14. C interoperability

### 14.1 Calling C from Cx

- **Foreign headers.** `#include` of a foreign header (anything not `.cxh`) parses it with the **full C23 language**, including every keyword removed in §3. Its declarations are imported as foreign.
- **Foreign semantics.** Foreign functions use the C ABI and have unknown effects. Their pointer parameters are treated as `[[cx::alias, cx::escapes, cx::nullable]]`, except that C `restrict` parameters are treated as exclusive. Foreign structs are `c_layout`.
- **Cx annotations.** A `.cxh` may redeclare a foreign function with Cx attributes, for example effects or non-null parameters, to give Cx callers more precise facts. These redeclarations are trusted contracts. The Cx standard headers do this for libc (§15).
- **Foreign variadics.** Foreign variadic functions may be called. Arguments undergo C's default argument promotions.

### 14.2 Calling Cx from C

- A Cx function callable from C is declared `[[cx::c_abi]] extern` (§10.2).
- Its parameter contracts (§5.1) and effect summary are part of its interface. A C caller that violates them, for example by passing aliasing pointers, has UB.
- `cxc --emit-c-header` generates a C header for a module's `c_abi` exports. `restrict` marks exclusive parameters and comments describe the contracts.
- In checked builds, `c_abi` entry points validate non-null parameters and extent overlap at the boundary.

### 14.3 Building mixed programs

C and Cx modules link together with no glue. The same compiler binary compiles both: `.c` files in C23 mode and `.cx` files in Cx mode. LTO works across both.

**Long-term:** a binding generator can turn foreign headers into `.cxh` files ahead of time. Then the Cx-mode front end never needs to parse C, and the C parser becomes a separate tool (§20).

---

## 15. Standard library

**Rule 15.1: Cx-annotated C headers.** Cx provides `.cxh` versions of the C standard headers. Each redeclares the C functions with Cx parameter attributes and effect summaries. For example:

| Function | Parameters | Effects |
|---|---|---|
| `memcpy` | exclusive (the default) | `reads(src), writes(dst)` |
| `memmove` | `[[cx::alias]]` on both | `reads(src), writes(dst)` |
| `strlen` | `const char *s` | `reads(s)` |
| `malloc` | — | `alloc` |
| `free` | `[[cx::escapes, cx::nullable]] void *p` | `alloc` |
| `qsort` | — | `callback`, with the comparator typed as an effect-bounded function pointer when called from Cx |
| `printf` | — | `io` |

**Rule 15.2: changed and removed headers.**

| Header | Status in Cx |
|---|---|
| `<math.h>` | Pure functions, no `errno` (§8) |
| `<errno.h>` | Available only for inspecting results of foreign `io` functions |
| `<setjmp.h>`, `<stdarg.h>` | Not available to Cx code |
| `<stdatomic.h>` | Explicit operations on ordinary objects (§12.1) |
| `<complex.h>` | Replaced by `<cxcomplex.h>` |
| `<threads.h>` | Kept |
| Annex K (`_s` functions) | Not provided |

**Rule 15.3: new Cx headers.**

| Header | Provides |
|---|---|
| `<cxbyte.h>` | `byte`, `cx_bit_cast` |
| `<cxptr.h>` | `cx_expose`, `cx_with_addr`, `cx_from_exposed` |
| `<cxwrap.h>` | Wrapping integer types |
| `<cxmath.h>` | Reductions and dot products |
| `<cxfmt.h>` | Typed formatting |
| `<cxmmio.h>` | MMIO intrinsics |
| `<cxcomplex.h>` | Complex structs with inline operations |

---

## 16. Build modes, checking and undefined behaviour

### 16.1 Modes

| Mode | Flag | Checked violations | Contracts |
|---|---|---|---|
| **checked** (default at `-O0`) | `-fcx-checked` | Trap with source location | Assumed; optional sanitizer |
| **release** (default at `-O1` and above) | `-fcx-release` | UB: the compiler assumes they don't happen | Assumed |

### 16.2 Catalogue

| Rule | Violation | Checked build | Release build | Static diagnosis |
|---|---|---|---|---|
| 5.1.1 exclusive | Modified object accessed through a non-derived pointer during the call | Overlap check for parameters with extents | UB | Same object or known-overlapping ranges passed twice |
| 5.1.2 `const` immutable | Pointee of a `const` parameter modified during the call | — | UB | Same object passed as `const` and non-`const` |
| 5.1.3 non-escaping | Parameter stored or retained | — | — | **Always** (compile-time) |
| 5.1.4 non-null | Null passed to a non-null parameter | Trap at entry | UB | `nullptr` literal, or a value known to be null |
| 5.1.5 / 5.6 extent | Access past an extent | Bounds trap | UB | Constant indices |
| 5.3 ownership | Owned object reachable through another path | Sanitizer (future) | UB | Obvious double-store or escape |
| 5.4 type-based aliasing | Access through the wrong type | Type sanitizer | UB | Casts between unrelated pointer types warn |
| 5.5 provenance | Integer→pointer without provenance | — | UB | Cast forms restricted (compile-time) |
| 6.3 progress | Infinite loop or recursion without effects | — | UB | Empty infinite loop rejected |
| 7.1 overflow | Integer overflow; division by zero; `INT_MIN / -1` | Trap | UB | Constant folding |
| 7.3 shift | Bad shift count or signed overflow | Trap | UB | Constant counts |
| 8.3 FP environment | Depends on flags or rounding outside `FENV_ACCESS` | — | Unspecified results | — |
| 9.2 enum | Out-of-range value | Trap on conversion | UB | Constants |
| 12.1 data race | Race | ThreadSanitizer | UB | — |
| C23 leftovers | Out-of-bounds access, use-after-free, uninitialized reads | As C (sanitizers) | UB | As C |

Everything else C23 calls undefined and that Cx does not list here either remains UB, or is removed with the feature that caused it: unsequenced side effects (§11.1), `setjmp`, VLAs, union punning (§5.4), K&R calls.

---

## 17. Optimization map

What the Cx compiler emits, which LLVM passes benefit, and the evidence.

| Cx guarantee | IR emitted | Passes that benefit | Evidence |
|---|---|---|---|
| Exclusive parameters | `noalias`; scoped metadata after inlining | AA (identified objects), GVN, LICM, LoopVectorize (no runtime checks), DSE | [E01]: 69 vs 36 asm lines of checks. [A§4.2]. |
| Non-escaping parameters | `captures(ret: …)` / `captures(none)` | BasicAA call mod/ref, SROA and mem2reg after calls, DSE | [E03]; [A§4.1] |
| `const` parameters immutable | `readonly` + `noalias`, `!invariant.load` | GVN across calls, LICM | [E04] |
| Non-null + dereferenceable | `nonnull`, `dereferenceable(N)` | LICM speculation of conditional loads (842 `CondExecuted` misses in SQLite), SimplifyCFG | [A§4.1] |
| Writable exclusive parameters | `writable` + `noalias` | LICM scalar promotion (`AliasAnalysis.cpp:998-1021`) | SQLite: 117 promotions vs 24,989 misses |
| Owned regions | `!alias.scope`/`!noalias` on loads, stores and calls | ScopedNoAliasAA → GVN, LICM, DSE, vectorizer | 93.4% "unknown" callees [A§4.1] |
| Effect summaries | `memory(...)`, `nocallback`, `nosync`; `!callees` | BasicAA, GlobalsAA, MemorySSA, FunctionAttrs | 71% (SQLite) and 70% (Lua) of GVN misses are calls |
| Module-private default; summary files | `internal`; declarations annotated from `.cxs` | GlobalOpt, ArgPromotion, DeadArgElim, fastcc, inliner | +4.7% (Lua one-file), −3.3% (SQLite split), 6/6 rounds [A§5.4] |
| No interposition | `dso_local` / protected | Inliner, IPO, direct calls | [E10] |
| `char` not universal; `byte` only | Precise TBAA | GVN, LICM, LoopVectorize | [E02]: 34×; SQLite TBAA −60% store clobbers |
| No union punning; distinct enums and signedness | Union TBAA, enum nodes, `!range` | GVN, SimplifyCFG, InstCombine | [A§4.2] |
| All-integer overflow violation | `nsw`, `nuw` on arithmetic, `shl` and GEPs | IndVarSimplify, SCEV, LoopVectorize (no SCEV checks), LSR | [E05] |
| Pure math | `llvm.sqrt` etc., `memory(none)` | LoopVectorize, LICM, GVN | [E06]: 2.0× |
| Scoped reassociation | `reassoc nsz` on scoped operations | LoopVectorize reductions | [E07]: 8.1× |
| Forward progress; reducible CFG | `mustprogress`; loop metadata | FunctionAttrs `willreturn`, LoopDeletion, LICM | [E11] |
| No `setjmp`, no varargs definitions | No `returns_twice`, no `va_start` | Inliner, RegisterCoalescer, StackColoring, TRE | [E08] |
| No jumps past declarations | Lifetime markers always | StackColoring, DSE | `CGDecl.cpp:1630-1640` |
| Unspecified struct layout | Smaller structs | Cache use; more aggregates fit `cxcall` registers | [E12]: 40 → 24 bytes |
| `cxcall` | Aggregates ≤ 32 bytes in registers | Call overhead | [E09] |
| No `volatile` / `_Atomic` qualifiers | Explicit volatile/atomic instructions only | LICM, SROA, GlobalOpt on everything else | [A§4.8] |
| No `_Complex`, `long double`, `_BitInt` | No `__muldc3`/`__divdc3`, no x87, no multi-word division | Vectorizer, register allocation | [E13] |

**Expected impact, stated honestly.**

- **Measured:**
  - Cliffs of 2×–34× in the kernels these rules target.
  - 3.3–4.7% whole-program from module visibility alone.
- **Not yet measured:** the whole-program effect of exclusive or non-escaping parameters, owned regions and effect summaries. This is **the central hypothesis**. It targets the dominant barrier found (call opacity), and §20 describes how to measure it before committing to syntax.

---

## 18. Worked examples

### 18.1 Byte buffers ([E02])

```c
/* C and Cx source are identical */
struct cbuf { char *data; size_t len; };
void fill(struct cbuf *b, char c) { for (size_t i = 0; i < b->len; i++) b->data[i] = c; }
```

| | Behaviour |
|---|---|
| **C** | The `char` store may overwrite `b->data` and `b->len`. Both are reloaded every iteration, one byte is stored per iteration, and the loop is not vectorized: 0.474 ns/byte. |
| **Cx** | `char` has its own alias class, so the store cannot modify a `char *` or a `size_t`. The loads are hoisted, and the loop becomes `memset` or vector stores: the hoisted-local version measured 0.014 ns/byte. |

### 18.2 Escaping locals and `const` ([E03], [E04])

```c
void ext(int *p);        /* Cx: non-escaping by default */
void opaque(void);       /* Cx module function: summary known, e.g. effects(io) */
int f(void) { int x = 1; ext(&x); int a = x; opaque(); return a + x; }
int g(const int *p) { int a = *p; opaque(); return a + *p; }
```

| | Behaviour |
|---|---|
| **C** | `x` is captured by `ext`, so it is reloaded after `opaque()`. `*p` is reloaded too. |
| **Cx** | `x` is never captured and `opaque` cannot reach it: one load, and `x` is promoted to a register. `*p` is immutable for the whole call: one load. |

### 18.3 Separate buffers in a struct (owned regions)

```c
struct filter { [[cx::owned]] float *in; [[cx::owned]] float *out; size_t n; float gain; };
void run(struct filter *f) {
    for (size_t i = 0; i < f->n; i++) f->out[i] = f->in[i] * f->gain;
}
```

| | Behaviour |
|---|---|
| **C** | `out` may alias `in`, `n` and `gain`. Vectorizing needs runtime checks, and `f->n` and `f->gain` must be reloaded after each store, unless the programmer copies them into locals. |
| **Cx** | The paths `f`, `f.in` and `f.out` are distinct scopes. Loads of `f->n` and `f->gain` are hoisted, and the loop vectorizes with no runtime checks. |

### 18.4 FP reduction ([E07])

```c
float sum(size_t n, const float a[n]) {
    float s = 0;
#pragma cx fp_reassociate(on)
    for (size_t i = 0; i < n; i++) s += a[i];
    return s;
}
/* or: return cx_sumf(n, a); */
```

The loop vectorizes: 8.1× measured with the equivalent flags. Code outside the pragma keeps IEEE order.

### 18.5 Interpreter dispatch without computed goto

```c
typedef int (*op_fn)(struct vm *vm, const struct insn *ip);
extern const op_fn ops[];                        /* module-private table */
static int op_add(struct vm *vm, const struct insn *ip) {
    vm->acc += ip->imm;                          /* overflow-checked in checked builds */
    [[cx::musttail]] return ops[ip[1].op](vm, ip + 1);
}
```

**Result:**
- Each handler ends in a tail jump, which is the threaded-code shape that computed goto provides in GNU C.
- `op_fn` is module-private, so `!callees` lists the handlers.
- The handlers' summaries (`reads(ip) writes(vm)`) keep `ip` data in registers across dispatch.

### 18.6 Porting idioms

| C idiom | Cx |
|---|---|
| `void *memmove(void *d, const void *s, size_t n)` | `void *memmove([[cx::alias]] void *d, [[cx::alias]] const void *s, size_t n)` |
| `for (size_t i = n; i-- > 0;)`, which underflows at the end | `for (size_t i = n; i > 0; i--) use(i - 1);` |
| `uint32_t h = h * 31 + c;` (a hash that relies on wrapping) | `wuint32_t h = h * 31 + c;` |
| `if (x < n)` with `int x`, `size_t n` | `if (x >= 0 && (size_t)x < n)` (the mixed-signedness error forces this) |
| `union { float f; uint32_t u; } v; v.f = x; return v.u;` | `return cx_bit_cast(uint32_t, x);` |
| `list_add(struct list *l, struct node *n)` storing `n` | `list_add(struct list *l, [[cx::escapes]] struct node *n)` |
| `volatile uint32_t *reg = (void *)0x40000000; *reg = 1;` | `mmio_write32((void *)0x40000000, 1);` |
| `_Atomic int ready; ready = 1;` | `int ready; atomic_store_explicit(&ready, 1, memory_order_release);` |
| `double _Complex z = a * b;` | `cx_cdouble z = cx_cmul(a, b);` (`<cxcomplex.h>`) |
| `goto retry;` (backward) | `for (;;) { … if (ok) break; }` |

---

## 19. Removed and simplified features: summary

**Removed** (each also listed in the section shown):

1. **Keywords (§3.2):** `restrict`, `volatile`, `_Atomic`, `register`, `auto`, `typeof_unqual`, `_Noreturn`, `_Complex`, `_Imaginary`, `_Decimal32`, `_Decimal64`, `_Decimal128`, `_BitInt`, plus the five alternate spellings.
2. **Types (§3.3):** `long double`, VLAs, `alloca`.
3. **Calls:** Cx variadic definitions and `<stdarg.h>` (§6.5); `setjmp`/`longjmp` and all `returns_twice` functions (§6.4); calling `main` (§6.7).
4. **Declarations:** K&R functions, implicit `int`, implicit function declarations, tentative definitions and common symbols (§4.5); composite types (§4.5); block-scope typedefs and typedef shadowing (§13.3).
5. **Memory:** type punning through unions (§5.4); `char` as a universal alias (§5.4); implicit integer↔pointer conversions and provenance-less `inttoptr` (§5.5).
6. **Numbers:** implicit narrowing and mixed-signedness arithmetic (§7.2); `errno` from math (§8); implicit dynamic FP environment (§8).
7. **Control flow:** backward `goto`, jumps past declarations, `case` labels in nested statements (Duff's device), unspecified evaluation order (§11); computed goto, nested functions, statement expressions (§3.3).
8. **Lexical and initializers:** digraphs (§13.4); brace elision and designator overrides (§11.4).
9. **Array syntax:** the `[static n]` form (§5.6); trailing `[0]`/`[1]` arrays as flexible arrays (§5.6).
10. **Library:** plain `_Atomic` access and runtime memory orders (§12.1); Annex K.

**Simplified:**

1. **Qualifiers:** `const` is the only type qualifier. The Sema qualifier logic for `volatile`, `restrict` and `_Atomic` goes away.
2. **Dialects:** one dialect instead of 13 C dialects shared with C++ [A§2.1].
3. **Grammar:** parseable without symbol-table lookup (§13.3).
4. **Headers:** header units parsed once (§13.1).
5. **Linkage:** module-private default, one definition, identical redeclarations (§4).
6. **Parameters:** exclusive, non-escaping, non-null defaults. In C these would each need an annotation that Clang ignores in most positions (§5.1).
7. **Evaluation order:** defined (§11.1).

---

## 20. Implementation and measurement plan

Design first, measure before freezing syntax. Each phase ends with the census [A§4] and the ablation benchmarks [A§5] rerun on SQLite, Lua and zstd, ported as needed. Each phase must also pass those projects' own test suites, because many contracts are unchecked.

| Phase | Deliverable in the vendored Clang | Where |
|---|---|---|
| **0: emission-only prototypes** | C-mode flags that emit Cx facts for existing C code, used on audited code only | See below |
| **1: Cx mode** | `-std=cx1` / `.cx` files: the keyword set (§3), Sema rules (§§4–13 compile-time checks), removed features, attributes (Appendix B) | `clang/lib/Sema`, `clang/lib/Parse`, `clang/include/clang/Basic/TokenKinds.def`, `LangStandards.def` |
| **2: information** | Owned-region scope metadata (§5.3), effect inference and summary files (§§4.3, 6.1), function-pointer effects and `!callees` (§6.2) | New CodeGen scope pass; `CGCall.cpp`; a summary writer and reader |
| **3: layout and ABI** | Unspecified layout (§9.4), `cxcall` (§10) | `clang/lib/AST/RecordLayoutBuilder.cpp`; `clang/lib/CodeGen/Targets/X86.cpp`, `AArch64.cpp`; LLVM X86/AArch64 calling-convention tables |
| **4: checking** | Checked-mode traps (§16), boundary validation (§14.2), sanitizers for exclusivity and ownership | CodeGen; compiler-rt |

**Phase 0 flags:**

| Flag | What it emits | Where |
|---|---|---|
| `-fcx-param-noalias` | `noalias` + `captures` + `nonnull` + `dereferenceable` on every pointer parameter | `CGCall.cpp:3520-3604` |
| `-fcx-char-not-alias` | Separate TBAA nodes for the char types | `CodeGenTBAA.cpp:166-174` |
| `-fcx-unsigned-nuw` | `nuw` on unsigned arithmetic | `CGExprScalar.cpp:4763-4766` |
| `-fcx-mustprogress-functions` | Function-level `mustprogress` | `CodeGenFunction.h:646-651` |
| `-fcx-internal-default` | Internal linkage unless the definition is `extern` | `CodeGenModule.cpp:6762-6828` |
| `-fno-math-errno` | Already exists | — |

Phase 0 answers the central hypothesis (§17): how much of the call-opacity barrier the parameter contract alone removes, before any syntax work.

---

## 21. Open questions and risks

1. **Exclusivity is a contract.** Like `restrict`, a violation miscompiles silently. Mitigations: static rejection of the provable cases, checked-mode overlap checks, and a planned exclusivity sanitizer (a shadow-memory "borrow" tracker). *Risk: medium to high for ported code.*
2. **Ownership ergonomics.** Graphs, doubly-linked lists and shared caches (SQLite's `Pager`, shared by many connections) don't fit tree ownership. They stay shared, with C semantics. How much of real code can use `[[cx::owned]]` is unknown and needs porting studies.
3. **Unsigned overflow as a violation** (the Swift model) breaks idioms such as reverse loops and hashes. Mitigations: wrapping types, checked-mode traps, diagnostics for known idioms. The alternative, keeping unsigned wraparound and emitting `nuw` only where it is provable, is weaker; phase 0 measures the difference.
4. **Summary files and incremental builds.** A change to a function's inferred effects is an interface change and triggers dependent rebuilds, as with C++ module interfaces. We may need a "summary stability" mode that only ever widens effects.
5. **Layout determinism across compiler versions.** The layout algorithm (§9.4) must be frozen as part of the ABI once released. Profile-guided hot/cold layout is deferred, because it would make layout depend on profiles.
6. **`cxcall` specification.** The exact register assignment per target, and debugger and unwinder support, need a separate ABI annex.
7. **Runtime extents.** **(inference)** LLVM expresses dereferenceability with constant sizes (the `dereferenceable(N)` attribute; `Loads.cpp:225`). Runtime-size facts from `T a[n]` need an LLVM extension to help beyond bounds checks.
8. **C callers of Cx code** can violate contracts. Checked builds validate the boundary; release builds trust it.
9. **Evidence gaps.** The benchmarks so far are integer-heavy. Numeric, media and HPC workloads, where §8 matters most, need their own benchmark set before the FP rules are tuned.
10. **Keyword removals and existing code.** `volatile`, `_Atomic` and `restrict` are common in C headers. Foreign headers keep them (§14.1). Porting Cx code means rewriting MMIO and atomics (Appendix C).

---

## Appendix A. Changes against C23, by clause

| C23 clause | Cx change |
|---|---|
| 5.1.2 Execution environments | `main` may not be called or have its address taken (§6.7) |
| 6.2.2 Linkages of identifiers | File scope defaults to internal; `extern` on a definition exports it (§4.1) |
| 6.2.4 Storage durations | No VLAs; automatic lifetimes begin at the declaration, which jumps cannot bypass (§11.2) |
| 6.2.5 Types | Plain `char` unsigned (§7.4); removed types (§3); closed enums (§9.2); wrapping integer typedefs (§7.1); `byte` (§5.4) |
| 6.2.6 Representations | Struct layout unspecified unless `c_layout` (§9.4) |
| 6.2.7 Compatible and composite types | Nominal struct identity; identical redeclarations; no composite types (§4.5) |
| 6.3.1 Arithmetic conversions | Promotions kept; mixed signedness and implicit narrowing are errors (§7.2) |
| 6.3.2.3 Pointers | Strict provenance (§5.5); `void *` conversions restricted (§7.2) |
| 6.4.1 Keywords | 41 kept, 13 removed, alternate spellings removed (§3) |
| 6.4.6 Punctuators | Digraphs removed (§13.4) |
| 6.5 Expressions | Left-to-right evaluation (§11.1); effective-type rules without char exemption, with `byte` (§5.4); overflow of all standard integer types is a violation (§7.1); shifts (§7.3) |
| 6.5.3.2 Function calls | Prototypes required; parameter contracts (§5.1); varargs only for foreign callees (§6.5) |
| 6.7.1 Storage-class specifiers | `static` at file scope is redundant; `register` and `auto` removed; `typedef` only at file scope (§13.3) |
| 6.7.3 Type specifiers | `_Complex`, `_BitInt`, `_Decimal*` and `long double` removed; closed enums; struct layout; unions without punning |
| 6.7.4 Type qualifiers | Only `const`, with the parameter immutability guarantee (§5.1.2); `restrict`, `volatile` and `_Atomic` removed |
| 6.7.5 Function specifiers | `inline` means "body in interface" (§6.8); `_Noreturn` removed |
| 6.7.7.4 Function declarators | Array parameters imply extents; `[static n]` removed (§5.6) |
| 6.7.11 Initialization | No brace elision; no designator override (§11.4) |
| 6.7.13 Attributes | `cx::` attribute set (Appendix B); `[[unsequenced]]` and `[[reproducible]]` given semantics (§6.1) |
| 6.8 Statements | Forward-only `goto`; no bypassing declarations; top-level `case` labels; forward progress (§§6.3, 11) |
| 6.9 External definitions | One definition; no tentative definitions (§4.5) |
| 6.10 Preprocessing | Header units for `.cxh` (§13.1); `cx` pragmas (§13.5) |
| 7 Library | §15: pure math, atomics on ordinary objects; `<setjmp.h>`, `<stdarg.h>`, `<complex.h>` and Annex K removed; new `cx` headers |
| Annex F | `FLT_EVAL_METHOD` 0; `math_errhandling` = `MATH_ERREXCEPT` (§8) |
| Annex G | Removed with `_Complex`; `<cxcomplex.h>` provides it opt-in (§3.2) |

## Appendix B. Attribute, pragma and builtin reference

**Parameter attributes:**

- **`[[cx::alias]]`**: the parameter may alias other pointers; the exclusivity and immutability guarantees are dropped (§5.1).
- **`[[cx::escapes]]`**: the callee may retain, store or free the pointee (§5.1.3).
- **`[[cx::nullable]]`**: the parameter may be null; it has no dereferenceability guarantee (§5.1.4).

**Member attribute:**

- **`[[cx::owned]]`**: the pointer member owns its pointee (§5.3).

**Type and typedef attributes:**

- **`[[cx::wrapping]]`**: an integer typedef with modular arithmetic (§7.1).
- **`[[cx::may_alias]]`**: universal aliasing type; reserved for standard headers (§5.4).

**Struct, union and enum attributes:**

- **`[[cx::c_layout]]`**: exact C layout (§9.4).
- **`[[cx::flags]]`**: an open, bitwise enum (§9.2).

**Function attributes:**

- **`[[cx::effects(items)]]`**: an effect upper bound (§6.1). The item grammar is `none` | `reads(x, …)` | `writes(x, …)` | `alloc` | `io` | `callback` | `sync`, where `x` is a parameter name, `module` or `shared`.
- **`[[cx::c_abi]]`**: C calling convention for export to C (§10.2).
- **`[[cx::interposable]]`**: the exported symbol may be interposed (§4.2).
- The C23 attributes `[[unsequenced]]`, `[[reproducible]]`, `[[noreturn]]`, `[[nodiscard]]`, `[[maybe_unused]]` and `[[deprecated]]`.

**Statement attributes:**

- **`[[cx::musttail]]`**: guaranteed tail call (§6.6).
- The C23 attribute `[[fallthrough]]`.

**Pragmas:**

- `#pragma cx fp_reassociate(on|off)` (§8)
- `#pragma STDC FENV_ACCESS`
- `#pragma STDC FP_CONTRACT`
- `#pragma STDC CX_LIMITED_RANGE`

**Header-provided builtins:**

| Header | Builtins |
|---|---|
| `<cxbyte.h>` | `byte`, `cx_bit_cast(T, x)` |
| `<cxptr.h>` | `cx_expose(p)`, `cx_with_addr(p, a)`, `cx_from_exposed(a)` |
| `<cxwrap.h>` | `wint8_t`…`wint64_t`, `wuint8_t`…`wuint64_t` |
| `<cxmath.h>` | `cx_sumf`, `cx_sum`, `cx_dotf`, `cx_dot`, min/max reductions |
| `<cxfmt.h>` | `cx_print`, `cx_format`, `CX_ARG` |
| `<cxmmio.h>` | `mmio_read8/16/32/64`, `mmio_write8/16/32/64` |
| `<cxcomplex.h>` | `cx_cfloat`, `cx_cdouble` and operations |

## Appendix C. Porting checklist (C → Cx)

1. **Rename and export.** Rename `.c` → `.cx` and `.h` → `.cxh` for code you own. Add `extern` to every definition used by another module, and delete now-redundant `static`s.
2. **Parameters:**
   - Mark `[[cx::escapes]]` on parameters that are stored or freed. The compiler lists them all.
   - Mark `[[cx::alias]]` where overlapping arguments are legitimate. Audit the in-place operations.
   - Mark `[[cx::nullable]]` where `NULL` is a valid argument.
3. **Integers.** Fix mixed-signedness and narrowing errors. Replace intentionally wrapping arithmetic with `<cxwrap.h>` types, and rewrite reverse unsigned loops.
4. **Remove the deleted features:**

   | C feature | Cx replacement |
   |---|---|
   | `volatile` | `<cxmmio.h>` or atomics |
   | `_Atomic` | Explicit operations |
   | `restrict` | Delete it (it is the default) |
   | `setjmp` | Error returns |
   | Varargs definitions | `<cxfmt.h>` style |
   | VLAs | Heap or fixed arrays |
   | `long double` / `_Complex` | `double` / `<cxcomplex.h>` |
   | Backward `goto` | Loops |
   | Union punning | `cx_bit_cast` |

5. **Byte buffers.** Keep `char` for text. Use `byte` only for untyped memory access.
6. **Layout.** Mark structs that are serialized or shared with C as `[[cx::c_layout]]`.
7. **Optional, for performance:**
   - add `[[cx::owned]]` to pointer fields that own their pointee;
   - add effect bounds to function-pointer typedefs;
   - put `#pragma cx fp_reassociate(on)` around reductions.
8. **Test.** Run the test suite in checked mode (`-fcx-checked`), then in release mode.
