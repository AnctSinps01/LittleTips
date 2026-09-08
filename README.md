# LittleTips

一个只依赖 Windows 原生 Win32 API 的轻量桌面便签。没有运行时框架或网络访问。

## 功能

- 普通窗口 / 始终置顶切换
- 带阻尼、边缘反弹和墙面摩擦的鼠标自动躲避
- 只读查看 / 编辑模式切换
- 当前用户开机自启
- 自动保存 UTF-8 文本、窗口位置、尺寸与置顶状态
- 托盘常驻，单击托盘图标可显示便签，关闭窗口时隐藏，托盘菜单可彻底退出
- 单实例运行和多显示器 DPI 适配

右键便签或托盘图标打开菜单，可开关“自动躲避鼠标”。躲避功能会在编辑、拖动、缩放或隐藏便签时暂停。只读模式下拖动便签内容可移动窗口，拖动窗口边缘可调整尺寸。快捷键 `Ctrl+E` 切换编辑，`Ctrl+T` 切换置顶，`Ctrl+R` 切换自动躲避，`Ctrl+W` 隐藏/显示便签，`Ctrl+Shift+Q` 退出。

数据保存在 `%LOCALAPPDATA%\LittleTips`。

## 构建

Visual Studio 2022：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成文件位于 `build\Release\LittleTips.exe`。也支持 MinGW：

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```
