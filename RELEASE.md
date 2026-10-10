# LongShot 发布与源码说明

## 默认最小运行包

`release.cmd` 现在默认生成 `LongShot-0.1.0-minimal-时间戳.zip`，不含源码、测试或历史 EXE；附带微软 VC++ x64 运行库安装器。
保留 Qt Core/Gui/Widgets、Windows 平台、Vista 样式、ICO/JPEG 插件和许可文件；PNG 支持在 Qt Gui 内。
去掉 ANGLE、SVG、TIFF、WebP 等可选模块和插件，当前 Widgets 截图编辑流程不使用 OpenGL。
微软运行库安装器 vc_redist.x64.exe（14.51.36247.0）已随包提供，缺少或过旧时手动安装。源码在发布页面作为独立下载提供，原有完整包作为归档保留。
打包时同步生成独立的 `LongShot-0.1.0-source-时间戳.zip`，包含本次应用源码；不增加运行包体积，Qt 源码仍复用对应独立归档。
此最小包只核对文件与归档，未做桌面启动验收。

## 可选完整包

需要完整源码随包时，运行 `python tools/package_release.py --include-sources`。
以下章节说明完整包的内容和供源方式；不代表默认最小包也包含源码。

## 可交付包

执行 `release.cmd` 只编译、核对依赖并打包，不运行程序、测试或桌面自动化；默认最小模式。
完整模式输出到 `dist/releases/LongShot-0.1.0-full-时间戳.zip` 及同名 `.sha256`。解压后运行 LongShot.exe。
请分发这个完整 ZIP，不要把开发用 dist 目录或其中的单独 EXE 当作完整发布包。

包内包括：

- LongShot.exe 与实际使用的 18 个 Qt DLL／插件。
- LICENSE、THIRD_PARTY_NOTICES.md 和 licenses：项目 GPLv3、Qt 许可、第三方许可及版权声明。
- sources/LongShot-source.zip：本次发布时的应用代码、测试代码、图标资源、构建及打包脚本、文档。
- sources/Qt-5.12.9-source.zip：本机 Qt 5.12.9 安装提供的 qtbase、qtsvg、qtimageformats 完整模块源码（包括内嵌第三方代码、模块构建文件），以及 Qt 根目录构建文件。
- sources/qt-build-config：对应安装的 Qt 功能与构建配置，供核对和重建参考。
- MANIFEST.json：实际 DLL 到源码模块的映射、Qt 源码 tag、各文件 SHA256 和核查范围。

对应源码直接随二进制提供，不以未来的书面供源承诺代替，也不要求用户另外向作者索取。
如另行拆分下载，必须在二进制同一发布位置提供这两个源码包及构建说明，且不额外收费。
原始 Qt 源码保留各文件许可；附带源码中的未编译平台、示例、测试文件不意味着它们都被链接进 Windows 程序。

## 依赖核对与许可选择

| 组件 | 发布文件 | 来源／许可处理 |
|---|---|---|
| LongShot | LongShot.exe、项目源码及自有图标 | GPL-3.0-only；源码包含本轮修改 |
| Qt Base 5.12.9 | Core、Gui、Widgets、Windows 平台、Vista 样式、GIF/ICO/JPEG 插件 | 模块代码选择 GPLv3 授权选项；嵌入第三方代码按原许可 |
| Qt SVG 5.12.9 | Qt5Svg、SVG 图像与图标插件 | 模块代码选择 GPLv3 授权选项；保留 XSVG 声明 |
| Qt Image Formats 5.12.9 | ICNS、TGA、TIFF、WBMP、WebP 插件 | 模块代码选择 GPLv3 授权选项；libtiff、libwebp 原许可及专利文本随包 |
| ANGLE | libEGL、libGLESV2 | 来自 Qt Base，保留 BSD 及其子组件声明 |
| Windows／MSVC | 系统 DLL、C++ 运行库 | 外部系统运行前提；Windows 系统 DLL 不随包；微软原装 vc_redist.x64.exe 按其自身许可分发，不属于 GPL 项目代码 |

