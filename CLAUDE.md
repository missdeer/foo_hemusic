# CLAUDE.md — 6-rule

These rules apply to every task in this project unless explicitly overridden.
Bias: caution over speed on non-trivial work. Use judgment on trivial tasks.
Each rule ends with a ❌/✅ pair — match the pattern, not the slogan.

## Rule 0 — Modern CLI only (enforced by PreToolUse hook)
Mappings: `find`→`fd`, `grep`→`rg`, `cat`→`bat` (or Read), `ls`→`eza`, `diff`→`delta`, JSON parsing → `jq` (never `python -c "import json"`).
The hook `.claude/hooks/legacy-cli-pretool.sh` will **deny** any Bash call whose first segment-token is a legacy tool and tell you the replacement — reissue with the modern equivalent, don't retry the same command. `git grep` / `git diff` are fine.
- ❌ `find . -name "*.go" | xargs grep TODO`
- ✅ `fd -e go -x rg TODO`

## Rule 1 — Think, ask, surface conflicts
State assumptions before coding. If two interpretations are both plausible, present them and ask — don't pick silently. If two patterns in the codebase contradict, pick one (more recent / more tested), say why, flag the other for cleanup; never blend them. Use the model only for judgment work (classification, drafting, summarization, extraction); for routing / retries / deterministic transforms, write code — don't ask the model.
- ❌ Picking interpretation A and producing 200 lines of code for it; or writing a retry loop by prompting the model.
- ✅ "I see two readings: A or B. Going with A because X — confirm if you meant B." / Retries live in a `for` loop with explicit backoff.

## Rule 2 — Minimal, surgical, conformant changes
Smallest diff that solves the stated problem. No speculative features, no abstractions for single-use code, no "improvements" to adjacent code / comments / formatting. Match the codebase's existing style even if you disagree — if you genuinely think a convention is harmful, surface it; don't fork silently. Senior-engineer test: would they call this overcomplicated or out-of-scope? If yes, simplify.
- ❌ Bug fix that also renames variables in nearby functions "while we're here", or introduces a `Strategy` interface for one caller.
- ✅ Smallest diff that fixes the bug; new abstraction only when ≥2 real call sites exist.

## Rule 3 — Read before you write
Before adding code: read the relevant exports, immediate callers, shared utilities in `libs/`. "Looks orthogonal" is dangerous — structure usually exists for a reason. Confirm a new helper has a real call site before committing it; `unusedfunc` / `unusedparams` are blocking findings, not advisories.
- ❌ Writing `parseDate()` helper and trusting nothing similar exists.
- ✅ `rg -i 'parseDate|ParseDate' libs/ tools/` first, then either reuse or add.

## Rule 4 — Goal-driven loop
Define success criteria up front, then iterate until verified. Don't follow a fixed step list — strong criteria let you self-correct. For feature work, "compiles" and "`go vet` clean" are not "feature works" — exercise the actual behavior and cite the evidence (command run, output observed).
- ❌ "Done — `go build ./...` passes."
- ✅ "Criteria: import R41 into `dewu-burgeon-sales-daily` for week N. Ran `./bin/...`; row count matches source xlsx (1,234); spot-checked 3 rows against `usage.md` query."

## Rule 5 — Report honestly: checkpoint, fail loud, tests verify intent
**Checkpoint** after each significant step — what's done, what's verified, what's left; if you lose track, stop and restate. **Fail loud** — "completed" is wrong if anything was skipped silently; "tests pass" is wrong if any were skipped or marked `t.Skip`; surface uncertainty, don't hide it. **Tests encode intent**, not just behavior — a test that can't fail when the business rule changes is broken; assert *why* the value matters (the rule), not just *what* it is right now.
- ❌ "All 3 subtasks done!" when subtask 2 silently fell through to a default, or a test that just re-encodes the current return value with no link to the business rule.
- ✅ "2 of 3 done. Subtask 2 hit Y — need your call on Z before continuing." / `assert sale_price == cost * (1 + REQUIRED_MARGIN)` instead of `assert sale_price == 13.75`.

# 工作目录范围

