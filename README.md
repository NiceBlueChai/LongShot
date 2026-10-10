# LongShot

一个轻量的 Windows 截图工具。托盘常驻，框选后直接标注，完成自动复制，也支持向下长截图。

## 下载

前往 [Releases](https://github.com/NiceBlueChai/LongShot/releases/latest)，下载名称含 `minimal` 的 ZIP，解压后运行 `LongShot.exe`。

包内附带 `vc_redist.x64.exe`，缺少 VC++ 运行库时安装即可。升级前请保存图片并退出旧版托盘程序。

## 使用

1. 单击托盘图标或按截图快捷键，开始截图。快捷键可在托盘“设置”中选择。
2. 鼠标悬停自动选中窗口或控件，单击确认；也可以直接拖动框选。拖动边框调整大小。
3. 选择矩形、箭头、文字或马赛克，直接在选区内标注。
4. 点击“完成”复制图片并收起；点击“保存”导出 PNG。按 `Esc` 取消。

**长截图：** 框选后点击“长截图”，确认绿色滚动区域，再点“开始”。鼠标移出绿色区域暂停，移回继续；按 `Q` 或点击“完成”结束采集。结果支持缩放、拖动查看和标注。

常用操作：

- 自动选区：`Tab` 切换手工框选，滚轮切换父／子区域。
- 放大镜：`C` 复制颜色，`Shift` 切换 RGB／HEX。
- 长图编辑：滚轮缩放，空格拖动，`Ctrl+Z` 撤销，`Ctrl+Y` 重做。

目前每次截图限定在一个显示器内，不支持跨屏拖选。长截图仍在完善，重复内容和动态页面可能导致拼接失败；停止时保留已确认的内容。

## 构建

需要 Windows、MSVC x64、Qt 5.12.9（Widgets / Test）和 CMake 3.16+。

在 Visual Studio x64 开发者命令提示符中执行，将 Qt 路径替换为自己的安装位置：

```bat
set "QT_ROOT=D:\Qt\Qt5.12.9\5.12.9\msvc2017_64"
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_ROOT%"
cmake --build build
"%QT_ROOT%\bin\windeployqt.exe" --release --no-translations build\LongShot.exe
```

构建后运行 `build\LongShot.exe`。功能路线图见 [TODO.md](TODO.md)。

## 许可证

[GPL-3.0-only](LICENSE)。Qt、Microsoft VC++ 运行库等第三方组件保留各自许可证。
