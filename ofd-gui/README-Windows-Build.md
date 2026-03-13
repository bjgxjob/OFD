# ofd-gui Windows 构建说明

## 目标

自动产出 `ofd-gui.exe`（含 Qt 运行时）并打包为 `ofd-gui-win-x64.zip`。

## 推荐方式（无需本机 Windows 编译）

使用 GitHub Actions 工作流：

- 工作流文件：`.github/workflows/build-windows-ofd-gui.yml`
- 触发方式：
  - 推送到 `main/master`
  - 手动触发（workflow_dispatch）

产物位置：

- Actions Artifacts: `ofd-gui-win-x64`
- 压缩包名：`ofd-gui-win-x64.zip`

## 本机 Windows 手工构建（可选）

前置：

- Visual Studio 2022 Build Tools（含 C++）
- CMake >= 3.16
- Ninja
- Qt 6.x（MSVC 版本）

命令（PowerShell）：

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --config Release
windeployqt --release build/windows-release/ofd-gui/Release/ofd-gui.exe
```

