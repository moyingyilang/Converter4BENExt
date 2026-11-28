# 来源覆盖矩阵（2026-11-27）

本表只描述**已实现且经过宿主校验**的能力；标注“未支持”的项不是实现细节，而是当前明确的缺口。

## 1. 源格式

| 来源 | 变体 | 状态 | 说明 |
|---|---|---|---|
| EFMI | 标准 `ComponentN` / 新 `EntryPoint_ComponentN` 合并骨架，skin stride **16** | ✅ | 4×UNORM16 权重 + 4×UINT16 全局 id |
| EFMI | 旧 ALPHA，skin stride **12** | ✅ | u16 权重 + u8 本地骨骼索引 |
| EFMI | 合并骨架，skin stride **8** | ✅（L3 推断） | Playbook：4×UINT16 全局 id 的刚性布局，取首个 id；其余 id 仅校验 |
| EFMI | 刚性 skin stride **4** | ✅（L3 推断） | 首个 UINT16 为全局骨骼 id，其余为填充 |
| EFMI | 浮点权重 skin stride **24** | ✅（L3 推断） | 4×FLOAT32 权重（归一化后量化）+ 4×UINT16 全局 id；样本：伊冯 comp10 |
| EFMI | 旧版 hash 不匹配当前 catalog | ✅ | `inspect-legacy` + `--profile`，或一键 `convert --character <chr> --legacy-auto`（自动生成 draft profile 并转换）；仅旧 mod 无法可靠映射时仍会拒绝 |
| EFMI | 旧包 source hash identities | ✅（2026-11-27） | catalog `entry_identity_evidence` 可重建成 `entries`；`--identities` + `--character` 走严格转换，旧 Zhuang 包 9 meshes / `be-bem validate` OK |
| EFMI | 旧布局缺尾部顶点属性 | ✅（显式 `--compat-layout`） | 仅当 source stride **小于** target 时尾部零填充；不删除/不重排；source > target 仍明确拒绝 |
| EFMI | 旧全局骨骼 → 目标局部骨骼 | ✅（`--skeleton` TOS 表） | `avtos` 输出 global_id→bone_name；converter 按 bone name 查目标组件 `bone_names`；dapan TOS 324 根，64/64 catalog bones 命中 |
| EFMI | stride12 本地骨，源骨骼数 ≤ 目标组件骨骼数 | ✅（`--compat-bones` 直映） | 显式直映 source 0..N-1 → target 0..N-1；旧 Zhuang 从 3 提到 8 meshes，`be-bem validate` OK；属于兼容映射，需人工复核动画 |
| EFMI | 选项/热键/多外观 | 🟡 | 已检测 `KeySwap`/`KeyToggle` 并打印组/选项（样本 SI V1.0：5 组）；BEM 1.1 option_groups 尚未映射，转换仍是固定单外观 |
| EFMI | 新贴图段名 `Resource_Texture_<hash>` + `TextureOverride_Texture_<hash>` | ✅ | 旧 `TextureNNN` 与新 hash 命名都支持（样本 SI V1.0：贴图 0→31） |
| EFF | Format 3（`EFF_AUTHOR_FILE3` v3, kind=1） | ✅ | Replace/Part/Mesh/Material/Texture |
| EFF | `handling=replace/keep/skip/hide` | ✅ | hide → BEM `operation: hide`，skip/keep → `keep` |
| EFF | `.effres` kind=2 材质参数/颜色/关键字 | ✅ | 解析 `float.*` / `value4.*` / `keyword.*` / `render_queue`，输出 BEM 1.5 `draws[].material_overrides` |
| EFF | `render_queue` / 渲染状态 | ✅ | 输出 `material_overrides.render_queue` + `shader_passes`（来自材质 JSON 的 `disabledShaderPasses`），声明 `material-render-state-v1` |
| EFF | `.effres` 的 `texture.<prop>` 槽位语义 | 🟡 | **只解析、不落到 manifest**：目前贴图进 `draw.textures`（匿名整数引用），未生成 `texture_slots`；缺 `property_id`，见 §6 |
| EFF | 材质名不在 catalog `materials` 里 | ✅ | 打印 `material skip: <asset> not in catalog materials of component <cid> (material_overrides dropped)`，丢弃覆盖而不是回落到 slot 0 |
| EFF | `.effres` 未声明 `render_queue` | ✅ | 不写该字段（`-1` 只是「未设置」哨兵，不当数据发射；见 §6） |
| EFF | 物理/特效/blendshape/LOD | ❌ | 不是 BEM 1.0 可表达范围，需明确声明不支持 |
| EFF | 运行期骨骼重定向 Compute Shader | ❌ | 跨 IB 绘制依赖它；删掉会错绑 |
| EFF/EFMI | 非 catalog 的自定义贴图/材质 | ❌ | 需要 donor 或 catalog 材质表；当前跳过并报错 |
| 3DMigoto 非 EFMI 包 | 纯 `ShaderOverride`/command list | ❌ | 没有网格入口，不在当前迁移范围 |
| EFMI 动态合并骨架 | `Pool_MergedSkeleton*` / `CommandList_MergedSkeleton*`，无静态 Component | ❌ | 依赖运行期池/命令列表拼装，当前明确拒绝；需专门解析器 |

