#!/bin/sh
# Verify that upstream/ is still the unmodified llvm-project llvmorg-23.1.2
# import: every subtree must match the git tree hash recorded at import time,
# and the working tree must have no local changes under upstream/.
# All Cx compiler changes belong in cx/, never in upstream/.
set -eu
cd "$(dirname "$0")/.."
status=0
while read -r dir want; do
    got=$(git rev-parse "HEAD:upstream/$dir" 2>/dev/null || echo missing)
    if [ "$got" = "$want" ]; then
        echo "ok        upstream/$dir"
    else
        echo "MODIFIED  upstream/$dir (tree $got, expected $want)"; status=1
    fi
done <<'EOF'
llvm        61787c463b4480d027ed002709cee7fe692e2d95
clang       75aaad8757ca25dfb29bd0c91a9973d655f98afa
cmake       8564cd0da20c5f686ecc22766cdcdc3142bb12c8
third-party 270e6e0e497aeaa07ea4181ba733b8f13a7e1c66
libc        b20ef90dbd50c54742d1d6db6653bf90edb2adc6
EOF
if ! git diff --quiet HEAD -- upstream || [ -n "$(git ls-files --others --exclude-standard upstream | head -1)" ]; then
    echo "MODIFIED  upstream/ has uncommitted changes"; status=1
fi
[ $status -eq 0 ] && echo "check-upstream: upstream/ is the unmodified llvmorg-23.1.2 import"
exit $status
