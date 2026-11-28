# Converter4BENeXt

**EFMI / EFF → BEN (`Better Endfield Next`) `.bem` 迁移工具。**

只做迁移，不做加载器、不做运行时、不碰游戏资源。目标是把旧 mod 作者做好的模型
（EFMI / 3DMigoto 以及后续 EFF Format 3）转成 BEN 能导入的 BEM 包。

- 语言：C++17（无 Python、无 .NET 运行时）
- 依赖：clang++/gcc、zlib、libarchive（`.zip` 用内置 zlib，`.rar`/`.7z` 用 libarchive）；cJSON / bcdec / stb_image 已 vendored
- 状态：EFMI 标准 ComponentN + 合并骨架、EFF Format 3 已能出包；来源覆盖矩阵见 [`docs/COVERAGE.md`](docs/COVERAGE.md)

## 为什么只做迁移

BEN 自己已经有转换器；重复做加载器/运行时没有意义。这个项目只补「旧格式 → BEM」这一段，
输出用 BEN 的读取器和 `validate_manifest` 对拍，不通过不改、不交付。

## 构建

```bash
make            # -> build/c4ben
```

## 用法

```bash
./build/c4ben convert <mod目录|.zip|.rar|.7z> \
    --catalog <BEN catalog目录> \
    --out <输出.bem>

# 旧 EFMI（hash 对不上当前 catalog）
./build/c4ben inspect-legacy <old-mod> --catalog <catalog> --character <chr_...> --out mapping.json
./build/c4ben convert <old-mod> --catalog <catalog> --profile mapping.json --allow-review --out <输出.bem>
# 一键：自动 inspect + 生成 draft profile + 转换（适合旧 ALPHA 包）
./build/c4ben convert <old-mod> --catalog <catalog> --character chr_XXXX --legacy-auto --out <输出.bem>
# 贴图 hash 漂移时按 catalog descriptor 唯一匹配（L3 推断，可关）
./build/c4ben convert <mod> --catalog <catalog> --texture-fingerprint --out <输出.bem>
# 外部提取器（ForTMR 等）给出的 hash→component/texture 身份映射
./build/c4ben convert <mod> --catalog <catalog> --identities identities.json --out <输出.bem>
# catalog 只有布局、entries 为空时（新版 VFS 回填），按唯一布局指纹自动锚定组件
./build/c4ben convert <mod> --catalog <catalog> --character chr_XXXX --auto-entries --out <输出.bem>
# 导出 EFMI 选项组与「变体 → 组件」绘制条件（BEM 1.1 option_groups 的前置数据）
./build/c4ben convert <mod> --catalog <catalog> --option-report options.json --out <输出.bem>
# EFMI 有 KeySwap/KeyToggle 时默认自动输出 BEM 1.1（option_groups + component_rules）
./build/c4ben convert <mod> --catalog <catalog> --out <输出.bem>
# 强制 1.1（或显式指定）；--no-options 则压回固定单外观 1.0
./build/c4ben convert <mod> --catalog <catalog> --emit-options --out <输出.bem>
./build/c4ben convert <mod> --catalog <catalog> --no-options --out <输出.bem>
# opt-in：RGBA32/BC2 压缩成 BC1/BC3（默认仍 RGBA32）
./build/c4ben convert <mod> --catalog <catalog> --texture-compress --out <输出.bem>
```

- `<mod目录>` 需要有 `mod.ini`；也可直接传 `.zip` / `.rar` / `.7z`，包内允许带一层目录，多处 `mod.ini` 会报歧义。
- catalog 用 BEN 仓库的 `tools/CustomModel/catalog/`（AGPL-3.0，外部提供，不随本仓库分发）。
- 输出标准 BEM 二进制；写 `.zip` 扩展名也可以出 ZIP 容器。

## 已验证

| mod | 目标 | 结果 |
|---|---|---|
| `edmin_maid.zip`（管理员女） | `chr_0003_endminf` | 9/11 组件、18 贴图，BEN `read_package` + `validate_manifest` 通过，已导入成功 |
| `mi_fu_swimsuit151.7z`（弭弗） | `chr_0031_mifu` | 8/11 组件、4 贴图，部分 |
| `YvonneSwimsuit.zip`（伊冯） | `chr_0017_yvonne` | 8 组件、18 贴图，校验通过 |
| `Simplified_Fangyi.rar`（庄方宜） | `chr_0030_zhuangfy` | 8 组件、13 贴图，校验通过 |
| `zhuangfangyi...` 旧 ALPHA-7 | `chr_0030_zhuangfy` | `--profile` 实验包，2 组件，仅用于验证兼容层 |

## EFF Format 3（实验性）

已经接入 `EFF_AUTHOR_FILE3` kind=1 的 EFFMESH v6 快照：

- 读 `mod.ini`（`format=3`）的 `[Replace]` / `[Part]` / `[Mesh]` / `[Material]` / `[Texture]`
- 从 `.effres` 的 payload 里找 `EFFMESH\0`，解析顶点/法线/切线/UV/index/skin/骨骼路径
- 多个 `Part` 合并成一个 BEM 组件，按 catalog 的 `target.asset` 找角色
- EFF 的辅助骨骼（飘带/裙摆）不在 BEN 目标组件时，从叶子往上找最近祖先，保证 cloth 仍跟随父骨骼
- BEM 1.0 固定外观；EFF 物理/特效/blendshape/LOD 暂不转换

