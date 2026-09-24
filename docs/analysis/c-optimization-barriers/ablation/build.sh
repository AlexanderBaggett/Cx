#!/bin/bash
# usage: build.sh <proj> <variant> "<extra cflags>" [lto]
set -e
: "${WORK:?set WORK to the work dir (contains src/)}"; S=$WORK; export PATH="${LLVM:?set LLVM to the LLVM 23.1.2 install dir}/bin:$PATH"
P=$1; V=$2; X="$3"; LTO=$4
SRC=$S/src; OUT=$S/abl/bin/$P-$V; O=$S/abl/obj/$P-$V; rm -rf $O; mkdir -p $O
if [ -n "$LTO" ]; then X="$X -flto"; fi
link_it() {
  if [ -n "$LTO" ]; then
    OL=$(echo "$CF" | grep -oE -- '-O[0-3]' | head -1)
    llvm-lto $OL --exported-symbol=main -o $O/lto.native.o $O/*.o
    clang $O/lto.native.o -o $OUT "$@"
  else
    clang $CF $X $O/*.o -o $OUT "$@"
  fi
}
case $P in
  sqlite|sqlite_sep)
    CF="-O2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0"; [ $P = sqlite_sep ] && CF="$CF -DSQLITE_CORE=1"
    if [ $P = sqlite ]; then
      clang $CF $X -I$SRC/sqlite-bld -c $SRC/sqlite-bld/sqlite3.c -o $O/sqlite3.o
    else
      ls $SRC/sqlite-bld/tsrc/*.c | grep -vE '/(shell|tclsqlite-ex|geopoly|icu|fts3_icu)\.c$' | \
        xargs -P4 -I{} sh -c "clang $CF $X -I$SRC/sqlite-bld/tsrc -I$SRC/sqlite-bld $(ls -d $SRC/sqlite/ext/*/ | sed "s#^#-I#" | tr "\n" " ")  -c {} -o $O/\$(basename {} .c).o"
    fi
    clang $CF $X -I$SRC/sqlite-bld -c $SRC/sqlite/test/speedtest1.c -o $O/speedtest1.o
    link_it -lm ;;
  lua|lua_one)
    CF="-O2 -std=c99 -DLUA_COMPAT_5_3 -DLUA_USE_LINUX"
    if [ $P = lua ]; then
      ls $SRC/lua/*.c | grep -vE '/(onelua|ltests)\.c$' | xargs -P4 -I{} sh -c "clang $CF $X -c {} -o $O/\$(basename {} .c).o"
    else
      clang $CF $X -c $SRC/lua/onelua.c -o $O/onelua.o
    fi
    link_it -lm -ldl ;;
  zstd)
    CF="-O3 -DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DBACKTRACE_ENABLE=0 -I$SRC/zstd/lib -I$SRC/zstd/lib/common"
    find $SRC/zstd/lib/common $SRC/zstd/lib/compress $SRC/zstd/lib/decompress $SRC/zstd/lib/dictBuilder $SRC/zstd/programs -name '*.c' | \
      xargs -P4 -I{} sh -c "clang $CF $X -c {} -o $O/\$(echo {} | md5sum | cut -c1-8)_\$(basename {} .c).o"
    link_it -lpthread ;;
esac
echo "built $OUT"
