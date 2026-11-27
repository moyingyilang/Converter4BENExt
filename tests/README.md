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
