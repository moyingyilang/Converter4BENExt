#!/data/data/com.termux/files/usr/bin/bash
# texture IO / BEM 1.1 真实样本回归（需要手机上的 .work 样本）
set -u
ROOT=${ROOT:-/data/data/com.termux/files/home}
C4=${C4:-$ROOT/Converter4BENeXt/build/c4ben}
CAT=${CAT:-$ROOT/.work/runtime-catalog/catalog-upstream-final}
BEM=${BEM:-$ROOT/BEN/Better-Endfield-Next-mod-ForTMR4A/native/bem/target/release/be-bem}
TMP=${TMP:-$ROOT/.work/texture-io-regression}
mkdir -p "$TMP"
pass=0; fail=0; skip=0
meshes_of(){ sed -n 's/.*meshes=\([0-9]*\).*/\1/p' "$1" | tail -1; }
run_case(){
  name=$1; src=$2; expect=$3; shift 3
  [ -f "$src" ] || { echo "SKIP $name (no sample)"; skip=$((skip+1)); return; }
  out="$TMP/$name.bem"; log="$TMP/$name.log"
  "$C4" convert "$src" --catalog "$CAT" "$@" --out "$out" >"$log" 2>&1
  rc=$?; n=$(meshes_of "$log"); n=${n:-0}
  if [ $rc -ne 0 ] || [ "$n" -lt "$expect" ]; then echo "FAIL $name meshes=$n expect>=$expect rc=$rc"; fail=$((fail+1)); return; fi
  if [ -x "$BEM" ] && ! "$BEM" validate "$out" >"$TMP/$name.validate" 2>&1; then echo "FAIL $name validate: $(tail -1 "$TMP/$name.validate")"; fail=$((fail+1)); return; fi
  echo "PASS $name meshes=$n"; pass=$((pass+1))
}
run_case si-7z "$ROOT/.work/bem-mods3/732494_si_v10_ca9ce.7z" 14 --auto-entries
run_case yif-eff3 "$ROOT/.work/bem-mods3/745092_yif.zip" 4 --auto-entries
run_case si-bc "$ROOT/.work/bem-mods3/732494_si_v10_ca9ce.7z" 14 --auto-entries --texture-compress
run_case argent-rabbitfx "$ROOT/.work/bem-mods3/744828_argent_flow.rar" 6 \
  --character chr_0037_minghe --auto-entries --emit-options
echo "SUMMARY pass=$pass fail=$fail skip=$skip"
[ "$fail" -eq 0 ]