已验证：

- `lvtsfw.zip`（莱万汀）→ `lvtsfw.bem`，4 个 replace 组件，BEN `read_package` 通过
- 旧版 EFMI（`TextureOverride_ComponentN`，VB2 stride12 = u16 权重 + u8 本地骨骼）已兼容；
  `zhuangfangyi` 的 11 个组件 hash 与当前 catalog 全不匹配（旧游戏版本），不硬转

## 旧 EFMI 兼容层

旧包的组件 hash 不会命中新 catalog，不能按 hash 硬转。流程改成：

1. `inspect-legacy` / `map`：拆出旧组件指纹（index 数、顶点数、骨骼范围、VB stride），
   和指定角色的当前 catalog 打分，输出 `mapping.json` 草稿（`status: draft`、`needs_review`）
2. `convert --profile mapping.json`：严格按映射转；组件/骨骼/贴图没有显式映射就拒绝
3. `--allow-review` 只用于测试草稿，不要拿去直接分发

当前实测：zhuangfangyi 的 11 个旧组件中，2 个能按 profile 转出（comp6→hair、comp9→face），
其余因目标顶点布局 stride 不一致，默认拒绝；显式加 `--compat-layout` 时仅对 **source stride < target stride** 做尾部零填充（不删除/不重排，source 更大仍拒绝）。

## 已知限制

- 贴图：DDS BC1/3/4/5/7 原生直通；BC2/DXT3 解码为 RGBA32；TGA/PNG/JPG 解码为 RGBA32；BC6H（HDR）、cubemap、volume、texture array 明确拒绝（BEM 2D 无法表达）。
- EFF 的材质/贴图只在 catalog 里能找到同名 `source.asset` 时绑定；自定义 Liino 贴图会跳过并报告。
- EFF `.effres` kind=2 已解析 float/color/keyword；`material_overrides`（含 `render_queue`、`shader_passes`）需要 BEM 1.5 的完整组合结构，**当前降级为合法的 BEM 1.0 并丢弃该字段**（运行时会打印 warning），不产出会被 validator 拒绝的包。
- EFF 辅助骨骼用祖先替代，会损失飘带/裙摆的二级摆动。
- EFF 的 blendshape、物理、特效、LOD 不转；EFF 网格的 UV0–UV7 并集与 color 会保留。
- EFMI 支持标准 ComponentN 的 skin stride 4/8/12/16/24（8/4/24 为 L3 推断，见 docs/COVERAGE.md）；EFMI 选项/热键/多外观会检测并打印（KeySwap/KeyToggle），`--option-report` 导出选项组与 `$draw_*` 的「组+取值 → 组件」条件，`--emit-options` 可输出 **BEM 1.1**（`option_groups` + `component_rules`，KeySwap 多外观保留）。限制：条件只表达单组的「等于」判断，且要求 catalog 提供完整 `attributes`、三条流 stride 与属性布局可由重排对齐；不满足时自动退回 BEM 1.0 并说明原因。
- 合并不住的组件会 `keep`，不猜；catalog 缺 hash 的包不输出，或走 `inspect-legacy` 兼容层。
- 新版 VFS 回填的 catalog 只有 `components` 布局、`entries`/`textures` 为空。用 `--auto-entries` 可按「draw 索引数精确相等 + 顶点数不超过目标 + stride 一致」的唯一布局指纹自动补条目；多个候选时报 ambiguous 并跳过，不猜。目标是新角色包不必手工写 mapping。
- 旧版 catalog/资源缺失时无法自动完成身份锚定（样本：庄方宜旧包）；EFMI ALPHA-5 包可用 `--legacy-auto --texture-fingerprint` 恢复几何+贴图。
- 非模型包（GUI/HUD/POI）与动态 `Pool_MergedSkeleton` EFMI 变体会明确报错，不再误报 catalog 不匹配。
- 二进制 payload 未压缩；大包会到几十 MB。
- 只用 BEN 的读取器/校验器对拍，未替换 BEN 自己的转换器；实机渲染仍需作者验证。

## Catalog 回填

旧 mod 的 EFMI hash 随游戏版本变化；如果只有旧 mod、没有旧游戏资源，无法可靠映射。
VFS 回填方案见 [`docs/CATALOG-BACKFILL.md`](docs/CATALOG-BACKFILL.md)：从旧版本 `.blc/.ab`
抽原生资料 + 按 BEN 的 CRC32C 规则生成候选 catalog，交 BEN 复核。

## 路线

1. `inspect`：先报告角色/组件/骨骼/贴图/catalog 缺口，不达标不出包
2. EFF Format 3 解析（Unity SerializedFile + author resource）
3. EFMI 选项 → BEM 1.1 option_groups
4. BEN catalog 版本适配 + 更多真实包回归
5. 如果 BEN 愿意，作为上游 PR 或独立工具并给 BEN

## 许可

**AGPL-3.0-only**（见 `LICENSE`）。cJSON 为 MIT（见 `NOTICE`）。

这个项目与 BEN 官方、游戏开发商/发行商无关联；只做格式迁移，不包含游戏资源或反作弊绕过。
