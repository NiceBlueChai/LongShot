# 发布

运行 `release.cmd`，从最新源码编译并生成 `dist/releases/LongShot-版本-minimal-时间戳.zip` 和 `.sha256`。
脚本不运行桌面测试，不生成额外源码压缩包，也不改写仓库中的许可目录。

## 版本

`CMakeLists.txt` 的 `project(LongShot VERSION ...)` 是版本来源。
程序关于窗口、EXE 文件属性和运行包名称使用同一版本；GitHub Release 标签使用 `v版本`。
发布前提交并推送，标签指向实际参与构建的提交。

## 运行包

- LongShot.exe、Qt Core/Gui/Widgets/Svg 及必要平台、样式和图片插件。
- `ocr/`：PP-OCRv5 检测和识别模型、CPU ONNX Runtime、对应许可及下载来源。
- 微软原版 `vc_redist.x64.exe`，需要时由用户安装。
- 项目及第三方许可说明、文件 SHA-256 清单。

不包含测试程序、调试符号、历史 EXE、PixPin 文件或 Qt 源码。
项目源码使用 GitHub 标签自动生成的源码归档，不另上传重复的源码 ZIP。

## 核查

打包器核对 Qt 文件来源、VC 安装包哈希和 ZIP 完整性；OCR 下载脚本固定版本并核对 SHA-256。
发布前检查 ZIP 内 EXE 与本次构建一致、OCR 和 VC 安装包齐全，并运行受影响功能的检查。
升级时先保存图片并退出旧版，再解压新版，保留 `ocr` 目录。
