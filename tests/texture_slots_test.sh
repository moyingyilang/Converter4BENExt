#!/usr/bin/env bash
# BEM 1.2+ 贴图槽（texture_slots）回归
#
# 语义要点（BEM_FORMAT_SPEC §9）：
#   - texture_slots[].id 是**字符串标签**，由包决定，不是运行时属性 ID；
#   - 运行时按候选贴图的 original_name 逐字匹配原贴图来替换，
#     所以 pack 里不需要 property_id（那是运行时候自己算的）；
#   - draw.textures 用 {"slot": id} 引用槽。
#
# 该特性默认关闭（--texture-slots 开启）：格式已按规范实现且双校验器通过，
# 但 BEN 运行时是否已消费 texture_slots 尚未确认，默认打开会让贴图整个失效。
set -uo pipefail
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
SAMPLE=${SAMPLE:-$ROOT/.work/bem-mods3/745092_yif.zip}
TMP=${TMP:-$ROOT/.work/texture-slots}

pass=0; fail=0; skip=0
ok()  { echo "PASS $*"; pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
sk()  { echo "SKIP $*"; skip=$((skip+1)); }
finish() { echo "SUMMARY pass=$pass fail=$fail skip=$skip"; [ "$fail" -eq 0 ]; }

[ -x "$C4" ] || { echo "FAIL c4ben 不可执行: $C4"; exit 1; }
[ -d "$CAT" ] || { sk "catalog 不存在: $CAT"; finish; exit $?; }
[ -r "$SAMPLE" ] || { sk "样本缺失: $SAMPLE"; finish; exit $?; }

rm -rf "$TMP"; mkdir -p "$TMP/mod"
case "$SAMPLE" in
  *.zip) unzip -qo "$SAMPLE" -d "$TMP/mod" >/dev/null 2>&1 ;;
  *)     bsdtar -xf "$SAMPLE" -C "$TMP/mod" >/dev/null 2>&1 ;;
esac
INI=$(find "$TMP/mod" -name mod.ini -print -quit 2>/dev/null)
[ -n "${INI:-}" ] || { sk "解包后没有 mod.ini"; finish; exit $?; }
MODDIR=$(dirname "$INI")

# 只有带材质覆盖的 EFF 包才会升到 1.5（1.2+ 才允许 texture_slots），
# 用 fixture-eff-overrides 注入，保证 minor>=2。
"$C4" fixture-eff-overrides "$MODDIR" --catalog "$CAT" >"$TMP/fixture.log" 2>&1 \
  || { sk "fixture-eff-overrides 不可用"; finish; exit $?; }

# 1) 默认：不写 texture_slots，draw.textures 保持裸整数
"$C4" convert "$MODDIR" --catalog "$CAT" --out "$TMP/off.bem" >"$TMP/off.log" 2>&1
if [ -s "$TMP/off.bem" ]; then
  if grep -qa '"texture_slots"' "$TMP/off.bem"; then
    bad "默认输出出现了 texture_slots（应为 opt-in）"
  else
    ok "默认不输出 texture_slots"
  fi
  if grep -qa '"textures":\[0' "$TMP/off.bem" || grep -qa '"textures":\[1' "$TMP/off.bem"; then
    ok "默认 draw.textures 仍是裸整数"
  else
    bad "默认 draw.textures 不是裸整数"
  fi
else
  bad "默认转换失败: $(tail -1 "$TMP/off.log")"
fi

# 2) --texture-slots：写出槽 + 能力声明 + 对象引用
"$C4" convert "$MODDIR" --catalog "$CAT" --texture-slots --out "$TMP/on.bem" >"$TMP/on.log" 2>&1
if [ -s "$TMP/on.bem" ]; then
  if grep -qa '"texture_slots"' "$TMP/on.bem"; then
    ok "开启后输出 texture_slots（$(grep -o 'texture_slots: [0-9]* 个槽' "$TMP/on.log" | head -1)）"
  else
    bad "开启后仍没有 texture_slots"
  fi
  if grep -qa '"texture-slots"' "$TMP/on.bem"; then
    ok "声明了 texture-slots 能力"
  else
    bad "缺少 texture-slots 能力声明"
  fi
  if grep -qa '"slot":"' "$TMP/on.bem"; then
    ok "draw.textures 使用 {\"slot\": id} 引用"
  else
    bad "draw.textures 没有使用槽引用"
  fi
  if grep -qa '"candidates"' "$TMP/on.bem"; then
    ok "槽带 candidates"
  else
    bad "槽缺少 candidates"
  fi
else
  bad "--texture-slots 转换失败: $(tail -1 "$TMP/on.log")"
fi

# 3) native 校验
if [ -x "$BEM" ]; then
  if "$BEM" validate "$TMP/on.bem" >"$TMP/validate.log" 2>&1; then
    ok "native validate: $(head -1 "$TMP/validate.log")"
  else
    bad "native validate: $(tail -1 "$TMP/validate.log")"
  fi
else
  sk "native validate（be-bem 不可执行: $BEM）"
fi

finish
