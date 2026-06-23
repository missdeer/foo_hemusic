# foo_hemusic 已完成工作

> 已完成阶段与条目清单。每条仅保留标题 + 对应 JIRA issue 指针，详细实施记录见各 issue 评论。
> 尚未完成项见 [`PLAN.md`](./PLAN.md)。

---

## Phase 0 — 工程脚手架

- [x] [HEMUSIC-39](https://jira.ismisv.com/browse/HEMUSIC-39) — CMakeLists 从源码编译 pfc + SDK + component_client → fb2k_sdk 静态库
- [x] [HEMUSIC-40](https://jira.ismisv.com/browse/HEMUSIC-40) — 最小 component.cpp（DECLARE_COMPONENT_VERSION + initquit 打印加载日志）
- [x] [HEMUSIC-41](https://jira.ismisv.com/browse/HEMUSIC-41) — Release 构建产出 foo_hemusic.dll（vcpkg toolchain，boost-json/catch2）
- [x] [HEMUSIC-42](https://jira.ismisv.com/browse/HEMUSIC-42) — foobar2000 实机加载 DLL，确认组件列表显示 foo_hemusic 0.0.1

---

## Phase 1 — 鉴权链路

- [x] [HEMUSIC-43](https://jira.ismisv.com/browse/HEMUSIC-43) — device_info（Flutter 伪装身份 + Catch2 测试）
- [x] [HEMUSIC-44](https://jira.ismisv.com/browse/HEMUSIC-44) — oauth_flow 消息层（请求体构造 + 响应解析 + json_codec）
- [x] [HEMUSIC-45](https://jira.ismisv.com/browse/HEMUSIC-45) — http_client WinHTTP 同步传输层（GET/POST + Bearer 注入 + 超时）
- [x] [HEMUSIC-46](https://jira.ismisv.com/browse/HEMUSIC-46) — login_flow 编排（providers → session → 轮询 status → result）
- [x] [HEMUSIC-47](https://jira.ismisv.com/browse/HEMUSIC-47) — token_store 双 token DPAPI 加密原子落盘
- [x] [HEMUSIC-48](https://jira.ismisv.com/browse/HEMUSIC-48) — core/config（cfg_string deviceId / apiBaseUrl / tokenStorePath）
- [x] [HEMUSIC-49](https://jira.ismisv.com/browse/HEMUSIC-49) — 临时登录 UI login_dlg（无模式 Win32 窗口 + worker 线程）
- [x] [HEMUSIC-50](https://jira.ismisv.com/browse/HEMUSIC-50) — GET /v1/user/info 消息层（UserInfo 解析）

---

## Phase 2 — API 客户端层

- [x] [HEMUSIC-51](https://jira.ismisv.com/browse/HEMUSIC-51) — http_client（Phase 1 已落地；异步两套 API 推迟决策）
- [x] [HEMUSIC-52](https://jira.ismisv.com/browse/HEMUSIC-52) — ApiClient AuthToken 注入 + 401 自动刷新重放
- [x] [HEMUSIC-53](https://jira.ismisv.com/browse/HEMUSIC-53) — Session 进程级会话层（TokenStore + 内存快照 + buildClient 工厂）
- [x] [HEMUSIC-54](https://jira.ismisv.com/browse/HEMUSIC-54) — SongInfo / LinkInfo / SongInfoArtistInfo / SongInfoAlbumInfo 共享模型
- [x] [HEMUSIC-55](https://jira.ismisv.com/browse/HEMUSIC-55) — PlaylistInfo + AlbumInfo 共享模型 + CategoryInfo
- [x] [HEMUSIC-56](https://jira.ismisv.com/browse/HEMUSIC-56) — MvInfo 模型 + /v1/page/discover 页面解析
- [x] [HEMUSIC-57](https://jira.ismisv.com/browse/HEMUSIC-57) — PlatformInfo 模型 + /v1/platforms 列表解析
- [x] [HEMUSIC-58](https://jira.ismisv.com/browse/HEMUSIC-58) — ArtistInfo 共享模型
- [x] [HEMUSIC-59](https://jira.ismisv.com/browse/HEMUSIC-59) — Radio endpoint 解析（/v1/radios + /v1/radio/songs）
- [x] [HEMUSIC-60](https://jira.ismisv.com/browse/HEMUSIC-60) — Playlist 详情 endpoint 解析（/v1/playlist + /v1/playlist/songs）
- [x] [HEMUSIC-61](https://jira.ismisv.com/browse/HEMUSIC-61) — Album 详情 endpoint 解析（/v1/album，meta + 内嵌歌曲）
- [x] [HEMUSIC-62](https://jira.ismisv.com/browse/HEMUSIC-62) — Ranking endpoint 解析（/v1/rankings + /v1/ranking）
- [x] [HEMUSIC-63](https://jira.ismisv.com/browse/HEMUSIC-63) — Search endpoint 解析（/v1/search 综合 + /v1/{type}/search 分类）

---

## Phase 3 — 播放管线（纯逻辑地基）

- [x] [HEMUSIC-64](https://jira.ismisv.com/browse/HEMUSIC-64) — hemusic:// URL 编解码（SongRef build/parse + url_codec 共享 percent codec）
- [x] [HEMUSIC-65](https://jira.ismisv.com/browse/HEMUSIC-65) — url_resolver 直链解析（30s LRU 缓存 + 重试 + invalidate）

---

## Phase 4 — UI 基础设施

- [x] [HEMUSIC-66](https://jira.ismisv.com/browse/HEMUSIC-66) — 注册 Default UI 的 ui_element_v2 主面板 MainPanel
- [x] [HEMUSIC-67](https://jira.ismisv.com/browse/HEMUSIC-67) — theme（Theme 结构 + themeFromCallback）
- [x] [HEMUSIC-68](https://jira.ismisv.com/browse/HEMUSIC-68) — d2d 基础设施（factory + HwndCanvas + decodeImage + makeBitmap）
- [x] [HEMUSIC-69](https://jira.ismisv.com/browse/HEMUSIC-69) — image_cache（URL→Bitmap LRU + 工作线程池异步 fetch+decode）
- [x] [HEMUSIC-70](https://jira.ismisv.com/browse/HEMUSIC-70) — nav 页面栈（PageKind + PageEntry + Stack）
- [x] [HEMUSIC-71](https://jira.ismisv.com/browse/HEMUSIC-71) — resolveDiscoverPlatform 纯逻辑（platform 选择降级）
- [x] [HEMUSIC-72](https://jira.ismisv.com/browse/HEMUSIC-72) — 最小 discover_page 跑通（新歌段渲染 + 主题切换）

---

## Phase 5 — UI 业务页面

- [x] [HEMUSIC-14](https://jira.ismisv.com/browse/HEMUSIC-14) — 搜索页 slice 1：综合搜索骨架 + 顶部 Tab 栏（发现/搜索）
- [x] [HEMUSIC-8](https://jira.ismisv.com/browse/HEMUSIC-8) — 登录成功后主面板自动刷新（Session listener）
- [x] [HEMUSIC-13](https://jira.ismisv.com/browse/HEMUSIC-13) — 发现页完整 4 段（新歌 / 新专 / 精选歌单 / 精选 MV）+ 卡片网格 + 纵向滚动
- [x] [HEMUSIC-12](https://jira.ismisv.com/browse/HEMUSIC-12) — 完整登录对话框（替换临时 login_dlg 的表现层）
- [x] [HEMUSIC-31](https://jira.ismisv.com/browse/HEMUSIC-31) — 发现页封面异步加载
- [x] [HEMUSIC-32](https://jira.ismisv.com/browse/HEMUSIC-32) — UI 全局 per-monitor DPI (PMv2) 感知
- [x] [HEMUSIC-15](https://jira.ismisv.com/browse/HEMUSIC-15) — 歌单详情页
- [x] [HEMUSIC-17](https://jira.ismisv.com/browse/HEMUSIC-17) — 歌手详情页（歌曲 / 专辑 / MV Tab + 分页加载）
- [x] [HEMUSIC-16](https://jira.ismisv.com/browse/HEMUSIC-16) — 专辑详情页
- [x] [HEMUSIC-36](https://jira.ismisv.com/browse/HEMUSIC-36) — 电台浏览页 + RadioDetail（HEMUSIC-18 拆分）
