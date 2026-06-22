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

> **工作流程（每个 `HEMUSIC-N` 条目通用）**：
> 1. **详细 scope / 上下文**：见对应 JIRA ticket 描述（点链接跳转），本文件只留一行简述作指针。
> 2. **任务完成后**：
>    a. 把该条目从 PLAN.md **移动到 [`DONE.md`](./DONE.md)**（保留 JIRA 链接，并在条目后追加一段落地总结：实际改动 / 关键决策 / 测试覆盖 / 实机验证状态）。
>    b. 把 JIRA ticket 状态置为 **「完成」**（`jira transitions HEMUSIC-N` 查可用 id，再 `jira transition HEMUSIC-N <id>`；或在 Web 端点完成）。
>    c. 可选：在 ticket 加一条 comment 引用 commit / DONE.md 段落，便于追溯。

> Phase 0（工程脚手架）与 Phase 1（鉴权链路）已完成，详见 [`DONE.md`](./DONE.md)。Phase 0 的 `.fb2k-component` 打包脚本一项推迟到 Phase 8 一并做。

### Phase 2 — API 客户端层 · 剩余条目

> 已完成部分见 [`DONE.md`](./DONE.md)：http_client 同步层、`AuthToken` + 401 刷新拦截、`Session` 进程级会话层，以及 SongInfo/PlaylistInfo/AlbumInfo/MvInfo/PlatformInfo/ArtistInfo 共享模型 + Radio/PlaylistDetail/AlbumDetail/Ranking/Search 五块 endpoint 响应解析。

