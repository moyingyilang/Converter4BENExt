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
tests/bc_encoder_test.sh            # BC1/BC3/BC4/BC5 host round-trip
tests/bc_integration_test.sh        # fixture BC2→BC1/BC3，normal→BC5
tests/bc_mask_test.sh               # fixture --mask-texture，mask→BC4（8 字节/块）
tests/eff_overrides_test.sh         # fixture-eff-overrides，真实 BEM 1.5
tests/rabbitfx_binding_test.sh      # fixture --rabbitfx，slot→catalog 贴图名
```

- 样本路径/校验器可通过 `ROOT`、`C4`、`CAT`、`BEM`、`SAMPLE`、`CHAR` 覆盖；
- 没有样本或 native `be-bem` 时，脚本输出 `SKIP` 而不是误报失败；
- `texture_io_regression.sh` 的 `argent-rabbitfx` 依赖 `chr_0037_minghe` 的 catalog；
  目前该角色 `textures` 为空，脚本仍验证几何/选项，贴图绑定由 `rabbitfx_binding_test.sh` 用
  有 textures 的角色覆盖。

## 合成样本生成器（不依赖游戏资源）

三个快速回归都靠它们造数据，行为差异如下：

```bash
# EFMI 基础样本（可选 --rabbitfx / --mask-texture）
./build/c4ben fixture <out-dir> --catalog <dir> --character <chr_...> [--rabbitfx] [--mask-texture]

# 给已有 EFF mod 目录注入材质覆盖（不改动原有资源）
./build/c4ben fixture-eff-overrides <eff-mod-dir> --catalog <dir> [--character <chr_...>]
```

| 开关 | 作用 | 服务的用例 |
|---|---|---|
| 默认 | 生成 mod.ini + Meshes + Textures，贴图族按 BC1/BC2/BC3/BC5/BC7 轮转 | `bc_integration_test.sh` |
| `--rabbitfx` | 追加 `[ResourceTexture0]`（文件名带 `t=<catalog hash>`）+ `Resource\RabbitFX\Diffuse = ref ...` 绑定，走 EFMI 的 RabbitFX 解析路径 | `rabbitfx_binding_test.sh` |
| `--mask-texture` | 追加一张 catalog 里真实存在的 mask 类贴图（如 `T_fx_mask_03_02_M`），以 BC2 写入（读取时解码为 RGBA32） | `bc_mask_test.sh` |
| `fixture-eff-overrides` | 给 EFF mod 注入 `[MaterialC4benMatched]`（命中 catalog）与 `[MaterialC4benUnmatched]`（故意不命中），各带 float/value4/keyword/render_queue，并追加到 `[Replace*]` 的 `parts=` | `eff_overrides_test.sh` |

注意事项：

- `--mask-texture` 依赖 catalog 里的 mask 类贴图名（目前只有 `chr_0036_jsspsi` 有），
  找不到时打印 warning 并跳过，用例据此 `SKIP`；
- `--rabbitfx` 同理依赖 catalog `textures` 非空（`chr_0037_minghe` / `chr_0040_sweetpie` 为空）；
- `fixture-eff-overrides` 必须能改写 `[Replace*]` 的 `parts=` 行——Part 是从该键枚举的，
  追加新行不会生效（`Ini` 取首个同名键）。

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
