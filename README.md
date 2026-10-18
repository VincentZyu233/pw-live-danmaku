<div align="center">

# pw-live-danmaku

**Renders a Bilibili live chat as a transparent overlay panel, published to OBS as a PipeWire video node.**

[English](README.md) · [简体中文](README.zh-CN.md)

<a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-informational?style=flat-square" alt="MIT License"></a>
<a href="https://aur.archlinux.org/packages/pw-live-danmaku-git"><img src="https://img.shields.io/badge/AUR-pw--live--danmaku--git-informational?style=flat-square" alt="AUR package"></a>
<img src="https://img.shields.io/badge/platform-Linux-informational?style=flat-square" alt="Linux">

</div>

<br>

<div align="center">
<img src="docs/panel-480x1080.png" width="300" alt="480x1080 panel: paid messages, a guard card, gift rows, entry notices, a literal &lt;script&gt; line">
</div>

That is `--demo --dump` output at the default `480x1080`, and it covers every message kind the panel supports in one frame: pinned paid messages and a guard card, gift rows merged with a count, entry notices merged into one line, avatars, emotes, and a `<script>` rendered as literal glyphs. The background is **genuinely transparent**: there is no plate behind the panel when you overlay it on a stream.

---

> One process · No browser · **Zero child processes** · Nothing rendered while no consumer is attached · 94 MB private memory

## Features

It replaces the "OBS browser source plus a large blob of CSS reshaping the chat DOM" approach: the same picture, one process, zero child processes, none of it sitting in a browser.