- [ ] [HEMUSIC-1](https://jira.ismisv.com/browse/HEMUSIC-1) — `UnauthorizedRedirect` / `ErrorMessage` 拦截器（SDK/UI 绑定，随 UI 接线；`CaptchaChallenge` 归 Phase 6）
- [ ] [HEMUSIC-2](https://jira.ismisv.com/browse/HEMUSIC-2) — 其余 endpoint（comment / favourite / user_playlist / mv-url / lyric / captcha 等）随 Phase 4/5/6 UI 逐个补
- [ ] [HEMUSIC-3](https://jira.ismisv.com/browse/HEMUSIC-3) — `tests/api_client_test.cpp` 契约测试覆盖（已覆盖刷新拦截 + 共享模型，endpoint 级随 typed client 补）

**交付**：单元测试覆盖 §5~14 的主要 GET 接口。

### Phase 3 — 播放管线 · 剩余条目

> **排期调整（2026-06-21，用户决策）**：本阶段所有**需真实 foobar2000 SDK** 的项（`hemusic_input`、`playlist_writer`、`album_art`，以及依赖它们的手粘 `hemusic://` 实机验证）**推迟到 UI（Phase 4/5）之后再做**，统一在 UI 接线阶段连同 SDK 服务注册一并落地。纯逻辑可单测的两块（`hemusic://` URL 编解码、`url_resolver`）已在本阶段先行完成，详见 [`DONE.md`](./DONE.md)。

- [ ] [HEMUSIC-4](https://jira.ismisv.com/browse/HEMUSIC-4) — **（⏭ UI 之后）** `hemusic_input`：`input_service` + `input_decoder_v3`，注册 `hemusic` 协议
- [ ] [HEMUSIC-5](https://jira.ismisv.com/browse/HEMUSIC-5) — **（⏭ UI 之后）** `playlist_writer`：批量 `playlist_manager::v3::playlist_add_locations`
- [ ] [HEMUSIC-6](https://jira.ismisv.com/browse/HEMUSIC-6) — **（⏭ UI 之后）** `album_art_extractor`：从 `hemusic://` URL 抽 id/platform → `/v1/song/cover`
- [ ] [HEMUSIC-7](https://jira.ismisv.com/browse/HEMUSIC-7) — **（⏭ UI 之后）** 手动粘 `hemusic://song?id=X&platform=Y` 在 foobar2000 实机播放验证

**交付**：在 foobar2000 里手动粘一条 `hemusic://` URL，能播完整首歌且封面正确。

### Phase 4 — UI 基础设施（已完成，详见 [`DONE.md`](./DONE.md)）

> **Phase 4 收尾 /review 遗留 should-fix（2026-06-22，Codex+Antigravity 双评，已判 Phase 4 无必修；作为 Phase 5 已知输入留此）**：
> 1. ~~[HEMUSIC-8](https://jira.ismisv.com/browse/HEMUSIC-8) — 登录成功后主面板自动刷新（Session listener）~~ ✅ 已完成，见 [`DONE.md`](./DONE.md#phase-5--ui-业务页面)
> 2. [HEMUSIC-9](https://jira.ismisv.com/browse/HEMUSIC-9) — `discover_page` 缓存 `IDWriteTextFormat` / brush / 预转宽字符串（随虚拟滚动组件落地）
> 3. [HEMUSIC-10](https://jira.ismisv.com/browse/HEMUSIC-10) — device-loss 时 `InvalidateRect` 不被 `ValidateRect` 撤销（需改 `HwndCanvas::paint` 返回需重绘标志）
> 4. [HEMUSIC-11](https://jira.ismisv.com/browse/HEMUSIC-11) — 区分 2xx 解析失败与真空空段（endpoint 接线层按需加判别，勿改 api/ 宽容立场）

### Phase 5 — UI 业务页面（10~14 天）

按优先级，每一页都需要：列表 / 卡片虚拟滚动、空态、加载态、错误态、入列按钮。

1. ~~[HEMUSIC-12](https://jira.ismisv.com/browse/HEMUSIC-12) — **登录对话框**（替换临时 login_dlg；Linux.do 引导 + 等待授权 + 取消）~~ ✅ 已完成（表现层 D2D 化 + `themeFromHost()` 主题融合 + 旋转进度环 + 自绘取消按钮；线程模型不动），详见 [`DONE.md`](./DONE.md#phase-5--ui-业务页面)。
   - [ ] [HEMUSIC-32](https://jira.ismisv.com/browse/HEMUSIC-32) — **UI 全局 per-monitor DPI (PMv2) 感知**（HEMUSIC-12 /review 拆出；进程级 PMv2 声明 + 把 per-window DPI 贯通共享 `HwndCanvas`，影响 main_panel/discover_page/login_dlg）
2. ~~[HEMUSIC-13](https://jira.ismisv.com/browse/HEMUSIC-13) — **发现页** 完整 4 段（新歌 / 新专 / 精选歌单 / 精选 MV）~~ ✅ 已完成（4 段 + 网格布局 + 纵向滚动；封面占位框），详见 [`DONE.md`](./DONE.md#phase-5--ui-业务页面)。
   - ~~[HEMUSIC-31](https://jira.ismisv.com/browse/HEMUSIC-31) — **发现页封面异步加载**~~ ✅ 已完成（ImageCache 加固 + cover_cache initquit 单例 + paint 接线真实封面 + hemusic_ui_core 测试库），详见 [`DONE.md`](./DONE.md#phase-5--ui-业务页面)。
3. [HEMUSIC-14](https://jira.ismisv.com/browse/HEMUSIC-14) — **搜索**（顶栏输入 + 联想浮层 + 综合 / 分类 Tab）
4. [HEMUSIC-15](https://jira.ismisv.com/browse/HEMUSIC-15) — **歌单详情**（banner + 歌曲列表 + 全部入列）
5. [HEMUSIC-16](https://jira.ismisv.com/browse/HEMUSIC-16) — **专辑详情**（同歌单详情结构）
6. [HEMUSIC-17](https://jira.ismisv.com/browse/HEMUSIC-17) — **歌手详情**（歌曲 / 专辑 / MV Tab，分页加载）
7. [HEMUSIC-18](https://jira.ismisv.com/browse/HEMUSIC-18) — **排行榜 / 电台 / 歌单广场**（共用卡片网格组件）
8. [HEMUSIC-19](https://jira.ismisv.com/browse/HEMUSIC-19) — **「我的」Tab**：收藏 4 类 + 自建歌单 CRUD（含登出 UI；落地登出时一并修 [HEMUSIC-30](https://jira.ismisv.com/browse/HEMUSIC-30) 登出期 in-flight fetch 覆盖登出态）

视觉对齐：**每页实现前用 Chrome CDP 观察官网 [`y.wjhe.top`](https://y.wjhe.top/) 对应页面的布局 / 元素 / 交互行为作为基准**（结构以官网为准，移动端 Flutter 布局仅作参考）；Flutter 工程 [`../HE-Music-Flutter/lib/features/`](../HE-Music-Flutter/lib/features/) 的 `presentation/pages` 用来对照**信息层级与字段映射**。颜色字体跟 fb2k 主题走，**不**照搬官网色板，也**不**复刻 Material / 网页端阴影、涟漪、粗体强调色。

**交付**：登录后，能从 UI 里把任意歌单/专辑加进 foobar2000 播放列表并播放。

### Phase 6 — 验证码与异常 UX（2~3 天）

- [ ] [HEMUSIC-20](https://jira.ismisv.com/browse/HEMUSIC-20) — `captcha_dlg` 滑块验证码对话框（Direct2D + Win32 trackbar）
- [ ] [HEMUSIC-21](https://jira.ismisv.com/browse/HEMUSIC-21) — 验证码拦截器与 UI 主线程对接（marshal 到 UI 弹 modal + 通过后重放）
- [ ] [HEMUSIC-22](https://jira.ismisv.com/browse/HEMUSIC-22) — 网络 / token 过期 / 接口失败 UX 提示（`console::print` + `popup_message_v3`）

### Phase 7 — 元数据增强与缓存（2 天）

- [ ] [HEMUSIC-23](https://jira.ismisv.com/browse/HEMUSIC-23) — 歌曲详情批量补全 `/v1/song`（api.md §7.3）
- [ ] [HEMUSIC-24](https://jira.ismisv.com/browse/HEMUSIC-24) — 持久化封面磁盘缓存（`%APPDATA%\foobar2000\user-components\foo_hemusic\cache\covers\`）
- [ ] [HEMUSIC-25](https://jira.ismisv.com/browse/HEMUSIC-25) — foobar2000 metadb 持久化 hint 元数据（免每次启动 `/v1/song/detail`）

### Phase 8 — 打包发布（1~2 天）

- [ ] [HEMUSIC-26](https://jira.ismisv.com/browse/HEMUSIC-26) — 双架构构建脚本 `build-x86.bat` / `build-x64.bat`
- [ ] [HEMUSIC-27](https://jira.ismisv.com/browse/HEMUSIC-27) — `.fb2k-component` 打包脚本（zip：`foo_hemusic.dll` + x64 子目录，含 Phase 0 推迟项）
- [ ] [HEMUSIC-28](https://jira.ismisv.com/browse/HEMUSIC-28) — README（安装 / 配置 / Linux.do 授权 / FAQ）
- [ ] [HEMUSIC-29](https://jira.ismisv.com/browse/HEMUSIC-29) — 可选：组件签名 / 自动更新（暂不做）

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