打包器会比较每个部署 Qt DLL 与指定 Qt 安装文件的 SHA256，并检查源码模块与安装配置均为 5.12.9。
Qt5Core.dll 的 qt_prfxpath 字段由 windeployqt 从安装路径改为 `.`，其余字节须与安装文件完全一致；MANIFEST 同时记录原始／部署哈希和该修改。
出现未知 DLL、缺失许可文件、除上述可复现补丁外的来源不一致、源码版本不符或应用源码比 EXE 新时中止，不静默生成“已审计”包。
qt_attribution.json 中列出的第三方归属均汇总到 THIRD_PARTY_NOTICES.md；引用的许可证原文复制到 licenses。
IJG README、FreeType 许可全文、WebP PATENTS/AUTHORS 额外附带；其余逐文件声明保留在完整模块源码内。
此清单覆盖随包模块的源码声明，其中包含非 Windows、测试或工具条目，不把它们全部声称为运行时依赖。

This software is based in part on the work of the Independent JPEG Group.
Portions of this software are copyright © 2006 The FreeType Project (www.freetype.org). All rights reserved.

随包运行库安装器版本为 14.51.36247.0，已核对微软有效签名及与 Visual Studio 原装文件一致的 SHA256；打包脚本锁定该哈希，更新安装器时须重新核对。安装器不自动执行，条款由其安装界面提供。官方获取地址：
https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist
https://aka.ms/vc14/vc_redist.x64.exe
Windows API／UI Automation／MSAA 使用操作系统提供的库，不把系统 DLL 复制进包。

## 重建应用

解压 sources/LongShot-source.zip，安装 CMake、MSVC x64、Qt 5.12.9 开源版。
在已运行 vcvars64.bat 的命令提示符中，进入 LongShot 源码根目录：

```bat
set "QT_ROOT=D:\Qt\Qt5.12.9\5.12.9\msvc2017_64"
set "PATH=%QT_ROOT%\bin;%PATH%"
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_ROOT%"
cmake --build build
```

这里编译但不执行测试。原先 build.cmd 包含测试，应按 README 中说明区别使用。
测试源码仍提供给接收者自由运行。运行库 DLL 可以用兼容的自编译版本替换；应用不检查或锁定 Qt DLL 的签名。

## 重建 Qt 模块

解压 Qt-5.12.9-source.zip。qtbase 包含 qmake 与基础工具源码，qtsvg 与 qtimageformats 使用该 qmake 构建。
原装 Qt DLL 的配置记录指向 MSVC 2017 系列工具集；建议用兼容的 MSVC 2017 x64 环境构建 Qt 5.12.9，
准备该版本 Qt 要求的 Perl、Python 等构建工具，具体依赖与参数参见模块自带 configure 帮助及配置文件。
以下是生成兼容共享库的示例，路径替换为解压位置；不是对官方 DLL 的逐字节复现声明。

```bat
mkdir qtbase-build
cd qtbase-build
..\Qt-5.12.9\qtbase\configure.bat -opensource -confirm-license -shared -release -platform win32-msvc -opengl dynamic -nomake examples -nomake tests -prefix D:\Qt-rebuilt
nmake
nmake install
cd ..
mkdir qtsvg-build
cd qtsvg-build
D:\Qt-rebuilt\bin\qmake.exe ..\Qt-5.12.9\qtsvg\qtsvg.pro
nmake
nmake install
cd ..
mkdir qtimageformats-build
cd qtimageformats-build
D:\Qt-rebuilt\bin\qmake.exe ..\Qt-5.12.9\qtimageformats\qtimageformats.pro
nmake
nmake install
```

Qt5Core 的相对路径修补可用上游 windeployqt，或源码中 `tools/package_release.py` 的 `relocated_core(data)` 函数复现；该函数同时用于逐字节核查本包补丁。修改仅为路径数据，未修改 Qt 算法代码。

LongShot 本身仍使用 CMake；上述 qmake 仅用于上游 Qt 模块自己的构建系统。
Qt 的官方源码入口：https://download.qt.io/archive/qt/5.12/5.12.9/ 。包内已经附上对应模块源码，不依赖外链持续可用。

## 后续发布

- 修改代码后重新编译、重新运行 release.cmd，生成新的源码快照和哈希清单；不要把旧源码包配新 EXE。
- 换 Qt 版本、增加 DLL 或依赖时，更新 tools/package_release.py 的版本／文件映射和许可收集，再重新核对。
- 打包器不会强行退出旧 LongShot 进程，不收集用户图片、设置、诊断目录或历史 EXE。
- 已进行的交付核查是来源哈希、版本、许可文件存在性及 ZIP CRC；未运行应用测试或重编整个 Qt。

参考条款：https://www.gnu.org/licenses/gpl-3.0.html （第 6 节）
Qt 官方要求：https://www.qt.io/development/open-source-lgpl-obligations
