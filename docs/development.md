# 开发与构建

面向贡献者与打包者：架构、构建细节、目录结构、依赖许可、贡献约定。

协议与渲染层面的硬性约束、实测结论和踩过的坑在 [internals.md](internals.md)，**改渲染路径前先读
那篇**。用户侧的三篇：历史 [history.md](history.md)、性能 [performance.md](performance.md)、
凭据 [cookies.md](cookies.md)。

## 架构

单进程，各阶段在同一进程内通过内存传递数据。

| 阶段 | 输入 | 输出 | 实现 |
| --- | --- | --- | --- |
| 协议引导 | 房间号 / URL | 真实房间号、WBI 签名、连接信息 | `bili` + `ws`，HTTP 引导 + TLS WebSocket 读循环，常驻连接，不 fork 子进程 |
| 事件解析 | 压缩的 WebSocket 帧 | `Message` 快照 | `bili`（JSON 事件）、`pb`（无 schema 的 protobuf）、`notice`（置顶卡片） |
| 素材获取 | 头像 / 表情 / 礼物图标 URL | 解码后的 cairo 表面（LRU 缓存） | `images` + `avatars` + 子模块的 `AssetCache`，后台线程下载 |
| 版面渲染 | `Message` 列表 + 表面 | BGRA 帧（预乘 alpha） | `panel` + `notice`，cairo + pango |
| 跨重启历史 | 行的增删 | 磁盘上的一个 JSON 文件 | `history`，原子写 + 节流，见 [history.md](history.md) |
| 本机弹幕源 | 面板画出的同一行 | `127.0.0.1` 上的 WebSocket text 帧 | `broadcast`（`--ws` / `--ws-rtf`），见 [internals.md](internals.md) |
| 视频输出 | BGRA 帧 | `Stream/Output/Video` 节点 | [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)，libpipewire |

`Message`（`src/message.hpp`）是整条链路的分界线：站点实现把各自的事件翻译成 `Message`，面板只把
`Message` 变成像素，完全不知道消息从哪来。**加第二个站点是再写一个翻译器的事，而不是改面板**——
Twitch 的接口位就是为它留的。

进程私有内存 94 MB，实测 CPU 约 3.0% 单核，详见 [performance.md](performance.md)。

### 线程

| 线程 | 职责 |
| --- | --- |
| 主线程 | `pwvideo::VideoNode::run()`（PipeWire 主循环），`process` 回调内只做渲染与拷贝 |
| 站点线程 | HTTP 引导 + WebSocket 读循环；把消息写进 `Shared`，帧线程只取快照 |
| 广播线程 | `--ws` 时的本机 WebSocket 服务端：单线程 poll 收发，站点线程只往队列里投递，不等慢客户端 |

与视频节点库那条契约一致：**帧回调内绝不进行网络访问，也不分配**。`Shared::snapshot()` 在锁内做一次值
拷贝，帧线程拿到的是快照而不是活引用。心跳由**读循环的读超时驱动**（`ws.next(msg, 1000)` 每秒返回一次
Timeout），不另开定时器线程。

