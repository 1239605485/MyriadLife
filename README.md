# 万灵繁生（MyriadLife）

这是一个 TEFKernel Android ARM64 模组工程（v1.0.12）。Boss 默认总数为 2 倍，最多 10 只。Boss 复制挂接到经过签名校验的 `NPC.SpawnBoss(int, int, int, int, float, float, float, float)` 入口，在原版生成完成后额外调用原生 Boss 生成入口；不再重复玩家侧 `SummonItemCheck`。参考“强制召唤”放行已有 Boss 时的召唤限制。诊断写入模组私有目录的 `myriadlife_diagnostics.log`。

设置界面由 `Info.json` 声明，TEFManager 会把设置保存到模组私有目录的 `config.json`：

```json
{
  "schemaVersion": 1,
  "values": {
    "spawn_multiplier": 5,
    "boss_double": true,
    "boss_multiplier": 2
  }
}
```

## GitHub Actions 编译

仓库自带 `.github/workflows/build.yml`，使用 `android-actions/setup-android@v4` 避免请求已下架的 `tools` 包。推送到 GitHub 后，Actions 会使用 Android NDK 编译 `arm64-v8a`，并生成与 TEFManager 发布包一致的 ZIP：

```text
Info.json
Manifest.json
MyriadLife.json
Resources/lib/libMyriadLife.android.arm64.so
```

本地手动编译时，需要 Android NDK 和 CMake：

```powershell
$ndk = "$env:ANDROID_NDK_HOME"
$cmake = "cmake"
cmake -S . -B build-arm64 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a `
  -DANDROID_PLATFORM=android-21
cmake --build build-arm64 --config Release
```

编译产物 `libMyriadLife.so` 放入发布包时需要改名为：
`Resources/lib/libMyriadLife.android.arm64.so`