## 2. 纹理

| 输入 | 状态 | 输出 |
|---|---|---|
| DDS BC1 / BC3 / BC4 / BC5 / BC7 | ✅ | 原生 BEM 格式（10/12/26/27/25） |
| DDS BC2 / DXT3 | ✅ | 解码为 RGBA32；加 `--texture-compress` 可重编码为 BC3/BC1（BEM 不接受 BC2） |
| TGA/PNG 的 BC 压缩 | ✅（opt-in `--texture-compress`） | 无 alpha → BC1；有 alpha → BC3；normal-like → BC5（R/G 两个 BC4 通道）；纯标量编码器，arm64 可用 |
| DDS BC5 法线编码 | ✅（opt-in `--texture-compress`） | normal/normalmap/_nm/_n 贴图写 BEM fmt 27，避免 BC1 法线失真 |
| DDS RGBA8 / BGRA8（DXGI 28/87） | ✅ | RGBA32（BGRA 自动换序） |
| DDS BC6H | ❌ | HDR 无法无损装进 8-bit RGBA32；明确拒绝 |
| TGA / PNG / JPG | ✅ | RGBA32，mips=1；sRGB 由槽位/文件名推断 |
| 自定义贴图（catalog 无身份） | 🟡 | `--texture-fingerprint` 可按 catalog descriptor（宽/高/mip）唯一匹配；多候选则跳过；EFMI legacy 与 EFF 路径都生效 |
| DDS cubemap / volume / texture array | ❌ | 明确拒绝（BEM 2D 无法表达），不再静默当 2D 读 |

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

## 3.1 2026-11 新增样本（GameBanana 下载，19 个含 mod.ini）

| 样本 | 类型 | 结果 | 说明 |
|---|---|---|---|
| FemaleEndminThiccMod | EFMI ALPHA-5 | OK（legacy-auto，10 meshes / 21 textures） | 旧 hash 不匹配；`--character chr_0003_endminf --legacy-auto --texture-fingerprint` 自动 profile + descriptor 贴图匹配（遗留 1 个单骨骼组件因源骨骼数 4>目标 1 拒绝） |
| SI V1.0 | EFMI | OK（14 meshes） | 修复 `Resource_Texture_<hash>` 段名解析后通过 |
| Fluorite OG GUI / Avywenna OG GUI | EFMI | OK | 模型包带 GUI |
| Liino VELMORA / Ardelia SR / yif / kay Graffiti Girl | EFMI | OK | 含 keep 组件 |
| 8 个 Kay 分类 mod | EFMI | FAIL | 当前 catalog 没有 Kay 角色 → 需要新 catalog/游戏资源 |
| SI V1.0（新 EFMI 段名） | EFMI | OK | 修复 `Resource_Texture_<hash>` 段名后贴图 0→31，含 `material_overrides` 验证通过 |
| Advanced HUD Control / Dynamic Login Screen | GUI | 明确拒绝 | 不是模型包（无 Component 入口） |

新增健壮性：非模型包给出明确错误；`Resource_Texture_<hash>`、缺 stride 不再抛 `stoi`。

## 3.2 外部 identity 适配（ForTMR/BEN 工具输出）

`convert --identities <json>` 接受外部提取器产出的中性映射，用于 hash 漂移或旧版本：

```json
{
  "schema": 1,
  "characters": {
    "chr_0036_jsspsi": {
      "entries": {"<EFMI component hash>": 3},
      "textures": {"<texture hash>": "T_actor_jsspsi_body_01_D"}
    }
  }
}
```

