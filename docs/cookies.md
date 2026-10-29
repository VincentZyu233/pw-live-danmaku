# 凭据

B 站的未登录连接拿得到弹幕正文、颜色、头像和表情，但会对**昵称**打码（`LOG_IN_NOTICE` 原文
`为保护用户隐私，未登录无法查看他人昵称`）。要真昵称就得带 cookie。

打码**不是全局的**：同一条匿名连接上，`INTERACT_WORD_V2` 的 110 条里 106 条昵称被打码，而
`LIKE_INFO_V3_CLICK` 的 6 条**全部**是实名，`ENTRY_EFFECT` 的 18 条也全部实名。所以未登录时画面上
仍然会有几个真名。实测数字与理由见 [internals.md](internals.md#两个反直觉的实测结果)。

## 怎么拿

浏览器 devtools → Network → 任意一个发往 B 站的请求 → Request Headers → Cookie，拷成
`SESSDATA=...; bili_jct=...; DedeUserID=...` 这样的字符串，存成文件：

```bash
mkdir -p ~/.config/pw-live-danmaku
printf '%s' 'SESSDATA=...; bili_jct=...; DedeUserID=...' > ~/.config/pw-live-danmaku/cookie
chmod 600 ~/.config/pw-live-danmaku/cookie
./pw-live-danmaku --room 545068 --cookie-file ~/.config/pw-live-danmaku/cookie
```

只需要 `SESSDATA` 就能显示昵称；其余字段是 B 站自己的前端也在带的，照抄即可。

## 自动导出（可选）

`tools/get_bili_cookie.py` 会自动打开浏览器、等待你手动扫码登录，然后通过
Chrome DevTools Protocol 读取 Cookie：

```bash
uv run tools/get_bili_cookie.py --path /usr/bin/google-chrome
uv run tools/get_bili_cookie.py --path /usr/bin/microsoft-edge \
  --output ~/.config/pw-live-danmaku/cookie
```

脚本默认使用临时浏览器 profile，不碰日常浏览器；登录完成后打印
`SESSDATA`、`bili_jct`、`DedeUserID` 以及可用的 `buvid3`。给 `--output` 时会把
同一串 Cookie 写入文件并设为 `0600`。它不处理密码，登录过程始终由你在浏览器里完成。

如果自动检测不到浏览器，用 `--path` 指定 Chrome、Chromium 或 Edge 的可执行文件。
需要再次使用同一个登录态时可以加 `--profile-dir DIR`，但这会把 Cookie 留在该目录里，
请自己控制权限。

## 两种给法

| 参数 | 值的来源 | 会不会进 `ps(1)` |
| --- | --- | --- |
| `--cookie-file PATH` | 文件，可 `chmod 600` | 不会，只传路径 |
| `--cookie STR` | 命令行 | **会**，任何本地用户都能读到，所以程序会打印警告 |

**推荐 `--cookie-file`。** 两者互斥，同时给是非零退出码。两种方式下 cookie **都不会被写进日志**，
也不会出现在任何错误信息里。

## systemd：写 `%h`，不要写 `~`

unit 里的路径必须用 systemd 的 `%h` specifier：

```bash
make install-service SERVICE_ARGS="--room 545068 --cookie-file %h/.config/pw-live-danmaku/cookie"
```

**systemd 不展开 `~`**，它把这个字符原样交给程序。程序再按字面路径 `~/.config/...` 去读，而 unit 的
`WorkingDirectory` 是 `$HOME`，于是它实际找的是 `$HOME/~/.config/...`——一个不可能存在的路径。

这个坑的实际后果不是一次报错，而是**每 3 秒一次崩溃重启**：程序退出 1，`Restart=on-failure` +
`RestartSec=3` 把它拉起来，再崩，再拉。`StartLimitBurst` 的默认值（10 秒内 5 次）拦不住它，因为每次
重试本身也在消耗配额，窗口永远不满。现在 unit 里显式写了 `StartLimitIntervalSec=60` /
`StartLimitBurst=5`，5 次失败后就停住并保持 failed，等人来看。

程序自己也展开 `~`（`--cookie-file` 与 `--history-file` 都展开）。这不是冗余：shell 里的 `~` 是 shell
的功能，从不经过程序，所以它对 systemd 写的 unit 一无所知。两者都要——`%h` 只有 unit 能用，而程序侧的
展开让同一份参数在任何来源下含义一致。

unit 里还需要 `ReadOnlyPaths=-%h/.config/pw-live-danmaku` 才能读到那个 cookie 文件；前缀 `-` 不能去掉，
否则没有 cookie 的用户会因为目录不存在而在 `ExecStart` 之前就起不来，而 cookie 是可选的。

## 退出码

`--room` 缺失、`--cookie` 与 `--cookie-file` 同时给、cookie 文件读不到，都返回非零退出码，
**绝不会返回 0**——否则 systemd 的重启策略下会变成静默重启循环并报告 SUCCESS。同样的道理适用于
未知的命令行参数：那不是请求帮助，是参数错误。

## Twitch 侧

Twitch 的 EventSub 读聊天要求用户自带带 `user:read:chat` 的 token，属于同一类「用户自带凭据」的
口子。实现尚未落地，见 README 的「未做的事」。

仓库里的 `.gitleaks.toml` 沿用 gitleaks 默认规则，只额外放行了三个 B 站公开、每日轮换的 WBI key
（它们是签名回归向量，不是凭据）；`.gitignore` 里也排除了 `*.cookie` 与 `cookie.txt`。
**会话 cookie 不要提交进仓库。**