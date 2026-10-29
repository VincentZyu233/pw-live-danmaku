# Browser cookie helper

`get_bili_cookie.py` opens a browser with a temporary profile, waits for you to
log in to Bilibili manually, and reads the cookies through the Chrome DevTools
Protocol.

```bash
uv run tools/get_bili_cookie.py
uv run tools/get_bili_cookie.py --path /usr/bin/google-chrome
uv run tools/get_bili_cookie.py --path /usr/bin/microsoft-edge \
  --output ~/.config/pw-live-danmaku/cookie
```

The script prints the cookie needed by `pw-live-danmaku`:

```text
SESSDATA=...; bili_jct=...; DedeUserID=...; buvid3=...
```

Options:

| Option | Description |
| --- | --- |
| `--path`, `--browser-path` | Browser executable; otherwise Chrome/Chromium/Edge is detected automatically |
| `--output PATH` | Also write the cookie to this file with mode `0600` |
| `--timeout SECONDS` | How long to wait for login, default `300` |
| `--keep-open` | Leave the browser open after the cookie is read |
| `--profile-dir DIR` | Reuse a profile instead of a temporary one |
| `--keep-profile` | Keep the temporary profile after exit |

The script does not ask for or handle the Bilibili password. Login happens in
the browser, normally by scanning the QR code with the Bilibili app. The
temporary profile is removed on exit unless `--keep-profile` is used.

Cookie values are credentials. Do not paste them into issue reports, chat, shell
history, or commits. Prefer `--output` plus `pw-live-danmaku --cookie-file` over
passing the cookie on the command line.