本目录 `D:\Shareware\HEMusic\` 下有四个子目录，性质不同：

| 子目录 | 性质 | 本 CLAUDE.md 是否适用 |
|---|---|---|
| [`foo_hemusic/`](./foo_hemusic/) | **本项目主体**——foobar2000 插件，C++ 源码目录（C++20，CMake 构建） | ✅ 是 |
| [`HE-Music-Flutter/`](./HE-Music-Flutter/) | 参考项目，主要参考其调用 HE-Music 后端 API 的方法；**有自己的 CLAUDE.md** | ❌ 在该目录工作时以其内部 CLAUDE.md 为准 |
| [`foobar2000-SDK-2025-03-07/`](./foobar2000-SDK-2025-03-07/) | 最新 foobar2000 SDK 源码 + 编译出的库二进制，只读参考 | ❌ 不修改 SDK 源码 |
| [`wtl/`](./wtl/) | 微软 WTL 库：foobar2000 SDK 的依赖项，同时也用于 foo_hemusic 的 UI 构建 | ❌ 不修改 |

# 项目状态

**目前仅有 [`foo_hemusic/PLAN.md`](./foo_hemusic/PLAN.md)，没有任何源码、构建脚本或测试。** 任何代码改动都必须先确认 PLAN.md 是否已覆盖该决策；如未覆盖，先讨论再写代码。

# 项目定位

`foo_hemusic` 是为 **foobar2000 v2** 开发的组件（component / .fb2k-component DLL），把 HE-Music 后端作为音乐源接入 foobar2000：

- **登录**：HE-Music 后端唯一支持的 OAuth provider 是 `linuxdo`，对客户端就是一个普通 provider 字符串
- **播放**：自定义 `hemusic://` 协议，foobar2000 触发播放时即时调 `/v1/song/url` 解析直链
- **UI**：foobar2000 原生 `ui_element` + WTL + Direct2D 自绘 + libPPUI 列表控件，**不引入 WebView2 / Sciter / CEF**

详细架构与开发阶段见 [`foo_hemusic/PLAN.md`](./foo_hemusic/PLAN.md)。

# 三份关键参考材料（按权威度排序）

| 来源 | 路径 | 用途 |
|---|---|---|
| **Flutter 工程源码** | [`HE-Music-Flutter/lib/`](./HE-Music-Flutter/lib/) | API 实际调用方式、字段别名兼容逻辑、UI 信息层级的**最终事实源** |
| HE-Music API 文档 | [`HE-Music-Flutter/api.md`](./HE-Music-Flutter/api.md) | 接口清单与字段说明 |
| foobar2000 SDK | [`foobar2000-SDK-2025-03-07/`](./foobar2000-SDK-2025-03-07/) | C++ SDK 头文件、示例 `foo_sample`、`libPPUI`、`pfc` |

**当 api.md 与 Flutter 源码冲突时，以 Flutter 源码为准**。`api.md` 已于 2026-06-19 按 `lib/` 全面校订过一轮（认证 / 双 token / 设备管理 / 字段名等），目前可信度较高；但它仍是二手材料，鉴权与播放直链等关键路径仍以源码复核为准。需特别注意：

- **认证已是双 token 模型**：登录 / OAuth / 扫码兑换返回 `access_token` + `refresh_token` + `expires_at`（旧后端可能只回单字段 `token`，Flutter 端回退兼容）。401 由 Flutter 的 `TokenRefreshInterceptor` 用 `refresh_token` 自动 `POST /v1/auth/token/refresh` 续期并重放原请求，排除路径见 api.md §0.4。
- **OAuth 申请授权链接是 `POST /v1/auth/session`**（body 带 `provider` / `redirect_uri` / `device_info`），不是旧文档写的 `GET /v1/auth/code/url`。
- 登录 / 授权 / 续期请求都带 `device_info`（`device_id` / `platform` / `app_type` / `app_version` / `device_name`，见 api.md §0.7）。
- 各类列表响应有大量字段别名（如 `image` / `masterImageBase64`），Flutter 用一组候选 key 兼容（参考各 `fromMap` 实现）。

