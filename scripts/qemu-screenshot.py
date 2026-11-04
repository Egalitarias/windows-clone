#!/usr/bin/env python3
"""Boot out/uefi in QEMU, wait, and save a screenshot as a PNG.

Usage: python scripts/qemu-screenshot.py [output.png] [wait-seconds] [monitor-cmd ...]

Extra arguments are QEMU monitor commands run (in order) before the
screenshot, e.g. to drive the guest:
    python scripts/qemu-screenshot.py out.png 14 "mouse_move 100 50" "sleep 0.3" "sendkey h"
"sleep N" pauses N seconds between commands.

Needs QEMU (Scoop: `scoop install qemu`) and a built loader
(`make CC=clang uefi-loader`). Serial output is written to out/run/serial.log.
Standard library only.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MONITOR_PORT = 45454


def find_qemu():
    exe = shutil.which("qemu-system-x86_64")
    if exe:
        return exe
    scoop = os.path.join(os.path.expanduser("~"), "scoop", "apps", "qemu", "current")
    cand = os.path.join(scoop, "qemu-system-x86_64.exe")
    if os.path.exists(cand):
        return cand
    sys.exit("qemu-system-x86_64 not found")


def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, "rb").read()
    # P6 <w> <h> <maxval>\n<binary>
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", "unexpected PPM header"
    w, h = map(int, parts[1].split())
    pix = parts[3]
    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    open(png_path, "wb").write(png)
    return w, h


def main():
    out_png = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "run", "screen.png")
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
    commands = sys.argv[3:]
    qemu = find_qemu()
    share = os.path.join(os.path.dirname(qemu), "share")
    code = os.path.join(share, "edk2-x86_64-code.fd")
    vars_tpl = os.path.join(share, "edk2-i386-vars.fd")
    run_dir = os.path.join(ROOT, "out", "run")
    os.makedirs(run_dir, exist_ok=True)
    vars_fd = os.path.join(run_dir, "vars.fd")
    shutil.copyfile(vars_tpl, vars_fd)
    ppm = os.path.join(run_dir, "screen.ppm")
    if os.path.exists(ppm):
        os.remove(ppm)

    cmd = [
        qemu, "-machine", "q35", "-m", "512", "-display", "none", "-vga", "std",
        "-serial", "file:" + os.path.join(run_dir, "serial.log"),
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT,
        "-drive", "if=pflash,format=raw,readonly=on,file=" + code,
        "-drive", "if=pflash,format=raw,file=" + vars_fd,
        "-drive", "format=raw,file=fat:rw:" + os.path.join(ROOT, "out", "uefi"),
    ]
    data_img = os.path.join(run_dir, "data.img")
    if os.path.exists(data_img):
        cmd += ["-drive", "if=none,id=data,format=raw,file=" + data_img,
                "-device", "ide-hd,drive=data,bus=ide.1"]
    proc = subprocess.Popen(cmd, cwd=ROOT)
    try:
        time.sleep(wait)
        s = socket.create_connection(("127.0.0.1", MONITOR_PORT), timeout=10)
        s.settimeout(5)
        time.sleep(0.5)
        for command in commands:
            if command.startswith("sleep "):
                time.sleep(float(command.split()[1]))
            else:
                s.sendall((command + "\n").encode())
                time.sleep(0.15)
        time.sleep(0.5)
        s.sendall(("screendump %s\n" % ppm.replace("\\", "/")).encode())
        time.sleep(1.5)
        s.sendall(b"quit\n")
        s.close()
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    if not os.path.exists(ppm):
        sys.exit("screendump failed")
    w, h = ppm_to_png(ppm, out_png)
    print("saved %s (%dx%d)" % (out_png, w, h))


if __name__ == "__main__":
    main()