PipeWire 侧（节点注册、缓冲声明、帧率协商、帧回调契约）全部在
[`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)
子模块里，本项目只提供「给我一帧」的回调；该层的排查手段见它的
[internals.md](../lib/pw-video-simple-interface/docs/internals.md)。

## 构建

```bash
git submodule update --init --recursive   # 视频节点库是子模块
make                                      # → ./pw-live-danmaku
make PORTABLE=1                           # 同上，但不加 -march=native
make run                                  # 构建并运行
make test                                 # 协议与历史格式的单测
make verify                               # 挂一个消费者把帧落盘（需要 gst-launch-1.0）
make compile-commands                     # 生成 compile_commands.json 供 clangd 使用
```

子模块的源码直接编进本项目的构建树：同一套编译参数、没有需要跟踪的 ABI、子模块目录里不留 `.o`。
未初始化子模块时 `make` 会带明确提示直接报错。

### 编译参数

| 参数 | 说明 |
| --- | --- |
| `-O3 -g -funroll-loops` | 默认优化级别。`-g` 保留调试信息，发布构建去掉它即可 |
| `-march=native` | 每帧文字合成的热循环收益明显。代价是二进制绑定本机指令集 |
| `PORTABLE=1` | 去掉 `-march=native`。换机器运行或打包分发时用它 |
| `EXTRA_CXXFLAGS` | 追加自己的编译参数 |
| `SERVICE_ARGS` | `make install-service` 时写进 unit 的启动参数 |
| `ROOM` | `make verify` 时用的房间号，默认 545068 |
| `PWNODE_DIR` | 子模块目录，默认 `lib/pw-video-simple-interface` |

`-march=native` 打在每帧文字合成的热循环上，理由与实测在 [internals.md](internals.md) 的
「M1 渲染侧」一节。改热循环之前先写微基准，别凭直觉改。

### systemd unit

仓库里的 `pw-live-danmaku.service` 是含 `@REPO@` 与 `@ARGS@` 的模板，由 `make install-service` 渲染
（而不是复制）到 `~/.config/systemd/user/`。该目标只写 unit 并 `daemon-reload`，不会 enable 或启动。

```bash
make install-service
make install-service SERVICE_ARGS="--room 545068 --size 480x1080 --fps 30 --history 60"
make uninstall-service
```

unit 里有三处不是随手写的：`StartLimitBurst=5` 显式收口（默认 10 秒 5 次拦不住「每 3 秒重试一次」
这种循环，因为每次重试本身也在消耗配额）、`KillSignal=SIGTERM`（程序自己处理 SIGINT/SIGTERM 并在
PipeWire 主循环里销毁流，systemd 若抢先 SIGKILL 会出现 `pw_stream_destroy called from wrong context`）、
`StateDirectory=pw-live-danmaku`（给 `--history` 一个可写目录）。带 cookie 时路径要写 `%h` 而不是 `~`，
理由见 [cookies.md](cookies.md)。

### 编辑器工具链

`make compile-commands` 生成 `compile_commands.json`（已被 `.gitignore` 排除）。子模块头文件只能通过
Makefile 里的 `-I` 找到，没有这份数据库 clangd 解析不了 `pwvideo.hpp` / `text.hpp`，会报一片假错误。
`.pi-lens.json` 是 pi-lens 的配置，目前只排除了 `Makefile`。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `README.md` | 英文 README |
| `README.zh-CN.md` | 中文 README |
| `LICENSE` | MIT 许可证全文 |
| `docs/` | 本目录下的文档与 `--dump` 效果图 |
| `docs/internals.md` | 协议实测、渲染侧约束、踩过的坑、调试命令（改代码前先读） |
| `docs/history.md` | 跨重启历史的行为与实现 |
| `docs/performance.md` | 性能实测数据与省开销的做法 |
| `docs/cookies.md` | 凭据（cookie）的给法与 systemd 陷阱 |
| `docs/development.md` | 本文：架构、构建、目录结构、贡献约定 |
| `Makefile` | 构建脚本，含 `test` / `verify` / `run` / `install-service` / `uninstall-service` / `compile-commands` 目标 |
| `pw-live-danmaku.service` | systemd 用户服务模板，由 `make install-service` 渲染安装 |
| `src/message.{hpp,cpp}` | `Message` 与 `Fragment`：与平台无关的消息模型（整条链路的分界线） |
| `src/bili.{hpp,cpp}` | B 站引导、WBI 签名、JSON 事件解析、入场/礼物合并 |
| `src/ws.{hpp,cpp}` | 自写的 RFC 6455 TLS WebSocket 客户端（OpenSSL，不引第三方）；握手 Accept 的计算导出给服务端复用 |
| `src/broadcast.{hpp,cpp}` | 本机 WebSocket 弹幕源：三字段线格式、Resonite RTF 标签，以及 loopback 上的 RFC 6455 服务端（明文，单线程 poll） |
| `src/pb.{hpp,cpp}` | 无 schema 的 protobuf 读取器（`data.pb` 的 base64 blob） |
| `src/notice.{hpp,cpp}` | 置顶图层：醒目留言与上舰卡片，按金额停留 |
| `src/panel.{hpp,cpp}` | cairo + pango 聊天面板：静态层 + 增量移位、左侧色条、折行 |
| `src/images.{hpp,cpp}` | 表情与礼物图标的缓存（`ImageStore`） |
| `src/avatars.{hpp,cpp}` | 头像缓存，每帧从行几何重画而不烘焙 |
| `src/history.{hpp,cpp}` | 跨重启的历史文件 |
| `src/json.{hpp,cpp}` | 极简只读 JSON 解析器 |
| `src/demo_faces.{hpp,cpp}` | `--demo` 的合成头像，让离线调版面也能看见头像框的位置 |
| `src/main.cpp` | 模块组装、命令行解析、PipeWire 节点与渲染回调 |
| `tests/` | `bili_test`（钉住原始 blob）、`history_test`（往返与窗口）、`json_test`、`broadcast_test`（线格式与握手/成帧） |
| `lib/pw-video-simple-interface/` | git 子模块：视频节点（注册、缓冲、帧率协商）与 cairo 辅助模块（帧、文字、素材缓存、HTTP） |

## 依赖与许可

运行期依赖全部是发行版仓库里的系统库，不涉及语言包管理器：

| 库 | 用途 | 上游声明的许可 |
| --- | --- | --- |
| cairo | 绘制 | LGPL-2.1-only OR MPL-1.1 |
| pango | 文字排版 | LGPL-2.0-or-later |
| gdk-pixbuf | 图片解码 | LGPL-2.0-or-later |
| glib | gdk-pixbuf 依赖 | LGPL-2.0-or-later |
| fontconfig | 字体解析与注册（`--font-file`） | MIT-style |
| PipeWire | 视频节点 | MIT OR LGPL-2.1-or-later |
| libcurl | HTTP 引导与素材下载 | curl 许可（MIT 类） |
| OpenSSL | TLS | Apache-2.0 |
| brotli | WebSocket 帧解压 | MIT |
| zlib | gzip 解压 | Zlib |

这些库均以动态链接使用，没有捆绑或修改其代码，各自适用其自身许可。

OBS 侧的 [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) 是 GPLv2 的独立程序，与本项目之间
只有 PipeWire 节点的运行时数据流，不构成链接或派生关系，故各自许可互不影响。

## 测试

```bash
make test
```

协议层没有 UI，所以直接测而不是通过画面测：

| 测试 | 钉住的东西 |
| --- | --- |
| `tests/bili_test.cpp` | **原始 blob**，不是重新拼的载荷——站方改打包方式会在这里失败，而不是变成一个悄悄空掉的画面；另有 WBI 签名的完整向量、表情切分规则 |
| `tests/history_test.cpp` | 文件往返、窗口、节流，以及必须被拒绝的文件 |
| `tests/json_test.cpp` | 只读 JSON 解析器 |
| `tests/broadcast_test.cpp` | 弹幕线格式（含 RTF 转义）、RFC 6455 的示例握手向量、帧收发、ping/pong，以及两种拒连（未 mask 的帧、非 WebSocket 请求） |
| `tests/ws_test.cpp` | 客户端的成帧与消息层：分片重组（分片之间还插一个 ping）、二进制帧、close 回显、客户端帧的 mask 位；用一个自签证书的脚本 TLS 服务端驱动真实的 connect/握手/读路径 |

端到端那一步是 `make verify`：挂一个消费者，把帧落盘，用文件大小反推尺寸（BGRA，逐帧
`width*height*4` 字节）。需要 `gst-launch-1.0` 与 `pipewiresrc`；没有它们就跑不了，此时渲染侧的性能
数字只能沿用上一次实测。

## 贡献

- 行为改动：附命令与其输出。
- 性能结论：附测量数据。没有测量来源的数字不写进文档。
- 协议结论：附抓包或复现命令。`docs/internals.md` 里每条结论都带对照数据或命令，照这个标准写。
- 改渲染路径前先读 [internals.md](internals.md)；PipeWire 相关代码在
  [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface)。
- 说明工作是怎么产出的（建议而非要求），便于回溯。
- 补丁的审核、验证与后续维护由提交者承担。

本项目没有 CLA；贡献按 MIT 进入本项目。

写文档时区分两类内容：README 只留普通用户需要的东西（怎么构建、怎么用、参数含义），实现细节、实测
数据与调参原理放进 `docs/`。README 是英文为主、中文镜像同步维护的两份，`docs/` 目前仅中文。