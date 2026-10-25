#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/probe.c" <<'SOURCE'
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
 (void)argv;
 char *a=malloc(8),*b=calloc(2,8),*c=strdup("probe");
 a=realloc(a,32);free(b);free(c);
 if(argc==1)free(a);
 return 0;
}
SOURCE
"${CC:-cc}" -O0 -g -fno-pie -no-pie -o "$work/probe" "$work/probe.c" "$root/tools/allocation_audit.c" -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup,--wrap=strndup
"$work/probe"
set +e
KRU_ALLOCATION_AUDIT_LOG="$work/findings" "$work/probe" leak > "$work/out" 2>&1
status=$?
set -e
[[ $status == 99 ]]
rg -q 'FAIL 1 blocks / 32 bytes' "$work/out"
rg -q '^1 blocks / 32 bytes$' "$work/findings"
echo 'PASS allocation-audit/clean allocations pass and deliberate realloc leak fails'
