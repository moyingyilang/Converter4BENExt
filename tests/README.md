# 测试

本目录不保存游戏资源或 mod 包。

手工回归：

1. 用你自己的 EFMI 包解压出目录（含 mod.ini）。
2. BEN catalog 放到本地目录。
3. `./build/c4ben convert <mod目录> --catalog <catalog> --out /tmp/x.bem`
4. 用 BEN 的 `read_package` / `validate_manifest` 校验。
5. 把结论（角色/组件命中/贴图/keep 项）写进 docs/ACCEPTANCE.md 的记录表。

已知回归样本：
- edmin_maid -> chr_0003_endminf：可交付
- mi_fu -> chr_0031_mifu：部分
- nom_liino -> chr_0035_liino：不可交付（catalog 缺 hash）
