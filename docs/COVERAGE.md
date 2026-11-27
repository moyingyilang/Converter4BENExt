# 来源覆盖矩阵（2026-11-27）

本表只描述**已实现且经过宿主校验**的能力；标注“未支持”的项不是实现细节，而是当前明确的缺口。

## 1. 源格式

| 来源 | 变体 | 状态 | 说明 |
|---|---|---|---|
| EFMI | 标准 `ComponentN` 合并骨架，skin stride **16** | ✅ | 4×UNORM16 权重 + 4×UINT16 全局 id |
| EFMI | 旧 ALPHA，skin stride **12** | ✅ | u16 权重 + u8 本地骨骼索引 |
| EFMI | 合并骨架，skin stride **8** | ✅（L3 推断） | Playbook：4×UINT16 全局 id 的刚性布局，取首个 id；其余 id 仅校验 |
| EFMI | 刚性 skin stride **4** | ✅（L3 推断） | 首个 UINT16 为全局骨骼 id，其余为填充 |
| EFMI | 浮点权重 skin stride **24** | ✅（L3 推断） | 4×FLOAT32 权重（归一化后量化）+ 4×UINT16 全局 id；样本：伊冯 comp10 |
| EFMI | 旧版 hash 不匹配当前 catalog | 🟡 | 走 `inspect-legacy` + `--profile mapping.json`；只有旧 mod 时无法可靠映射（需旧 catalog/资源） |
| EFMI | 选项/热键/多外观 | ❌ | 目前固定单外观；BEM 1.1 option_groups 尚未映射 |
| EFF | Format 3（`EFF_AUTHOR_FILE3` v3, kind=1） | ✅ | Replace/Part/Mesh/Material/Texture |
| EFF | `handling=replace/keep/skip/hide` | ✅ | hide → BEM `operation: hide`，skip/keep → `keep` |
| EFF | `.effres` kind=2 材质参数/颜色/关键字 | ✅ | 解析 `float.*` / `value4.*` / `keyword.*` / `render_queue`，输出 BEM 1.5 `draws[].material_overrides` |
| EFF | `render_queue` / 渲染状态 | ✅ | 输出 BEM 1.5 `material_overrides.render_queue` + `material-render-state-v1`（shader_passes 暂无来源） |
| EFF | 物理/特效/blendshape/LOD | ❌ | 不是 BEM 1.0 可表达范围，需明确声明不支持 |
| EFF | 运行期骨骼重定向 Compute Shader | ❌ | 跨 IB 绘制依赖它；删掉会错绑 |
| EFF/EFMI | 非 catalog 的自定义贴图/材质 | ❌ | 需要 donor 或 catalog 材质表；当前跳过并报错 |
| 3DMigoto 非 EFMI 包 | 纯 `ShaderOverride`/command list | ❌ | 没有网格入口，不在当前迁移范围 |

## 2. 纹理

| 输入 | 状态 | 输出 |
|---|---|---|
| DDS BC1 / BC3 / BC4 / BC5 / BC7 | ✅ | 原生 BEM 格式（10/12/26/27/25） |
| DDS BC2 / DXT3 | ✅ | 解码为 RGBA32（BEM 运行时不接受 BC2） |
| DDS RGBA8 / BGRA8（DXGI 28/87） | ✅ | RGBA32（BGRA 自动换序） |
| DDS BC6H | ❌ | HDR 无法无损装进 8-bit RGBA32；明确拒绝 |
| TGA / PNG / JPG | ✅ | RGBA32，mips=1；sRGB 由槽位/文件名推断 |
| 自定义贴图（catalog 无身份） | ❌ | 跳过并报告 |

## 3. 当前样本覆盖（`tests/coverage.sh`，输出经 `be-bem validate`）

| 源 | 结果 | meshes | textures | skipped(keep) | validate |
|---|---|---|---|---|---|
| edmin | OK | 9 | 18 | 2 | OK |
| Simplified_Fangyi | OK | 9 | 14 | 0 | OK |
| lvt（EFF，含材质覆盖） | OK | 4 | 5 | 0 | OK（v1.5） |
| Mi Fu Swimsuit1.5 | OK | 8 | 4 | 3 | OK |
| NoM_Liino_Secret ServiceV1.4.1(Max) | OK | 9 | 1 | 7 | OK |
| YvonneSwimsuit | OK | 9 | 18 | 2 | OK |
| ZhuangFangyi Ultimate Nayuta Wuwei | FAIL | - | - | - | 旧版 catalog 不匹配 |

`skipped(keep)` 是目标组件保留原模型（不是错误）；`FAIL` 只用于没有任何可用身份锚定的包。

## 4. 仍有缺口（需要更多样本/旧资料）

1. **旧版 catalog/资源**：庄方宜旧包 11 个组件 hash 全不匹配，只有 mod 无法确定身份；
2. **stride 8 的真实样本**：目前只有 playbook 规则，没有实机包；
3. **EFMI 选项/热键包**：需要至少一个带选项的包验证 option_groups 映射；
4. **EFF Format 2 / 其它版本**、武器/多角色/多 target 包；
5. **BC6H/TGA/PNG 贴图**：TGA/PNG 已能转，但还没有真实来源包回归；
6. **EFF 自定义 Liino 贴图**：需要 donor 或 catalog 材质表。

## 5. 复现

```bash
make
BEM_CLI=/path/to/be-bem tests/coverage.sh <mods-root> <BEN-catalog-dir> build/coverage.md
```
