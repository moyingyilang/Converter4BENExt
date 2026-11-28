#!/usr/bin/env bash
# BC4（mask/roughness/metal）压缩路径回归
#
# BC4 的判定 token（_mask/rough/metal/_mra/_orm/_ao/_r.）在真实样本里很难自然出现：
# fixture 的贴图名来自 catalog 的 material_texture_properties（如 T_..._D / _N），
# 都不含这些 token。因此这条分支长期没有测试真正执行过。
#
# 本用例用 fixture --mask-texture 把 catalog 里真实存在的 mask 类贴图名
# （如 T_fx_mask_03_02_M）作为独立纹理段写入，确保：
#   maskLikeName 命中 -> encodeRgbaBc4 -> BEM fmt 26
# 这条链路被真正走一遍，并断言压缩体积符合 BC4 的 8 字节/块。
set -uo pipefail
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
CHAR=${CHAR:-chr_0036_jsspsi}
TMP=${TMP:-$ROOT/.work/bc4-mask}

pass=0; fail=0; skip=0
ok()  { echo "PASS $*"; pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
sk()  { echo "SKIP $*"; skip=$((skip+1)); }
finish() { echo "SUMMARY pass=$pass fail=$fail skip=$skip"; [ "$fail" -eq 0 ]; }

[ -x "$C4" ] || { echo "FAIL c4ben 不可执行: $C4"; exit 1; }
[ -d "$CAT" ] || { sk "catalog 不存在: $CAT"; finish; exit $?; }

rm -rf "$TMP"; mkdir -p "$TMP"

if ! "$C4" fixture "$TMP/mod" --catalog "$CAT" --character "$CHAR" --mask-texture >"$TMP/fixture.log" 2>&1; then
  bad "fixture --mask-texture 失败: $(tail -1 "$TMP/fixture.log")"; finish; exit 1
fi
if grep -q 'mask texture:' "$TMP/fixture.log"; then
  ok "fixture 写入 mask 贴图（$(grep 'mask texture:' "$TMP/fixture.log" | sed 's/^ *//')）"
else
  sk "catalog 里没有 mask 类贴图名（$CHAR），无法覆盖 BC4"
  finish; exit $?
fi

if ! "$C4" convert "$TMP/mod" --catalog "$CAT" --texture-compress --out "$TMP/out.bem" >"$TMP/convert.log" 2>&1; then
  bad "convert 失败: $(tail -1 "$TMP/convert.log")"; finish; exit 1
fi

# 1) BC4 分支必须真的被执行
if grep -q 'texture compress: BC4' "$TMP/convert.log"; then
  ok "BC4 编码被执行（$(grep 'texture compress: BC4' "$TMP/convert.log" | sed 's/^ *//')）"
else
  bad "BC4 分支没有被执行（mask 类贴图没有触发压缩）"
fi

# 2) 必须是因为 mask 语义（而不是碰巧走了 BC1/BC3）
if grep -q 'mask-like:' "$TMP/convert.log"; then
  ok "分类命中 mask-like"
else
  bad "没有打印 mask-like，分类可能走错分支"
fi

# 3) 体积必须符合 BC4：8 字节/块；64x64 = 4x4 块，BC4 应为 2048 字节
line=$(grep 'texture compress: BC4' "$TMP/convert.log" | head -1)
if echo "$line" | grep -q -- '-> 2048 bytes'; then
  ok "BC4 压缩体积正确（-> 2048 bytes，8B/块）"
else
  bad "BC4 压缩体积异常: $line"
fi

# 4) 输出仍然合法
if [ -s "$TMP/out.bem" ]; then
  if [ -x "$BEM" ]; then
    if "$BEM" validate "$TMP/out.bem" >"$TMP/validate.log" 2>&1; then
      ok "native validate: $(head -1 "$TMP/validate.log")"
    else
      bad "native validate: $(tail -1 "$TMP/validate.log")"
    fi
  else
    sk "native validate（be-bem 不可执行: $BEM）"
  fi
else
  bad "convert 未产出文件"
fi

finish
