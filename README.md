# Converter4BENExt

**EFMI / EFF → BEN (`Better Endfield Next`) `.bem` 迁移工具。**

只做迁移，不做加载器、不做运行时、不碰游戏资源。目标是把旧 mod 作者做好的模型
（EFMI / 3DMigoto 以及后续 EFF Format 3）转成 BEN 能导入的 BEM 包。

- 语言：C++17（无 Python、无 .NET 运行时）
- 依赖：clang++/gcc、zlib；cJSON 已 vendored（MIT）
- 状态：**MVP**，EFMI 标准 ComponentN + 合并骨架已能出包；EFF 未实现

## 为什么只做迁移

BEN 自己已经有转换器；重复做加载器/运行时没有意义。这个项目只补「旧格式 → BEM」这一段，
输出用 BEN 的读取器和 `validate_manifest` 对拍，不通过不改、不交付。

## 构建

```bash
make            # -> build/c4ben
```

## 用法

```bash
./build/c4ben convert <解压后的mod目录> \
    --catalog <BEN catalog目录> \
    --out <输出.bem>
```

- `<mod目录>` 需要有 `mod.ini`（EFMI 目录/解压后的包）。
- catalog 用 BEN 仓库的 `tools/CustomModel/catalog/`（AGPL-3.0，外部提供，不随本仓库分发）。
- 输出标准 BEM 二进制；写 `.zip` 扩展名也可以出 ZIP 容器。

## 已验证

| mod | 目标 | 结果 |
|---|---|---|
| `edmin_maid.zip`（管理员女） | `chr_0003_endminf` | 9/11 组件、18 贴图，BEN `read_package` + `validate_manifest` 通过，已导入成功 |
| `mi_fu_swimsuit151.7z`（弭弗） | `chr_0031_mifu` | 8/11 组件、4 贴图，部分 |
| `nom_liino...rar`（梨诺） | `chr_0035_liino` | catalog 缺 hash，已判定不可交付 |

## 已知限制

- EFF Format 3（`EFF_AUTHOR_FILE3`）还不支持；识别到会拒绝，不硬转。
- EFMI 只支持标准 ComponentN；EFMI 选项/热键/多外观还没有映射到 BEM option_groups。
- 合并不住的组件会 `keep`，不猜；catalog 缺 hash 的包不输出。
- 二进制 payload 未压缩；大包会到几十 MB。
- 只用 BEN 的读取器/校验器对拍，未替换 BEN 自己的转换器；实机渲染仍需作者验证。

## 路线

1. `inspect`：先报告角色/组件/骨骼/贴图/catalog 缺口，不达标不出包
2. EFF Format 3 解析（Unity SerializedFile + author resource）
3. EFMI 选项 → BEM 1.1 option_groups
4. BEN catalog 版本适配 + 更多真实包回归
5. 如果 BEN 愿意，作为上游 PR 或独立工具并给 BEN

## 许可

**AGPL-3.0-only**（见 `LICENSE`）。cJSON 为 MIT（见 `NOTICE`）。

这个项目与 BEN 官方、游戏开发商/发行商无关联；只做格式迁移，不包含游戏资源或反作弊绕过。
