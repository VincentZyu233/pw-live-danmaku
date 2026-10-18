# 实现细节

修改代码前先读本文。以下每条都来自实机验证，而非文档转述；其中若干条官方文档没有记载，或社区文档描述与实际行为不符。

本文与项目代码均在 LLM 辅助下编写。所有结论都经过实机复现——未经复现的推测不收录，因此每项都附有可重跑的对照数据或命令。

同目录另有四篇：跨重启的历史 [history.md](history.md)、性能 [performance.md](performance.md)、凭据 [cookies.md](cookies.md)、开发与构建 [development.md](development.md)。

## 进度

**M0（协议打通）、M1（聊天面板本体）与 M2（入场 / 礼物 / 点赞事件）已完成。**

链路：`room_init` → WBI 签名 `getDanmuInfo` → TLS WebSocket 握手 → JSON 认证包 → brotli 解包 → `Message` → cairo 渲染 → PipeWire 视频节点 → 消费者落盘。

M1 起画面就是聊天面板本身（不是调试卡片）。M2 把 `INTERACT_WORD_V2` / `SEND_GIFT_V2` / `LIKE_INFO_V3_CLICK` 三类事件也画进同一列，见[这三类事件已经不是 JSON 了](#这三类事件已经不是-json-了)。`--demo` 可以不连网调版面。`--history` 把最近 N 行写盘并在下次启动画回来，见 [history.md](history.md)。

## 三个协议坑：都与社区文档相反

### 一、认证包用 JSON，protobuf 版本会被直接断连

绝大多数开源实现（Go / Python / Rust 的 bilibili 直播库）发送 protobuf 的 `ClientVerifyReq`。**实测会被服务器立刻断开连接。** 四种字段编号变体全部失败：

| 认证包 body | 结果 |
| --- | --- |
| protobuf，`uid` 作 bytes（wire type 2） | 建连后立即断开 |
| protobuf，`uid` 作 int64（varint） | 建连后立即断开 |
| protobuf，追加 `buvid` 为字段 7 | 建连后立即断开 |
| **JSON** | **拿到认证回包，成功** |

实测使用的 JSON body：

```json
{"uid":0,"roomid":21452505,"protover":3,"platform":"web","type":2,"key":"<token>"}
```

包头仍为 `proto=3, type=7`。**照抄社区文档去写 protobuf 会浪费很久的调试时间**，这条务必先看。

### 二、认证回包声明 proto=3，但 body 是明文 JSON

这是最容易踩的一个：拿到 `proto=3` 就去 brotli 解压，必然失败。

实测抓到的认证回包，一共 **26 字节**：

```text
00 00 00 1a  00 10  00 03  00 00 00 08  00 00 00 01  7b 22 63 6f 64 65 22 3a 30 7d
└─ totalSize=26 ─┘ hsize=16 proto=3 type=8  seq=1  └──────── {"code":0} ────────┘
```

而**同一个 proto=3 的弹幕批次是真的 brotli**。所以 `proto` 只能当提示，不能当契约。

因此 `Bili::forEachJson()` 先嗅探 body（以 `{` 或 `[` 开头即视作文档），再决定是否解压——见 `src/bili.cpp`。早前一版直接信 `proto`，症状是卡片上显示 `auth reply could not be decompressed`。

### 三、`getDanmuInfo` 会风控，四个条件缺一不可

不加签名的请求直接被拒：

```bash
curl -s 'https://api.live.bilibili.com/xlive/web-room/v1/index/getDanmuInfo?id=545068'
# {"code":-352,"message":"-352","ttl":1}
```

实测逐项验证：

| 条件 | 结果 |
| --- | --- |
| 无 WBI 签名 | `code:-352` |
| WBI 签名，但无 buvid3 / 无 Referer / 无 Origin | `code:-352` |
| **WBI 签名 + buvid3 + `Referer` / `Origin` + 浏览器 UA** | **`code:0`，拿到 token 与 host_list** ✅ |

**buvid3 不需要登录**，`https://api.bilibili.com/x/frontend/finger/spi` 的 `data.b_3` 免费可得，等同浏览器首次访问时生成的设备 id。

WBI 签名算法（`Bili::mixinKey` / `md5Hex`）：

1. `https://api.bilibili.com/x/web-interface/nav` 取 `data.wbi_img.img_url` / `sub_url` 的**文件名部分**（去掉扩展名）。
   注意该 URL 是伪装的 token，**不要去请求它**；未登录时该接口返回 `code:-101`，但 `wbi_img` 仍然存在，所以匿名可用。
2. `raw = img_key + sub_key`（64 字符），按 64 项置换表 `MIXIN_KEY_ENC_TAB` 重排，取前 **32** 位。
3. 参数按 key 排序 → 值里剔除 `!'()*` → URL 编码 → 拼 `&wts=<unix秒>` → `md5(query + mixin_key)` 即 `w_rid`。

实测取 key 的那一步长这样（注意 `code` 是 **-101 未登录**，但 `wbi_img` 依然存在，所以匿名可用）：

```json
{
  "code": -101,
  "message": "账号未登录",
  "data": {
    "isLogin": false,
    "wbi_img": {
      "img_url": "https://i0.hdslb.com/bfs/wbi/7cd0849…b84077c.png",
      "sub_url": "https://i0.hdslb.com/bfs/wbi/4932caf…70ac45.png"
    }
  }
}
```

这两个值**不是密钥**：由匿名接口下发、全站共用、每日轮换，公开可见且随时会变，本文因此只写出首尾片段。它们之所以要被记下来，是因为 WBI 签名由它们推出，而签名一旦算错，从外面只看得到 API 回一个 `code:-352`。**完整的一组实测向量固化在 `tests/bili_test.cpp` 里**，那是它该待的地方；文档不重复，以免两处各写一份、日后只改一处。

`room_init`（`/room/v1/Room/room_init?id=`）**完全不需要签名或登录**，并且它是把短房间号换成真实房间号的唯一途径。

## 一个自己踩到的传输层坑：SSL_read 必须先于 poll

非阻塞 socket 上，**先 poll 再 `SSL_read` 一次是错的**。当一条 TLS record 跨 TCP 段到达时，`SSL_read` 会把缓冲区里已有的字节消费掉、返回 `SSL_ERROR_WANT_READ`，而调用方把这当成超时。

症状极具迷惑性：TCP 连上了、TLS 握手成功、请求也写出去了，然后卡在 `upgrade response timed out`——尽管服务器已经回了 `101 Switching Protocols`。

用打印 `poll`/`SSL_read` 返回值的办法定位到的：

```text
poll(events=1,to=10000)=1 revents=1   ← 数据可读
SSL_read=-1                            ← 但返回 WANT_READ
```

对照实验证明与 B 站无关：同一份请求字节，用**阻塞** socket 的裸程序可以正常拿到 101。所以正确写法是 `readSome()` 现在的样子——**先 `SSL_read`，只在 OpenSSL 表示还要更多字节时才 poll**，且整段操作用一个 deadline 而不是每次重置。`writeAll()` 同理。

这个坑在 M0 之前不出现，是因为那时的测试都是阻塞 socket。

## 已实测观察到的弹幕字段

`DANMU_MSG` 的 `info` 是**一个**数组（不是多个 `info` 键），元素含义：

| 下标 | 含义 | 实测值 |
| --- | --- | --- |
| `info[0]` | 模式数组：`[?, mode, fontSize, color, timestampMs, rnd, ?, uidHash, ...]`，第 15 项是 `{"extra": "<json 字符串>"}` | `mode=1, fontSize=25, color=16777215` |
| `info[1]` | 弹幕正文 | `老丈人只为名` |
| `info[2]` | `[0, "昵称", ...]` | `旱獭邮箱_` |
| `info[9]` | `{"user": {...}, "extra": "<json 字符串>"}` | 含 `mode` / `dm_type` |

**表情弹幕的正文里是字面量 token**，不是图片也不是编号：

```text
[夏日热浪_想要]   [夏日热浪_爱你]   [夏日热浪_害羞]
```

格式为 `[表情名_类型]`。M1 画表情时按这个 token 切分正文。

`info[0][0]` 与 `info[9].extra` 里的 `mode` 在本次样本中恒为 `0`，与 `info[0][1]=1` 并存。**顶部 / 底部 / 逆向 / 高级弹幕的取值语义尚未实测**，需要在一个热闹房间里抓全模式样本再定；在此之前 `Danmaku::mode` 原样透传，不做映射。

### 匿名连接看不到昵称

```text
LOG_IN_NOTICE: {"notice_msg":"为保护用户隐私，未登录无法查看他人昵称"}
info[2][1] 实际值：旱獭邮箱_ / 蔷薇少女3 / 欠***
```

弹幕正文、颜色、字号、时间戳**全部正常**。所以弹幕显示本身不需要登录；要显示昵称才需要 `SESSDATA`。`--cookie` 就是为这个留的口子。

**但头像不一样：匿名连接照样拿得到。** 这一条纠正了本文早期版本写下的「B 站匿名连接没有头像」——那个结论是错的，因为它是从「昵称被掩码」推出来的，而不是实测。实测 `info[0][15].user.base.face` 是一个完整的 `https://i1.hdslb.com/bfs/face/....jpg`，无需任何凭据。所以头像走的是「拿到 URL → 后台线程下载」的普通图片流程，和 Twitch 侧需求一致。

### 其他事件类型

实测在同一连接上出现过（`main.cpp` 只取画得出来的那些，其余记录在 `last event`）：

```text
LOG_IN_NOTICE   NOTICE_MSG   WATCHED_CHANGE   ONLINE_RANK_COUNT   STOP_LIVE_ROOM_LIST
ROOM_REAL_TIME_MESSAGE_UPDATE   POPULAR_RANK_CHANGED   ONLINE_RANK_V3
```

超级留言 / 上舰 / **入场 / 礼物 / 点赞**走同一条连接，只是 `cmd` 不同——付费卡片、上舰卡片与这三类事件都不需要新连接。

## 这三类事件已经不是 JSON 了

入场、礼物、点赞三类事件在 M2 之前**一条都画不出来**，而且症状不像解析失败：**`INTERACT_WORD_V2` 与 `SEND_GIFT_V2` 现在完全不带 JSON**，`data` 里只剩 `dmscore` 和一个 base64 的 protobuf 字段 `pb`，没有别的。只读 JSON 的解析器看到的是两个近乎空的对象，于是把它们当"不是给我看的"丢掉——从外面看和"网络没通""账号没权限"一模一样。

### 实测的到达速率（房间 21852，约 105 万在线，匿名连接，70 秒）

| 事件 | cmd | 条数 | 速率 |
| --- | --- | --- | --- |
| 入场 | `INTERACT_WORD_V2` | 110 | **1.57 / s** |
| 弹幕 | `DANMU_MSG` | 25 | 0.36 / s |
| 礼物 | `SEND_GIFT_V2` | 18 | 0.26 / s |
| 点赞 | `LIKE_INFO_V3_CLICK` | 6 | 0.09 / s |

**入场比弹幕多四倍多**，这个数字决定了下面两个设计决定，不是估计出来的。

### 字段表（全部来自上面这一份抓包）

**`INTERACT_WORD_V2`**（110/110 都有 2、8 两项）

| 字段 | 含义 |
| --- | --- |
| `2` | 昵称（匿名连接下通常被打码） |
| `8` | 时间戳，**毫秒** |
| `22.2.2` | 头像 URL。**刻意不读**，理由见下 |
| `9.*` / `22.3.*` | 粉丝勋章（`9.5`/`22.3.2` 等级，`9.6`/`22.3.3` 颜色） |
| `6` | 房间号，`7` 秒级时间戳，`5` 恒为 1 |

**`SEND_GIFT_V2`**

| 字段 | 含义 |
| --- | --- |
| `2` / `3` | 送礼者昵称 / 头像 |
| `10.2` | 礼物名（`10.1` 是 `gift_id`） |
| `10.3` | 这次送的数量（**这一条要画的就是它**） |
| `10.5` | 单价（金瓜子，1000 = 1 元），`10.8` 是 `coin_type` |
| `10.10` | 发送时间，**秒** |
| `10.18` | 站方给的动词，实测是 `投喂` |
| `10.35.1` / `10.35.2` | 礼物图标：PNG / webp。**只取 1**，gdk-pixbuf 不保证能读 webp |
| `10.29.1` | 收礼者昵称（主播） |
| `13.1` | 本次连击里的累计数，**不是这条的数量**，所以不用 |

**`LIKE_INFO_V3_CLICK`**（仍是普通 JSON）

| 字段 | 含义 |
| --- | --- |
| `data.uname` | 昵称 |
| `data.uinfo.base.face` | 头像 |
| `data.like_text` | 正文，实测恒为 `为主播点赞了` |
| `data.uinfo.guard.level` | 舰长档位。**不读**，理由见下 |

三类事件里只有 `LIKE_INFO_V3_CLICK` 不带时间字段——抓包里没有，所以 `Message::tsMs` 在这一支上**保持 0**，其余五个处理分支（弹幕 / 入场 / 礼物 / 醒目留言 / 上舰）都填。目前没有任何逻辑读非付费消息的 `tsMs`（停留时钟只吃 `MsgKind::Paid`），所以留空不花钱；而填一个猜出来的值，往后也没有任何东西能发现它是错的。

这些字段号没有 `.proto` 可依据，是抓包量出来的；`tests/bili_test.cpp` 里钉住的是**原始 blob**，不是重新拼的载荷，所以站方改打包方式会在这里失败，而不是变成一个悄悄空掉的画面。

### 两个反直觉的实测结果

**一、昵称打码不是全局的。** 同一条匿名连接上，`INTERACT_WORD_V2` 的 110 条里 106 条昵称被打码（`l***`），而 `LIKE_INFO_V3_CLICK` 的 6 条**全部**是实名。`ENTRY_EFFECT` 的 18 条也全部是实名。所以"匿名连接拿不到昵称"这句只对弹幕和入场成立，点赞事件反而是少数几个能白拿真名的来源之一。

**二、礼物图标可以直接复用表情那套管线。** `10.35.1` 的图标 URL 和 `extra.emots` 里的表情 URL 是同一类东西：全站复用的少量图。礼物行因此把图标作为一个 `Fragment::Emote` 发出去，走现成的下载、缓存、绘制路径，**没有新增任何网络设施**。实测礼物图标下载正常，行内渲染成带圆角的小图。

### 三个刻意不做的事

- **入场不带头像。** 头像缓存 256 条，而入场是 1.57 人/秒、每人一个只出现一次的 URL：缓存必然抖动，而且这些人多半用的是站方默认头像（`noface.jpg`）。所以 `22.2.2` 明明在载荷里也不取，行直接从面板内边距起排。
- **入场 / 点赞不画左侧色条，并且整体压暗到 0.5。** 色条是一行里最抢眼的东西；这两类事件的速率与弹幕差一个数量级，视觉权重也得差得比"一个色阶"更多。压暗**三处一起压**：昵称、正文、以及字外面那圈黑色描边（`0.85 × alpha`）。只压填充而留着满强度的描边，等于在半亮的字外面套一道硬黑边——在亮画面上那道边比被压暗的字还显眼，正好把想压的东西压下去了。
- **`LIKE_INFO_V3_CLICK` 的 `uinfo.guard.level` 不映射成舰长配色。** 6 条实测全是 `0`，非零取值对应哪个档位是猜的——本文不收未经复现的推测。

### 剩下的同类事件，都不画，理由各不相同

| cmd | 实测 | 不画的原因 |
| --- | --- | --- |
| `ENTRY_EFFECT` | 18/70s，`copy_writing` = `<%名字%> 来了`，`uinfo.base.name` **实名** | 是入场的**子集**（站方只为部分入场做入场动画），画了会重复；且只有它带实名，值得记一笔但不值得为它写第二条路径 |
| `COMBO_SEND` | 2/70s，`gift_name` / `combo_num` / `combo_total_coin` 全是明文 JSON | 是**连击汇总**，与 `SEND_GIFT_V2` 指向同一次送礼（同一 `combo_id`），画了会重复 |
| `DM_INTERACTION` | 11/70s，`data.data` 是 JSON 字符串；`type` 104 = `投喂一个%s支持主播`，106 = `大家正在点赞` + `pv_cnt` | 是"当前 N 人在投喂 / N 人在点赞"的**聚合横幅**，不是某一个人的事件，且属于横幅而非聊天列 |
| `LIKE_INFO_V3_UPDATE` | 11/70s，只有 `click_count` | 房间点赞**总数**的周期性刷新，不是某人点赞了一次 |
| `ROOM_REAL_TIME_MESSAGE_UPDATE` / `POPULAR_RANK_CHANGED` / `ONLINE_RANK_V3` | 各 1~6 次 | 榜单与房间公告，不是聊天 |

### 自己踩到的一个坑：wire type 的枚举不能顺序写

`src/pb.hpp` 里 `Wire` 是按序号声明的（`Varint, Fixed64, Length, Fixed32`），于是 `Fixed32` 的值是 **3**，而 protobuf 的 wire type 5 才是 32 位定长。后果是**凡是含 32 位定长字段的载荷，从那个字段起整个扫描就结束**——礼物的 `10.16` 正好是 1.0f，于是 `10.17` 之后的 `10.18`（动词）和 `10.35`（图标）全部读成"不存在"。

症状具有欺骗性：不是解析报错，也不是礼物名消失（`10.2` 在出问题那个字段之前），而是**动词退回了默认值、图标消失**，看起来像站方换了字段。修法是把编号写进枚举（`Fixed32 = 5`），并在 `pb.hpp` 里记下这个坑。

### 读 protobuf 本身：字符串与子消息在 wire 上无法区分

没有 schema 的时候，长度定长字段既可能是字符串也可能是子消息，`str()` 只能靠"是不是合法 UTF-8"来挡一手。**而这一手并不够**：实测礼物的 `10.35`（`gift_info`）是 229 字节、绝大多数是 URL 加小 varint，**它通过 UTF-8 校验**，`str(35)` 会返回一串噪声。真正让调用正确的是"字段号来自抓包"，UTF-8 校验只是防止**取错字段号**时把二进制画成昵称。这一点写在了 `pb.hpp` 的注释里，免得后来者以为那个校验足够。

### base64 的字母表也要挡住

`data.pb` 实测是标准字母表，但解码前仍把 `-`/`_` 翻回 `+`/`/`。理由和上面两个坑是同一个：**这两个事件解不出来时没有任何症状**——整条被丢掉，从外面看与"网络没通""账号没权限"完全一致，也就是 M2 开头想消灭的那种歧义。站方哪天换成 URL-safe 的打包方式，不翻译就是静默丢掉整类事件，翻译一下是三行代码。

### 入场批次在断线时也要发出去

合并窗口 5 秒，所以断线瞬间手上最多攒着 5 秒的入场。socket 已经没了，不会再有下一条 notice 来触发 `flush()`，于是在 Close / Error 分支上先调 `EntryMerger::flushAll()`（不等窗口）把它们发完再 `return`。这些人确实进来过，而画面上其余部分还留在那儿，所以这一行该在。

### 同一个人连投同一个礼物，合成一行（`GiftMerger`）

入场合并解决的是「人多」，礼物合并解决的是「同一个人重复」。礼物按钮是**连点的**：一个人看中某个礼物，一口气点十次，就是十行字面相同的话，每行挤掉一行真正打出来的弹幕。而礼物行本来就带数量（单个 `×6` 是一次送出六个），所以合并出来的行只是把数量加上，样子还是那一行，不是新造一句话。

**键是「观看者 + 礼物」而不是只有观看者。** 一个人投两种礼物是两件事，各占一行才对；只按人分组会把它们并成一行，反而丢了信息。**头像也在键里**：匿名连接下昵称被打码成同样几种形状（`氧***`），把两个恰好撞名的不同的人并成一行，等于宣称其中一个送了另一个人送的礼物。昵称 + 头像一起才够区分。

**窗口从这一串的第一次礼物起算，且不被后来的礼物延长。** 延长的话，一个不停连点的人可以把一行永远按在画面上不去——那就是这个功能要换掉的那个刷屏，只是变慢了。

**这一行是等窗口走完才画的，而不是画完再改。** 面板把一行画进静态层之后就只剩像素了，没有东西可以改；`Message::count` 因此和 `amountValue` 一样是「数字与显示字符串分开放」——把 `×6` 从字符串里解析回数字，就等于把显示格式又变成了数据源。解析和合并都走同一个 `setGiftCount()`，所以行上的字和 `count` 不可能各说各话。代价是一个孤零零的礼物最多晚 3 秒出现；`--gift-merge 0` 就是为此留的。

和入场不同，礼物的一次 `flush()` **可能吐出不止一行**：两个人同时投不同的礼物并不是彼此的重复。所以 `flush()` / `flushAll()` 往 `std::vector` 里追加，调用方按顺序发布；`--count` 可能在中间叫停，剩下的就一起放弃——和它在一条 websocket 批次中途叫停是同一件事。`mergeKey` 为空的行**不参与合并**（原样放行）：站点没给出可比对的键时，宁可各占一行，也不要把所有礼物并成一行。

### 解析开销

对上面这 242 份抓包做基准（`Json::parse` + `parseMessage` 全流程，含 base64 与 protobuf）：

```text
4.8 us per document
16 us per second of stream      （按上表 242 份 / 70 秒的速率折算）
```

即每秒输入占用约 0.0016% 单核，与渲染相比可以忽略。这份数字**不包含渲染**，渲染侧的实测窗口见 [performance.md](performance.md)。

## 视频节点侧：与视频节点库一致

节点注册、`SPA_PARAM_Buffers`、`TRIGGER` 不叠 `DRIVER`、`media.role=Production` 全部由 `lib/pw-video-simple-interface` 负责，本文不重复；那四条硬性要求与定位手段见
[`lib/pw-video-simple-interface/docs/internals.md`](../lib/pw-video-simple-interface/docs/internals.md)。

**一条要单独强调的坑**：`CairoFrame::blitTo()` 是**裁剪，不是缩放**（内部是 `min(w, width_)`）。协商尺寸小于配置尺寸时，右侧和下方直接被切掉，不会等比缩小。所以 **OBS 源的宽高必须等于 `--size`**，否则画面缺一块。

落盘验证：

```bash
./pw-live-danmaku --room 7734200 --seconds 45 &
sleep 6
gst-launch-1.0 -q pipewiresrc target-object=pw-live-danmaku num-buffers=10 ! \
    video/x-raw,format=BGRA ! filesink location=/tmp/frames.raw
ls -l /tmp/frames.raw        # 实测 17280000 = 10 × 480 × 900 × 4，逐字节吻合
```

没有消费者连接时流停在 `PAUSED`，渲染回调一次都不被调用——所以 OBS 里没开这个源时，弹幕侧开销为零。

## 为什么自己写 WebSocket 客户端

libcurl 8.x 其实带 WebSocket 支持（`curl_ws_recv` / `curl_ws_send` 符号存在），但没有采用，理由三条：

- libcurl 的 WebSocket 接口**官方标注为 experimental**；本项目只依赖发行版提供的稳定接口。
- 子模块的 `HttpClient` 是阻塞 GET 且不暴露 `CONNECT_ONLY`，用它就得为一个站点去改子模块。
- 剩下要自己写的只有 RFC 6455 的成帧，约 250 行，而 TLS 层的 OpenSSL 本来就要。握手所需的 SHA-1 与 base64 也出自 OpenSSL，没有引入任何新依赖。

成帧与消息层（分片重组、ping 自动回 pong、close 回显、mask 位）由 `tests/ws_test.cpp` 钉住：它自己生成一张自签证书、自己扮服务端，按用例顺序写出确切的帧，跑的是客户端真实的 connect / 握手 / 读路径。直播流里 B 站不发分片帧，所以“一条消息拆在多个帧里”只有脚本服务端能触发——分片重组曾把消息清空后再换出，每条分片消息都变成空串，而这个 bug 因此活过了所有实机运行。

## 本机 WebSocket 弹幕源（`--ws`）

`--ws` 把面板已经画出来的行再发一份到 `127.0.0.1`，一条消息一个 text 帧，字段用 Tab 分隔：
`头像URL`、`名字`、`正文`。发的是**合并之后**的行（`--entry-merge` / `--gift-merge` 之后），所以
同一个人连点十次礼物是一个计 10 的帧。**单向**：客户端发来的一切只被读掉、不回话（ping 例外，
回 pong），也不重放历史，中途连上只看得到下一条。

`--ws-rtf` 再按 [Resonite 的富文本标签表](https://wiki.resonite.com/Text_formatting) 给名字与正文加
标记。实测抓取（`--room 545068 --count 6 --ws 18999 --ws-rtf`，原始 socket 客户端，一行一个帧）：

```text
	<alpha=#80><color=#9AA0A6>bili_30704624353</color></alpha>	<alpha=#80>进入了直播间</alpha>
http://i2.hdslb.com/bfs/face/...jpg	<color=#9AA0A6>Loyers</color>	直接下班吧
https://i0.hdslb.com/bfs/face/...jpg	<color=#9AA0A6>别教゛</color>	不计较就别打字了呗[汤圆][汤圆]
```

名字颜色取自面板那组配色（自选色优先，否则角色色，普通用户是那支灰）；礼物动词 `投喂` 用礼物金，
未到货的礼物名用礼物粉；入场与点赞用 `<alpha=#80>` 压到面板的 `dimAlpha = 0.5`。**这几个值在
`broadcast.cpp` 里各写了一份**，没有去读 `PanelTokens`：读它会把 cairo / pango 拉进一个只需要 socket
的模块，代价比六个常量高。代价是改面板配色时这里不会跟着变。

`<` 的转义只用 `<noparse=N>` 的计数形式，而且一次只保护一个字符：`<noparse=1><`。理由两条：闭合
形式 `<noparse>...</noparse>` 包不住可能自带 `</noparse>` 的弹幕文本；而一次一个字符让计数单位
（字节 / 码点 / UTF-16 码元）变得不重要，因为 `<` 在三种单位下都是 1，弹幕两侧却多半是中文，三者互
不相同，猜错就会把我们自己的 `</color>` 释放成标记。**标签在游戏内的实际渲染尚未实测**，这里只按
wiki 列出的语法实现。

慢客户端积压到 `kWsBacklog`（256 条）时，丢的是**最新**的消息而不是最旧的。最旧那条可能已经往 socket
写出去一半，半帧无法修补：剩下的字节会被客户端当成帧头继续解析，整条流从此错位，症状正是客户端把旧
文本再报一遍、或报出一段没人发过的话。丢新消息只丢整条，字节流始终是合法前缀。回归测试用 2000 条 8 KB
消息把连接堵到触发上限，再逐帧验证「每一帧都能解析、且都是发过的消息」。

## 凭据与文件路径

cookie 的取用、systemd 的 `%h` 与 `~` 怎么选、以及为什么程序自己展开 `~`，
都在 [cookies.md](cookies.md)。

## `DANMU_MSG` 的真实结构（M1 实测）

抓一条真实弹幕，`info` 有 **18** 个元素，实测输出：

```text
info 长度: 18
info[1] text   = 委屈吗？
info[2][1] name = 友好的益生菌
info[9]        = {}                      ← 是对象，且为空
info[0][15] is an object
  user present = yes
  base.face       = https://i1.hdslb.com/bfs/face/e9e57ba0...1b1cabe27.jpg
  base.name       = 友好的益生菌
  base.name_color = 0
  guard           = null
  medal.guard_lvl = 0   medal.name=德云色
  extra len=984
  extra.mode=0 dm_type=0 content=委屈吗？
  info[0][1..4] = mode=1 fs=25 color=16777215 ts=1791051148579
```

**关键：`extra` 和 `user` 都在 `info[0][15]`，不在 `info[9]`。** `info[9]` 是 `{ct, ts}`。

M0 版本的 `parseDanmaku` 写的是 `json["info"].at(9)["extra"]`，也就是说**它从来没读到过任何 extra**——`dm_type` 恒为 0 只是因为没人看。这个错误当时没能暴露，是因为 `mode` 恰好可以从 `info[0][1]` 拿到，输出看着是对的。**这是"字段读不到"与"字段读对"在外观上无法区分的一个例子**，只能靠打印真实结构来发现。

由此可得的面板需要的东西，全部匿名可得：

| 字段 | 路径 | 用途 |
| --- | --- | --- |
| 头像 | `info[0][15].user.base.face` | 圆形头像，框高等于正文一行行高（默认字号 28px → 42px） |
| 昵称 | `info[0][15].user.base.name` | 与 `info[2][1]` 一致 |
| 昵称颜色 | `info[0][15].user.base.name_color` | 十进制 RGB，0 表示无特殊色 |
| 上舰 | `info[0][15].user.medal.guard_level` 或 `user.guard.guard_level` | 1 总督 / 2 提督 / 3 舰长 |
| 表情（整条一个表情） | `info[0][12] == 1` 时看 `info[0][13]` | `{emoticon_unique, url, width, height, ...}` |
| 表情（正文内嵌） | `info[0][15].extra.emots` | `{ "[token]": {url,width,height} }` |

**所以表情图片也是现成的**，不需要自己查表。两种载体取哪一份，看 `dm_type`（详见[表情弹幕的图片](#表情弹幕的图片)）；只读 `extra.emots` 的版本会在主播自定义表情上只画出名字文字。

仍然拿不到的：**房管 / 主播身份**。这两个身份不在 `DANMU_MSG` 的载荷里，匿名连接无法区分，所以 B 站侧的用户名着色实际只有「普通」和「舰长」两档。这两个槽位留给 Twitch（`badges` tag 有 moderator/subscriber/broadcaster）。

## 表情弹幕的正文是字面量 token

这一节说的是**内嵌**的那种（`dm_type == 0`）：`extra.emots` 的键就是正文里出现的那段字符串：

```text
[夏日热浪_想要]   [夏日热浪_爱你]   [夏日热浪_害羞]
```

所以切分规则是：**只有平台自己声明过的方括号串才算表情**，其余方括号按普通文本处理。`Bili::splitFragments()` 就是这么做的，单测里有一条专门钉住「未声明的方括号不被吞掉」。

另一种（整条就是一个表情，`dm_type == 1`）根本不走切分：正文就是表情名，图片直接给出来。

## M1 渲染侧：四个必须遵守的约束

### 一、`TextRenderer` 只有一个 PangoLayout

`TextRenderer::layout()` 复用同一个 `PangoLayout`，每次调用都把它重新绑定到传入的 context。这在「一次排版、一次绘制」时是优点（比每次新建 layout 便宜），但**在遍历排版结果的循环里是陷阱**：

```cpp
PangoLayout* bl = text_.layout(cr, body, bs);          // 拿到整个正文的 layout
for (int i = 0; i < pango_layout_get_line_count(bl); ++i) {
  PangoLayoutLine* pl = pango_layout_get_line_readonly(bl, i);
  std::string slice = body.substr(pl->start_index, pl->length);
  text_.layout(cr, slice, ls);                          // ← 把 bl 本身覆盖掉了
  outlined(bl, ...);                                    // bl 已经指向 slice 的单行 layout
}
```

症状是**换行的第二行及以后整段丢失**，而且第一行的内容也会错乱。正确写法是**先把所有行区间收集到一个 vector，再开始画**。

### 二、测量与绘制必须用同一个折行宽度

第一版 `measureRow()` 用整个可用宽度排版，`paintRow()` 却用 `可用宽度 - 昵称宽度` 排版（因为首行要和昵称并排）。于是 measure 算出一行的高度、paint 画出两行，超出部分被下一条消息的移位覆盖——**又是一个「少了一截但看不出原因」的症状**。

现在 `measureRow()` 也先量昵称宽度，再用同一个 `avail - nw` 排版正文，两者必然一致。

### 三、入场动画会让「同一时刻的 update + render」拍到全透明

新消息不进静态层，而是作为「正在淡入的那一条」由 `render()` 逐帧叠加，200ms 后才烘进静态层。所以如果 `update(fresh, now)` 和 `render(cr, now)` 用**同一个** `now`，动画进度就是 `t=0` → alpha 0 → **整帧空白**。

这不是理论问题：M1 的 `--dump` 第一版就是这样，对着一个确实收到 3 条消息的房间拍出了一张全空白 PNG。

`--dump` 因此先连续渲染 12 帧（每帧 +33ms）再取最后一帧，把动画推到结束之后——与姊妹项目对自己有状态后处理效果的做法一致。

### 四、图片还没到时的占位文字，步进要按文字宽度而不是图片框

表情与礼物图标在图未到达时会把 token / 礼物名按字面画在图片应在的位置（`drawEmote()` 的回退分支）。**原来的实现无论走哪个分支都只推进 `tok_.emote`（一个 28px 的方框）**，于是占位文字比框宽时，后面的文字直接画在它上面。

实测症状（`--demo` 的礼物行）：

```text
串***: 投喂 小番茄6      ← “×6”的 × 压在“茄”上，只剩一个 6 露在外面
```

三个汉字在 28px 字号下约 84px，是框的三倍，所以这是礼物行的常态而不是边角情况。修法是让 `drawEmote()` **返回它实际推进的距离**：有图时是图片框，无图时是那段文字量出来的宽度。表情那边同样受益——`upower_[水豚噜噜_吃瓜]` 这类 token 同样远宽于方框。

**注意这修的是绘制，不修测量**：`buildBody()` 是把图片替换成 U+FFFC 之后再交给 pango 排版的，所以**换行位置按方框宽度算**。占位文字比方框宽时，一行的实际宽度可能超出测出来的可用宽度。真正的图通常几百毫秒内就到了，实测礼物行拿到图之后完全正常；但离线 `--demo` 没有下载器，永远走回退分支，所以这个偏差在 `--demo` 里看得见。

## 性能

静态层 + 增量移位的做法、实测窗口、以及继续压低开销该动哪个参数，都在
[performance.md](performance.md)。

## 卡片类消息：字段来自文档，尚未在本机观测到

`SUPER_CHAT_MESSAGE` 与 `GUARD_BUY` 走的是同一条连接（只是 `cmd` 不同），字段名取自 `pskdje/bilibili-API-collect` 的 `docs/live/message_stream.md`：

| cmd | 取用字段 | 渲染 |
| --- | --- | --- |
| `SUPER_CHAT_MESSAGE` | `data.user_info.uname` / `user_info.face` / `message` / `price` / `start_time`；`user_info.guard_level` 与 `medal_info.guard_level`（1 总督 / 2 提督 / 3 舰长，后一个可为 `null`） | 醒目留言（置顶图层，青色） |
| `GUARD_BUY` | `data.username` / `guard_level` / `price`（金瓜子，1000 = 1 元） | 上舰卡（实心绿） |

SC 的发送者在 `data.user_info` 里，**不在 `data` 顶层**：文档表里根本没有 `data.uname` / `data.face`，按顶层读会得到空串，卡片就少掉“谁付的钱”那一行（只剩金额和正文）。头像 URL 与其他分支一样过 `httpsUrl()`（样本里 `user_info.face` 常是 `http://`）。

**这两条尚未在本机抓到真实样本**（本机常开的房间太安静，入场/礼物/点赞那些热闹房间没有醒目留言），所以解析器里每个字段都是可选的：字段对不上时得到一条内容更少的消息，而不是把消息丢掉。面板侧的两种卡片样式则已经按参考 CSS 实现在 `--demo` 里（并有单测钉住字段映射）。

`price` 同时以两种形式进模型：`amount` 是给人看的 `"CN¥30"`，`amountValue` 是给置顶图层算停留时长的数字。从显示字符串里反解数字是件容易出错的事，所以宁可两个字段都存。

## 醒目留言：独立的置顶图层

醒目留言**不进滚动列表**。它是对全场说的、读一次就过、并且付了钱的留言；放进列表里它会被后面几条挤走，而且没有自己的办法离开。于是 `PinnedLayer`（`src/notice.cpp`）单独管这些消息：每条自己测量、自己按自己的时钟退场，同时可以挂多条。

- **顶边对齐，最新的在最上面**。新来的挤在最显眼的位置，旧的往下顺延。
- **正文自动换行，绝不省略**。这是它跟上舰卡唯一实质性的区别：卡片的正文按设计就是一行，但醒目留言正是发送者花钱要被读到的东西，截断等于把唯一的内容丢掉。断行位置由 pango 决定，所以画出来的行就是测量过的那些行。
- **按金额停留，到点淡出**，`PinnedLayer::dwellFor()` 就是你给的那张表（含边界，边界值归它自己那一档）。停留结束后用 `kFadeMs` 淡出，然后这条被删掉、下面的一条顶上来的位置——**只有删除才重新堆叠，淡出过程不移动任何东西**，因为透明度不改变布局。
- **单价 0 或缺金额时**取最短档而不是 0：一条出现又在一帧内消失的留言看起来像故障，而不是像功能。

**这个图层没有自己的缓存表面**，和滚动面板相反。滚动面板缓存画面是因为内容无界且基本不变；这里只有寥寥几张卡，而淡出期间每帧透明度都在变。缓存既没有收益，又让逐条透明度不可能实现——一个共享表面只能整体合成。它是直接往这一帧里画的，而它便宜正是因为条目数量少且有上界。

## 头像：四个独立的 bug，症状都是「看起来不对」

这一节值得单列，因为同一条路径上前后踩了四个坑，**每一个的症状都是「画面不对」，而每一个的真正原因都不在它看上去的地方**。而且前两个只有实机才能暴露。

### 一、缓存容量和解码尺寸用了同一个字段

`ImageStore` 最初只有一个 `size` 字段，同时是 LRU 容量**和**解码像素尺寸。传 `96` 做容量，于是每张头像按 **96×96** 解码，而面板画进的是 24px 的框：

```cpp
cairo_set_source_surface(cr, surf.get(), cx - r, cy - r);  // 96x96 图锚在左上角
cairo_paint(cr);                                           // 被 24px 圆裁掉
```

cairo 把源图像锚定在**左上角**，所以 24px 的圆裁到的正是那张图的**左上角 24×24**。现象是「只显示头像的一小块」。

教训：一个字段承担两种职责时，读代码看不出问题，只有数字对不上才会暴露。容量与像素尺寸已拆开（48px 解码 / 256 缓存），并且 `drawAvatar` 不再假设尺寸已正确——先缩放再画，将来万一再不匹配也只是缩放略有损失。

### 二、头像被烘焙进静态层，晚到的图永远补不上

面板把每行**烘焙**进静态层后不再重绘，而头像当时是跟着一起烘焙的。所以头像在消息到达那一刻还没下载完时，烘进去的就是灰色占位圆；**图后来真的到了，也永远没人重画那一行**。

这个 bug 的迷惑性在于**所有计数都正常**：`fetched=48 failed=0`、空 `face` 字段 0 个。图确实到了，只是到的时候没有人重画。

现在头像不烘焙，每帧从行的几何位置画（几十个小圆，开销可忽略）。迟到的图**下一帧自然就在**，不需要任何重绘机制。

### 三、动画行被画了两次

改成每帧画之后，`update()` 仍然把动画中的那行 push 进 `rows_`，于是 `drawAvatars()` 画了一遍（无位移的最终位置），动画 group 里又画了一遍。表现是**头像钉在原地不动，而旁边的文字从左边滑过来**。

修法：`animating_` 时 `drawAvatars()` 跳过 `rows_.back()`，那行交给 group 负责，头像和文字一起位移、一起淡入。

### 四、位置公式用了下边缘而不是上边缘

```cpp
const double bottom = r.y + r.h;
const double screenTop = height_ - (contentH_ - bottom);   // ← 多了 r.h
```

可见窗口是内容的最后 `height_` 像素，所以一行的**上边缘**映射到屏幕要减去窗口起点：`screenTop = height_ - (contentH_ - r.y)`。用下边缘等于多加一个行高，**每个头像都比自己的文字低整整一行**。

而动画行走 `render()` 里的 `height_ - animRowH_`，那个是对的——于是「滑入的头像」和「静止的头像」各自都对不上自己的文字，看上去就像滑入动画的头像对不上实际头像。

**这个 bug 静态 dump 里一眼可见**，前三轮没看出来是白捡的：它与前三个不同，不需要实机复现。

### 两种「灰」不是一回事

- **站方默认头像**：URL 是 `https://i0.hdslb.com/bfs/face/member/noface.jpg`，文件名就写着 noface。多个不同用户共用同一个文件。浅灰色「电视头」火柴人。
- **占位圆盘**：本项目在图未到达时画的纯灰圆。

实测一个热门房间里两者同时存在（48 个不同头像里，51 次 URL 观测中有 4 次重复，全部指向 noface.jpg）。**分不出来的时候要看计数，而不是看画面**：`failed=0` 且 URL 有重复 ⇒ 站方默认；`failed` 不为 0 或 pending 不降 ⇒ 占位。

## 表情弹幕的图片

**表情分两种载体，两种都要接，否则只会看到文字。**

| 形式 | 触发条件 | 正文 | 图片位置 |
| --- | --- | --- | --- |
| 整条就是一个表情 | `info[0][12] == 1`（`dm_type`） | 表情名，**不带方括号** | `info[0][13].url` |
| 正文里内嵌表情 | `dm_type == 0` 且 `extra.emots != null` | 混排文本 | `extra.emots["[token]"].url` |

主播自定义表情（主播自己上传的那套）走的是**第一种**。实测抓包（房间 545068，2026-10-04）：

```text
cmd        DANMU_MSG
info[1]    嘻嘻                  ← 表情名，没有方括号
info[0][12]  1                ← dm_type：0 文本，1 表情，2 语音
info[0][13]  {"bulge_display":1,"emoticon_unique":"room_545068_9780","height":162,
              "in_player_area":1,"is_dynamic":0,
              "url":"http://i0.hdslb.com/bfs/live/46ec99....png","width":162}
info[0][14]  {}                ← voice_config
info[0][15].extra  {...,"dm_type":1,"emoticon_unique":"room_545068_9780","emots":null,...}
```

**同一种载体里，正文有两种写法**，这是最容易看漏的地方：

| `emoticon_unique` | 正文 | 例子（实测） |
| --- | --- | --- |
| `room_<房间号>_<id>` | 裸表情名 | `room_545068_9780` → `嘻嘻`；`room_1852504554_97350` → `Evil挥手` |
| `upower_[token]` | **带方括号** | `upower_[Neuro sama收藏集_晚安]` → `[Neuro sama收藏集_晚安]`；`upower_[水豚噜噜_吃瓜]` → `[水豚噜噜_吃瓜]` |

也就是说：**方括号不代表它是内联 token**。看到正文是 `[xxx]` 就去查 `extra.emots`，在收藏集 / 表情包表情上必然查不到（`emots` 是 `null`），然后就把 token 当文字画了出来。判据只有 `dm_type`。

几个要点：

1. **`extra.emots` 在这种消息里是 `null`。** 只读 `extra.emots` 的版本会把每个表情画成名字文字——症状是「只显示 token」，而且看上去完全像渲染出了问题。
2. `info[0][13]` 可能是嵌套对象，也可能是 **JSON 字符串**（同一条消息里的 `info[0][14]` 就是字符串 `"{}"`），两种写法都得读。
3. 这个 `url` 有时是 **`http://`** 而不是 https（`/bfs/live/` 上实测过；`/bfs/garb/` 上是 https）。同一个路径用 https 访问实测返回同一张图（均为 162×162 PNG，15906 字节），所以下载前统一改成 https：照原样取会在过滤 80 端口的网络里彻底失败，而那个症状和「图没下载到」一模一样。

对照参考：[blivechat](https://github.com/xfgryujk/blivechat) → [blivedm `models/web.py`](https://github.com/xfgryujk/blivedm)：`info[0][12]` = `dm_type`，`info[0][13]` = `emoticon_options`，`blivechat` 在 `dm_type == 1` 时把整条当成图片消息（`ContentType.EMOTICON`）——上面三种 `emoticon_unique` 写法它一视同仁。

`extra.emots` 直接给出 `{ "[token]": {url,width,height} }`，所以内嵌表情的图片也不需要自己查表。同一房间的实测样本：

```text
info[1]    喵锅[委屈]忍耐啊！！[委屈]        ← dm_type=0，一条里两个内嵌表情
extra.emots  {"[委屈]":{"count":1,"descript":"[委屈]","emoji":"[委屈]",
                        "emoticon_id":218,"emoticon_unique":"emoji_218","height":20,
                        "url":"http://i0.hdslb.com/bfs/live/....png","width":20}}
```

`emoticon_unique` 的前缀目前观察到四种：`emoji_<id>`（内嵌的官方小表情）、`room_<房间号>_<id>`（主播自定义）、`upower_[token]`（收藏集 / 表情包表情）、`official_<id>`（整条一个表情的官方表情，来自 blivedm 文档，本机未抓到）。**前缀不重要，重要的是判据 `dm_type` 与图片所在的字段**。

排版做法：**先把每个表情替换成一个对象替换字符 U+FFFC，交给 pango 排版**，再把每个视觉行在 U+FFFC 处切开，交替绘制文本段和图片。这样**换行由 pango 决定**，表情落点由 pango 的排版结果决定；如果自己按 fragment 手工断行，就得重新实现一遍中英文混排的断行点，那才是难的部分。

U+FFFC 本身不会被画出来。图片没到时把 token 按字面文本画在图片应在的位置，消息仍然可读。

表情与头像**用两个 `ImageStore` 实例**：表情是一个被所有人反复复用的很小的集合，值得比头像大得多的缓存；头像则一人一张。

## 跨重启的历史

窗口语义、原子写与节流、按房间分、分割线为什么是列里的一行、以及 systemd 的
`StateDirectory`，都在 [history.md](history.md)。

## 未实测 / 已知缺口

- `SUPER_CHAT_MESSAGE` / `GUARD_BUY` 的真实载荷（本机这几个房间都太安静；倒是抓 M2 那三类事件的热闹房间里没有醒目留言）。
- 醒目留言的字数上限（30 元 40 字 … 2000 元 100 字）**没有实现**：那是发送端的限制，渲染端只负责把收到的内容完整显示。
- 顶部 / 底部 / 逆向 / 高级弹幕的 `mode` 取值语义。对叠加层来说**这很可能不重要**：面板本来就把它们当普通文本行渲染，`mode` 只影响弹幕在视频上的位置，不影响聊天气泡。
- 弹幕洪峰。本机样本房间 30 秒最多 29 条；热门房间会到每秒数十条。已有界队列（`kPendingCap=400`）与每帧上限（`kMaxPerFrame=40`）兜底，但丢弃策略尚未在高流量下验证。**入场事件让这条更紧迫**：本机实测 1.57 条/秒，是弹幕的 4 倍，且默认被合并成每 5 秒一行，所以洪峰时的行为主要取决于弹幕而不是入场。
- 语音弹幕（`dm_type == 2`，`info[0][14].voice_config`）：未处理，正文会当作普通文字画出来。
- Twitch：IRC 正在退役（见评估），EventSub 需要用户自带 token。
- **礼物图标的 webp 版本**（`SEND_GIFT_V2` 的 `10.35.2`）不取，只取 PNG（`10.35.1`），**即使 `10.35.1` 缺失也不用 webp 顶上**：gdk-pixbuf 是否带 webp 解码器取决于发行版，取了就变成"这台机器上有图标、那台上没有"。没有 URL 时该 fragment 回退成礼物名文字，行照样读得通。
- **入场 / 礼物 / 点赞的合并窗口只能用时间，不能按重要度筛**：入场载荷里带着 `dmscore`（B 站自己给的分，实测 110 条里 106 条是 4、2 条是 8、1 条是 30、1 条是 3），按它筛可以只留 3/110，但那也就几乎什么都不显示了。站点侧的 `ENTRY_EFFECT` 是同一件事的另一套筛法（站方自己只对 18/110 做入场动画），本文选择不实现它，见[这三类事件已经不是 JSON 了](#这三类事件已经不是-json-了)。
- **礼物行不显示礼物价值。** `10.5` 的单价与 `10.3` 的数量都在，但人气票是 100 金瓜子 = 0.1 元，把这种数字印在叠加层上只会是噪声；要做也应当只在总额够大时显示，那需要另定一个阈值。

## 调试方法

不开 OBS 也能看到全部状态：`--dump` 直接出一张 PNG，卡片上写着当前处于哪一步。

```bash
./pw-live-danmaku --room 545068 --dump /tmp/x.png --count 3 --verbose   # 等到收到 3 条为止
```

单测（协议层无 UI，直接测）：

```bash
make test
```