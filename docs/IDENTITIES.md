# 用 `--identities` 补旧包身份（texture / component）

`--identities` 只补「身份」，不生成布局；适合旧版 hash 漂移、catalog 缺 `entries`/`textures` 的包。

## 场景：SI 的 4 张无身份贴图

当前 catalog（`catalog-upstream-final` / `catalog-entries`）下，SI 包有 4 个贴图 hash 找不到身份：

```text
908140d5 / 7d330dbd / 6376c672 / b115da91
```

转换时现在会明确打印：

```text
texture skip: 908140d5 has no catalog identity (retry with --texture-fingerprint)
...
```

`--texture-fingerprint` 也匹配不上，说明 catalog 的宽/高/mip 指纹不唯一。需要外部提供「hash → 游戏原生贴图名」。

## identities JSON

```json
{
  "schema": 1,
  "characters": {
    "chr_0036_jsspsi": {
      "entries": {
        "<EFMI component hash>": 3
      },
      "textures": {
        "908140d5": "T_actor_jsspsi_<...>",
        "7d330dbd": "T_actor_jsspsi_<...>",
        "6376c672": "T_actor_jsspsi_<...>",
        "b115da91": "T_actor_jsspsi_<...>"
      }
    }
  }
}
```

规则：

- `textures`：8 位小写 hash → catalog `textures` 里的 `name`（必须逐字等于游戏原生贴图名）；
- `entries`：EFMI component hash → catalog component id；
- 名字不能编：优先从 catalog `textures`、作者资料或 VFS/运行时导出取；
- 同名冲突会直接报错；重复相同值会跳过。

## 转换

```bash
./build/c4ben convert .work/bem-mods3/732494_si_v10_ca9ce.7z \
  --catalog .work/runtime-catalog/catalog-entries \
  --auto-entries \
  --identities identities.json \
  --out si.bem
```

预期：

```text
texture skip 数量：4 -> 0
textures=35（31 已有 + 4 新补）
native be-bem validate OK
```

如果补完仍有 skip，说明名字不在 catalog 或不是游戏原生名；保留日志，不要静默改 name。

## 合成验证的边界

可以造一份「假设名字」的 identities 只验证流程（skip 4→0、manifest texture 数 +4），但**不能**作为真实数据证明；真实运行仍要求 `original_name` 命中游戏材质纹理槽，否则会 `Texture name pin missing`。
