#!/bin/sh
# Compare the object files produced by the two toolchains.
#   codegen_compare.sh DIR_C DIR_CX [BIN_C BIN_CX]
# For each object: "identical" if the files match after removing the
# .comment section (it holds the compiler's version string); otherwise the
# sections that differ are listed. Exit status 1 if any object differs.
a=$1; b=$2
OBJCOPY=${OBJCOPY:-objcopy}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
same=0; diff=0
for fa in "$a"/*.o; do
  n=$(basename "$fa"); fb="$b/$n"
  if [ ! -f "$fb" ]; then echo "missing   $n"; diff=$((diff+1)); continue; fi
  $OBJCOPY --remove-section=.comment "$fa" "$tmp/a.o"
  $OBJCOPY --remove-section=.comment "$fb" "$tmp/b.o"
  if cmp -s "$tmp/a.o" "$tmp/b.o"; then
    same=$((same+1)); echo "identical $n"
  else
    diff=$((diff+1)); secs=""
    for s in $(readelf -SW "$tmp/a.o" | sed -n 's/^ *\[ *[0-9]*\] \([^ ]*\) .*/\1/p' | grep -v '^$'); do
      $OBJCOPY -O binary --only-section="$s" "$tmp/a.o" "$tmp/sa" 2>/dev/null
      $OBJCOPY -O binary --only-section="$s" "$tmp/b.o" "$tmp/sb" 2>/dev/null
      cmp -s "$tmp/sa" "$tmp/sb" || secs="$secs $s"
    done
    echo "DIFFERS   $n:${secs:- (symbol or relocation tables only)}"
  fi
done
echo "codegen-compare: $same identical, $diff different object files"
# Linked executables: compare the loaded sections (code, read-only data, data,
# unwind tables); the .comment section holds the compiler version string.
if [ -n "$3" ] && [ -n "$4" ]; then
  bdiff=""
  for s in .text .rodata .data .eh_frame .init_array .fini_array; do
    $OBJCOPY -O binary --only-section="$s" "$3" "$tmp/ba" 2>/dev/null
    $OBJCOPY -O binary --only-section="$s" "$4" "$tmp/bb" 2>/dev/null
    cmp -s "$tmp/ba" "$tmp/bb" || bdiff="$bdiff $s"
  done
  $OBJCOPY --remove-section=.comment "$3" "$tmp/ea"; $OBJCOPY --remove-section=.comment "$4" "$tmp/eb"
  if cmp -s "$tmp/ea" "$tmp/eb"; then echo "codegen-compare: linked executables identical (except .comment)"
  elif [ -z "$bdiff" ]; then echo "codegen-compare: linked executables: loaded sections identical; other metadata differs"
  else echo "codegen-compare: linked executables differ in:$bdiff"; diff=$((diff+1)); fi
fi
[ "$diff" -eq 0 ]
