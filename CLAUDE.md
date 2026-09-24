# CLAUDE.md

## Git
- Commits must be authored and committed as `Alexander Baggett <alexander.baggett@gmail.com>`. Before the first commit in a session, run `git var GIT_AUTHOR_IDENT` and `git var GIT_COMMITTER_IDENT` and set `user.name`/`user.email` if they show anything else.
- Do not add `Co-Authored-By` or `Claude-Session` trailers to commit messages, or Claude attribution to PR descriptions.

## Source layout
- `upstream/` holds the unmodified llvm-project `llvmorg-23.1.2` sources (`85ac560262434c9ccfc0c183ec22d4138ed647fb`): `llvm/`, `clang/`, `cmake/`, `third-party/` and `libc/`. It builds the reference compiler for the benchmarks. **Never edit anything under `upstream/`.** `tools/check-upstream.sh` verifies it against the import's tree hashes.
- `cx/` is a copy of `upstream/` and is the Cx compiler. **All Cx compiler changes go here.** See what Cx has changed with `git diff --no-index upstream/clang cx/clang` (or `diff -ru upstream cx`).
- `libc/` is needed because LLVM's CMake requires libc's shared headers (`cmake/Modules/FindLibcCommonUtils.cmake`).
- `LICENSE.TXT`, `.clang-format` and `.clang-tidy` at the root are upstream's and apply to both trees.
- LLVM is licensed Apache-2.0 WITH LLVM-exception (`LICENSE.TXT`); keep upstream license headers intact.

## Building the compilers
- `tools/build-compilers.sh [ref|cx|both]` builds `upstream/` into `../ref-build` (reference) and `cx/` into `../cx-build` (Cx), with the same CMake configuration: Release, X86 target only, project `clang`.
- After changing `cx/`, rebuild with `ninja -C ../cx-build clang`. The benchmarks in `bench/` use both builds.
