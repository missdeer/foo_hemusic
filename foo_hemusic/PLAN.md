# foo_hemusic 开发计划

foobar2000 组件 (component)，把 HE-Music 后端做成"接入式音乐源"。UI 布局对齐官方网站 [`y.wjhe.top`](https://y.wjhe.top/)（用 Chrome CDP 观察，见 §3.5），配色跟 fb2k 主题；鉴权走 Linux.do，播放时即时解析直链。

> 参考实现：[`../HE-Music-Flutter/api.md`](../HE-Music-Flutter/api.md)（接口契约，PLAN 中所有 `/v1/...` 路径默认指此文档）
> SDK：[`../foobar2000-SDK-2025-03-07/`](../foobar2000-SDK-2025-03-07/)（2025‑03‑07 版，foobar2000 v2 兼容）

---

## 1. 项目目标与范围

### 1.1 必做（MVP）

1. **登录**：通过 Linux.do OAuth 完成身份认证，换取 HE-Music 后端 Bearer Token。
2. **浏览**：内嵌 UI 面板，可在 foobar2000 主窗口里查看：
   - 发现（首页聚合）
   - 搜索（综合 + 分类）
   - 歌单广场 / 歌单详情 / 自建歌单
   - 专辑详情、歌手详情
   - 排行榜、电台
   - "我的"：收藏歌曲 / 歌单 / 歌手 / 专辑
3. **入列**：从任何曲目列表把单曲 / 整张专辑 / 整张歌单加入当前 foobar2000 播放列表。
4. **播放**：foobar2000 触发播放时，组件即时调用 `/v1/song/url` 解析真实直链并交由内置解码器播放。
5. **元数据**：曲目标题 / 艺人 / 专辑 / 时长 / 封面在 foobar2000 状态栏、播放列表、专辑封面面板正常显示。

### 1.2 第二期（Nice‑to‑have）

- 歌词面板（对接 `/v1/song/lyric`，与 foobar2000 自带 lyric show 兼容）
- MV 播放（独立窗口，video_player 调 `/v1/mv/url`）
- 下载到本地（落地到用户配置目录，自动入库）
- 滑动验证码处理（嵌入 WebView 内自动唤起）
- 扫码登录（PC 端为生成端，移动端确认）

### 1.3 明确不做

- 不做 foobar2000 内的 MV 解码器（视频另起窗口）
- 不内嵌完整 Discourse 论坛（Linux.do 仅作 OAuth provider）
- 不重写 HE-Music 后端，但允许提交补丁（见 §3.1 的未决问题）

---

## 2. 技术选型

| 维度 | 选型 | 理由 |
|---|---|---|
| 语言 | C++17 | foobar2000 SDK 强约束 |
| 构建 | MSVC 2022 + CMake | SDK 自带 `.sln`，但 CMake 更利于多目标；保留 SDK 原 sln 作为子项目 |
| 架构 | 32 + 64 位双构建 | foobar2000 v2 起 64 位为主，但用户基数仍含 32 位 |
| UI 宿主 | **foobar2000 原生 ui_element** | 与 fb2k 主程序风格融合，无运行时依赖 |
| UI 实现 | **Direct2D + DirectWrite 自绘** + 必要处用 Win32 控件（输入框、按钮） | 列表/卡片自绘；**布局 / 元素 / 行为基本对齐官网 `y.wjhe.top`**（Chrome CDP 观察），配色 / 字体跟随 fb2k 主题（见 §3.5） |
| 列表组件 | SDK 自带的 `libPPUI` 提供的 `CListControl` / `CListControlOwnerData` | 已有虚拟滚动、双缓冲、皮肤适配，省一大块基建 |
| HTTP | **WinHTTP** | SDK `http_client` 功能不够；所有带 token 的请求统一走 WinHTTP |
| JSON | **Boost.JSON**（vcpkg `boost-json`） | 分离编译，编译开销小于 nlohmann 单头；`if_contains` 适配 Flutter 多候选 key 别名兼容 |
| 依赖管理 | **vcpkg manifest**（`vcpkg.json`） | CMake 接 vcpkg toolchain 自动安装 boost-json / catch2 |
| 持久化 | foobar2000 `cfg_var` + Windows DPAPI（token） | token 不进明文 cfg |
| OAuth 回调 | **无需本地服务器**，HE-Music 后端接收回调，客户端轮询 `/v1/auth/status` | Flutter 已验证可行 |
| 单元测试 | Catch2 | 与 SDK 解耦的纯业务层（API client / URL resolver） |

> UI 宿主已确定走 foobar2000 原生 ui_element，不引入 WebView2 / Sciter / CEF。视觉策略分两层（详见 §3.5）：**布局 / 元素 / 交互行为基本对齐官方网站 `y.wjhe.top`**（实现前用 Chrome CDP 观察其 DOM 与运行时行为，第一版可略有偏差、不追求像素级）；**配色 / 字体跟随 fb2k 当前主题**（随明暗换肤），不照搬官网或 Material 色板。

---

## 3. 关键技术难点与解决方案

### 3.1 Linux.do OAuth → HE-Music Token

HE-Music 后端**唯一支持**的登录方式就是 Linux.do，`/v1/auth/providers` 返回的 list 里只有 `"linuxdo"`。Flutter 工程已经把流程跑通，**客户端不需要做任何 OAuth 特化**——`linuxdo` 对客户端就是一个普通 provider 字符串。

参考实现：[`lib/features/auth/presentation/pages/login_page.dart`](../HE-Music-Flutter/lib/features/auth/presentation/pages/login_page.dart) 与 [`lib/features/online/data/online_api_client.dart`](../HE-Music-Flutter/lib/features/online/data/online_api_client.dart)。

**关键事实（以 Flutter 源码为准，与 api.md §2 文字描述略有出入）**：

1. **拿授权链接是 `POST`，不是 GET**：
   ```
   POST /v1/auth/code/url
   Body: { "provider": "linuxdo", "device_info": {...}, "redirect_uri"?: "..." }
   Resp: { "url", "state", "check_interval", "expire_at" }
   ```
   `redirect_uri` 在 Flutter 里**不传**，由后端使用自己配置的默认回调（这意味着回调由 HE-Music 后端自己接收，**客户端不需要本地 loopback HTTP 服务器**——这比原计划简单一大截）。

2. **流程**：
   ```
   POST /v1/auth/code/url        → { url, state, check_interval }
   ShellExecute(url)             → 打开系统浏览器，用户在 Linux.do 完成授权
   每 check_interval 秒一次:
     GET  /v1/auth/status?state= → { status: "pending|success|expired|error" }
   status == "success" 时:
     GET  /v1/auth/result?state= → { token }
   ```

3. **device_info**：参考 [`lib/core/device/device_info_provider.dart`](../HE-Music-Flutter/lib/core/device/device_info_provider.dart)，C++ 侧用 `GetComputerName` + OS 版本 + 组件版本拼一个等价 map。

### 3.2 滑块验证码

api.md §4 说明：`403 + reason=CAPTCHA_REQUIRED` 触发，返回底图 + 拼图块 + 凹槽坐标，提交时只校验最终点击坐标。

**实现**：弹出 Win32 模态对话框，Direct2D 自绘。
- 拦截器捕获 403 → 暂停原请求线程，主线程上 `DialogBoxIndirect` 弹出验证码窗口
- 窗口里有两个 Direct2D 渲染区：底图（带凹槽的大图）+ 拼图块（小图），下方一条 trackbar
- 用户拖 trackbar，拼图块 X 坐标随之线性映射到 [0, master_width - thumb_width]
- 松开后 POST `/v1/captcha`，`is_success && !is_expired` 则关闭对话框，原请求线程被唤醒，**保留 RequestOptions 重放**
- 失败重抓新一张图（图片资源直接 base64 解码到 IWICBitmap，无需第三方库）

> 滑动条参数（width/height/凹槽 Y 坐标等）解析跟 Flutter 一样存在大量字段别名，移植 [`SlideCaptchaPayload.fromMap`](../HE-Music-Flutter/lib/features/auth/data/datasources/captcha_api_client.dart) 的兼容列表即可。

### 3.3 `hemusic://` 自定义协议 + 即时直链解析

**问题**：`/v1/song/url` 返回的直链有时效性，不能在"加入播放列表"时就解析，必须在播放瞬间解析。

**方案**：实现自定义 `input_service`，注册 `hemusic` 协议。

播放列表里存的 URL 形如：
```
hemusic://song?id=<songId>&platform=<platform>&hint_title=<urlencoded>&hint_artist=<...>
```

播放流程：
1. foobar2000 调用 `input_entry::g_open_for_decoding("hemusic://...")`
2. 组件的 `input_service::open()` 解析 query，调 `/v1/song/url` 拿到真实 URL（mp3/m4a 等）
3. **内部委托**：用 `input_entry::g_open_for_decoding(real_url, ...)` 拿到一个 wrapped `input_decoder`，本组件作为 thin proxy，把 `decode_run` / `decode_seek` / `get_info` 全部转发
4. `get_info` 时融合 query string 里的 hint 与 `/v1/song/detail` 返回的元数据

**为什么不直接把 URL 解析后写入 playlist**：直链 token 通常 5~30 分钟过期，会话恢复后必失效。

**实测要点**：foobar2000 SDK 的 `input_helpers.h`、`input_impl.h` 中提供了 `input_decoder_v2/v3` 的 forwarder 模式；参考 SDK 内 `foo_sample/foo_sample_input.cpp` 学习接口实现。

### 3.4 元数据与封面

- `playable_location` 用 `hemusic://song?...` 作为路径；fb2k 会把它视作 unique track key
- `get_info()` 至少要给：`title / artist / album / length / track_number`
- 封面：实现 `album_art_extractor` 服务，URL 拼自 api.md §7.5；token 必须带 query，**注意此路径不走 Dio 拦截器，等价于裸 URL**
- 解决并发：`/v1/song/detail` 不必每首歌都打，列表入列时如果手里已有完整 `SongInfo`，就把它编进 hint 参数

### 3.5 UI 视觉对齐与主题

UI 分两层取舍——**结构层对齐官网、表现层跟随 fb2k 主题**：

**① 布局 / 元素 / 交互行为：与官方网站 [`https://y.wjhe.top/`](https://y.wjhe.top/) 基本对齐**（HE-Music 的 Electron+Vue3 官方端）。页面结构、控件排布、信息层级、交互行为（hover / 点击 / 翻页 / 入列）要照官网。第一版**不追求像素级**复刻，可接受略有偏差。**实现各页前用 Chrome CDP 连到浏览器实地观察 `y.wjhe.top` 的 DOM 布局、元素层级与运行时行为**（而非凭截图臆测），各页对应关系见 §5。Flutter 工程仍作 API 调用 / 字段兼容的最终事实源，但**UI 布局以官网为准**（移动端 Flutter 与桌面网页布局不同）。

**② 配色 / 字体：跟随 foobar2000 主题，不照搬官网色板。** 组件必须跟随 fb2k 用户可切换的明暗主题，否则会出现"亮色窗口里嵌一块暗色面板"的尴尬：
- 颜色：通过 SDK 的 `colours_api`（v2 中名为 `cui::colours::manager_instance_v3` 或 DUI 侧 `ui_config_manager`）获取当前主题前景 / 背景 / 选中色
- 字体：通过 `font_manager_v2` 拿主题字体，列表 / 标题用不同 size
- 不引入 Material / 网页端的阴影 / 涟漪动画 / 粗体强调色——这些会跟 fb2k 风格冲突

> 一句话：**官网决定"长什么样的结构与行为"，fb2k 主题决定"用什么颜色字体来画"**。原先"只保留卡片化布局 / 分区标题 / 入列按钮三处弱特征"已升级为"布局 / 元素 / 行为基本对齐官网"。

---

## 4. 架构与模块划分

```
foo_hemusic/
├── CMakeLists.txt
├── PLAN.md                      ← 本文件
├── README.md                    ← 用户安装指南（最后才写）
├── vcpkg.json                    ← vcpkg manifest（boost-json / catch2）
├── src/
│   ├── component.cpp            ← 组件入口、版本信息、initquit
│   ├── core/
│   │   ├── config.{h,cpp}       ← cfg_var、token 加密读写
│   │   ├── logging.{h,cpp}      ← 走 console::print + 文件 ring buffer
│   │   └── result.h             ← Result<T,E> 等价物
│   ├── net/
│   │   ├── http_client.{h,cpp}  ← WinHTTP 封装 + Bearer 注入 + 重试
│   │   ├── interceptors.{h,cpp} ← 401 跳登录 / 403 captcha 重放 / 错误吐司
│   │   └── json_codec.h         ← 工具，把 boost::json::value ↔ 业务结构体
│   ├── auth/
│   │   ├── oauth_flow.{h,cpp}   ← /v1/auth/providers + /code/url + /status + /result
│   │   ├── device_info.{h,cpp}  ← 收集 device_info 上报字段（对齐 Flutter）
│   │   └── token_store.{h,cpp}  ← DPAPI 加解密
│   ├── api/                     ← 与 api.md 章节一一对应
│   │   ├── platforms.h
│   │   ├── discover.h
│   │   ├── search.h
│   │   ├── song.h
│   │   ├── album.h
│   │   ├── artist.h
│   │   ├── playlist.h
│   │   ├── ranking.h
│   │   ├── radio.h
│   │   ├── mv.h
│   │   ├── favourite.h
│   │   ├── user_playlist.h
│   │   ├── comment.h
│   │   └── captcha.h
│   ├── playback/
│   │   ├── hemusic_input.{h,cpp}   ← input_service 实现，"hemusic" 协议
│   │   ├── url_resolver.{h,cpp}    ← /v1/song/url 缓存 + 失效重取
│   │   ├── album_art.{h,cpp}       ← album_art_extractor
│   │   └── playlist_writer.{h,cpp} ← 把 SongInfo[] → playlist_manager API
│   └── ui/
│       ├── ui_element.{h,cpp}      ← Default UI element registration（主面板入口）
│       ├── cui_panel.{h,cpp}       ← Columns UI 兼容（可选，二期）
│       ├── theme.{h,cpp}           ← 取 fb2k 主题色 / 字体 + HE-Music 视觉常量
│       ├── d2d.{h,cpp}             ← Direct2D 工厂、ID2D1HwndRenderTarget 生命周期、IWICBitmap 解码
│       ├── image_cache.{h,cpp}     ← 封面位图内存 LRU + 异步加载
│       ├── widgets/
│       │   ├── song_list.{h,cpp}   ← 基于 libPPUI CListControl 派生
│       │   ├── card_grid.{h,cpp}   ← 歌单 / 专辑卡片瀑布
│       │   ├── tab_bar.{h,cpp}     ← 顶部 Tab 切换（发现/搜索/我的）
│       │   ├── cover_view.{h,cpp}  ← 单个封面 + hover 入列按钮
│       │   └── search_box.{h,cpp}  ← Win32 edit + 联想浮层
│       ├── pages/
│       │   ├── login_dlg.{h,cpp}   ← 登录模态对话框
│       │   ├── captcha_dlg.{h,cpp} ← 滑块验证码模态对话框
│       │   ├── discover_page.{h,cpp}
│       │   ├── search_page.{h,cpp}
│       │   ├── playlist_detail_page.{h,cpp}
│       │   ├── album_detail_page.{h,cpp}
│       │   ├── artist_detail_page.{h,cpp}
│       │   ├── ranking_page.{h,cpp}
│       │   ├── radio_page.{h,cpp}
│       │   └── my_page.{h,cpp}
│       └── nav.{h,cpp}             ← 页面栈（前进/后退）
└── tests/
    ├── api_client_test.cpp
    ├── url_resolver_test.cpp
    └── token_store_test.cpp
```

---

## 5. 分阶段开发计划

每个阶段标注 **交付物** 和 **验收**。每阶段结束都要能跑（即使功能不全），不堆积"未编译"工作。

### Phase 0 — 工程脚手架（1~2 天）

- [x] CMakeLists 从源码编译 `pfc + SDK + component_client` 为 `fb2k_sdk` 静态库（helpers/libPPUI 待 UI 阶段加）
- [x] 写最小 `component.cpp`：`DECLARE_COMPONENT_VERSION` + `initquit` 打印加载日志
- [x] Release 构建产出 `foo_hemusic.dll`（vcpkg toolchain，boost-json/catch2 经 manifest 安装）
- [x] 在 foobar2000 实机加载 DLL，确认"组件"列表显示 `foo_hemusic 0.0.1`（已人工确认 2026-06-19）
- [ ] `.fb2k-component` 打包脚本（推迟到 Phase 8 一并做）

**交付**：foobar2000 启动后，"组件" 列表能看到 `foo_hemusic 0.0.1`。

### Phase 1 — 鉴权链路（2~3 天）

- [x] `device_info`：`src/auth/device_info.{h,cpp}` + Catch2 测试。身份**伪装成 Flutter**（`app_type="flutter"`、`device_id="flutter_windows_<uuid>"`、`platform="windows"`）；持久化 device_id 留给 caller（cfg_var，待 `core/config`）。测试断言 5 字段契约通过
- `oauth_flow`（拆分小步，以 Flutter `OnlineApiClient` 为准）：
  - [x] 消息层 `src/auth/oauth_flow.{h,cpp}` + `src/net/json_codec.h`：请求体构造（`buildSessionRequest`/`buildRefreshRequest`，`redirect_uri` 空则不带、`device_info` 必带）+ 响应解析（`parseAuthProviders`/`parseAuthCodeUrl`/`parseAuthStatus`/`parseAuthToken`），含 Flutter `_asInt` 字符串数字容错、`status` 分类枚举、旧后端单 `token`(顶层 + `data.token`) 回退。10 个 Catch2 用例覆盖契约（`tests/oauth_flow_test.cpp`，全绿）
  - [x] 传输层 `src/net/http_client.{h,cpp}`：WinHTTP 同步请求（GET/POST + JSON body + header 注入 `authorization: Bearer`，body 非空默认 `content-type: application/json`），`WinHttpSetTimeouts` connect 20s / read 30s。`crackUrl`（WinHttpCrackUrl 封装，补 scheme 默认端口 / 拼 path+query）抽出可单测，4 个 Catch2 用例（`tests/http_client_test.cpp`，全绿）。代理走 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`（WPAD/IE 代理留待后续）；异步两套 API 推迟到 Phase 2
  - [x] 编排 `src/auth/login_flow.{h,cpp}`：`runLogin` 串 `GET /v1/auth/providers`（**provider 解析**，非 gate）→ `POST /v1/auth/session` → `openUrl` → 按 `check_interval` 轮询 `/v1/auth/status` →（success 时）`GET /v1/auth/result`。**实机关键修正**：真实后端 `/v1/auth/providers` 返回 `{"list":["LinuxDo"]}`，provider id **大小写敏感**(`provider:"linuxdo"` → session **404**;`"LinuxDo"` → 200 拿到 `connect.linux.do` 授权 URL)。故 `resolveProvider` 把调用方请求的 `"linuxdo"` 对列表做**大小写不敏感匹配**,取后端原样 id(`"LinuxDo"`)传给 session —— 与 Flutter「用 providers 列表里的 id、不硬编码」一致;匹配不到则 `Failed("provider not offered by backend")`。(`expires_at` 后端回字符串 `"178..."`,已被 `json::i64` 数字串容错吸收。)状态机与 Flutter `login_page._pollAuthStatus` 对齐：仅 success/failed/expired 终止，pending/error/unknown 续轮；本地 `expires_at` 截止 + 每轮刷新 interval/expiresAt。三个副作用（transport / openUrl / wait）以 `std::function` 注入便于单测，**不碰 SDK**；`ShellExecuteW` 与可取消 wait 的真实实现留给临时 UI 接线。URL 拼接/percent-encode（`buildAuthUrl`/`percentEncode`）抽出可单测。**安全**:`openUrl` 前 `isHttpUrl` 校验授权 URL 仅放行 `http(s)://`，挡住被篡改后端经 `ShellExecuteW` 唤起任意协议处理器。**进度**:可选 `onProgress(LoginPhase)` 回调（`Connecting`/`WaitingForAuthorization`/`Finalizing`），让 worker 线程驱动"等待授权"UI 而不改阻塞模型。11 个 Catch2 用例覆盖成功/provider 缺失/failed/expired/取消/传输错误/**非 http(s) URL 拒绝**/**进度阶段序列**（`tests/login_flow_test.cpp`，全绿）
- [x] `token_store` `src/auth/token_store.{h,cpp}`：双 token 三元组(`access_token`/`refresh_token`/`expires_at`)→ Boost.JSON 序列化 → DPAPI(`CryptProtectData`，`CRYPTPROTECT_UI_FORBIDDEN`，按当前 Windows 用户作用域)→ 文件。分层:纯 `serializeToken`/`deserializeToken`(空 access_token 拒绝) + Win32 `dpapiProtect`/`Unprotect` + `TokenStore::save/load/clear`(`std::filesystem`，自动建父目录，损坏/异用户/缺失均返回 nullopt)。**写为原子**:`save` 先写 `.tmp` 再 `rename` 覆盖(同卷原子)，写失败不毁旧凭证。路径由构造参数注入(组件侧从 fb2k profile 目录拼，测试传 temp 路径)。链接加 `crypt32`。9 个 Catch2 用例:序列化往返、垃圾/无 token 拒绝、DPAPI 真实往返、密文≠明文、save/load/clear 往返、**原子覆写不留 `.tmp`**、**磁盘字节不含明文 token**、缺失/损坏文件(`tests/token_store_test.cpp`，全绿)
- [x] `core/config` `src/core/config.{h,cpp}`（SDK-bound，编入组件 DLL 目标，非 hemusic_core）：两个 `cfg_string`（各带固定随机 GUID）—— `deviceId()`（懒生成 `newDeviceId()` 后持久化，重启稳定）、`apiBaseUrl()`/`setApiBaseUrl()`（默认 `https://y.wjhe.top`，取自 Flutter `assets/app_config.json`，trim + 空回退默认）。`deviceId()` 首调用 `std::mutex` 串行化读-生成-持久化，避免并发首调铸出两个不同 id。`tokenStorePath()` 用 `extract_native_path(core_api::get_profile_path())` 取 fb2k profile 原生路径 → `<profile>/foo_hemusic/token.bin`（**profile 根子目录,已与用户确认采用** —— 避免组件重装被清；TokenStore 自动建目录）;`extract_native_path` 失败时回退手动 strip `file://`，**绝不落 CWD 相对路径**。`.clang-tidy` 加 `-cppcoreguidelines-avoid-non-const-global-variables`（cfg_var/`FB2K_SERVICE_FACTORY` 必须命名空间作用域非 const 全局，自注册 config I/O）。SDK-bound 无法 hemusic_core 单测，靠真实 SDK 符号编译+链接通过验证；运行期(device_id 持久化/profile 路径)随 UI 接线实机验
- [x] 临时登录 UI `src/ui/pages/login_dlg.{h,cpp}`（无模式 Win32 窗口，编入组件 DLL）：主菜单 `HE-Music > Login`（`mainmenu_group_popup` + `mainmenu_commands`）触发 `showLoginDialog()`。无模式窗口（STATIC 进度文字 + Cancel 按钮，owner=主窗口），登录在 worker `std::thread` 跑 `runLogin`，三个真实接缝：`transport`=`HttpClient::send`、`openUrl`=**worker 线程直接 `ShellExecuteW`**（`CoInitializeEx` 初始化 COM）、`wait`=`WaitForSingleObject(cancelEvent)`；`onProgress`→`PostMessage` 更新文字。**worker 只用 `PostMessage` 单向通知 UI，绝不同步 `SendMessage`** —— 故 UI 线程 join worker 不会跨线程死锁(经双 reviewer 审查修正)。成功后 worker 存 `TokenStore(config::tokenStorePath()).save(token)`（**检查返回值，失败则在 console 警告"凭证未写盘"**）+ 打 `/v1/user/info` 经 `parseUserInfo` 取用户名 → `PostMessage(DONE)` → 主线程 `console::print` + `popup_message` 显示。配置（baseUrl/deviceId/tokenPath/device_info）在主线程采集后传入 worker，避免 worker 碰 cfg_var/core_api。单实例（再次触发 SetForegroundWindow）；取消/关闭只 `SetEvent`，正常经 DONE 销毁窗口并 join；**`WM_DESTROY`（fb2k 退出/owner 销毁等外部路径）先 `SetEvent` 再 join，杜绝永久挂起**。CMake 加 `/utf-8`（中文字面量）；`.clang-tidy` 再加 `-performance-no-int-to-ptr` / `-interfaces-global-init` / `-virtual-class-destructor`（Win32 消息参数 + SDK 服务注册惯例）。**已知限制**：同步 WinHTTP 不可中断，取消/关闭时若卡在某次请求，join 最多等该请求 30s 读超时（有界，非挂起；Phase 2 异步 HTTP 彻底解决）。**编译+链接通过、零警告；端到端（点登录→浏览器授权→打印用户名）需在 foobar2000 实机手测，无法自动验证**
- [x] `GET /v1/user/info` 消息层 `src/api/user.h`（header-only，对齐 api/ 层约定）：`UserInfo`{id/username/nickname/email/status/avatarUrl} + `parseUserInfo`，字段对齐 Flutter `MyOverviewApiClient.fetchProfile`/`MyProfile`（顶层无 envelope，字符串 stringify+trim，`status` 字符串数字容错，`id` 数字转字符串）；`valid()` 以 `id` 非空为 token 验证门。4 个 Catch2 用例覆盖完整解析/类型容错/缺字段默认/非对象失效（`tests/user_info_test.cpp`，全绿）。**编排接线已完成**：login_dlg 登录成功后在 worker 线程打 `/v1/user/info`、`parseUserInfo` 取用户名并显示

**交付**：foobar2000 内点登录 → 系统浏览器打开 Linux.do → 授权后回 foobar2000 状态变 success → 弹窗显示当前 HE-Music 用户名。**✅ 已于 2026-06-20 实机端到端跑通**（菜单 `HE-Music > Login` → `connect.linux.do` 授权 → 成功弹窗显示账号）。显示名优先级 nickname→username→id（对齐 Flutter `MyAccountCard`）。

> **Phase 1 实机手测清单**（需真实 Linux.do/HE-Music 账号，自动化无法覆盖）：
> 1. `scripts\build.bat Release` → 把 `build/Release/foo_hemusic.dll` 拷到 fb2k 的 `user-components-v2/foo_hemusic/`（或用 fb2k「安装组件」）。
> 2. 启动 fb2k，菜单出现 `文件 > HE-Music > Login`；点击弹出"HE-Music 登录"窗口，显示"正在连接…/请在浏览器中完成授权…"。
> 3. 系统浏览器打开 Linux.do 授权页；授权后窗口转"授权成功，正在获取账号…"并自动关闭，控制台打印 `登录成功，用户 <名>`，弹窗显示用户名。
> 4. 验证 `%APPDATA%\foobar2000\foo_hemusic\token.bin` 生成且非明文（不含 access/refresh 原文）。
> 5. 取消路径：授权前点「取消」→ 窗口转"正在取消…"，当前轮询/请求结束后关闭，控制台打印"登录已取消"。

### Phase 2 — API 客户端层（3~5 天，可与 P1 并行）

- [x] `http_client`：WinHTTP **同步** API（Phase 1 已落地：统一头注入 + Bearer，超时 connect 20s / read 30s）。**异步两套 API 推迟**——目前无调用方（登录走 worker 线程同步 + 可取消 wait），异步随 Phase 4 UI 的封面/列表并发加载一并做，避免提前造无人用的基建
- 拦截器链 `AuthToken → UnauthorizedRedirect → CaptchaChallenge → ErrorMessage`，按纯逻辑 vs SDK/UI 绑定拆分推进：
  - [x] **`AuthToken` + 401 自动刷新重放** `src/net/api_client.{h,cpp}`（hemusic_core，纯逻辑 + 注入 transport，无 SDK）：`ApiClient::send` 注入当前 `access_token` 为 Bearer（**client 独占鉴权**，覆盖 req 上已有的 bearer）；命中 401 且**路径不在排除名单** + 有 `refresh_token` 时，`POST /v1/auth/token/refresh`（复用 `buildRefreshRequest`/`parseAuthToken`）→ 更新缓存三元组 + 触发 `onTokensRefreshed`（供 caller 持久化 TokenStore）→ **用新 token 单次重放**原请求；重放仍 401 直接返回（**不再二次刷新,杜绝死循环**）。对齐 Flutter `TokenRefreshInterceptor`：刷新响应 `refresh_token` 为空时**沿用旧 refresh**（api.md §6,长期凭据不被非轮换刷新清掉）；刷新失败（非 2xx / 传输错 / 空 access）则缓存不变、原 401 透传。`isAuthExcludedPath` 用正则 `/(login|token/refresh|auth/result|auth/qr/result|auth/logout)\b`（api.md §0.4）抽出可单测，**只匹配 path**（先剥掉 query + scheme/host，对齐 Flutter `requestOptions.path`）——堵住 `?next=/login` 之类 query 误命中导致 401 静默不刷新（经 /review Codex 指出后修正）。**并发共享单次刷新**（Flutter Completer 那段）**暂不做**：当前 `HttpClient` 单线程、`ApiClient` 非线程安全,无并发 401 场景；多线程取数据时再补（**Phase 4 接入多线程取数时必须补 single-flight + 加锁**，两个 reviewer 均已提示）。10 个 Catch2 用例覆盖:排除名单匹配（含 query 误命中防回归）/2xx Bearer 注入/401 刷新重放(断言刷新体带 refresh_token、重放带新 token、缓存推进、回调触发)/排除路径不刷新/无 refresh 不刷新/刷新失败三态(非2xx·传输错·空access)/空 refresh 沿用旧/**单次重放不死循环**/非 401 透传（`tests/api_client_test.cpp`,全绿）
  - [ ] `UnauthorizedRedirect`（清 token → 跳登录 UI）/ `ErrorMessage`（吐司）—— SDK/UI 绑定，随 UI 接线做；`CaptchaChallenge` 归 Phase 6 专章
- 按 api.md 14 个章节实现 strongly‑typed API client（按需，UI 用到哪个补哪个）。**先铺最被复用的共享领域模型**，再按 UI 需求接 endpoint：
  - [x] **`SongInfo` 共享模型** `src/api/song.h`（header-only，对齐 api/user.h 模式）：移植 Flutter `shared/models/he_music_models.dart` 的 `SongInfo`/`LinkInfo`/`SongInfoArtistInfo`/`SongInfoAlbumInfo` + 全部 `_*` 辅助。这是发现/搜索/歌单/专辑响应**共用的列表原语**，Phase 3 播放层也从它派生 `hemusic://` hint。忠实复刻字段别名/类型容错：`name`→`title`、`artists`→`artist` 走 Dart `??`（**仅 null/缺失回退，present 空串不回退**——用 `if_contains`+`is_null` 区分"缺失"与"空值"，避免后端 `name:""` 被 title 静默救回掩盖数据缺陷）；`cover` 取 cover/pic/imgurl/image/thumb 首个非空；`platform` 空/缺失回退 `fallbackPlatform`；`album` 接对象或裸串；`links` 丢 `quality<=0 && url 空` 项；`parseSongList` 丢 id 或 name 为空的歌（无法播放/展示）；`sublist` 递归解析（前向声明打破互递归）；nullable `path/size/quality/alias` 用 `std::optional`，`_nullableInt` present-但-不可解析保留 0、缺失才 nullopt。数值串经既有 `json::toI64` 容错。10 个 Catch2 用例覆盖：全字段解析/**name??title 三态(缺失回退·空串不回退·present 优先)**/platform 回退/cover 别名优先级/artists(串数组·单串·artist 别名·空名丢弃)/album(对象·裸串·空→none)/links OR 过滤/list id|name 过滤/sublist 递归+平台透传/数值串容错+nullable 缺失（`tests/song_model_test.cpp`，全绿）
  - [x] **`PlaylistInfo` + `AlbumInfo` 共享模型** `src/api/playlist.h` / `src/api/album.h`（header-only）：移植 Flutter `PlaylistInfo`/`CategoryInfo`/`AlbumInfo`。两者是发现页"精选歌单/新专"、搜索、歌单广场、专辑/歌手详情、"我的"共用的集合卡片。复用 song.h 的 `model_detail::` 辅助（**`song_detail` 已重命名 `model_detail`**，因 cover/artists/links/countText/bool 等已被多模型共用，非 song 专属）；新增 `countText`（负数/null/不可解析→"0"，是展示字符串故绝不出负号）、`boolean`（真 bool 透传，否则 "true"/"1"）、`countTextCoalesce`。`PlaylistInfo`：song_count/play_count 容 camelCase 别名、`is_default` 走 Flutter `_int==1 || _bool` 双判定、`categories` 列表（空名丢弃、平台不透传——对齐 Flutter `CategoryInfo.fromMap` 不收 fallback）。`AlbumInfo`：artists 容 `artist` 别名、publish_time/play_count camelCase 别名、type(int)、is_finished(bool)。**关键契约（实读 `home_discover_api_client.dart` 校正）**：discover 的 `_parseList` 对 playlist/album/mv **不做 id/name 过滤**（只有 `_songs`/`_radios` 这类 model 内置 helper 过滤），故**不提供** `parsePlaylistList`/`parseAlbumList`（避免无调用方 + 避免发明 Flutter 没有的过滤）；list 映射留给 endpoint 层按需做。**另一关键点**：父对象自身的 `platform` 字段**不**透传给子 `songs`，只有 `fallbackPlatform` **入参**透传（Flutter `_songs(raw['songs'], fallbackPlatform)`）——真实 discover 流恒以 `fromMap(item, fallbackPlatform: platformId)` 调用。15 个 Catch2 用例（`tests/list_models_test.cpp`，全绿）：playlist 全字段+categories/count camelCase+负数夹 0/is_default 四真三假/platform 回退；album 全字段+artist 别名/camelCase 别名/platform 回退
  - [x] **`MvInfo` 模型 + `/v1/page/discover` 页面解析** `src/api/mv.h` / `src/api/discover.h`（header-only）：`MvInfo` 移植 Flutter（links/cover/play_count camelCase 别名/type/duration/platform 回退）。`parseDiscoverPage(body, platformId)` → `DiscoverPage{newSongs, newAlbums, featuredPlaylists, featuredMvs}`，对齐 `home_discover_api_client.dart` 的四段 `new_songs`/`new_albums`/`featured_playlists`/`featured_mvs`，每段以选中 platformId 作为 item 的 fallback。**忠实复刻契约（统一走 `mapAll<T>` 模板 + 函数指针 `parseSongInfo`/`parseAlbumInfo`/`parsePlaylistInfo`/`parseMvInfo`）**：① **四段全部不过滤**——顶层 section 都按对象逐个映射、不丢 id/name 空者（= 数据源 `_parseList`）；id/name `where` 过滤**只存在于** Playlist/Album **内嵌**的 `_songs`，**不**上浮到顶层，故 `new_songs` 同样不过滤。（初版误用带过滤的 `parseSongList` 解析 `new_songs`,经 /review 第 1 轮 Codex+Antigravity 同时指出后改为 `mapAll(parseSongInfo)`。）② **非 object 数组项跳过**（Flutter `_asMap` 遇非 map 抛异常；纯解析器不抛,跳过而非产出空壳条目）；③ **刻意宽容 divergence**：Flutter `_parseList` 对缺失/非数组段会 **throw AppException**,纯解析器不抛——缺失/非数组段降级为空 vector（音乐浏览器渲染空/部分页比崩溃体面,调用方仍可判全空）。9 个 Catch2 用例（`tests/discover_test.cpp`,全绿）：MvInfo 全字段+thumb/playCount 别名+platform 回退；discover 四段全解析+fallback 透传；**四段均保留全部对象项不过滤**；**非对象项跳过不留空壳**；缺段/非数组降级空。**HTTP 接线**（GET `?platform=`，经 `ApiClient::send`）留给 Phase 4 UI（UI 负责平台选择 + `/v1/platforms`）
  - [x] **`PlatformInfo` 模型 + `/v1/platforms` 列表解析** `src/api/platforms.h`（header-only）：移植 Flutter `features/home/domain/entities/home_platform.dart` 的 **`HomePlatform`**（精简版：id/name/shortName/status/featureSupportFlag + `available()`/`supports()`），而非 `online_platform.dart` 的 `OnlinePlatform`——**按 Flutter 源码自身的分层取舍**：home/discover feature 用精简模型，`imageSizes`/`qualities` 只在 search/detail 的 `OnlinePlatform` 出现，留给其 Phase 5 消费方补，不提前铺。这是发现/排行/搜索顶部**平台选择 Tab** + 各 endpoint **按平台能力 gating** 的数据源。忠实复刻：`id`/`name` trim；`shortName` 取 `shortname` trim，空则回退 `name`；`status` 容字符串数字（`available()`=status==1）；`feature_support_flag` 是位掩码故用 `unsigned long long`（已定义能力位最高 1<<47，uint64 容得下），数字/数字串/否则 0，**负数夹 0**（`platform_detail::featureFlag` 本地解析，因 `json::toI64` 是有符号、语义不符）。`parsePlatformList` 解 `{"list":[...]}` 信封，**丢 id 或 name 为空的项**（Flutter `fromMap` 对其 throw，纯解析器降级跳过=对齐 `parseSongList` 立场），缺/非数组 `list` → 空 vector。9 个 Catch2 用例（`tests/platforms_test.cpp`，全绿）：全字段解析+supports 掩码/shortName 回退(缺·空白)/status·flag 字符串数字容错/flag 垃圾·负数夹 0/**1<<47 高位不丢**/status 缺失默认不可用/list 信封丢无效项(空id·空name·非对象)/缺·非数组 list 降级空。**HTTP 接线**（GET，经 `ApiClient::send`）留给 Phase 4 UI（平台选择）
  - [x] **`ArtistInfo` 共享模型** `src/api/artist.h`（header-only）：移植 Flutter `shared/models/he_music_models.dart` 的 `ArtistInfo`（id/name/cover/platform/description/mvCount/songCount/albumCount/alias，全 String，count 类为展示文本）。这是搜索 artist 结果 + 歌手详情页（Phase 5 #6 标题 + 歌曲/专辑/MV Tab）共用的实体。复用 song.h 的 `model_detail::`（cover/countText/countTextCoalesce/coalesce）。**关键契约**：① `mv_count ?? mvCount ?? video_count` 三键 `??` 链——`coalesce(o,"mv_count","mvCount")` 后若结果 nullptr/`is_null` 再退 `video_count`，忠实复刻 present-but-null 继续下传（不停在 null）；song_count/album_count 走既有双键 `countTextCoalesce`；count 负数/垃圾夹 "0"（展示串绝不出负号）。② platform 空回退 `fallbackPlatform`，非对象输入产出全空 ArtistInfo（platform 仍取 fallback）。**与 album.h/playlist.h 一致：只提供单项 `parseArtistInfo`,不提供 list parser**——Flutter 顶层无带过滤的 ArtistInfo list helper（`_artists` 是 song 子艺人 `SongInfoArtistInfo`,类型不同）,list 映射留给 search endpoint 层。7 个 Catch2 用例（`tests/artist_model_test.cpp`,全绿）：全字段解析/mvCount 三键链(优先级·present-null 继续下传·全缺→"0")/count camelCase 别名+负数·垃圾夹 0/cover 别名优先级/platform 回退(缺·present 覆盖)/非对象→全空。**HTTP 接线**(GET，经 `ApiClient::send`)随 search/artist endpoint typed client 补
  - [x] **endpoint 响应解析层（纯逻辑，HTTP 接线留给 UI）**：按 discover.h 立场——api/ header-only 纯解析器，`{platform}` / `keyword` / `id`+`title` 等请求上下文作入参，**HTTP（GET 经 `ApiClient::send` 走刷新拦截）随 Phase 4/5 UI 接**。统一沿用既有宽容策略：Flutter `throw` 处降级为空 vector / 跳过非对象项；歌曲列表沿用 `parseSongList`「丢空 id/name 不可播条目」。已落地五块：
    - [x] **Radio** `src/api/radio.h`：`RadioInfo`{name/id/cover/platform} + `RadioGroupInfo`{name/radios/platform}。`parseRadioInfo`/`parseRadioList`（`_radios` 过滤 id&name 空者）/`parseRadioGroupInfo`（子 radios 继承 group 解析后的 platform 作 fallback）/`parseRadioGroups`（`/v1/radios`→`{groups}`，缺/非数组降级空）/`parseRadioSongs`（`/v1/radio/songs`→`{list}`→`parseSongList`）。5 用例（`tests/radio_test.cpp`）
    - [x] **Playlist 详情** `src/api/playlist_detail.h`：`/v1/playlist` meta + `/v1/playlist/songs`（两次调用，分开解析、UI 拼合）。`parsePlaylistDetailInfo(body,id,platform,title)`→`PlaylistInfo`（songs 留空）+ `parsePlaylistSongs(body,platform)`。**详情专属 count 语义**（区别于共享 `parsePlaylistInfo`）：song_count/play_count 保留原始裸串（缺失为 `""` 而非夹 "0"）、song_count 多 `trackCount` 别名、creator 空→`"-"`、name 空→回退请求 title、id/platform 取自请求。6 用例（`tests/playlist_detail_test.cpp`）
    - [x] **Album 详情** `src/api/album_detail.h`：单接口 `/v1/album`（歌曲内嵌）。`parseAlbumDetailInfo(body,id,platform,title)`→`AlbumInfo`（含 songs）。`album_detail::resolveSongList` 复刻 `_resolveSongList`（songs/tracks/song_list/songlist + 嵌套 data/detail/album）；详情专属：artists **只读 `artists` 键**（无 `artist` 别名，区别于共享 `parseAlbumInfo`）、song_count 缺失回退**解析到的歌曲数**、publish_time 多 `createTime` 别名、play_count 裸串。7 用例（`tests/album_detail_test.cpp`）
    - [x] **Ranking** `src/api/ranking.h`：`RankingPreviewSong`/`RankingInfo`/`RankingGroup`/`RankingDetail` 四模型。`/v1/rankings`→`parseRankingGroups`（`{groups}`，group 名空→UTF-8 字节 `"\xE6\xA6\x9C\xE5\x8D\x95"`=榜单，避开 tests 目标无 `/utf-8`）；`/v1/ranking`→`parseRankingDetail(body,platform,fallbackId)`（info+songs+hasMore/lastId/totalCount/description）。`parseRankingInfo` id 空→fallbackId→`"-"`、preview 取前 3 首（name + `songArtistText` 拼接 artist，各空→`"-"`）。`ranking_detail::readBoolKeys`/`readIntKeys` 复刻 `_readBool`/`_readInt`（int>0、numeric-string 全匹配、double/非数字串跳过）。**songs 用请求 platform 作 fallback（非 body 的榜单 platform）**，对齐 Flutter `_parseSongs(payload, platform)`。新增 `songArtistText(SongInfo)`=`SongInfo.artist` getter（names join `" / "`，空→`"-"`）入 song.h。6 用例（`tests/ranking_test.cpp`）
    - [x] **Search** `src/api/search.h`：综合 `/v1/search`→`ComprehensiveSearchResult`{keyword + `bestMatch` + 5 段 `SearchSection<T>`{items/hasMore/totalCount}}；分类 `/v1/{type}/search`→`parse{Song,Playlist,Album,Artist,Video}Search`（`{list}` typed）。**段类型固定故 eager typing**（Flutter 存裸 map 懒解析，C++ 直接类型化，等价）；best_match 用 `std::variant<Song/Playlist/Album/Mv/Artist Info>` + resourceType（oneof nested `map[resourceType]`，primary 先 recommendations 后，unknown/空 data 丢弃）。`search_detail::sectionList` 复刻 `_extractList`（list/items/data + 一层嵌套）、`readBoolField`/`readIntField`（含 `data` 嵌套回退）。keyword 取 body `key`（present 含空串即用，对齐 `?? null`），缺失才回退请求 keyword。条目映射复用 `discover_detail::mapAll`（保留全部对象项不过滤，对齐 searchMusic 裸 list）。**段/best-match 用请求 platform 作 fallback**——刻意优于 Flutter `_safePlatform` 返回 `"-"`（仅喂展示串、对播放是无效 platform key）。6 用例（`tests/search_test.cpp`）
  - [ ] 其余 endpoint（comment、favourite、user_playlist、mv/url、lyric、captcha 等）随 Phase 4/5/6 UI 逐个补，统一经 `ApiClient::send` 走刷新拦截
- [ ] `tests/api_client_test.cpp`：用 hurl / mock server 跑契约测试（已建文件,先覆盖刷新拦截器 + SongInfo/PlaylistInfo/AlbumInfo 模型；endpoint 级 GET 契约随 typed client 逐个补）

**交付**：单元测试覆盖 §5~14 的主要 GET 接口。

### Phase 3 — 播放管线（4~6 天）

> **排期调整（2026-06-21，用户决策）**：本阶段所有**需真实 foobar2000 SDK** 的项（`hemusic_input`、`playlist_writer`、`album_art`，以及依赖它们的手粘 `hemusic://` 实机验证）**推迟到 UI（Phase 4/5）之后再做**，统一在 UI 接线阶段连同 SDK 服务注册一并落地。纯逻辑、可单测的两块（`hemusic://` URL 编解码、`url_resolver`）已在本阶段先行完成，为后续 SDK 接线铺好地基。下方各 SDK 项保留在此仅作内容归档，执行顺序见 §6 里程碑。

- [x] **`hemusic://` URL 编解码**（纯逻辑，先于 SDK 接线）：`src/playback/hemusic_url.h`（header-only）`SongRef`{id/platform/hintTitle/hintArtist/hintAlbum/hintDuration/hintCover} + `buildSongUrl`/`parseSongUrl`。这是 `hemusic_input`/`url_resolver`/`album_art` 共用的 playable-location 原语：曲目入列时存自描述 URL，播放瞬间才解析直链，`get_info` 先用内嵌 hint 渲染列表行。**URL 即 fb2k 唯一 track key**，故 `buildSongUrl` 字段定序（id→platform→hint_title→hint_artist→hint_album→hint_duration→hint_cover），同一 `SongRef` 恒输出 byte 一致;空 hint/0 时长省略。query 值走 percent-encode;只有 **id+platform 必填**（缺任一无法解析直链，`parseSongUrl` 返回 nullopt——对齐 `parseSongList` 丢不可播条目的立场）;scheme 大小写不敏感、authority 须精确等于 `song`、未知 key 忽略、值内裸 `=` 按首个 `=` 切分保留(容手粘 URL)、空 token 跳过。配套 **percent 编解码抽到共享 `src/net/url_codec.h`**（`hemusic::url::percentEncode`/`percentDecode`，header-only）：原 `auth/login_flow` 的 `percentEncode` 已搬来此处（≥2 调用方,符合"重复即抽取"）,`login_flow`/`buildAuthUrl` 改用 `url::percentEncode`(局部变量 `url`→`out` 避免遮蔽命名空间);新增 `percentDecode`(空格编码恒为 `%20` 故 `+` 保留字面;畸形 `%`/`%4`/`%zz` 原样透传;字节级解码保 UTF-8 往返)。15 个 Catch2 用例(`tests/url_codec_test.cpp` 4 + `tests/hemusic_url_test.cpp` 11,全绿)：percentDecode 反演/`+`字面/畸形透传/任意字节(含 UTF-8)往返;build 定序+编码+省略空 hint/0 时长;parse 全字段往返(含 `?&=%`/UTF-8/裸`=`)/拒非 hemusic scheme/拒非 song authority(含 `songs` 前缀)/scheme 大小写不敏感/必填 id+platform/junk 时长夹 0+未知 key 忽略/空 token 跳过。**SDK 接线**(`input_service::open` 调 `parseSongUrl` → `/v1/song/url`;`SongInfo`→`SongRef` 的 artist 拼接等生产侧映射)随下面各 SDK 项补
- [ ] **（⏭ UI 之后）** `hemusic_input` 实现 `input_service` + `input_decoder_v3`，注册 `hemusic` 协议
- [x] `url_resolver`：30s LRU 缓存 + 重取（纯逻辑，注入 transport + clock，可单测，不碰 SDK，编入 hemusic_core）`src/playback/url_resolver.{h,cpp}`。`resolve(SongRef, quality=320, format="mp3")` → `SongUrlResolution{url,format}`：先查缓存（命中且新鲜直接返回），未命中/过期则 GET `/v1/song/url`。**契约对齐 Flutter**（`online_api_client.fetchSongUrl`/`he_audio_handler._fetchSongUrl(WithRetry)`/`online_controller.resolveSongUrl`）：请求 query `id/platform/quality(=320)/format(=mp3)` + header `User-Agent: heAudioUserAgent`（PLAN R2，后端对直链 gating UA）；响应 `{url,format}`，url trim 后空则失败，format 缺回退 requested 再回退 `mp3`；**重试**最多 3 次，可重试条件=传输错(`!ok`)/`status>=500`/`2xx 但 url 空`，**4xx 立即失败不重试**（401 刷新由注入的 transport=`ApiClient::send` 上游处理，403 captcha 归 Phase 6）。**缓存**：`std::list`+`unordered_map` 实现 LRU，key=`id\x1fplatform\x1fquality\x1fformat`（`\x1f` 不会出现在任何分量，拼接无歧义），TTL 默认 30s（直链分钟级过期）、容量默认 128；`now-ts<=ttl` 为新鲜（命中 splice 到队首），过期惰性删除；超容量淘汰队尾 LRU。失败的 resolve **不入缓存**。`invalidate(ref[,quality,format])` 供 input 层在播放缓存直链遇 401/403 失效时强制重解析（"401/403 重取"路径）；id/platform 空直接 nullopt 不发请求（对齐 Flutter `_validateNotEmpty`）。**不线程安全**（调用方串行化，single-flight 随 Phase 4 并发补——与 `ApiClient` 同口径）。配套 **URL 拼接 `url::buildUrl(base,path,query)` 抽到 `net/url_codec.h`**（≥2 调用方：`buildAuthUrl` 现委托它 + resolver），key/value 均 percent-encode。12 个 Catch2 用例（`tests/url_resolver_test.cpp`，全绿）：请求 query+UA 头/新鲜命中不重发(TTL 边界 `<=`)/过期重取/invalidate 强制重取/quality 入 key/5xx 重试恢复·3 次耗尽不入缓存/传输错重试/4xx 不重试/2xx 空 url 重试后失败/空 id|platform 零请求/LRU 淘汰最久未用+命中刷新；外加 `parseSongUrlResponse` 单测(url trim·缺 url→nullopt·format 回退链·非 JSON→nullopt)。**SDK 接线**（生产 transport=`ApiClient::send`、clock=steady_clock、input 层 open→resolve→委托解码、播放失败→invalidate）随 `hemusic_input` 项补
- [ ] **（⏭ UI 之后）** `playlist_writer`：把一个 `vector<SongInfo>` 一次性加入活动播放列表（用 `playlist_manager::v3::playlist_add_locations`）
- [ ] **（⏭ UI 之后）** `album_art`：实现 `album_art_extractor`，从 `hemusic://` URL 抽 id/platform，去 `/v1/song/cover`
- [ ] **（⏭ UI 之后）** 在 foobar2000 控制台用 `Add Location...` 手动粘 `hemusic://song?id=X&platform=Y` 验证

**交付**：在 foobar2000 里手动粘一条 `hemusic://` URL，能播完整首歌且封面正确。

### Phase 4 — UI 基础设施（4~5 天）

> **开工前**：用 Chrome CDP 连到浏览器打开 [`y.wjhe.top`](https://y.wjhe.top/)，观察发现页的 DOM 布局、分区结构、卡片/列表尺寸比例与交互行为，作为 `discover_page` 及后续各页的布局基准（见 §3.5）。配色仍跟 fb2k 主题，不取官网色板。

- [ ] 注册 Default UI 的 `ui_element_v2`，单个 GUID 作为主面板（用户可在 fb2k layout editor 拖出来）
- [ ] `theme`：从 fb2k 拿主题色 / 字体，组装 HE-Music 视觉常量（圆角、间距、卡片尺寸）
- [ ] `d2d`：Direct2D 工厂 / 渲染目标的生命周期管理（WM_PAINT → BeginDraw/EndDraw、设备丢失重建）
- [ ] `image_cache`：URL → IWICBitmap LRU，封面下载走业务 HTTP 队列；占位图、加载动画
- [ ] `nav`：页面栈数据结构 + 顶部面包屑（"歌单 > XX 的歌单"）
- [ ] 最小 `discover_page` 跑通：能渲染发现页第一段（新歌列表）

**交付**：foobar2000 layout editor 里 Add Panel → "HE-Music"，能看到发现页的新歌列表，配色随 fb2k 主题切换。

### Phase 5 — UI 业务页面（10~14 天）

按优先级，每一页都需要：列表 / 卡片虚拟滚动、空态、加载态、错误态、入列按钮。

1. **登录对话框**（含 Linux.do 引导 + "等待授权"进度环 + 取消）
2. **发现页**（`/v1/page/discover`，4 段：新歌 / 新专 / 精选歌单 / 精选 MV）
3. **搜索**（顶栏输入框 + 联想浮层；综合页 + 分类切换 Tab）
4. **歌单详情**（顶部 banner + 歌曲列表 + "全部加入播放列表"按钮）
5. **专辑详情**（同上）
6. **歌手详情**（标题 + Tab：歌曲 / 专辑 / MV，翻页加载）
7. **排行榜 / 电台 / 歌单广场**（共用卡片网格组件）
8. **"我的" Tab**：收藏（4 类）+ 自建歌单（CRUD）

视觉对齐：**每页实现前用 Chrome CDP 观察官网 [`y.wjhe.top`](https://y.wjhe.top/) 对应页面的布局 / 元素 / 交互行为作为基准**（结构以官网为准，移动端 Flutter 布局仅作参考）；Flutter 工程 [`../HE-Music-Flutter/lib/features/`](../HE-Music-Flutter/lib/features/) 的 `presentation/pages` 用来对照**信息层级与字段映射**。颜色字体跟 fb2k 主题走，**不**照搬官网色板，也**不**复刻 Material / 网页端阴影、涟漪、粗体强调色。

**交付**：登录后，能从 UI 里把任意歌单/专辑加进 foobar2000 播放列表并播放。

### Phase 6 — 验证码与异常 UX（2~3 天）

- [ ] `captcha_dlg`：Direct2D 绘制底图 + 拼图块，Win32 trackbar 控制 X 偏移
- [ ] 拦截器与 UI 主线程对接：业务线程被阻塞 → marshal 到 UI 线程弹 modal → 通过后释放阻塞重放
- [ ] 网络断线 / token 过期 / 接口失败：fb2k 状态栏文字提示（`console::print` + `popup_message_v3`）

### Phase 7 — 元数据增强与缓存（2 天）

- [ ] 歌曲详情批量补全（api.md §7.3 `/v1/song`）
- [ ] 持久化封面磁盘缓存（`%APPDATA%\foobar2000\user-components\foo_hemusic\cache\covers\`）
- [ ] foobar2000 metadb 持久化 hint 元数据，避免每次启动都打 `/v1/song/detail`

### Phase 8 — 打包发布（1~2 天）

- [ ] 双架构构建脚本：`build-x86.bat` / `build-x64.bat`
- [ ] `.fb2k-component` 打包（实质是 zip，包含 `foo_hemusic.dll` + x64 子目录 `foo_hemusic.dll`）
- [ ] README：安装、配置、Linux.do 授权步骤、常见问题
- [ ] 可选：签名 / 自动更新（暂不做）

**交付**：一个可直接拖到 foobar2000 安装的 `.fb2k-component`。

---

## 6. 里程碑与时间表（理想估时）

| 里程碑 | 内容 | 累计天数 |
|---|---|---|
| M1 | Phase 0~1 完成，能登录 | ~5 天 |
| M2 | Phase 2 完成 + Phase 3 纯逻辑（URL 编解码 / url_resolver）完成 | ~15 天 |
| M3 | Phase 4 完成，UI 基础设施跑通 | ~20 天 |
| M4 | Phase 5 完成 + Phase 3 SDK 项（hemusic_input / playlist_writer / album_art）接线，MVP 全功能可用（含手动播放 hemusic:// URL 实机验证） | ~33 天 |
| M5 | Phase 6~8 完成，正式可发布版 | ~38 天 |

时间为单人全职估算。原生 UI 的 Phase 4~5 比 WebView 方案多 ~4 天，但节省了 webui 工程链路与 IPC 调试时间，整体接近。

> **Phase 3 SDK 项排期**（2026-06-21 用户决策）：`hemusic_input`/`playlist_writer`/`album_art` 等需真实 fb2k SDK 的项推迟到 UI（Phase 4/5）之后，与 UI 接线阶段一并落地，故原 M2「能手动播放 hemusic:// URL」拆分——纯逻辑地基归 M2，SDK 接线 + 实机播放验证归 M4。

---

## 7. 风险与未决问题（开工前需明确）

| # | 风险 / 问题 | 影响 | 处理 |
|---|---|---|---|
| R1 | libPPUI / CListControl 的虚拟滚动是否足够流畅承担长列表（如歌手所有歌曲 thousands+）？ | 影响 UI 流畅度 | Phase 4 用真实数据压测，必要时降采样 / 分页 |
| R2 | `/v1/song/url` 直链是否会带音频流量限速 / 跨域 / Referer / UA 校验？ | 影响 `hemusic_input` 委托播放 | Phase 3 用真实账号实测，必要时在 WinHTTP 加 `User-Agent: heAudioUserAgent`（对齐 Flutter `HeAudioHandler._fetchSongUrl`） |
| R3 | foobar2000 v1 是否要兼容？ | 决定 SDK 调用面 | **默认只支持 v2**（SDK 2025‑03‑07 已主推 v2） |
| R4 | 32 位用户占比 | 决定是否双构建 | 先只发 x64，根据反馈决定补 x86 |
| R5 | 滑块验证码是否经常触发？是否有 IP 维度限制？ | 影响登录体验 | Phase 6 验收阶段实测频率 |
| R6 | 商标 / 许可：foobar2000 SDK 商业用途条款 | 分发合规 | 阅读 [`sdk-license.txt`](../foobar2000-SDK-2025-03-07/sdk-license.txt)，确认免费分发条款 |
| R7 | HE-Music API 字段是否会变？ | 维护成本 | 客户端做版本号探测（若后端提供），否则锁定字段别名兼容（参考 Flutter 工程的 `fromMap`） |
| R8 | HE-Music 后端是否会限制非官方客户端？ | 可能被封 UA / token | `User-Agent` 注意伪装策略；与 Flutter 一致，必要时在登录界面提示用户该客户端非官方 |

---

## 8. 立即可执行的下一步

按顺序：

1. **环境准备**：装 VS2022（含 MSVC + ATL + WinSDK 10.0.22621）、CMake 3.27+。（**已无需 Node**——UI 全原生）
2. **Phase 0 开工**：先把 `foo_sample` 编出来加载到 foobar2000，验证工具链。这一步如果走不通，后面所有计划都要返工。
3. **抓包对照**：用真实账号在 Flutter app 跑一次完整登录 + 播放，抓 HTTP 流量做基线（验证 `device_info` 字段、`User-Agent`、`/v1/song/url` 直链是否带 Referer 等隐式约束）。这一步在 Phase 1 / Phase 3 之前做最省事。
4. **libPPUI 摸底**：读一遍 SDK 里 [`libPPUI/CListControl.h`](../foobar2000-SDK-2025-03-07/libPPUI/) 和 `foo_sample` 中用到 CListControl 的范例，先在 Phase 0 试出一个能渲染一万行假数据的 ui_element 面板。如果这一步 libPPUI 不够用，需要追加自研列表组件的工作量。
