#!/data/data/com.termux/files/usr/bin/bash
# BC 压缩集成测试：用 fixture 生成 BC2 DDS，再 --texture-compress 转 BEM。
set -eu
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
TMP=${TMP:-$ROOT/.work/bc-integration}
rm -rf "$TMP"; mkdir -p "$TMP"
"$C4" fixture "$TMP/mod" --catalog "$CAT" --character chr_0036_jsspsi >"$TMP/fixture.log" 2>&1
"$C4" convert "$TMP/mod" --catalog "$CAT" --texture-compress --out "$TMP/out.bem" >"$TMP/convert.log" 2>&1
grep -qE 'texture compress: BC|texture-compress skipped' "$TMP/convert.log" || { echo "FAIL: no texture-compress decision line"; exit 1; }
if [ -x "$BEM" ]; then
  "$BEM" validate "$TMP/out.bem" >/dev/null 2>&1 || { echo "FAIL: validate"; exit 1; }
else
  echo "SKIP validate: native be-bem not found"
fi
grep 'texture compress' "$TMP/convert.log" | head -3
echo "PASS bc-integration"
