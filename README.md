# LittleTips

一个只依赖 Windows 原生 Win32 API 的轻量桌面便签。没有运行时框架或网络访问。

## 功能

- 圆角便签，四周一圈由深到浅的描边
- 始终置顶，鼠标靠近时自动躲避（带阻尼、边缘反弹和墙面摩擦）
- 只读查看 / 编辑模式切换
- 当前用户开机自启
- 自动保存 UTF-8 文本与窗口位置
- 窗口随文字内容自动包络，输入、换行或删除文字时同步调整宽高
- 托盘常驻，关闭便签即退出，托盘菜单也可退出
- 不在任务栏和 Alt+Tab 窗口切换列表中显示
- 单实例运行和多显示器 DPI 适配

右键便签或托盘图标打开菜单：编辑模式、开机自启、退出；快捷键只有 `Ctrl+E` 切换编辑。只读模式下点击便签任意位置即可拖动，窗口尺寸由文字内容自动决定。

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