# 与 HE-Music-Flutter 工程的关系

[`HE-Music-Flutter/`](./HE-Music-Flutter/) 是同一后端的移动端 / 桌面端实现，**作为参考实现使用**。

- 它**有自己独立的 CLAUDE.md / bd 工具链**——那套约定**不适用于 foo_hemusic**。当看到 Flutter 工程的 CLAUDE.md 提到 `bd` / `MEMORY.md` / "session completion 必须 push" 等，**不要套用到本项目**
- 复用 Flutter 工程的设计时，照搬业务逻辑（API 调用顺序、状态机、字段兼容），**不要**照搬 Material 视觉风格（阴影、涟漪、强调色），foo_hemusic 视觉跟 foobar2000 主题走

# 开工前的环境与验证步骤

PLAN.md §8 列出按顺序的下一步。最关键的两点：

1. **Phase 0 必须先编出 `foo_sample` 并加载到 foobar2000 验证工具链**。SDK 入口在 [`foobar2000-SDK-2025-03-07/foobar2000/foo_sample/foo_sample.sln`](./foobar2000-SDK-2025-03-07/foobar2000/foo_sample/foo_sample.sln)（VS2022 MSBuild 工程）。本项目自身用 **CMake** 构建；SDK 自带的 `.sln` 仅用来先验证工具链 / SDK 二进制能跑通，不作为 foo_hemusic 的构建系统。
2. **抓包对照**：用真实账号在 Flutter app 跑一次完整登录 + 播放，记录 HTTP 流量做基线（特别是 `device_info` 字段实际格式、`User-Agent`、`/v1/song/url` 是否带 Referer 等隐式约束）。

## 第三方构建依赖：WTL

foobar2000 SDK 依赖 **WTL (Windows Template Library)**，VS2022 不自带（VS 只带 ATL，没带 WTL），SDK 包里也没带。**未配置 WTL 直接编译 `foo_sample` 会报 `cannot open atlapp.h / atlctrls.h / atlcrack.h`**。

本仓库的处理：

- WTL 源码放在 [`wtl/`](./wtl/)（来自 https://github.com/Win32-WTL/WTL）
- 根目录的 [`Directory.Build.targets`](./Directory.Build.targets) 自动把 `wtl/Include` 注入所有子 vcxproj 的 `AdditionalIncludeDirectories`——**不修改 SDK 任何文件**
- 如果 WTL 装到别处，设环境变量 `WTLIncludeDir=D:\path\to\WTL\Include` 覆盖默认路径

**不要**用以下方式配置（已被否决）：
- ~~直接编辑 SDK 的 `.vcxproj`~~ ——污染上游，升级 SDK 时冲突
- ~~改 `Microsoft.Cpp.x64.user.props`~~ ——污染机器全局 VS 配置

## pfc / libPPUI 必须用 FB2K 后缀配置

foobar2000 组件链接 `shared.dll`，而 `shared.dll` 已经导出了 `pfc::crashHook` / `pfc::winFormatSystemErrorMessageHook` 等符号。`pfc` 项目默认的 `Debug` / `Release` 配置会重复编译这些符号 → LNK2005。

**规则**：sln 里 `pfc` 和 `libPPUI` 这两个依赖项目，配置永远要选 **`Debug FB2K`** / **`Release FB2K`**（带 FB2K 后缀），让组件链接时这两个符号由 `shared.dll` 提供。

设置位置：**生成 → 配置管理器**，每个平台（Win32/x64）的每个配置（Debug/Release）下，手动把 `pfc` / `libPPUI` 的"配置"列切到 FB2K 后缀版本。

依据见 SDK 源码注释：[`foobar2000-SDK-2025-03-07/pfc/suppress_fb2k_hooks.h`](./foobar2000-SDK-2025-03-07/pfc/suppress_fb2k_hooks.h) 第 5-7 行。

# 已敲定的设计约束（不要重新提议）

下列决策已在 PLAN 讨论过并选定，除非用户明确要求重新评估，不要再提：

