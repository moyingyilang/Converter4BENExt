#!/usr/bin/env bash
# RabbitFX 贴图绑定回归
#
# 用 fixture --rabbitfx 生成 RabbitFX 风格的合成包（贴图 hash 来自 catalog textures，
# 文件名带 t=<8hex>），验证 EFMI 的 RabbitFX 解析路径能把
#   [ResourceTextureN] + `Resource\RabbitFX\Diffuse = ref ResourceTextureN`
# 解析成 catalog 贴图名，并真正绑到 draw.textures 上。
#
# 需要 catalog 的 textures 表非空（minghe/sweetpie 为空，会 SKIP）。
set -uo pipefail
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
CHAR=${CHAR:-chr_0036_jsspsi}
TMP=${TMP:-$ROOT/.work/rabbitfx-binding}

pass=0; fail=0; skip=0
ok()  { echo "PASS $*"; pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
sk()  { echo "SKIP $*"; skip=$((skip+1)); }
finish() { echo "SUMMARY pass=$pass fail=$fail skip=$skip"; [ "$fail" -eq 0 ]; }

[ -x "$C4" ] || { echo "FAIL c4ben 不可执行: $C4"; exit 1; }
[ -d "$CAT" ] || { sk "catalog 不存在: $CAT"; finish; exit $?; }

rm -rf "$TMP"; mkdir -p "$TMP"

# 1) 生成 RabbitFX fixture
if "$C4" fixture "$TMP/mod" --catalog "$CAT" --character "$CHAR" --rabbitfx >"$TMP/fixture.log" 2>&1 \
   && grep -q 'rabbitfx:' "$TMP/fixture.log"; then
  ok "fixture --rabbitfx ($(grep 'rabbitfx:' "$TMP/fixture.log" | sed 's/^ *//'))"
else
  if grep -q '找不到可用贴图' "$TMP/fixture.log"; then
    sk "catalog textures 为空，无法验证 RabbitFX 绑定（$CHAR）"
    finish; exit $?
  fi
  bad "fixture --rabbitfx 失败: $(tail -1 "$TMP/fixture.log")"
  finish; exit 1
fi

# 生成的 ini 必须含 RabbitFX 绑定写法
if grep -q 'Resource\\RabbitFX\\Diffuse = ref ResourceTexture0' "$TMP/mod/mod.ini"; then
  ok "ini 含 Resource\\RabbitFX 绑定"
else
  bad "ini 缺少 Resource\\RabbitFX 绑定"
fi

# 2) 转换
if ! "$C4" convert "$TMP/mod" --catalog "$CAT" --out "$TMP/out.bem" >"$TMP/convert.log" 2>&1; then
  bad "convert 失败: $(tail -1 "$TMP/convert.log")"; finish; exit 1
fi
OUT="$TMP/out.bem"
[ -s "$OUT" ] || { bad "convert 未产出文件"; finish; exit 1; }

# 3) 贴图必须被识别（不是 0）
if grep -q 'textures: 0 matched' "$TMP/convert.log"; then
  bad "RabbitFX 贴图未被识别（textures: 0 matched）"
else
  ok "RabbitFX 贴图已识别（$(grep -o 'textures: [0-9]* matched' "$TMP/convert.log" | head -1)）"
fi

# 4) manifest 里必须是 catalog 原名（T_actor_... ），不是 hash 或空
name=$(grep -ao 'T_actor_[A-Za-z0-9_]*' "$OUT" | head -1)
if [ -n "$name" ]; then ok "manifest 贴图名: $name"; else bad "manifest 里没有 catalog 贴图名"; fi

# 5) draw.textures 不能为空
if grep -qa '"textures":\[0' "$OUT"; then
  ok "draw.textures 非空"
else
  bad "draw.textures 为空（RabbitFX 未绑定到 draw）"
fi

# 6) native 校验
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
