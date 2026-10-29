#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["websocket-client>=1.7"]
# ///

"""Launch a browser, wait for a manual Bilibili login, and print the cookie.

The script intentionally does not handle Bilibili passwords. It opens the
normal login page in a temporary browser profile and waits for you to scan the
QR code with the Bilibili app. Cookies are read through the Chrome DevTools
Protocol and written to the terminal; --output also saves the cookie string to
a 0600 file for pw-live-danmaku --cookie-file.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional

try:
    import websocket
except ImportError:
    print(
        "缺少 websocket-client。建议用 uv 运行：\n"
        "  uv run tools/get_bili_cookie.py\n"
        "或者先安装：python3 -m pip install websocket-client",
        file=sys.stderr,
    )
    raise SystemExit(1)

LOGIN_URL = "https://passport.bilibili.com/login"
REQUIRED_COOKIES = {"SESSDATA", "bili_jct", "DedeUserID"}
COOKIE_ORDER = [
    "SESSDATA",
    "bili_jct",
    "DedeUserID",
    "buvid3",
    "buvid4",
    "b_nut",
]
BROWSER_NAMES = [
    "google-chrome",
    "google-chrome-stable",
    "chromium",
    "chromium-browser",
    "microsoft-edge",
    "microsoft-edge-stable",
    "brave-browser",
]


def find_browser(path: Optional[str]) -> Path:
    if path:
        candidate = Path(path).expanduser()
        if candidate.is_file():
            return candidate.resolve()
        found = shutil.which(path)
        if found:
            return Path(found)
        raise FileNotFoundError(f"找不到浏览器：{path}")

    for name in BROWSER_NAMES:
        found = shutil.which(name)
        if found:
            return Path(found)

    raise FileNotFoundError(
        "没有找到受支持的浏览器，请用 --path 指定 Chrome/Chromium/Edge 的路径"
    )


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def fetch_json(url: str, timeout: float = 1.0) -> object:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.load(response)


def wait_for_page(port: int, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    list_url = f"http://127.0.0.1:{port}/json/list"
    last_error: Optional[Exception] = None

    while time.monotonic() < deadline:
        try:
            pages = fetch_json(list_url)
            if isinstance(pages, list):
                candidates = [
                    page
                    for page in pages
                    if page.get("type") == "page"
                    and page.get("webSocketDebuggerUrl")
                ]
                for page in candidates:
                    if "bilibili.com" in page.get("url", ""):
                        return page["webSocketDebuggerUrl"]
                if candidates:
                    return candidates[0]["webSocketDebuggerUrl"]
        except (OSError, ValueError, urllib.error.URLError) as exc:
            last_error = exc
        time.sleep(0.2)

    raise RuntimeError(f"浏览器调试端口没有就绪：{last_error}")


class CDP:
    def __init__(self, websocket_url: str):
        self.websocket = websocket.create_connection(
            websocket_url,
            timeout=5,
            origin="http://127.0.0.1",
        )
        self.next_id = 0

    def call(self, method: str, **params) -> dict:
        self.next_id += 1
        request_id = self.next_id
        self.websocket.send(
            json.dumps({"id": request_id, "method": method, "params": params})
        )

        while True:
            message = json.loads(self.websocket.recv())
            if message.get("id") != request_id:
                continue
            if "error" in message:
                raise RuntimeError(
                    f"CDP {method} 失败：{message['error'].get('message', message['error'])}"
                )
            return message.get("result", {})

    def close(self) -> None:
        try:
            self.websocket.close()
        except Exception:
            pass


def bilibili_cookies(cookies: list[dict]) -> dict[str, str]:
    result: dict[str, str] = {}
    for cookie in cookies:
        domain = str(cookie.get("domain", "")).lstrip(".").lower()
        name = str(cookie.get("name", ""))
        if (domain == "bilibili.com" or domain.endswith(".bilibili.com")) and name:
            result[name] = str(cookie.get("value", ""))
    return result


def format_cookie(cookies: dict[str, str]) -> str:
    names = [name for name in COOKIE_ORDER if name in cookies]
    names.extend(
        name
        for name in sorted(cookies)
        if name not in names and name in REQUIRED_COOKIES
    )
    return "; ".join(f"{name}={cookies[name]}" for name in names)


def write_cookie(path: Path, cookie: str) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, cookie.encode("utf-8"))
    finally:
        os.close(fd)
    os.chmod(path, 0o600)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="打开浏览器，手动登录 B 站，导出 pw-live-danmaku 可用的 Cookie。"
    )
    parser.add_argument(
        "--path",
        "--browser-path",
        dest="browser_path",
        help="浏览器可执行文件路径；默认自动查找 Chrome/Chromium/Edge",
    )
    parser.add_argument(
        "--output",
        help="把 Cookie 写入这个文件（权限 0600），例如 ~/.config/pw-live-danmaku/cookie",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300,
        help="等待登录的秒数（默认：300）",
    )
    parser.add_argument(
        "--keep-open",
        action="store_true",
        help="拿到 Cookie 后不关闭浏览器",
    )
    parser.add_argument(
        "--profile-dir",
        help="复用指定 profile（默认使用临时目录，退出时删除）",
    )
    parser.add_argument(
        "--keep-profile",
        action="store_true",
        help="保留临时 profile，方便下次继续登录",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.timeout <= 0:
        print("--timeout 必须大于 0", file=sys.stderr)
        return 2

    try:
        browser = find_browser(args.browser_path)
    except FileNotFoundError as exc:
        print(exc, file=sys.stderr)
        return 1

    profile = (
        Path(args.profile_dir).expanduser().resolve()
        if args.profile_dir
        else Path(tempfile.mkdtemp(prefix="pw-live-danmaku-cookie-"))
    )
    profile.mkdir(parents=True, exist_ok=True)
    if args.profile_dir:
        args.keep_profile = True

    port = free_port()
    command = [
        str(browser),
        f"--remote-debugging-port={port}",
        "--remote-allow-origins=*",
        f"--user-data-dir={profile}",
        "--no-first-run",
        "--no-default-browser-check",
        LOGIN_URL,
    ]

    print(f"浏览器：{browser}")
    print(f"临时 profile：{profile}")
    print("请在打开的浏览器里扫码登录 B 站；登录完成后脚本会自动继续。")

    process = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    cdp: Optional[CDP] = None
    cookie: Optional[str] = None

    try:
        websocket_url = wait_for_page(port, min(30, args.timeout))
        cdp = CDP(websocket_url)
        cdp.call("Network.enable")

        deadline = time.monotonic() + args.timeout
        last_status = 0.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("浏览器提前退出了")

            result = cdp.call("Network.getAllCookies")
            cookies = bilibili_cookies(result.get("cookies", []))
            missing = REQUIRED_COOKIES - cookies.keys()

            if not missing:
                cookie = format_cookie(cookies)
                break

            now = time.monotonic()
            if now - last_status >= 2:
                print(f"等待登录，还缺少：{', '.join(sorted(missing))}")
                last_status = now
            time.sleep(0.5)

        if not cookie:
            raise TimeoutError("等待登录超时，没有拿到完整 Cookie")

        print("\nCookie：")
        print(cookie)
        if args.output:
            output = Path(args.output)
            write_cookie(output, cookie)
            print(f"\n已写入：{output}（权限 0600）")

        print("\n用法：")
        print(f'  pw-live-danmaku --room 5861873 --cookie-file "{args.output or "cookie.txt"}"')

    except (RuntimeError, TimeoutError, OSError, ValueError) as exc:
        print(f"失败：{exc}", file=sys.stderr)
        return 1
    finally:
        if cdp:
            cdp.close()
        if not args.keep_open:
            try:
                process.terminate()
                process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    process.kill()
                except OSError:
                    pass
        if not args.keep_profile:
            shutil.rmtree(profile, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
