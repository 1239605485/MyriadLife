# 万灵繁生（MyriadLife）

这是一个 TEFKernel Android ARM64 模组工程。默认将 Terraria 的 `defaultMaxSpawns` 提高到原来的 5 倍，并把 `defaultSpawnRate` 降低到原来的五分之一。v1.0.1 增加 Boss 召唤倍率：默认在玩家使用 Boss 召唤物时，通过重复调用玩家的 `SummonItemCheck` 召唤入口生成第二只，并仅在重复调用期间绕过已有 Boss 的可用性检查。

Boss 召唤倍率开关和倍率可在 TEFManager 设置中调整（1–5 倍）。本功能参考了“强制召唤”模组二进制中的符号信息与钩子目标名称；该附件不包含可读源码，因此没有复制其实现代码。需要在 Android ARM64 的 Terraria 1.4.5.x / TEFKernel 环境中验证召唤入口签名和实际游戏表现。

设置界面由 `Info.json` 声明，TEFManager 会把设置保存到模组私有目录的 `config.json`：

```json
{
  "schemaVersion": 1,
  "values": {
    "spawn_multiplier": 5
  }
}
```

## GitHub Actions 编译

这是一个仓库根目录完整、可由 GitHub Actions 编译的工程。把本压缩包解压后，将其中的全部文件和 `.github` 文件夹上传到 GitHub 仓库根目录（不要再套一层 `MyriadLife-main` 文件夹），提交到 `main` 或 `master` 分支。也可以在仓库的 **Actions → Build Android ARM64 Mod → Run workflow** 手动启动。

工作流使用 `setup-android@v4`，避免旧版动作请求 Google 已移除的 `tools` 包；之后安装 Android SDK、NDK 27.2、CMake 3.22.1 和 Ninja，编译 `arm64-v8a`，并检查生成的共享库确实是 AArch64。随后生成适用于 TEFManager 的安装 ZIP：

```text
Info.json
Manifest.json
MyriadLife.json
Resources/lib/libMyriadLife.android.arm64.so
```

编译完成后，在对应运行记录页面底部的 **Artifacts** 下载 `MyriadLife-android-arm64`，解压后即可获得模组安装 ZIP。

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
