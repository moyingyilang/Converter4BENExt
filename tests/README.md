# 测试

本目录不保存游戏资源或 mod 包。

## 批量覆盖回归（推荐）

```bash
make
BEM_CLI=/path/to/be-bem tests/coverage.sh <mods-root> <BEN-catalog-dir> build/coverage.md
```

- 扫描 `<mods-root>` 下所有含 `mod.ini` 的目录；
- 每个包转换一次，输出 `.bem` 到 `$COVERAGE_OUT`（默认 `<mods-root>/coverage-out`）；
- 自动用 BEN `be-bem validate` 校验（设置 `BEM_CLI` 或 PATH 中有 `be-bem`）；
- 生成 markdown 表格：结果、meshes、textures、keep 数、validate。

## 快速回归脚本

```bash
make
tests/compat_regression.sh          # 旧 EFMI / Kay EFF3 / Yvonne EFMI
tests/texture_io_regression.sh      # SI 7z、yif EFF3、SI+BC、argent RabbitFX
tests/efmi_options_test.sh          # EFMI KeySwap 默认 1.1 / --no-options 回退 1.0
tests/bc_encoder_test.sh            # BC1/BC3/BC5 host round-trip
tests/bc_integration_test.sh        # fixture BC2→BC1/BC3，normal→BC5
tests/eff_overrides_test.sh         # fixture-eff-overrides，真实 BEM 1.5
tests/rabbitfx_binding_test.sh      # fixture --rabbitfx，slot→catalog 贴图名
```

- 样本路径/校验器可通过 `ROOT`、`C4`、`CAT`、`BEM`、`SAMPLE`、`CHAR` 覆盖；
- 没有样本或 native `be-bem` 时，脚本输出 `SKIP` 而不是误报失败；
- `texture_io_regression.sh` 的 `argent-rabbitfx` 依赖 `chr_0037_minghe` 的 catalog；
  目前该角色 `textures` 为空，脚本仍验证几何/选项，贴图绑定由 `rabbitfx_binding_test.sh` 用
  有 textures 的角色覆盖。

## 单项手工回归

1. 解压一个 EFMI/EFF 包（含 mod.ini）；
2. BEN catalog 放到本地目录；
3. `./build/c4ben convert <mod目录> --catalog <catalog> --out /tmp/x.bem`；
4. `be-bem validate /tmp/x.bem`；
5. 覆盖结论写进 `docs/COVERAGE.md`。

## 已覆盖的特殊路径

- BC2/DXT3、TGA、PNG 贴图 → RGBA32；
- EFF `handling=hide` → BEM `operation: hide`；
- EFMI skin stride 4/8/12/16/24；
- `--profile` 旧版映射（`inspect-legacy` + mapping.json）。

## 已知回归样本

见 `docs/COVERAGE.md` 第 3 节；旧版庄方宜包是已知 `FAIL`（缺旧 catalog）。