- **库选型优先级**：C++ 标准库 > Boost > Windows API / 组件 > 业界成熟流行库。避免自造轮子；标准库 / Boost / Win32 能解决的不引第三方；确需引入时选经过检验的成熟库（如已敲定的 Boost.JSON / Catch2），并在此处登记。
- UI 全部原生（**不用** WebView2 / Sciter / React / Tauri / Qt）
- OAuth 回调**不**起本地 loopback HTTP 服务器——HE-Music 后端自己接收，客户端只轮询 `/v1/auth/status`
- Linux.do **只是 HE-Music 的一个 OAuth provider 名**，客户端不直接与 Linux.do 交互
- Token 用 **DPAPI** 加密落地，不进明文 cfg；注意是 **双 token**（`access_token` + `refresh_token` + `expires_at` 都要存），401 时用 `refresh_token` 走 `POST /v1/auth/token/refresh` 续期后重放原请求
- 所有带 token 的 HTTP 请求统一走 C++ 侧 WinHTTP，不引入第三方 HTTP 库
- JSON 用 **Boost.JSON**（vcpkg `boost-json`），单元测试用 **Catch2**；依赖走 **vcpkg manifest**（`foo_hemusic/vcpkg.json`），CMake 接 `D:\vcpkg\scripts\buildsystems\vcpkg.cmake` toolchain。（原计划用 nlohmann/json，已于 2026-06-19 改为 Boost.JSON：分离编译、编译开销更小、`if_contains` 适配字段别名兼容）
- 默认只支持 foobar2000 **v2**，先发 x64

# 编码约定（当代码开始存在后适用）

- C++20，MSVC 2022，CMake 构建
- 文件命名 `snake_case.{h,cpp}`，类名 `PascalCase`，函数 / 变量 `camelCase`
- SDK 头文件已经全局污染了一堆宏（`pfc::string8` 等），业务代码用 SDK 类型时跟 SDK 风格，纯业务层（`api/`、`net/json_codec`）用 std + nlohmann
- 不要把 token / 用户 token 写进日志（`console::print`）；日志走 `logging.h` 里的 redacted helper（待实现）

# 常用命令

构建系统:CMake + **Ninja Multi-Config** + vcpkg manifest。用 **Ninja 生成器**是为了导出
`build/compile_commands.json`(VS 生成器不导出),供 clang-tidy / clang-format hook 使用。
Ninja 需要 MSVC 开发环境(cl 在 PATH),脚本已封装 vcvars 探测。在 `foo_hemusic/` 下执行:

```bash
scripts\configure.bat            # 首次配置(vcvars→Ninja→vcpkg 装 boost-json/catch2)
scripts\build.bat                # 构建 Release(组件 DLL + 测试);build.bat Debug 构 Debug
./build/Release/hemusic_tests.exe   # 跑 Catch2 单元测试
```

- VS 安装不在默认路径时,设环境变量 `VCVARS64=<vcvars64.bat 全路径>` 覆盖(见 `scripts/_vcvars.bat`)。
- 产物:`build/Release/foo_hemusic.dll`(组件)、`build/Release/hemusic_tests.exe`(测试)。
- `build/compile_commands.json` 由配置自动生成;PostToolUse/Stop 的 clang-tidy hook 依赖它存在,
  缺失时静默跳过(故新克隆需先 `scripts\configure.bat` 一次)。
- `.fb2k-component` 打包脚本待 Phase 8。

## clang 工具链 hook(`.claude/hooks/`)

- `clang-format-postedit.sh`:编辑 `foo_hemusic/{src,tests}` 的 C++ 文件后按 `.clang-format` 自动格式化。
- `clang-tidy-postedit.sh` / `clang-tidy-stop.sh`:按 `.clang-tidy` 静态检查(经 `compile_commands.json`)。
- 工具解析:优先 PATH,其次 `CLANG_TOOLS_DIR`(在 `settings.local.json` 设为本机 LLVM bin),找不到则静默跳过。
- `.clang-format` 关 `SortIncludes`、`.clang-tidy` 关掉与 Win32 互操作冲突的检查(C 数组 / reinterpret_cast / include 排序)。