- **A chat panel**, not scrolling danmaku: bottom-aligned, per-role colours, a colour bar on the left, hanging indent on wrap
- **Avatars** are fetched live and cropped to a circle; **emote messages** are drawn from the inline images Bilibili hands over
- **Gifts, entries and likes** all land in the same column: the gift verb (`投喂`) is gold, the gift name that stands in when the icon has not arrived is pink, and both belong to the gift class together with the left colour bar; entries and likes are dimmed so they do not compete with what was actually typed
- **Repeated gifts from one viewer merge into a row**: within a window they are grouped by viewer *and* gift with the count summed (`×10`) instead of spamming one row per tap; entry notices merge the same way
- **Paid messages are pinned on their own layer**: top-aligned, wrapped and never truncated, dwell time of 1 minute to 2 hours by amount, several at once
- **Guard cards** are solid green and scroll with the list
- **Entry animation**: a new message takes its slot immediately, pushes the old ones up, then slides in
- **History across restarts**: `--history N` keeps the last N rows on disk and paints them back at the next start, above a rule that separates them from this session's live rows
- **A local WebSocket feed**: `--ws [PORT]` broadcasts every row as one text frame — `avatarUrl`, `name`, `body`, tab-separated — and `--ws-rtf` styles the name and body with [Resonite rich-text tags](https://wiki.resonite.com/Text_formatting) for a text component that parses them
- **Fully transparent by default**, with outlines on the text only, so it stays legible over bright content
- **Any text is rendered literally** — a `<script>` in the chat is glyphs, nothing is parsed as markup
- Any output size (`WxH`) and an adjustable frame-rate ceiling
- **Tune the layout without a live room**: `--demo` draws a fixed set of messages

Every dependency is a system library from the distribution: no browser, no Chromium / OBS browser source, and no language package manager involved.

## Install

### From the AUR

```bash
paru -S pw-live-danmaku-git     # or: yay -S pw-live-danmaku-git
```

The binary is `pw-live-danmaku` and lands on `PATH`, so there is no checkout to maintain. The package provides and conflicts with `pw-live-danmaku`, ships `x86_64` and `aarch64`, and follows the latest commit. It builds with `PORTABLE=1` and additionally strips `-march=native` out of the builder's own `CXXFLAGS`: a builder with a customised `makepkg.conf` would otherwise install a binary bound to the CPU it was built on, which dies with SIGILL on another machine. The video-node library is fetched as a second source, so no submodule checkout is needed. `obs-pwvideo` and a CJK font (`noto-fonts-cjk`) are optional dependencies.

The unit is rendered at `/usr/lib/systemd/user/pw-live-danmaku.service` with upstream's Makefile defaults, **which include upstream's own room number — change it**:

```bash
systemctl --user enable --now pw-live-danmaku
systemctl --user edit pw-live-danmaku    # override ExecStart=
```

Removal: `pacman -Rns pw-live-danmaku-git`. Do not run `make install-service` on that install: it writes to `~/.config/systemd/user/`, which shadows the packaged unit.

### From source

```bash
# Dependencies (Arch)
sudo pacman -S --needed base-devel cairo pango gdk-pixbuf2 libpipewire curl openssl \
                   brotli zlib

git submodule update --init --recursive   # the video-node library is a submodule
make                                      # → ./pw-live-danmaku
make PORTABLE=1                           # the same, without -march=native (distribution)
```

`make` fails with an explicit message if the submodule is not checked out. The video node comes from [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface) (a git submodule), pinned to the same commit as the sibling project `pw-mpris-visualcard`.

Architecture, build flags, the other `make` targets, repository layout and the dependency licences are in [docs/development.md](docs/development.md).

## Usage

### Run it

```bash
./pw-live-danmaku --room 545068
./pw-live-danmaku --room https://live.bilibili.com/545068    # a b23.tv short link also works
```

The node name (default `pw-live-danmaku`) is printed to the terminal.

### Add it to OBS

OBS ships `linux-pipewire`, which goes through xdg-desktop-portal, can only capture screens and windows, and therefore **cannot select this node**. Use the [**obs-pwvideo**](https://github.com/tasokait/obs-pwvideo) plugin instead.

1. Sources **+** → **PipeWire Video** (provided by `obs-pwvideo`).
2. Select **Live Chat**. The dropdown displays the node description (`--desc`, default `Live Chat`) and connects to the node name (`--node`, default `pw-live-danmaku`); the two are separate fields. obs-pwvideo enumerates nodes when the properties dialog is opened and does not refresh an open dialog; if the node is absent, confirm the process is running and reopen it.
3. **Set the source width and height to exactly `--size`** (default `480x1080`). A smaller negotiated size is **clipped, not scaled** — a mismatch loses part of the panel.
4. Overlay it on the scene; the background is transparent to begin with.

### Autostart

`pw-live-danmaku.service` is a template holding `@REPO@` and `@ARGS@`, so it is rendered rather than copied at install time:

```bash
make install-service                            # render into ~/.config/systemd/user/
make install-service SERVICE_ARGS="--room 545068 --size 480x1080 --fps 30 --history 60"
make uninstall-service
systemctl --user enable --now pw-live-danmaku   # enabling and starting is still yours
systemctl --user restart pw-live-danmaku        # restart after changing the arguments
```

`make install-service` writes the unit and runs `daemon-reload`; it never enables or starts anything. With a cookie, write paths as systemd's `%h`, not `~` — **systemd does not expand `~`** — or the unit dies and restarts every 3 seconds. The unit also carries `StateDirectory=pw-live-danmaku` so `--history` has somewhere writable. Both explained in [docs/cookies.md](docs/cookies.md).

### Verify it without OBS

```bash
./pw-live-danmaku --demo --dump /tmp/panel.png                       # fake data, no network
./pw-live-danmaku --room 545068 --dump /tmp/x.png --count 3 -v      # real chat, exit after 3 rows
make verify                                                          # attach a consumer, land the frames
make test                                                           # protocol and history-format tests
```

## Nicknames are masked unless you log in

An anonymous connection gets message bodies, colours, avatars and emotes, but Bilibili masks the **nickname** (`为保护用户隐私`). For real nicknames, copy a cookie out of the browser devtools and keep it in a file:

```bash
mkdir -p ~/.config/pw-live-danmaku
printf '%s' 'SESSDATA=...; bili_jct=...; DedeUserID=...' > ~/.config/pw-live-danmaku/cookie
chmod 600 ~/.config/pw-live-danmaku/cookie
./pw-live-danmaku --room 545068 --cookie-file ~/.config/pw-live-danmaku/cookie
```

`--cookie-file` is preferred over `--cookie`: the latter puts the session in the process arguments, where any local user can read it out of `ps(1)`. Neither form ever writes the cookie to the log.

**The masking is not global**: on the same anonymous connection, entry notices and chat messages come back masked while **likes** and the site's own entry animations carry real names, so an unauthenticated overlay still shows a few real names. Which fields the cookie needs, how to write the path in a unit, and why the program expands `~` itself are in [docs/cookies.md](docs/cookies.md); the measurements behind that claim are in [docs/internals.md](docs/internals.md#两个反直觉的实测结果).

## Options

### Room and identity

| Option | Default | Description |
| --- | --- | --- |
| `--room ID\|URL` | required | Room number or room URL; a `b23.tv` short link resolves too |
| `--cookie STR` | | Raw cookie header. Visible in `ps(1)`, and the program warns about it |
| `--cookie-file PATH` | | Read the cookie from a file (recommended, can be `chmod 600`). A leading `~` is expanded to `$HOME`. Details in [docs/cookies.md](docs/cookies.md) |

### Layout and fonts

The panel is laid out inside the canvas `--size` gives it: the width sets the wrap width, the height decides how many rows fit. Row height follows `--font-size` and **is not scaled to the height** — ask for a denser panel by asking for a smaller font.

| Option | Default | Description |
| --- | --- | --- |
| `--size WxH` | `480x1080` | Output size. **The OBS source size must match**: a smaller negotiated size is clipped, not scaled |
| `--font NAME[,...]` | CJK fallback chain | Font family chain, resolved per character by pango, so a Latin family can be paired with a CJK one |
| `--font-size N` | `28` | Chat text size in px, for the username and the body. The avatar box follows it: it is always one line tall |
| `--card-font-size N` | `30` | Card text size in px, for the paid and membership card lines |
| `--font-file PATH` | | Register a font file (or a directory) with fontconfig at startup, for this process only. Repeatable |

### Message merging

| Option | Default | Description |
| --- | --- | --- |
| `--entry-merge MS` | `5000` | Entry-notice merge window: everyone who arrives within it becomes one row (`某某 等 7 人进入了直播间`). Measured in a busy room, entries arrive at 1.57/s against 0.36/s of chat, so one row each pushes most of what was typed off the top. `0` gives every notice its own row |
| `--gift-merge MS` | `3000` | Gift merge window: one viewer sending the same gift within it becomes one row with the count summed (`投喂 人气票 ×10`). Gift buttons are tapped repeatedly — ten taps would otherwise push ten real messages off the top. The key is **viewer + gift**, so one viewer sending two gifts is still two rows; the avatar is in the key too, because anonymous connections mask nicknames into the same few shapes. The row waits out the window, so a lone gift appears this much late. `0` gives every gift its own row |

### History across restarts

| Option | Default | Description |
| --- | --- | --- |
| `--history N` | `0` | Keep the last N rows on disk and paint them back at the next start, above a rule. `0` is off: nothing is written or read |
| `--history-file PATH` | `$XDG_STATE_HOME/pw-live-danmaku/history.json` | Where the history lives; a leading `~` is expanded to `$HOME` |
| `--history-label TEXT` | `上次` | Text on the rule between restored and live rows; empty draws the rule alone |

Paid messages are not kept: they are pinned with a dwell clock, and re-pinning yesterday's would be a lie about when it was paid. The behaviour — atomic writes, throttling, per-room files, the writable directory the unit needs — is in [docs/history.md](docs/history.md).

### WebSocket feed

| Option | Default | Description |
| --- | --- | --- |
| `--ws [PORT]` | off; `8765` when given without a port | Serve every row on a local WebSocket: one text frame per message, three tab-separated fields — `avatarUrl`, `name`, `body`. Bound to `127.0.0.1`, plaintext and with no authentication, so it is a pipe for a consumer on this machine and **must not be exposed past it** |
| `--ws-rtf` | | Wrap the name and body fields in [Resonite rich-text tags](https://wiki.resonite.com/Text_formatting): the role colour on the name, the gift accent on a gift's verb and name, `<alpha>` dimming on entry and like rows, and `<noparse>` around anything the viewer typed, so chat text cannot inject tags. Requires `--ws` |

The frames carry the rows the panel drew, after `--entry-merge` and `--gift-merge`: ten taps of one gift button are one frame counting ten, not ten frames. Nothing is replayed, so a client that connects mid-stream sees the next message and nothing older. Real captured output at `--ws-rtf`, one frame per line (the avatar field is shown as `<url>` here):

```text
<url>	<color=#9AA0A6>Loyers</color>	直接下班吧
	<alpha=#80><color=#9AA0A6>红桃老K_-</color></alpha>	<alpha=#80>等 9 人进入了直播间</alpha>
```

The avatar field is a URL the panel itself fetched, and is empty on rows that have no avatar box, such as entry notices. The body keeps an emote as the token the sender typed (`[汤圆]`), because a text field cannot show the picture. A tab, newline or other control character in any field becomes a space, so chat text cannot split a field or add one. Any WebSocket client reads it: `websocat ws://127.0.0.1:8765/`, a browser console, or a Resonite WebSocket client.

### Output and diagnostics

| Option | Default | Description |
| --- | --- | --- |
| `--node NAME` | `pw-live-danmaku` | PipeWire node name; this is the value behind the OBS dropdown entry |
| `--desc TEXT` | `Live Chat` | Node description. **This is what the OBS dropdown displays**, not `--node` |
| `--fps N` | `30` | Frame-rate ceiling; consumers may negotiate lower, never higher |
| `--count N` / `--seconds N` | `0` | Exit after drawing N rows / running N seconds. `--count` counts the rows that **reach the panel**, so events folded away by `--entry-merge` or `--gift-merge` do not count |
| `--demo` | | Draw a fixed set of messages and no network at all. Overrides `--room` |
| `--dump FILE` | | Render one sample frame to PNG and exit (it waits for avatars and emotes to download) |
| `--verbose`, `-v` | | Log the protocol handshake, every message, and the avatar / emote download counters |
| `--help`, `-h` | | Print a short option summary |

## Performance

| Metric | Value |
| --- | --- |
| CPU | **≈3.0% of one core** (`480x1080` at 30fps, with a consumer attached) |
| Per frame | ≈1.0 ms |
| With no consumer attached | the render callback is not called at all, so the panel costs nothing |
| Private (anonymous) memory | **94 MB** |
| Child processes | **0** |

The trick is that laid-out text never changes again, so the whole picture accumulates on one static surface; each new message shifts it up by a row and draws itself in the strip that just became visible — **each message costs its own height, not the panel's**. Avatars and the animating row are redrawn every frame, which is a few dozen small discs. The measurement window, the breakdown of where the time goes and which knob moves which part are in [docs/performance.md](docs/performance.md).

## Documentation

| Document | For | Contents |
| --- | --- | --- |
| [docs/internals.md](docs/internals.md) | developers | Protocol findings, places where the community docs are wrong, pitfalls and measurements, debug commands — **read before changing code** |
| [docs/performance.md](docs/performance.md) | users, packagers | Measured CPU and memory, where the time goes, cutting CPU |
| [docs/history.md](docs/history.md) | users, developers | Using the cross-restart history, and how it behaves |
| [docs/cookies.md](docs/cookies.md) | users | Getting a cookie, passing it safely, `%h` versus `~` in a unit |
| [docs/development.md](docs/development.md) | contributors, packagers | Architecture, build flags and targets, repository layout, tests, dependency licences |

The `docs/` articles are Chinese-only for now; both READMEs are maintained.

## Not implemented

- **Twitch**: IRC is being retired (community-marked deprecated on 2026-09-02) and would have to move to EventSub, where reading chat needs a user-supplied token with `user:read:chat`. The architecture already has the seams for it (the `Site` abstraction, emote fragments), but no implementation exists yet. **Bilibili is the only working site today.**
- **Automatic room detection**: the room number has to be given by hand. A browser media session cannot supply it (Firefox reports a `blob:` `xesam:url`), and Chromium has no MPRIS on Linux.
- **Reconnection**: a failed connection exits and prints the reason onto the panel.
- **Floods in popular rooms**: there are bounds (a 400-entry queue and a 40-row-per-frame cap), but the drop policy has not been verified under real load.
- The **length cap on paid messages** (40 characters at CN¥30 up to 100 at CN¥2000) is not implemented: that is a sender-side limit, and the renderer only has to show what arrived.
- **Voice messages** (`dm_type == 2`) are not decoded; the body is drawn as ordinary text.

The full list, with the reasoning for each item, is in [docs/internals.md](docs/internals.md#未实测--已知缺口).

## License

Released under the **MIT License**, copyright **ZokuTe** (from 2026); see [LICENSE](LICENSE) for the full text.

The system libraries used at runtime are dynamically linked, none of their code is bundled or modified, and each remains under its own licence — the table is in [docs/development.md](docs/development.md). [obs-pwvideo](https://github.com/tasokait/obs-pwvideo) on the OBS side is a separate GPLv2 program whose only interaction with this project is the runtime data flow through a PipeWire node — no linking, no derivative work — so the two licences do not affect each other.

The video node itself comes from [`pw-video-simple-interface`](https://github.com/zlinux-live-util/pw-video-simple-interface), a git submodule (MIT).