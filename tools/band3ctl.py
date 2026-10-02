"""band3ctl: drives band3 through its test harness.

band3 started with --test_port=<port> takes one command per line on
127.0.0.1:<port> and answers each with one line of JSON. This sends them:

  python tools/band3ctl.py launch                 start band3 (minimized) and wait for it
  python tools/band3ctl.py state                  any harness command, e.g.
  python tools/band3ctl.py press green+strum_down
  python tools/band3ctl.py wait screen~main timeout=90s
  python tools/band3ctl.py run tests/game/boot.b3t
  python tools/band3ctl.py "hold down; wait frames=90; release all"
  python tools/band3ctl.py window offscreen       restore the game's window off every monitor
  python tools/band3ctl.py window shot out.png    what its window shows (not minimized)
  python tools/band3ctl.py window minimize

Commands joined with ; share one connection, which `hold` needs: band3 lets go
of everything held when a client disconnects. `run` replays a .b3t script: harness commands one per line, # comments. It
stops at the first command that fails, saves a screenshot of the moment, and
exits 1. It prints the replies that carry measurements (`native_view`'s stats). The commands are listed in the README's Test harness section.

`window` works on the game's window itself, through Win32, never activating it,
for checking what the window presents (the harness's screenshots are the
game's picture alone, the renderer's that `renderer` picks): a window launched minimized never paints, so
`offscreen` restores it to the right of every monitor, where it paints but nobody
sees it.

Standard library only.
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field

DEFAULT_PORT = 21071
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(REPO, "out", "build", "win-amd64-release", "band3.exe")


def parse_script(text):
    """The commands in a .b3t script, as (line number, command) pairs."""
    commands = []
    for number, line in enumerate(text.splitlines(), start=1):
        # a comment starts a line or follows whitespace, so band#1 stays whole
        line = re.sub(r"(^|\s)#.*$", "", line).strip()
        if line:
            commands.append((number, line))
    return commands


def split_commands(words):
    """Command-line words as harness commands, split at semicolons."""
    return [c.strip() for c in " ".join(words).split(";") if c.strip()]


def parse_reply(data):
    """One reply line as a dict; a line that isn't JSON reads as a failure."""
    text = data.decode("utf-8", errors="replace").strip()
    try:
        return json.loads(text)
    except ValueError:
        return {"ok": False, "error": "not a harness reply: " + text}


@dataclass
class ScriptResult:
    passed: bool
    line: int = 0
    command: str = ""
    reply: dict = field(default_factory=dict)


def run_script(conn, commands, name, report=None):
    """Sends each command in turn; stops at the first failure and screenshots it.

    A reply with measurements in it (`stats`, from `native_view`) goes to
    report(line number, command, reply) as well, for the script's reader.
    """
    for number, command in commands:
        reply = conn.command(command)
        if not reply.get("ok"):
            safe = re.sub(r"[^A-Za-z0-9_-]", "_", name)
            conn.command(f"screenshot fail-{safe}-line{number}")
            return ScriptResult(False, number, command, reply)
        if report and "stats" in reply:
            report(number, command, reply)
    return ScriptResult(True)


class Connection:
    def __init__(self, port, timeout=None):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        # a wait can take as long as its timeout says
        self.sock.settimeout(timeout)
        self.buffer = b""

    def command(self, line):
        self.sock.sendall(line.encode("utf-8") + b"\n")
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                return {"ok": False, "error": "band3 closed the connection"}
            self.buffer += chunk
        reply, self.buffer = self.buffer.split(b"\n", 1)
        return parse_reply(reply)

    def close(self):
        self.sock.close()


def connect(port, wait_s=0.0):
    """Connects, retrying for up to wait_s seconds while band3 starts."""
    deadline = time.monotonic() + wait_s
    while True:
        try:
            return Connection(port)
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


