# 万灵繁生 v1.0.14（Boss 召唤诊断版）

本版本基于 v1.0.13 崩溃隔离工程，仅增加一个可选诊断 Hook，目的是区分闪退发生在 Boss 召唤 Hook 安装/触发阶段，还是后续重复召唤逻辑阶段。

## 本版本行为

- 普通敌怪生成设置沿用 v1.0.13。
- “启用 Boss 召唤诊断”默认关闭。
- 开启后只在 `Player.SummonItemCheck` 前写入 `[BOSS_SUMMON_PROBE]` 日志，然后继续执行原版方法。
- 不重复调用 `SummonItemCheck`，不生成额外 Boss，也不绕过原版 Boss 召唤限制。
- Boss 倍率设置在本诊断版本中不生效。
- 不使用此前导致闪退的 v16 源码。

## 诊断测试

1. 先确认模组关闭 Boss 诊断时可以正常启动并进入世界。
2. 在 TEFManager 设置中开启“启用 Boss 召唤诊断”，重新启动游戏并进入测试世界。
3. 等待约 10 秒，使用一次 Boss 召唤物，不要连续多次点击。
4. 若游戏闪退，导出 TEFManager 日志；检查是否出现 `[BOSS_SUMMON_PROBE] ... callback reached`。
5. 若未闪退，也导出日志，确认回调是否被触发以及调用次数变化。

判读：启动时 `hook=installed` 仅表示 Hook 安装成功；`callback reached` 表示执行已进入诊断回调。此版本不执行任何额外召唤。具体崩溃位置仍需结合崩溃日志判断。

## GitHub 编译

将此目录作为仓库根目录推送到 GitHub，Actions 会构建 Android `arm64-v8a` 并生成 `MyriadLife-android-arm64.zip`。也可在 Actions 页面手动运行 `Build Android ARM64 Mod`。

## 版本

- `version`: `1.0.14`
- `versionCode`: `202609282`
- Android ABI: `arm64-v8a`
