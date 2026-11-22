# 交付验收

一个包只有同时满足以下条件才算“可交付”：

1. `c4ben convert` 成功，输出 `.bem`。
2. BEN `read_package()` 能读、`validate_manifest()` 不报错。
3. 组件命中率足够：不能出现“关键部件缺失但静默 keep”而不报告。
4. 在 BEN 管理器中实际导入成功。
5. 记录：目标角色、catalog revision、命中组件/贴图、keep 的组件、已知缺口。

不满足就不给包，只给报告。
