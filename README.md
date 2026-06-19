# foo_hemusic

把 [HE-Music](https://github.com/serious-snow/HE-Music) 后端作为"接入式音乐源"接入 **foobar2000 v2**（x64）的组件：登录走 Linux.do OAuth，播放时即时解析直链，UI 用 foobar2000 原生绘制。

> **状态：Phase 0 脚手架。** 目前组件只注册版本信息并在加载时往控制台打印一行日志，尚无业务功能。完整开发计划见 [`foo_hemusic/PLAN.md`](./foo_hemusic/PLAN.md)。

## 构建

**前置：** Windows + Visual Studio 2022（MSVC v143，C++ 桌面开发工作负载；UI 阶段还需 ATL）、CMake ≥ 3.21。

```bash
cmake -S foo_hemusic -B foo_hemusic/build -A x64
cmake --build foo_hemusic/build --config Release
# 产物：foo_hemusic/build/Release/foo_hemusic.dll
```

- **foobar2000 SDK 会自动获取**：CMake 配置时若本地不存在 SDK，会自动从官网下载并解压；存在则用现有的。SDK 从源码编译，无需先手动构建 SDK 的 `.sln`。
- **WTL** 已随仓库一并提供（`wtl/`），供后续 UI 使用。

## 安装到 foobar2000

把 `foo_hemusic.dll` 放进 foobar2000 的用户组件目录，每个组件一个独立子文件夹：

```
<profile>\user-components-x64\foo_hemusic\foo_hemusic.dll
```

开发期可用目录 Junction 指向构建输出，省去每次拷贝（免管理员）：

```cmd
mklink /J "<profile>\user-components-x64\foo_hemusic" "<repo>\foo_hemusic\build\Release"
```

重新编译前需**完全退出 foobar2000**（运行时 DLL 被进程锁定）。

## 目录结构

```
foo_hemusic/        组件源码 + CMakeLists.txt（本项目主体）
wtl/                vendored WTL（构建依赖）
CLAUDE.md           面向 AI 助手的项目约定
PLAN.md             见 foo_hemusic/PLAN.md
```

`foobar2000-SDK-2025-03-07/`（自动下载）与 `HE-Music-Flutter/`（参考实现）不纳入版本控制。

## 许可

- 本项目代码以 **GPL-3.0** 授权，见 [`LICENSE`](./LICENSE)。
- 第三方：foobar2000 SDK 适用其自身许可（`sdk-license.txt`）；WTL 适用 Microsoft Public License (MS-PL)。
