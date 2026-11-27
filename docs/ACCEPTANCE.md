# 交付验收

一个包只有同时满足以下条件才算“可交付”：

1. `c4ben convert` 成功，输出 `.bem`。
2. BEN `read_package()` 能读、`validate_manifest()` 不报错。
3. 组件命中率足够：不能出现“关键部件缺失但静默 keep”而不报告。
4. 在 BEN 管理器中实际导入成功。
5. 记录：目标角色、catalog revision、命中组件/贴图、keep 的组件、已知缺口。

不满足就不给包，只给报告。

## 2026-11 实机样本验收记录（GameBanana 下载）

| 批次 | 样本数 | 转换+`be-bem validate` 通过 | 说明 |
|---|---|---|---|
| 首批（用户提供） | 7 | 6 | 旧版庄方宜缺 catalog |
| 第二批 | 19 | 8（含 `--legacy-auto`） | Kay 角色缺 catalog、GUI/POI 明确拒绝 |
| 第三批 | 48 | 37 | EFF3 4 个全过；动态 Pool_MergedSkeleton 2 个明确拒绝 |
| 合计 | 74 | 51 | 其余为缺 catalog / 非模型包 / 新变体边界 |

覆盖通过的代表性来源：

- EFMI 静态 Component（含新 `EntryPoint_Component`、新旧贴图段名、skin stride 4/8/12/16/24）；
- EFF Format 3（Mesh/Material/Texture、UV0–UV7、材质 floats/colors/keywords/render_queue/shader_passes）；
- 旧 EFMI ALPHA（`--legacy-auto --texture-fingerprint`，几何+贴图恢复）。

明确边界（不交付，只报告）：

- 缺 catalog 的角色（Kay）、旧/新版本 hash 且无外部 identity；
- EFMI 动态合并骨架（`Pool_MergedSkeleton` / `CommandList_MergedSkeleton`）；
- EFMI options/热键（已检测，未映射 BEM option_groups）；
- 武器/多 target（缺 catalog）、EFF 物理/特效/blendshape/LOD；
- cubemap/volume/texture array、BC6H HDR。
