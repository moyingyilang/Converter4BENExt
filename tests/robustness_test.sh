#!/usr/bin/env bash
# 健壮性回归：畸形输入必须「明确报错」，不能崩溃
#
# 背景：曾经出现过 cJSON 空指针导致 SEGV（catalog 缺 attributes/materials 时），
# 这类崩溃只在特定数据下触发，靠人工试验很容易漏。这里用 fixture 生成合法样本，
# 再逐项破坏，断言：
#   1) 非零退出；
#   2) 退出码不是 139 (SIGSEGV) / 134 (SIGABRT) / 136 (SIGFPE)；
#   3) stderr 有内容（不是静默失败）。
set -uo pipefail
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
CHAR=${CHAR:-chr_0036_jsspsi}
EFF=${EFF:-$ROOT/.work/bem-mods3/745092_yif.zip}
TMP=${TMP:-$ROOT/.work/robustness}

pass=0; fail=0; skip=0
ok()  { echo "PASS $*"; pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
sk()  { echo "SKIP $*"; skip=$((skip+1)); }
finish() { echo "SUMMARY pass=$pass fail=$fail skip=$skip"; [ "$fail" -eq 0 ]; }

[ -x "$C4" ] || { echo "FAIL c4ben 不可执行: $C4"; exit 1; }
[ -d "$CAT" ] || { sk "catalog 不存在: $CAT"; finish; exit $?; }

rm -rf "$TMP"; mkdir -p "$TMP"
"$C4" fixture "$TMP/base" --catalog "$CAT" --character "$CHAR" >"$TMP/fixture.log" 2>&1 \
  || { sk "fixture 失败（catalog 数据不足）: $(tail -1 "$TMP/fixture.log")"; finish; exit $?; }

# expect_error <名字> <模组目录>
expect_error() {
  local name="$1" dir="$2"
  local log="$TMP/$name.log" out="$TMP/$name.bem"
  "$C4" convert "$dir" --catalog "$CAT" --out "$out" >"$log" 2>&1
  local rc=$?
  case "$rc" in
    0)   bad "$name: 畸形输入却成功返回 0"; return ;;
    139|134|136|135) bad "$name: 崩溃（退出码 $rc）"; return ;;
  esac
  if [ ! -s "$log" ]; then bad "$name: 退出码 $rc 但没有输出（静默失败）"; return; fi
  ok "$name（退出码 $rc，有诊断）"
}

# expect_soft <名字> <模组目录> <必须出现的日志片段>
# 贴图/资源损坏的策略是「跳过该项 + 报告，整体仍产出」——这类不算失败，
# 但必须可见（不能静默），且不能崩溃。
expect_soft() {
  local name="$1" dir="$2" pat="$3"
  local log="$TMP/$name.log" out="$TMP/$name.bem"
  "$C4" convert "$dir" --catalog "$CAT" --out "$out" >"$log" 2>&1
  local rc=$?
  case "$rc" in
    139|134|136|135) bad "$name: 崩溃（退出码 $rc）"; return ;;
  esac
  if ! grep -q "$pat" "$log"; then
    bad "$name: 没有出现预期诊断「$pat」（退出码 $rc）"; return
  fi
  ok "$name（跳过并报告：$(grep -m1 "$pat" "$log" | cut -c1-70)）"
}

# 1) mod.ini 被替换成垃圾
cp -r "$TMP/base" "$TMP/bad_ini"
printf 'this is not an ini\x00\x01\x02\n' > "$TMP/bad_ini/mod.ini"
expect_error bad_ini "$TMP/bad_ini"

# 2) 引用的 VB 文件缺失
cp -r "$TMP/base" "$TMP/missing_vb"
rm -f "$TMP/missing_vb"/Meshes/*.buf
expect_error missing_vb "$TMP/missing_vb"

# 3) DDS 被截断（只剩头部）
cp -r "$TMP/base" "$TMP/trunc_dds"
dds=$(find "$TMP/trunc_dds" -name '*.dds' -print -quit)
if [ -n "${dds:-}" ]; then
  head -c 40 "$dds" > "$dds.tmp" && mv "$dds.tmp" "$dds"
  expect_soft trunc_dds "$TMP/trunc_dds" "truncated DDS\|texture skip"
else
  sk "trunc_dds（fixture 没有生成 DDS）"
fi

# 4) DDS 头改成非法格式码
cp -r "$TMP/base" "$TMP/bad_fmt"
dds=$(find "$TMP/bad_fmt" -name '*.dds' -print -quit)
if [ -n "${dds:-}" ]; then
  # 整个文件替换成非 DDS 内容：走 "not DDS" 分支（比改某个偏移更可靠）
  printf 'NOT_A_DDS_FILE_AT_ALL' > "$dds"
  expect_soft bad_fmt "$TMP/bad_fmt" "not DDS\|unsupported DDS\|texture skip"
else
  sk "bad_fmt（fixture 没有生成 DDS）"
fi

# 5) 输出路径不可写（目录不存在）
log="$TMP/nodir.log"
"$C4" convert "$TMP/base" --catalog "$CAT" --out "$TMP/no/such/dir/x.bem" >"$log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then bad "no_out_dir: 不可写路径却返回 0"
elif [ "$rc" -ge 128 ]; then bad "no_out_dir: 崩溃（退出码 $rc）"
elif [ ! -s "$log" ]; then bad "no_out_dir: 静默失败"
else ok "no_out_dir（退出码 $rc，有诊断）"; fi

# 6) EFF：.effres 魔数被破坏（需要真实 EFF 样本，缺失则 SKIP）
if [ -r "${EFF:-}" ]; then
  mkdir -p "$TMP/eff"
  case "$EFF" in
    *.zip) unzip -qo "$EFF" -d "$TMP/eff" >/dev/null 2>&1 ;;
    *)     bsdtar -xf "$EFF" -C "$TMP/eff" >/dev/null 2>&1 ;;
  esac
  ini=$(find "$TMP/eff" -name mod.ini -print -quit 2>/dev/null)
  if [ -n "${ini:-}" ]; then
    moddir=$(dirname "$ini")
    # 破坏所有 .effres 的魔数：mesh 资源在 ini 里以 path= 引用，
    # 只破坏第一个可能打不中真正被读取的那个。
    nres=0
    while IFS= read -r res; do
      printf 'BROKEN_FILE_MAGIC' | dd of="$res" bs=1 seek=0 conv=notrunc status=none 2>/dev/null
      nres=$((nres+1))
    done < <(find "$moddir" -name '*.effres')
    if [ "$nres" -gt 0 ]; then
      expect_soft bad_effres "$moddir" "skip part\|not EFF\|EFF_AUTHOR\|not an EFF"
    else
      sk "bad_effres（样本里没有 .effres）"
    fi
  else
    sk "bad_effres（解包后没有 mod.ini）"
  fi
else
  sk "bad_effres（缺 EFF 样本: $EFF）"
fi

finish
