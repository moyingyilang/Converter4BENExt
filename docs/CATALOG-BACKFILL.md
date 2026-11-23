# Catalog 回填（VFS -> BEN catalog 候选）

目标：用 ForTMR 的 VFS 解包能力，从**旧版本游戏包**恢复 BEN catalog 需要的原生资料，
给 Converter4BENeXt 提供旧版本的角色/组件/贴图身份，让旧 EFMI 包也能识别。

不直接发布成 BEN 官方 catalog；输出标记 `candidate`，交 BEN 作者复核。

## 输入

- 游戏 VFS：`.blc` / `.ab` / 资源目录，必须和 mod 同一游戏版本
- 版本信息：manifest / version code / 平台（Android arm64 / x86_64）
- mod 的 EFMI 包（只用于回归，不作为 catalog 证据）

## 两阶段

### 1. 原生资料（可离线）

从 Unity SerializedFile 读：

- Mesh：name、vertex count、index count、submesh、骨骼路径/顺序、顶点布局
- Material：名称、槽位顺序、贴图引用
- Texture2D：名称、尺寸、格式、mip、sRGB、mip 数据
- 角色 prefab：组件 mesh 列表、world/UI 资源根、LOD0 过滤

输出 `candidate/components`，先和当前 BEN catalog 对比字段是否一致。

### 2. EFMI 身份（必须精确）

- Component hash：原始 index bytes 在绘制区域的 **CRC32C**，不包含 Buffer 描述符；
  只接受 first offset=0、base vertex=0、完整覆盖单 submesh 的情况。
- Texture hash：`texture_hash=0` 数据算法 + 44 字节 `D3D11_TEXTURE2D_DESC`；
  完整 mip 链、typed format、单层 Texture2D、DEFAULT usage、SRV bind。
- 不允许用文件名/组件号/顶点数猜身份；不确定的组件标 `excluded`。

## 输出

```json
{
  "kind": "bem-character-catalog",
  "status": "candidate",
  "game": { "platform": "android-arm64", "manifest": "...", "version": "..." },
  "components": { "...": { "mesh_name": "...", "original_index_count": 0,
                           "bone_names": [], "materials": [], "attributes": [] } },
  "entries": { "<crc32c>": 0 },
  "textures": { "<texture-hash>": { "name": "T_..." } }
}
```

## 旧 APK 需求

- 和 mod 同版本的 **完整安装包**（split APK + OBB 也可以，必需 assets）
- 版本号/构建日期；能拿到 `version.txt`/manifest 最好
- 不需要登录、不需要运行游戏；只要资源文件
- 不同版本的 VFS 密钥/版本号可能不同，ForTMR 的 blc 解析需要适配

## 验收

1. 当前版本：任选一个角色，生成的 `components` 与 BEN catalog 字段逐项一致
2. 旧版本：zhuangfangyi 的 11 个组件 hash 能命中候选 catalog
3. 用候选 catalog 转出 zhuangfangyi BEM，BEN `read_package` 通过
4. 全程不导出/分发游戏几何或纹理像素；catalog 只保留元数据
