# 万灵繁生 v1.1.3（Boss Hook 查找诊断版）

Android ARM64 / Terraria 1.4.5.8.x / TEFKernel 工程。

## Boss 召唤实现

此版本继续按“解除 Boss 召唤检查，再重复走原版召唤物入口”的路线实现，参考用户提供的“强制召唤”ARM64 模组。对该模组二进制的静态检查确认其目标包含 `Player.SummonItemCheck`、`Player.ItemCheck_CheckCanUse_Inner` 和 `Item.type`。本工程重用这些目标名，未复用其二进制代码，也未读取 MyriadLife-v16 源码。

最新日志显示 Boss 目标方法的 Hook 注册数量与预期不符，但没有记录目标名称。v1.1.3 改为按方法名获取目标，再记录两种方法的查找结果、参数数和 Hook ID；重复调用日志也记录实际识别的物品与调用次数，便于根据下一份运行日志定位。

- `ItemCheck_CheckCanUse_Inner` 只对识别出的 Boss 召唤物放行。
- 原版 `SummonItemCheck` 正常执行后，通过同一入口按倍率再次触发。
- 重入保护只包住额外触发，避免重复调用不断递归。
- 默认总数为 2；可在 TEFManager 设置中关闭或调整为 1–5。
- 不 Hook `NPC.AI`、`NPC.SpawnBoss` 或 Boss 状态机。

识别列表：Suspicious Looking Eye、Worm Food、Mechanical Skull、Mechanical Worm、Mechanical Eye、Slime Crown、Abeemination、Lihzahrd Power Cell、Bloody Spine、Celestial Sigil、Gelatin Crystal、Deer Thing。

## 测试顺序

1. 用测试世界或存档副本启动，确认模组加载。
2. 场上没有 Boss 时，用一种已识别的召唤物召唤一次，预期出现设定数量。
3. Boss 已存在时，再使用同种召唤物，检查限制是否解除以及总数是否按倍率增加。
4. 分别验证一次普通召唤、已有 Boss 时再次召唤，以及倍率设为 1 的情况。
5. 若闪退，导出 TEFManager 日志；重点关注 Hook 初始化、`Boss召唤翻倍`、`Boss召唤物重复触发失败`。

本地完成 C 源码语法和链接检查、JSON 与 Actions YAML 校验。当前环境未安装 Android NDK，ARM64 动态库需由 GitHub Actions 工作流构建。

## GitHub 编译

将本目录内容放在仓库根目录并推送到 GitHub，或在 Actions 页面手动运行 `Build Android ARM64 Mod`。工作流产物为 `MyriadLife-android-arm64.zip`。

## 版本

- `version`: `1.1.3`
- `versionCode`: `202609287`
- Android ABI: `arm64-v8a`
