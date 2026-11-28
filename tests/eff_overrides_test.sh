#!/usr/bin/env bash
# EFF 材质覆盖回归（BEM 1.5）
#
# 用 fixture-eff-overrides 给真实 EFF mod 注入两组材质：
#   - 命中 catalog 的材质：期望产出 material_overrides（float/value4/keyword/render_queue）
#   - 不命中 catalog 的材质：期望打印 material skip 并丢弃 overrides
# 输出应升到 minor=5，并通过 native be-bem 校验（若可用）。
#
# 用法：
#   ROOT=$HOME C4=<repo>/build/c4ben CAT=<catalog> BEM=<be-bem> SAMPLE=<eff.zip> \
#     bash tests/eff_overrides_test.sh
set -uo pipefail
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
SAMPLE=${SAMPLE:-$ROOT/mods-samples/745092_yif.zip}
TMP=${TMP:-$ROOT/.work/eff-overrides}

pass=0; fail=0; skip=0
ok()  { echo "PASS $*"; pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
sk()  { echo "SKIP $*"; skip=$((skip+1)); }
finish() { echo "SUMMARY pass=$pass fail=$fail skip=$skip"; [ "$fail" -eq 0 ]; }

if [ ! -x "$C4" ]; then echo "FAIL c4ben 不可执行: $C4"; exit 1; fi
if [ ! -x "$CAT" ] && [ ! -d "$CAT" ]; then sk "catalog 不存在: $CAT"; finish; exit $?; fi
if [ ! -r "$SAMPLE" ]; then sk "eff-overrides (样本缺失: $SAMPLE)"; finish; exit $?; fi

rm -rf "$TMP"; mkdir -p "$TMP/mod"
case "$SAMPLE" in
  *.zip) unzip -qo "$SAMPLE" -d "$TMP/mod" >/dev/null 2>&1 ;;
  *)     bsdtar -xf "$SAMPLE" -C "$TMP/mod" >/dev/null 2>&1 ;;
esac
INIFILE=$(find "$TMP/mod" -name mod.ini -print -quit 2>/dev/null)
if [ -z "${INIFILE:-}" ] || [ ! -f "$INIFILE" ]; then
  bad "eff-overrides (解包后没有找到 mod.ini)"; finish; exit 1
fi
MODDIR=$(dirname "$INIFILE")

# 1) 注入
if "$C4" fixture-eff-overrides "$MODDIR" --catalog "$CAT" >"$TMP/fixture.log" 2>&1 \
   && grep -q 'matched material' "$TMP/fixture.log"; then
  ok "fixture-eff-overrides ($(grep 'matched material' "$TMP/fixture.log" | sed 's/^ *//'))"
else
  bad "fixture-eff-overrides 失败: $(tail -1 "$TMP/fixture.log" 2>/dev/null)"
  finish; exit 1
fi

# 2) 转换
if ! "$C4" convert "$MODDIR" --catalog "$CAT" --out "$TMP/out.bem" >"$TMP/convert.log" 2>&1; then
  bad "convert 失败: $(tail -1 "$TMP/convert.log")"; finish; exit 1
fi
OUT="$TMP/out.bem"
if [ ! -s "$OUT" ]; then bad "convert 未产出文件"; finish; exit 1; fi

# 3) header minor 必须是 5
minor=$(od -An -tu2 -j10 -N2 "$OUT" 2>/dev/null | tr -d ' ')
if [ "$minor" = "5" ]; then ok "minor=5"; else bad "minor=$minor（期望 5，说明没有产生 material_overrides）"; fi

# 4) 四类覆盖内容都在
for pat in material_overrides _C4benScalarA _C4benColorA _C4BENFIXTURE 2450; do
  if grep -qa "$pat" "$OUT"; then ok "覆盖内容含 $pat"; else bad "覆盖内容缺 $pat"; fi
done

# 5) 不命中 catalog 的材质应被丢弃且有日志
if grep -q 'material skip: M_c4ben_no_such_material' "$TMP/convert.log"; then
  ok "unmatched 材质已丢弃（有 material skip 日志）"
else
  bad "unmatched 材质没有产生 material skip 日志"
fi

# 6) native 校验（不可用则 SKIP）
if [ -x "$BEM" ]; then
  if "$BEM" validate "$OUT" >"$TMP/validate.log" 2>&1; then
    ok "native validate: $(head -1 "$TMP/validate.log")"
  else
    bad "native validate: $(tail -1 "$TMP/validate.log")"
  fi
else
  sk "native validate（be-bem 不可执行: $BEM）"
fi

finish
