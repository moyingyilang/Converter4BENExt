#!/data/data/com.termux/files/usr/bin/bash
# 旧来源兼容回归：需要外部样本与 catalog，缺样本时跳过。
set -u
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-entries}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/debug/be-bem}
TMP=${TMP:-$ROOT/.work/compat-regression}
mkdir -p "$TMP"
pass=0; fail=0; skip=0
meshes_of(){ sed -n 's/.*meshes=\([0-9]*\).*/\1/p' "$1" | tail -1; }
run_case(){
  name=$1; dir=$2; expect=$3; shift 3
  [ -f "$dir/mod.ini" ] || { echo "SKIP $name (no sample)"; skip=$((skip+1)); return; }
  out="$TMP/$name.bem"; log="$TMP/$name.log"
  "$C4" convert "$dir" --catalog "$CAT" "$@" --out "$out" >"$log" 2>&1
  rc=$?; n=$(meshes_of "$log"); n=${n:-0}
  if [ $rc -ne 0 ] || [ "$n" -lt "$expect" ]; then
    echo "FAIL $name meshes=$n expect>=$expect rc=$rc"; fail=$((fail+1)); return
  fi
  if [ -x "$BEM" ]; then
    if ! "$BEM" validate "$out" >"$TMP/$name.validate" 2>&1; then
      if grep -q "未知能力" "$TMP/$name.validate"; then echo "WARN $name validator lacks capability"; else echo "FAIL $name validate"; fail=$((fail+1)); return; fi
    fi
  fi
  echo "PASS $name meshes=$n"; pass=$((pass+1))
}
run_case zhuang-old \
  "$ROOT/.work/bem-mods/zhuang/ZhuangFangyi_NayutaWuwei_Mod_By_FengliS/ZhuangFangyi Ultimate Nayuta Wuwei" 8 \
  --profile "$ROOT/.work/test-zhuang-entries2.bem.mapping.json" --allow-review --compat-layout --compat-bones
run_case kay-graffiti \
  "$ROOT/.work/bem-mods3/745876_kay_graffiti_girl_sfw/kay Graffiti Girl" 5 \
  --character chr_0040_sweetpie --legacy-auto --compat-layout --compat-bones
run_case yvonne-standard \
  "$ROOT/.work/bem-mods/yvonne/YvonneSwimsuit" 9
echo "SUMMARY pass=$pass fail=$fail skip=$skip"
[ "$fail" -eq 0 ]
