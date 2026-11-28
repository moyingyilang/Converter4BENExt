# `texture_slots` 设备 smoke test 清单

> 目的：把 `--texture-slots` 从「格式正确、双校验器通过」推进到「游戏内可验证」。
> 背景：BEN 运行时确实消费 `texture_slots`（`native/modules/custom_model/bem.cpp` 解析槽表，
> `module.cpp` 的 `ApplyTextureMask` 按 `original_name` 匹配材质纹理槽），
> 但匹配是**逐字**的，对不上会报 `Texture name pin missing`。
> 当前该特性**默认关闭**，验证通过后才考虑改默认。

## 1. 准备

| 项 | 要求 |
|---|---|
| 包 | EFF 源，转换后 `minor=5`（1.5 才会走槽路径） |
| 角色 | catalog `textures` 非空（**不要用 `chr_0037_minghe` / `chr_0040_sweetpie`**，两者均 0 条） |
| 命令 | `c4ben convert <mod-dir> --catalog <cat> --texture-slots --out out.bem` |

转换期自检（已内建）：

- 正常应打印 `texture_slots: N 个槽（按属性名）`；
- 若槽的 `original_name` 不在 catalog `textures` 里，会打印
  `texture slot warning: 槽 <id> 的 original_name "..." 不在 catalog textures 里，运行时可能报 Texture name pin missing`
  —— **出现这条就不要进设备测试，先修 catalog 或贴图身份**。

## 2. 导入前的最小核对

1. manifest 含顶层 `texture_slots`，且 `required_capabilities` 含 `texture-slots`；
2. 每个槽恰好一个候选（当前实现是单候选、无条件成立）；
3. `draws[].textures` 里出现 `{"slot": "<id>"}` 对象；
4. 槽 id 与属性名的对应符合预期，例如：

   ```
   basemap            -> T_actor_..._D   （texture._BaseMap）
   bumpmap            -> T_actor_..._N   （texture._BumpMap）
   metallicglossmap   -> T_actor_..._P   （texture._MetallicGlossMap）
   ```

5. 同一 draw 内各槽替换的原贴图**互不相同**（转换器已按 `original_name` 去重，冲突退回裸整数）。

## 3. 设备验证步骤

按顺序做，每步记录结果：

| # | 操作 | 期望 | 失败含义 |
|---|---|---|---|
| 1 | 导入并进入角色界面 | 模型正常加载、无崩溃 | 包结构问题（与本特性无关） |
| 2 | 查看运行日志 | **没有** `Texture name pin missing` | 有则说明 `original_name` 与游戏材质名不一致 |
| 3 | 观察贴图 | 该 draw 显示的是 mod 提供的贴图（颜色/花纹与源 mod 一致） | 显示原版贴图 = 槽未被应用 |
| 4 | 对比默认输出 | 用**不带** `--texture-slots` 的同一包再导一次，贴图应一致 | 若两者不同，说明槽路径解析有偏差 |
| 5 | 触发外观/选项切换（如包内有） | 贴图按选择正确切换 | 切换无反应 = 槽与选项条件没联动（当前单候选，本不该出现） |

## 4. 失败定位表

| 现象 | 最可能原因 | 处理 |
|---|---|---|
| 日志出现 `Texture name pin missing` | 候选 `original_name` ≠ 游戏原生材质纹理名 | 需要正确 catalog（或 `--identities` 补正贴图名）；不要靠近似名 |
| 模型正常但所有贴图变原版 | 运行时忽略了 `texture_slots`，或槽 id 未被解析 | 确认 BEN 版本包含 `bem.cpp` 的槽解析；退回默认（裸整数） |
| 部分贴图正确、部分丢失 | 冲突去重把某些贴图退回裸整数，而运行时对混合写法处理不同 | 检查该 draw 是否同时有 `{"slot": ...}` 和裸整数 |
| 贴图错位（花纹出现在别的部位） | 槽 id 与属性名映射反了 | 核对 `texture.<prop>` → 槽 id 的净化规则 |

## 5. 记录模板

```
包         :
角色/catalog:
minor      : 5
槽数量     :
游戏版本   :
BEN 版本   :
结果       : PASS / FAIL
日志关键行 :
备注       :
```

## 6. 通过之后

- 在 `docs/COVERAGE.md` §6.1 记录「设备验证通过」的日期与样本；
- 再讨论是否把 `--texture-slots` 改为 EFF 1.5 的默认（改默认需要同时更新
  `tests/texture_slots_test.sh` 的默认断言）；
- 在此之前保持 opt-in：形状正确但不可验证的功能，默认关闭比默认打开安全。
