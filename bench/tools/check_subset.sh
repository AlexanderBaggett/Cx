#!/bin/sh
# Lint: benchmark sources must stay in the C23 ∩ Cx subset (bench/README.md).
# Rejects keywords and features that Cx removes (docs/design/cx-language-spec.md §3, §19).
cd "$(dirname "$0")/.." || exit 2
pat='\b(restrict|volatile|_Atomic|register|auto|typeof_unqual|_Noreturn|_Complex|_Imaginary|_Decimal(32|64|128)|_BitInt|_Alignas|_Alignof|_Bool|_Static_assert|_Thread_local|setjmp|longjmp|va_start|va_arg|va_list|alloca|goto)\b|long[[:space:]]+double|\.\.\.[[:space:]]*\)'
# strip // and /* */ comments and string literals before matching
bad=$(for f in harness/*.c harness/*.cxh src/*.c src/*.cxh $(ls cx/*.cx cx/*.cxh 2>/dev/null); do
  sed -e 's://.*$::' "$f" | perl -0pe 's{/\*.*?\*/}{}gs; s{"(\\.|[^"\\])*"}{""}g' | grep -nE "$pat" | sed "s|^|$f:|"
done)
if [ -n "$bad" ]; then echo "check_subset: features removed in Cx found:"; echo "$bad"; exit 1; fi
echo "check_subset: OK (no removed keywords or features)"
