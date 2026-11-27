#!/data/data/com.termux/files/usr/bin/sh
# Batch conversion coverage report (no game resources bundled).
#
#   tests/coverage.sh <mods-root> <catalog-dir> [report.md]
#
# Every directory containing mod.ini under <mods-root> is converted once. When
# BEM_CLI points at BEN's `be-bem` (or be-bem is on PATH) each output is also
# validated by the same reader the runtime uses.
set -eu
ROOT="${1:?usage: tests/coverage.sh <mods-root> <catalog-dir> [report.md]}"
CAT="${2:?missing catalog-dir}"
REPORT="${3:-$ROOT/coverage.md}"
OUT="${COVERAGE_OUT:-$ROOT/coverage-out}"
C4BEN="${C4BEN:-./build/c4ben}"
BEM_CLI="${BEM_CLI:-be-bem}"
mkdir -p "$OUT"
printf '| source | result | meshes | textures | skipped | validate |\n| --- | --- | --- | --- | --- | --- |\n' > "$REPORT"
find "$ROOT" -name mod.ini -type f | sort | while IFS= read -r ini; do
    dir="$(dirname "$ini")"
    name="$(basename "$dir")"
    log="$OUT/$name.log"
    bem="$OUT/$name.bem"
    result="OK"
    if ! "$C4BEN" convert "$dir" --catalog "$CAT" --out "$bem" >"$log" 2>&1; then
        result="FAIL"
    fi
    meshes="$(sed -n 's/.*written .*meshes=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    textures="$(sed -n 's/.*written .*textures=\([0-9]*\).*/\1/p' "$log" | tail -1)"
    skipped="$(grep -c '^skip ' "$log" 2>/dev/null || true)"
    validate="-"
    if [ "$result" = "OK" ]; then
        if command -v "$BEM_CLI" >/dev/null 2>&1; then
            if "$BEM_CLI" validate "$bem" >/dev/null 2>&1; then validate="OK"; else validate="FAIL"; fi
        fi
    fi
    printf '| %s | %s | %s | %s | %s | %s |\n' "$name" "$result" "${meshes:--}" "${textures:--}" "${skipped:-0}" "$validate" >> "$REPORT"
    echo "$result $name"
done
echo "report: $REPORT"