- 只补 `hash → component` / `hash → texture name`，不生成布局；native layout 不支持仍会明确失败；
- 冲突（同一 hash 指向不同目标）直接拒绝；重复相同值跳过；
- 可配合 `--character` 强制目标角色。

## 3.3 2026-11 第二批样本（GameBanana 43 个含 mod.ini）

| 类别 | 数量 | 结果 |
|---|---|---|
| EFMI 静态 Component 包（含 OG GUI / 角色换装） | 30+ | ✅ 转换 + `be-bem validate` 通过（如 Typhoeus 13 meshes/26 textures、SI 14/34、Yvonne maid 10/16） |
| EFF Format 3（si / lx / chen / tangtang） | 4 | ✅ 转换 + 校验通过 |
| Loot Beacons 子包（POI/宝箱/收集物） | 5 | ❌ 非角色模型包，明确拒绝 |
| GUI/杂项（LZY、gift） | 2 | ❌ 非模型包，明确拒绝 |
| 武器/外观（Khravengger） | 1 | ❌ 当前 catalog 无目标 |
| 动态合并骨架（Si-Series3、Tangtang-Series1） | 2 | ❌ 新识别变体，明确报错（非静态 Component） |

## 4. 仍有缺口（需要更多样本/旧资料）

1. **旧版/新版 catalog**：庄方宜旧包、FemaleEndminThiccMod（EFMI ALPHA-5）以及全部 Kay 分类 mod
   都缺对应 catalog；Kay 是当前 catalog 里没有的角色，需要新游戏资源或 BEN 新版 catalog；
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

## 6. 已知限制（语义未闭环，不是 bug）

### 6.1 `texture.<prop>` 槽位未映射到 `texture_slots`

EFF 材质 `.effres` 里有 `texture._BaseMap=<Texture 段名>` 这类行，转换器目前**只解析**，贴图统一进 `draw.textures`（裸整数 = anonymous 引用），不生成 BEM 1.2+ 的 `texture_slots`。

原因（依据 BEN 侧文档与源码）：

- `texture_slots[].id` 必须是运行时 `material.get_texture_property_ids` 返回的 **int32 属性 ID**；
- catalog 的 `material_texture_properties` 只有 `property`（`_BaseMap` 等）与 `name`，**没有数值 ID**；
- `docs/custom_model/research/1.5.3/runtime-probes/CUSTOM_MODEL_RUNTIME_PROBE_ZHUANGFANGYI.md:53` 明确要求「保留实际 `property_id`；……不能猜测 ID→名称映射」。

**强行生成只能编造 ID，运行时匹配不上，比不写更糟。** 因此当前选择保持匿名整数引用（结构合法、校验通过），语义留白。

解除条件（任一）：

1. 从 BEN 运行时 / VFS 导出 `material.get_texture_property_ids`（属性名 → int32），catalog 扩展为
   `material_texture_properties: [{property, name, property_id}]`；
2. 或 BEN 侧提供属性 ID 的静态表。

具备数据后再生成 `texture_slots` + `draw.textures` 的 `{slot: <id>}` 命名引用（需要 minor≥2 并声明 `texture-slots`）。

`fixture-eff-overrides` 注入里的 `texture._BaseMap` 只用于验证**解析路径**，不承诺 manifest 语义。

### 6.2 `render_queue` 的 `-1` 不当数据发射

`-1` 是「未设置」哨兵而非有效渲染队列。早期实现因为发射条件写成 `>= -1`，会给任何未声明 `render_queue` 的材质凭空写一条 `render_queue: -1`——结构合法但语义是假的。现在只在源里**显式声明**且取值 `0..5000` 时才写出。

连带影响：`745092_yif.zip` 原本唯一的 override 就是这条伪造值，修复后它正确回落为 BEM 1.0（payloads 32 → 23）。

### 6.3 材质覆盖缺真实样本时的回归手段

真实 mod 里带完整 `float`/`value4`/`keyword`/`render_queue` 的 EFF 材质很少，1.5 路径容易失去回归锚点。用 `fixture-eff-overrides` 给任意 EFF mod 注入两组材质（一组命中 catalog、一组故意不命中）来覆盖：

```bash
c4ben fixture-eff-overrides <eff-mod-dir> --catalog <dir>
ROOT=... C4=C4 CAT=CAT BEM=BEM SAMPLE=... bash tests/eff_overrides_test.sh
```