def place_config(source, exe_folder):
    """Puts source beside the exe as band3.toml, for one start. A band3.toml
    already there is moved aside to band3.toml.band3ctl, so it survives even if
    band3ctl doesn't. Returns what puts the folder back as it was."""
    target = os.path.join(exe_folder, "band3.toml")
    saved = target + ".band3ctl"
    if os.path.exists(saved):
        raise RuntimeError(f"{saved} is left from an earlier --config; put it back as "
                           "band3.toml (or delete it) first")
    had_one = os.path.exists(target)
    if had_one:
        os.replace(target, saved)
    try:
        shutil.copyfile(source, target)
    except OSError:
        if had_one:
            os.replace(saved, target)
        raise

    def restore():
        if had_one:
            os.replace(saved, target)
        elif os.path.exists(target):
            os.remove(target)

    return restore


def config_problem(args):
    """Why launch's --config can't be used as asked, or None."""
    if args.config and args.no_harness:
        # the file is put back once band3 has read it, which with the launcher
        # showing is before a Save or Play there writes band3.toml: that would
        # merge into the player's file
        return ("--config can't be used with --no-harness: the launcher would save over the "
                "band3.toml band3ctl puts back. Put the file beside the exe yourself instead")
    return None


def launch(args):
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        sys.exit(f"no band3 at {exe}")
    problem = config_problem(args)
    if problem:
        sys.exit(problem)
    if not args.config:
        return start(args, exe)
    if not os.path.isfile(args.config):
        sys.exit(f"no config file at {args.config}")
    try:
        restore = place_config(args.config, os.path.dirname(exe))
    except (RuntimeError, OSError) as e:
        sys.exit(f"--config: {e}")
    try:
        return start(args, exe)
    finally:
        # band3 reads band3.toml only as it starts, before its window opens
        # and the harness answers, so it's done with it by now
        restore()


