# foo_hemusic 开发计划

foobar2000 组件 (component)，把 HE-Music 后端做成"接入式音乐源"。UI 风格对齐 [HE-Music-Flutter](../HE-Music-Flutter/)，鉴权走 Linux.do，播放时即时解析直链。

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
| UI 实现 | **Direct2D + DirectWrite 自绘** + 必要处用 Win32 控件（输入框、按钮） | 列表/卡片自绘，主题对齐 fb2k 当前色系；视觉对齐 HE-Music 仅在配色 / 排版上贴齐 |
| 列表组件 | SDK 自带的 `libPPUI` 提供的 `CListControl` / `CListControlOwnerData` | 已有虚拟滚动、双缓冲、皮肤适配，省一大块基建 |
| HTTP | **WinHTTP** | SDK `http_client` 功能不够；所有带 token 的请求统一走 WinHTTP |
| JSON | **Boost.JSON**（vcpkg `boost-json`） | 分离编译，编译开销小于 nlohmann 单头；`if_contains` 适配 Flutter 多候选 key 别名兼容 |
| 依赖管理 | **vcpkg manifest**（`vcpkg.json`） | CMake 接 vcpkg toolchain 自动安装 boost-json / catch2 |
| 持久化 | foobar2000 `cfg_var` + Windows DPAPI（token） | token 不进明文 cfg |
| OAuth 回调 | **无需本地服务器**，HE-Music 后端接收回调，客户端轮询 `/v1/auth/status` | Flutter 已验证可行 |
| 单元测试 | Catch2 | 与 SDK 解耦的纯业务层（API client / URL resolver） |

> UI 宿主已确定走 foobar2000 原生 ui_element，不引入 WebView2 / Sciter / CEF。视觉上"对齐 HE-Music-Flutter"理解为：用 fb2k 当前主题色系 + HE-Music 的内容结构（卡片、列表分区、入列按钮位置），不追求像素级复刻 Flutter Material 风格。

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

### 3.5 UI 主题与视觉对齐

foobar2000 主程序有用户可切换的明暗主题，组件 UI **必须跟随**，否则会出现"亮色窗口里嵌一块暗色面板"的尴尬。

- 颜色：通过 SDK 的 `colours_api`（v2 中名为 `cui::colours::manager_instance_v3` 或 DUI 侧 `ui_config_manager`）获取当前主题前景 / 背景 / 选中色
- 字体：通过 `font_manager_v2` 拿主题字体，列表 / 标题用不同 size
- HE-Music 视觉特征只在以下三处保留：
  - 卡片化布局（封面在左，文字在右，圆角 6px）
  - 列表分区标题（"为你推荐"、"新歌速递"等）
  - 入列按钮位置（每行右侧悬停显示）
- 不引入 Material design 阴影 / 涟漪动画 / 粗体强调色——这些会跟 fb2k 风格冲突

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
  - [ ] 传输层 `src/net/http_client.{h,cpp}`：WinHTTP 同步请求（GET/POST + JSON body + header 注入 `authorization: Bearer`），connect 20s / read 30s
  - [ ] 编排：`GET /v1/auth/providers` → `POST /v1/auth/session` → `ShellExecuteW(url)` → 按 `check_interval` 轮询 `/v1/auth/status` → `/v1/auth/result`（可取消）
- [ ] `token_store`：DPAPI 加密落地到 `%APPDATA%\foobar2000\user-components\foo_hemusic\token.bin`
- [ ] 临时 UI（Win32 dialog，含"正在等待授权"进度文字 + 取消按钮）发起登录流，**不依赖 WebView2 完工**
- [ ] `GET /v1/user/info` 验证 token

**交付**：foobar2000 内点登录 → 系统浏览器打开 Linux.do → 授权后回 foobar2000 状态变 success → 控制台打印当前 HE-Music 用户名。

### Phase 2 — API 客户端层（3~5 天，可与 P1 并行）

- [ ] `http_client`：WinHTTP 同步 + 异步两套 API，统一头注入，超时 connect 20s / read 30s
- [ ] 拦截器链：`AuthToken → UnauthorizedRedirect → CaptchaChallenge → ErrorMessage`
- [ ] 按 api.md 14 个章节实现 strongly‑typed API client（按需，UI 用到哪个补哪个）
- [ ] `tests/api_client_test.cpp`：用 hurl / mock server 跑契约测试

**交付**：单元测试覆盖 §5~14 的主要 GET 接口。

### Phase 3 — 播放管线（4~6 天）

- [ ] `hemusic_input` 实现 `input_service` + `input_decoder_v3`，注册 `hemusic` 协议
- [ ] `url_resolver`：30s LRU 缓存 + 401/403 重取
- [ ] `playlist_writer`：把一个 `vector<SongInfo>` 一次性加入活动播放列表（用 `playlist_manager::v3::playlist_add_locations`）
- [ ] `album_art`：实现 `album_art_extractor`，从 `hemusic://` URL 抽 id/platform，去 `/v1/song/cover`
- [ ] 在 foobar2000 控制台用 `Add Location...` 手动粘 `hemusic://song?id=X&platform=Y` 验证

**交付**：在 foobar2000 里手动粘一条 `hemusic://` URL，能播完整首歌且封面正确。

### Phase 4 — UI 基础设施（4~5 天）

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

视觉对齐：参考 [`../HE-Music-Flutter/lib/features/`](../HE-Music-Flutter/lib/features/) 各 feature 的 `presentation/pages` 抓**布局 / 信息层级**，颜色字体跟 fb2k 走。**不**复刻 Material 阴影、涟漪、粗体强调色。

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
| M2 | Phase 2~3 完成，能手动播放 hemusic:// URL | ~15 天 |
| M3 | Phase 4 完成，UI 基础设施跑通 | ~20 天 |
| M4 | Phase 5 完成，MVP 全功能可用 | ~33 天 |
| M5 | Phase 6~8 完成，正式可发布版 | ~38 天 |

时间为单人全职估算。原生 UI 的 Phase 4~5 比 WebView 方案多 ~4 天，但节省了 webui 工程链路与 IPC 调试时间，整体接近。

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
