# 设计：EFMI -> BEM

## 流水线

1. **读 mod.ini**：入口 `TextureOverride_EntryPoint_ComponentN` 的 hash、
   `Resource_ComponentN_{VB0,VB1,VB2,IB}`、`TextureOverride_TextureN`。
2. **身份**：用 BEN catalog 的 `entries` 把 EFMI hash 映射到 `character_id` / component；
   用 `textures` 把贴图 hash 映射到 `original_name`。
3. **合并骨架**：各组件本地骨骼表按组件顺序拼接成全局表；VB 的全局骨骼索引
   按 (owner, local) 重映射成 BEM 的本地 palette（u8）。
4. **蒙皮**：VB2 stride16（4×u16 权重 + 4×u16 索引）或 stride8（单个 u16 全局骨骼、
   刚性权重）→ BEM 允许的 12 字节流（UNorm16×4 权重 + UInt8×4 索引）。
5. **贴图**：DDS（DX10/BC1/3/4/5/7）→ BEM texture payload；`original_name` 用 catalog 名。
6. **BEM 写入**：BEM 1.0 固定外观；target 全组件，能替换的 replace，不能替换的 keep；
   写二进制容器（40B header + manifest + payload directory + payloads）。
7. **自检**：用 BEN 的 `read_package` / `validate_manifest` 对拍；失败不交付。

## 不做的事

- 不执行 EFMI 热键、GUI 表达式、自定义 Shader。
- 不猜测 catalog 里不存在的身份。
- 不把 EFF 当 EFMI 硬转。
- 不写游戏资源、不碰反作弊。

## 后续 EFF

EFF `format=3` 是 `EFF_AUTHOR_FILE3` 容器（header + metadata + payload），
mesh payload 是 Unity SerializedFile/AssetBundle 包；需要独立的 author resource
解析和 Unity Mesh 读取，不和 EFMI 混在一起做。
