# CLAUDE.md

## Git
- Commits must be authored and committed as `Alexander Baggett <alexander.baggett@gmail.com>`. Before the first commit in a session, run `git var GIT_AUTHOR_IDENT` and `git var GIT_COMMITTER_IDENT` and set `user.name`/`user.email` if they show anything else.
- Do not add `Co-Authored-By` or `Claude-Session` trailers to commit messages, or Claude attribution to PR descriptions.

## Source layout
- `llvm/`, `clang/`, `cmake/`, `third-party/`, `LICENSE.TXT`, `.clang-format` and `.clang-tidy` were imported unchanged from llvm-project `llvmorg-23.1.2` (`85ac560262434c9ccfc0c183ec22d4138ed647fb`) in commit "Import LLVM and Clang from llvm-project llvmorg-23.1.2". Diff against that commit to see what Cx has changed.
- LLVM is licensed Apache-2.0 WITH LLVM-exception (`LICENSE.TXT`); keep upstream license headers intact.
