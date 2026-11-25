# LittleTips 项目状态

更新时间：2026-11-25 13:05 (+08:00)

## 路径

- 仓库：`E:\Codes\c++\LittleTips`
- 远端：`https://github.com/AnctSinps01/LittleTips.git`（origin，当前分支 `main`）
- 部署位置：`D:\self_designed_exes\LittleTips.exe`（手工拷贝，仓库内无安装脚本）
- 运行时数据：`%LOCALAPPDATA%\LittleTips\`（`note.txt` / `settings.ini` / `startup.log`）

## 已验证事实

### 工具链（本机实查）

- cmake 4.0.3
- MinGW：g++ 15.1.0，`D:\MinGW\mingw64\bin`
- MSVC：Visual Studio 2022 **Build Tools** 17.14.36705.20，工具集 14.44.35207，
  路径 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
- Windows SDK：10.0.26100.0，装在 **`D:\Windows Kits\10`**（非默认 C 盘路径）
- `cl.exe` 不在 PATH，直接 `Get-Command cl.exe` 查不到；需 `vcvars64.bat`
  或 `cmake -G "Visual Studio 17 2022" -A x64`
- 上一轮"本机没有 MSVC"的结论是错的：查错了路径（在 `Program Files` 下找的是 IDE，
  实际装的是 `Program Files (x86)` 下的 Build Tools）。

### 构建（2026-11-25 实测）

- MinGW：`cmake --build build-mingw --clean-first` → exit 0，**0 warning**
  （`-Wall -Wextra -Wpedantic`）
- MSVC：`cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64` +
  `cmake --build build-msvc --config Release` → exit 0，**0 warning / 0 error**（`/W4`）
- 产物：`build-mingw\LittleTips.exe` 1,523,712 B；`build-msvc\Release\LittleTips.exe` 208,896 B。
  两次 clean 构建的 SHA256 不一样（PE 头里带构建时间戳），所以这里只记体积，
  要核对某一次构建就现场 `Get-FileHash`
- 两个 exe 都嵌入了 `app.manifest`（含 `PerMonitorV2`），已用二进制内文本核实
- MSVC 那一侧在修 `/MANIFEST:NO` 之前是**构建失败**的（CVT1100 资源重复 + LNK1123），
  即 README 里写的 MSVC 路径此前在本仓库走不通 —— 现在通了

### 运行态（2026-11-25 13:03 起）

- 已部署新构建：`D:\self_designed_exes\LittleTips.exe` = `build-mingw\LittleTips.exe`，
  SHA256 `93EE5321…57F0F4A8`，1,523,712 B
- 选 MinGW 版部署的原因：它只依赖系统 DLL + UCRT；MSVC 版还要
  `MSVCP140.dll` / `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll`（VC 运行库）
- 运行实例 PID 2176，13:03:40 启动，窗口类 `LittleTipsWindow` 可见（hwnd 459554）
- 换 exe 前必须先关旧实例（exe 被运行中的镜像锁住，覆盖会报
  "正由另一进程使用"）。用 `WM_CLOSE` 优雅关闭即可，会走 `WM_DESTROY` 保存，
  `note.txt` 时间戳随之更新（13:03:36），内容未丢
- 单实例修复实测生效：13:03:47 重复启动写入了 `already running` 行并以 exit 0 退出
- 自启：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LittleTips`
  = `"D:\self_designed_exes\LittleTips.exe"`
- 替换前的旧部署（`B5E32D15…D2267145`，即清理前构建）备份在
  `%TEMP%\lt-deployed-old.exe`
- `settings.ini` 里 `topmost=1` / `avoidMouse=1` 是提交 `38ea407` 遗留的孤儿键，
  当前源码既不读也不写（`WriteSetting` 只写 x/y/width/height），
  `WritePrivateProfileStringW` 会原样保留它们

### 本轮改动（已提交，`5352aad` 之后的 3 个代码 commit）

| commit | 内容 |
|---|---|
| `a4da3f9` 修复单实例误判；启动与重复启动写入 startup.log | 上一轮遗留的改动，不在下面清单内 |
| `31c6471` 修复 MSVC 构建 | 第 6 项（objbase.h）+ 第 7 项（`/MANIFEST:NO`） |
| `0f154ec` 清理死代码与冗余 include | 第 1–5 项 |

`src/main.cpp`：

1. 删 `#include <vector>`（全文无使用）—— `0f154ec`
2. 删空 `case WM_HOTKEY`（全工程 0 处 `RegisterHotKey`，不可达）—— `0f154ec`
3. 删 `case WM_KEYDOWN` 的 Ctrl+E 分支（消息循环在 dispatch 前已截获，不可达），
   并在循环处加注释说明 Ctrl+E 只在这一处处理 —— `0f154ec`
4. 菜单 ID `1001/1003/1005` → `1001/1002/1003`（1002/1004 是已删菜单项的空号）—— `0f154ec`
5. 3 处聚合体初始化改两段式（`MONITORINFO` ×2、`WNDCLASSEXW`），
   消除全部 17 条 `-Wmissing-field-initializers` —— `0f154ec`
6. 补 `#include <objbase.h>`：`WIN32_LEAN_AND_MEAN` 会让 MSVC 的 `gdiplus.h`
   缺 `STDMETHOD` / `IStream`，原本 MSVC 编译不过 —— `31c6471`

`CMakeLists.txt`：

7. MSVC 分支 `/MANIFEST:EMBED` → `/MANIFEST:NO`：`src/resources.rc` 已把
   `app.manifest` 嵌成资源 1，链接器再嵌一份会 CVT1100 资源重复 —— `31c6471`

注：`AddTrayIcon` 那处格式错位只存在于未提交的临时状态，重放后与 `HEAD` 一致，
没有进入任何 commit。

## 未决问题

1. **交互功能未人工验证**：新构建已跑起来，启动、窗口显示、单实例都已实测；
   但菜单（编辑模式/开机自启/退出）、`Ctrl+E`、躲避鼠标这几个需要人手操作，
   2026-11-25 用户说"验证等会再说"，尚未确认
2. `settings.ini` 的 `topmost` / `avoidMouse` 孤儿键未删（属用户数据文件，不是代码）
3. `SaveWindowPlacement` 仍写 `width` / `height`，但窗口启动后立刻被
   `ResizeWindowToFitText` 覆盖，这两个值只在"创建瞬间"起作用 —— 是否保留待定
4. `InitCommonControls()` 是没有 common control 时机的遗留 no-op 调用，未删
5. 部署靠手工拷贝 exe 到 `D:\self_designed_exes\`，无脚本；换 exe 前必须先关旧实例
6. MSVC 版构建能用但没部署（比 MinGW 版小很多，208,896 B vs 1,523,712 B，
   代价是依赖 VC 运行库）—— 要不要改用 MSVC 版待定
7. MSVC 构建日志里有一条 `'pwsh.exe' 不是内部或外部命令`（cmd 报错），
   不影响构建结果（exit 0、产物正确），来源未查明