def start(args, exe):
    # band3 finds band3_config.ini and its game data (assets/) from here
    cwd = os.path.abspath(args.cwd)
    # its own saves and profile, so a test never touches the player's
    user_data = os.path.abspath(args.user_data)
    if args.fresh and os.path.isdir(user_data):
        shutil.rmtree(user_data)
    os.makedirs(user_data, exist_ok=True)
    command = [exe, f"--user_data_root={user_data}"]
    # test_port also keeps the launcher away, so --no-harness is how to see it
    if not args.no_harness:
        command.insert(1, f"--test_port={args.port}")
    # muted, like minimized, so a test doesn't disturb whoever is at the
    # machine; the game's audio still runs, so songs play on as usual
    if not args.sound and not any(a.startswith("--audio_mute") for a in args.extra):
        command.append("--audio_mute=true")
    command += args.extra
    flags = 0
    startupinfo = None
    if os.name == "nt":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        if not args.show:
            # minimized and never activated, so it doesn't take the keyboard
            # from whatever is in front; the game keeps running and drawing
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startupinfo.wShowWindow = 7  # SW_SHOWMINNOACTIVE
    process = subprocess.Popen(command, cwd=cwd, creationflags=flags, startupinfo=startupinfo,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + args.timeout
    if args.no_harness:
        # nothing answers on the harness port; its window is the sign it's up
        while time.monotonic() < deadline:
            if process.poll() is not None:
                sys.exit(f"band3 exited with code {process.returncode} before its window opened")
            try:
                GameWindow(process.pid)
                print(json.dumps({"ok": True, "pid": process.pid}))
                return 0
            except RuntimeError:
                time.sleep(0.5)
        sys.exit(f"band3 (pid {process.pid}) opened no window within {args.timeout} s")
    while time.monotonic() < deadline:
        if process.poll() is not None:
            sys.exit(f"band3 exited with code {process.returncode} before the harness answered")
        try:
            conn = Connection(args.port)
            reply = conn.command("state")
            conn.close()
            print(json.dumps({"ok": True, "pid": process.pid, "state": reply.get("state")}))
            return 0
        except OSError:
            time.sleep(0.5)
    sys.exit(f"band3 (pid {process.pid}) didn't answer on port {args.port} "
             f"within {args.timeout} s; is test_port in use?")


def parse_size(text):
    """'1920x1080' as (1920, 1080); ValueError otherwise."""
    m = re.fullmatch(r"(\d+)x(\d+)", text.strip().lower())
    if not m or int(m.group(1)) <= 0 or int(m.group(2)) <= 0:
        raise ValueError(f"not a size: {text!r} (want <width>x<height>)")
    return int(m.group(1)), int(m.group(2))


def offscreen_origin(virtual_screen):
    """Where a window goes to be on no monitor: right of the virtual screen
    (left, top, width, height), by a margin, at its top."""
    left, top, width, _ = virtual_screen
    return left + width + 100, top


def is_build_exe(image, repo=REPO):
    """Whether a process's image is a band3.exe built under this checkout's
    out/build, not a worktree's (.claude/worktrees/<name>/out/build) or another's."""
    build = os.path.normcase(os.path.join(repo, "out", "build")) + os.sep
    image = os.path.normcase(os.path.abspath(image))
    return image.startswith(build) and os.path.basename(image) == "band3.exe"


def png_bytes(width, height, rgb_rows):
    """An 8-bit RGB PNG of rows of width*3 bytes each."""
    import struct
    import zlib

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    raw = b"".join(b"\x00" + row for row in rgb_rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class GameWindow:
    """The running game's window, moved and read without ever activating it:
    every call here leaves the foreground window as it was. Win32 only."""

    SW_SHOWNOACTIVATE = 4
    SW_SHOWMINNOACTIVE = 7
    SWP_NOSIZE, SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 0x1, 0x2, 0x4, 0x10
    PW_CLIENTONLY, PW_RENDERFULLCONTENT = 0x1, 0x2

    def __init__(self, pid=None):
        import ctypes
        from ctypes import wintypes
        self.ct, self.wt = ctypes, wintypes
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        # physical pixels everywhere, whatever the monitors' scaling
        self.user32.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
        self.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2
        # handles are pointer-sized; ctypes would pass a bare int as a 32-bit one
        H, P, I = wintypes.HWND, ctypes.c_void_p, ctypes.c_int
        for dll, name, res, args in (
                (self.user32, "GetForegroundWindow", H, []),
                (self.user32, "GetDpiForWindow", wintypes.UINT, [H]),
                (self.user32, "IsIconic", wintypes.BOOL, [H]),
                (self.user32, "IsWindowVisible", wintypes.BOOL, [H]),
                (self.user32, "GetWindowThreadProcessId", wintypes.DWORD, [H, P]),
                (self.user32, "GetWindowPlacement", wintypes.BOOL, [H, P]),
                (self.user32, "SetWindowPlacement", wintypes.BOOL, [H, P]),
                (self.user32, "GetWindowRect", wintypes.BOOL, [H, P]),
                (self.user32, "GetClientRect", wintypes.BOOL, [H, P]),
                (self.user32, "SetWindowPos", wintypes.BOOL, [H, H, I, I, I, I, wintypes.UINT]),
                (self.user32, "ShowWindow", wintypes.BOOL, [H, I]),
                (self.user32, "GetWindowLongW", wintypes.LONG, [H, I]),
                (self.user32, "AdjustWindowRectExForDpi", wintypes.BOOL,
                 [P, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD, wintypes.UINT]),
                (self.user32, "PrintWindow", wintypes.BOOL, [H, wintypes.HDC, wintypes.UINT]),
                (self.user32, "GetDC", wintypes.HDC, [H]),
                (self.user32, "ReleaseDC", I, [H, wintypes.HDC]),
                (self.gdi32, "CreateCompatibleDC", wintypes.HDC, [wintypes.HDC]),
                (self.gdi32, "CreateCompatibleBitmap", wintypes.HBITMAP, [wintypes.HDC, I, I]),
                (self.gdi32, "SelectObject", wintypes.HGDIOBJ, [wintypes.HDC, wintypes.HGDIOBJ]),
                (self.gdi32, "GetDIBits", I, [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT,
                                              wintypes.UINT, P, P, wintypes.UINT]),
                (self.gdi32, "DeleteObject", wintypes.BOOL, [wintypes.HGDIOBJ]),
                (self.gdi32, "DeleteDC", wintypes.BOOL, [wintypes.HDC]),
                (self.kernel32, "OpenProcess", wintypes.HANDLE,
                 [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]),
                (self.kernel32, "CloseHandle", wintypes.BOOL, [wintypes.HANDLE]),
                (self.kernel32, "QueryFullProcessImageNameW", wintypes.BOOL,
                 [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, P])):
            getattr(dll, name).restype = res
            getattr(dll, name).argtypes = args
        self.pid = pid if pid else self._find_pid()
        self.hwnd = self._find_window(self.pid)

    def _find_pid(self):
        """The one band3.exe running from this checkout's out/build."""
        ct, wt = self.ct, self.wt
        pids = (wt.DWORD * 4096)()
        needed = wt.DWORD()
        self.ct.WinDLL("psapi").EnumProcesses(pids, ct.sizeof(pids), ct.byref(needed))
        found = []
        for pid in pids[:needed.value // ct.sizeof(wt.DWORD)]:
            handle = self.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
            if not handle:
                continue
            try:
                buf = ct.create_unicode_buffer(1024)
                size = wt.DWORD(len(buf))
                if (self.kernel32.QueryFullProcessImageNameW(handle, 0, buf, ct.byref(size))
                        and is_build_exe(buf.value)):
                    found.append(pid)
            finally:
                self.kernel32.CloseHandle(handle)
        if not found:
            raise RuntimeError("band3 isn't running from out/build")
        if len(found) > 1:
            raise RuntimeError(f"more than one band3 is running ({found}); pick one with --pid")
        return found[0]

    def _find_window(self, pid):
        """The process's largest visible top-level window: the game's."""
        ct, wt = self.ct, self.wt
        best = []
        proc = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def visit(hwnd, _):
            owner = wt.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            if owner.value == pid and self.user32.IsWindowVisible(hwnd):
                placement = self._placement(hwnd)
                r = placement.rcNormalPosition
                best.append(((r.right - r.left) * (r.bottom - r.top), hwnd))
            return True

        self.user32.EnumWindows(proc(visit), 0)
        if not best:
            raise RuntimeError(f"band3 (pid {pid}) has no window yet")
        return max(best)[1]

    def _placement(self, hwnd=None):
        wt = self.wt

        class WINDOWPLACEMENT(self.ct.Structure):
            _fields_ = [("length", wt.UINT), ("flags", wt.UINT), ("showCmd", wt.UINT),
                        ("ptMinPosition", wt.POINT), ("ptMaxPosition", wt.POINT),
                        ("rcNormalPosition", wt.RECT)]

        p = WINDOWPLACEMENT()
        p.length = self.ct.sizeof(p)
        self.user32.GetWindowPlacement(hwnd or self.hwnd, self.ct.byref(p))
        return p

    def _set_placement(self, placement):
        if not self.user32.SetWindowPlacement(self.hwnd, self.ct.byref(placement)):
            raise RuntimeError(f"SetWindowPlacement failed ({self.ct.get_last_error()})")

    def _rect(self, client=False):
        r = self.wt.RECT()
        if client:
            self.user32.GetClientRect(self.hwnd, self.ct.byref(r))
        else:
            self.user32.GetWindowRect(self.hwnd, self.ct.byref(r))
        return r

    def _virtual_screen(self):
        m = self.user32.GetSystemMetrics
        return m(76), m(77), m(78), m(79)  # SM_X/Y/CX/CYVIRTUALSCREEN

    def _frame(self, width, height):
        """The window size whose client area is width x height."""
        ct, wt = self.ct, self.wt
        r = wt.RECT(0, 0, width, height)
        style = self.user32.GetWindowLongW(self.hwnd, -16)
        ex_style = self.user32.GetWindowLongW(self.hwnd, -20)
        self.user32.AdjustWindowRectExForDpi(ct.byref(r), wt.DWORD(style & 0xFFFFFFFF), False,
                                             wt.DWORD(ex_style & 0xFFFFFFFF),
                                             self.user32.GetDpiForWindow(self.hwnd))
        return r.right - r.left, r.bottom - r.top

    def status(self):
        w = self._rect()
        c = self._rect(client=True)
        return {"pid": self.pid, "hwnd": self.hwnd,
                "minimized": bool(self.user32.IsIconic(self.hwnd)),
                "window": [w.left, w.top, w.right - w.left, w.bottom - w.top],
                "client": [c.right - c.left, c.bottom - c.top],
                "foreground": self.user32.GetForegroundWindow() == self.hwnd}

    def offscreen(self):
        """Restored (so it paints) but right of every monitor, never activated."""
        x, y = offscreen_origin(self._virtual_screen())
        p = self._placement()
        r = p.rcNormalPosition
        width, height = r.right - r.left, r.bottom - r.top
        # the restored position first, so restoring doesn't show it on a monitor
        r.left, r.top, r.right, r.bottom = x, y, x + width, y + height
        p.showCmd = self.SW_SHOWNOACTIVATE
        self._set_placement(p)
        # the placement is in workspace coordinates, which can be off from the
        # screen's by a taskbar; this is in the screen's
        self.user32.SetWindowPos(self.hwnd, None, x, y, 0, 0,
                                 self.SWP_NOSIZE | self.SWP_NOZORDER | self.SWP_NOACTIVATE)
        # A window launched minimized restores at the size the SDK already
        # assumes, so its presenter never connects to it and nothing paints
        # until the size changes: a pixel taller and back makes it connect.
        r = self._rect()
        width, height = r.right - r.left, r.bottom - r.top
        flags = self.SWP_NOMOVE | self.SWP_NOZORDER | self.SWP_NOACTIVATE
        self.user32.SetWindowPos(self.hwnd, None, 0, 0, width, height + 1, flags)
        self.user32.SetWindowPos(self.hwnd, None, 0, 0, width, height, flags)

    def minimize(self):
        """Minimized without activating; restoring it later puts it on the
        primary monitor rather than where `offscreen` left it."""
        self.user32.ShowWindow(self.hwnd, self.SW_SHOWMINNOACTIVE)
        p = self._placement()
        r = p.rcNormalPosition
        vx, vy, vw, vh = self._virtual_screen()
        if r.left >= vx + vw or r.top >= vy + vh or r.right <= vx or r.bottom <= vy:
            width, height = r.right - r.left, r.bottom - r.top
            sw, sh = self.user32.GetSystemMetrics(0), self.user32.GetSystemMetrics(1)
            left, top = max(0, (sw - width) // 2), max(0, (sh - height) // 2)
            r.left, r.top, r.right, r.bottom = left, top, left + width, top + height
            p.showCmd = self.SW_SHOWMINNOACTIVE
            self._set_placement(p)

    def resize(self, width, height):
        """Its client area to width x height physical pixels, where it is."""
        fw, fh = self._frame(width, height)
        if self.user32.IsIconic(self.hwnd):
            p = self._placement()
            r = p.rcNormalPosition
            r.right, r.bottom = r.left + fw, r.top + fh
            p.showCmd = self.SW_SHOWMINNOACTIVE
            self._set_placement(p)
        else:
            self.user32.SetWindowPos(self.hwnd, None, 0, 0, fw, fh,
                                     self.SWP_NOMOVE | self.SWP_NOZORDER | self.SWP_NOACTIVATE)

    def click(self, x, y):
        """A left click at (x, y) in the client area's physical pixels, posted
        to the window as mouse messages, so it needs neither focus nor the real
        cursor. The window has to be painting (`offscreen`) for ImGui to see it.
        SDL answers a move with a mouse-leave (the real cursor is elsewhere) that
        puts the position back where the cursor is, unless a button holds the
        mouse: so the press follows its move at once, ahead of that, and the
        release (a frame later) comes without a move of its own."""
        if self.user32.IsIconic(self.hwnd):
            raise RuntimeError("the window is minimized; `window offscreen` it first")
        post = self.user32.PostMessageW
        post.restype = self.wt.BOOL
        post.argtypes = [self.wt.HWND, self.wt.UINT, self.wt.WPARAM, self.wt.LPARAM]
        position = (y & 0xFFFF) << 16 | (x & 0xFFFF)
        WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x200, 0x201, 0x202, 0x1
        for message, buttons, pause in ((WM_MOUSEMOVE, 0, 0), (WM_LBUTTONDOWN, MK_LBUTTON, 0.15),
                                        (WM_LBUTTONUP, 0, 0)):
            if not post(self.hwnd, message, buttons, position):
                raise RuntimeError(f"PostMessage failed ({self.ct.get_last_error()})")
            time.sleep(pause)

    def shot(self, path):
        """The client area as the window draws it, flip-model swap chains
        included (PrintWindow's PW_RENDERFULLCONTENT), into an RGB PNG. A
        minimized window has nothing to give."""
        ct, wt = self.ct, self.wt
        if self.user32.IsIconic(self.hwnd):
            raise RuntimeError("the window is minimized; `window offscreen` it first")
        c = self._rect(client=True)
        width, height = c.right - c.left, c.bottom - c.top
        screen = self.user32.GetDC(None)
        dc = self.gdi32.CreateCompatibleDC(screen)
        bitmap = self.gdi32.CreateCompatibleBitmap(screen, width, height)
        old = self.gdi32.SelectObject(dc, bitmap)
        try:
            if not self.user32.PrintWindow(self.hwnd, dc,
                                           self.PW_CLIENTONLY | self.PW_RENDERFULLCONTENT):
                raise RuntimeError(f"PrintWindow failed ({ct.get_last_error()})")

            class BITMAPINFOHEADER(ct.Structure):
                _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                            ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                            ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                            ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                            ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

            info = BITMAPINFOHEADER()
            info.biSize = ct.sizeof(info)
            info.biWidth, info.biHeight = width, -height  # top row first
            info.biPlanes, info.biBitCount = 1, 32
            pixels = ct.create_string_buffer(width * height * 4)
            self.gdi32.SelectObject(dc, old)
            if self.gdi32.GetDIBits(dc, bitmap, 0, height, pixels, ct.byref(info), 0) != height:
                raise RuntimeError("GetDIBits failed")
        finally:
            self.gdi32.DeleteObject(bitmap)
            self.gdi32.DeleteDC(dc)
            self.user32.ReleaseDC(None, screen)
        bgrx = pixels.raw
        rows, peak, total = [], 0, 0
        for y in range(height):
            row = bgrx[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
            peak = max(peak, max(rgb) if rgb else 0)
            total += sum(rgb)
            rows.append(bytes(rgb))
        with open(path, "wb") as f:
            f.write(png_bytes(width, height, rows))
        return {"path": os.path.abspath(path), "width": width, "height": height,
                "mean": round(total / max(1, width * height * 3), 2), "max": peak}


def window(args):
    if os.name != "nt":
        sys.exit("window: Windows only")
    try:
        w = GameWindow(args.pid)
        before = w.user32.GetForegroundWindow()
        reply = {"ok": True}
        if args.what == "offscreen":
            w.offscreen()
        elif args.what == "minimize":
            w.minimize()
        elif args.what == "size":
            if not args.arg:
                raise RuntimeError("window size <width>x<height>")
            size = parse_size(args.arg)
            w.resize(*size)
            if not w.user32.IsIconic(w.hwnd) and w.status()["client"] != list(size):
                # Windows keeps a window no bigger than the monitors around it
                reply["clamped"] = True
        elif args.what == "shot":
            if not args.arg:
                raise RuntimeError("window shot <file.png>")
            reply["shot"] = w.shot(args.arg)
        elif args.what == "click":
            m = re.fullmatch(r"\s*(\d+)\s*,\s*(\d+)\s*", args.arg or "")
            if not m:
                raise RuntimeError("window click <x>,<y>")
            w.click(int(m.group(1)), int(m.group(2)))
        reply.update(w.status())
        # none of it may take the keyboard from whatever had it
        reply["foreground_changed"] = w.user32.GetForegroundWindow() != before
    except (RuntimeError, ValueError, OSError) as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 1
    print(json.dumps(reply))
    return 0


def run(args):
    with open(args.script, encoding="utf-8") as f:
        commands = parse_script(f.read())
    name = os.path.splitext(os.path.basename(args.script))[0]
    conn = connect(args.port)
    try:
        started = time.monotonic()
        result = run_script(
            conn, commands, name,
            lambda number, command, reply: print(
                f"{args.script}:{number}: {command}: {json.dumps(reply['stats'])}"))
    finally:
        conn.close()
    if result.passed:
        print(f"PASS {args.script}: {len(commands)} commands in "
              f"{time.monotonic() - started:.1f} s")
        return 0
    print(f"FAIL {args.script}:{result.line}: {result.command}")
    print(json.dumps(result.reply))
    return 1


def main(argv):
    parser = argparse.ArgumentParser(
        description="Drive band3 through its test harness.",
        epilog="Anything else is sent as a harness command, e.g. `state` or `press green`.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = parser.add_subparsers(dest="action")

    p = sub.add_parser("launch", help="start band3 with the harness and wait for it")
    p.add_argument("--exe", default=DEFAULT_EXE)
    p.add_argument("--cwd", default=REPO,
                   help="where band3 runs from (finds band3_config.ini and assets/ there)")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--user-data", default=os.path.join(REPO, "out", "test_user_data"),
                   help="saves and profile for the test run, kept apart from the player's")
    p.add_argument("--fresh", action="store_true",
                   help="start from an empty --user-data, as a first boot (for scripts)")
    p.add_argument("--show", action="store_true",
                   help="open the window normally (by default it starts minimized, "
                        "without taking focus)")
    p.add_argument("--sound", action="store_true",
                   help="let the game play sound (by default it starts muted, --audio_mute=true)")
    p.add_argument("--no-harness", action="store_true",
                   help="without test_port, so the launcher can show (--launcher); waits for "
                        "the window instead of the harness, which won't answer")
    p.add_argument("--config", metavar="FILE",
                   help="start with FILE as the band3.toml beside the exe, putting back "
                        "whatever was there once band3 has read it (not with --no-harness, "
                        "whose launcher saves to band3.toml later)")
    p.add_argument("extra", nargs="*", help="more band3 arguments, e.g. --fast_start=true")

    p = sub.add_parser("run", help="replay a .b3t script")
    p.add_argument("script")

    p = sub.add_parser(
        "window", help="move, size, minimize or screenshot the running game's window "
                       "without ever activating it (Windows)")
    p.add_argument("what", choices=["offscreen", "shot", "minimize", "size", "click", "status"],
                   help="offscreen: restored, right of every monitor; shot <png>: its client "
                        "area as drawn (not while minimized); minimize; size <W>x<H>: its "
                        "client area in physical pixels; click <X>,<Y>: a left click there, "
                        "in client pixels; status")
    p.add_argument("arg", nargs="?", help="shot's PNG path, size's <W>x<H>, or click's <X>,<Y>")
    p.add_argument("--pid", type=int, help="which band3, when more than one runs")

    known = {"launch", "run", "window", "-h", "--help"}
    rest = argv[:]
    port_args = []
    while rest and rest[0].startswith("--port"):
        port_args += rest[:2] if rest[0] == "--port" else rest[:1]
        rest = rest[2:] if rest[0] == "--port" else rest[1:]
    if rest and rest[0] not in known:
        port = parser.parse_args(port_args).port
        # one connection for them all: band3 lets go of held inputs when a
        # client disconnects, so `hold x; wait ...; release x` needs to share one
        conn = connect(port)
        try:
            for command in split_commands(rest):
                reply = conn.command(command)
                print(json.dumps(reply))
                if not reply.get("ok"):
                    return 1
        finally:
            conn.close()
        return 0

    args = parser.parse_args(argv)
    if args.action == "launch":
        return launch(args)
    if args.action == "run":
        return run(args)
    if args.action == "window":
        return window(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
