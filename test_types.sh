#!/usr/bin/env bash
set -euo pipefail
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
${CC:-cc} -std=gnu11 -Wall -Wextra ${KRU_TYPE_TEST_FLAGS:-} tests/type_metadata.c src/types.c -o "$work/types"
"$work/types"
echo 'PASS types/canonical primitive identity, layout and literal ranges'
