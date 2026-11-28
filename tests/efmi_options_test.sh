#!/usr/bin/env bash
# EFMI KeySwap/KeyToggle：默认自动输出 BEM 1.1；--no-options 回退 1.0。
set -uo pipefail
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
SAMPLE=${SAMPLE:-$ROOT/.work/bem-mods3/732494_si_v10_ca9ce.7z}
TMP=${TMP:-$ROOT/.work/efmi-options}
pass=0; fail=0; skip=0
ok(){ echo "PASS $*"; pass=$((pass+1)); }
bad(){ echo "FAIL $*"; fail=$((fail+1)); }
sk(){ echo "SKIP $*"; skip=$((skip+1)); }
finish(){ echo "SUMMARY pass=$pass fail=$fail skip=$skip"; [ "$fail" -eq 0 ]; }
[ -x "$C4" ] || { echo "FAIL c4ben 不可执行"; exit 1; }
[ -f "$SAMPLE" ] || { sk "样本缺失: $SAMPLE"; finish; exit $?; }
rm -rf "$TMP"; mkdir -p "$TMP"
"$C4" convert "$SAMPLE" --catalog "$CAT" --auto-entries --out "$TMP/auto.bem" >"$TMP/auto.log" 2>&1
if grep -q 'emitting BEM 1.1: 5 option group(s)' "$TMP/auto.log"; then ok "默认自动 1.1 + 5 option groups"; else bad "默认没有自动 1.1"; fi
if [ -x "$BEM" ]; then
  "$BEM" validate "$TMP/auto.bem" >"$TMP/auto.validate" 2>&1 && grep -q 'v1.1' "$TMP/auto.validate" && ok "auto native v1.1" || bad "auto validate"
fi
"$C4" convert "$SAMPLE" --catalog "$CAT" --auto-entries --no-options --out "$TMP/no.bem" >"$TMP/no.log" 2>&1
if [ -x "$BEM" ]; then
  "$BEM" validate "$TMP/no.bem" >"$TMP/no.validate" 2>&1 && grep -q 'v1.0' "$TMP/no.validate" && ok "--no-options 回退 v1.0" || bad "--no-options validate"
fi
finish
