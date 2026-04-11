#!/usr/bin/env python3
"""
OTA firmware upload over the shared TCP log port.

Usage:
    python tools/ota_upload.py <host:port> <firmware.bin>

PlatformIO calls this automatically via:
    upload_protocol = custom
    upload_command  = python tools/ota_upload.py $UPLOAD_PORT $SOURCE
"""

import socket
import sys
import time


class LineReader:
    """Wraps a socket and buffers data so multiple lines in one recv() are not lost."""

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._buf = b""

    def readline(self, timeout: float) -> str | None:
        deadline = time.time() + timeout
        self._sock.settimeout(0.1)
        while time.time() < deadline:
            if b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                return line.decode("utf-8", errors="replace").strip()
            try:
                chunk = self._sock.recv(256)
            except socket.timeout:
                continue
            if not chunk:
                return None
            self._buf += chunk
        return None


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <host:port> <firmware.bin>")
        sys.exit(1)

    host_port = sys.argv[1]
    firmware_path = sys.argv[2]

    try:
        host, port_str = host_port.rsplit(":", 1)
        port = int(port_str)
    except ValueError:
        print(f"Error: expected host:port, got '{host_port}'")
        sys.exit(1)

    with open(firmware_path, "rb") as f:
        firmware = f.read()

    print(f"Firmware : {firmware_path} ({len(firmware):,} bytes)")
    print(f"Target   : {host}:{port}")
    print("Connecting...")

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect((host, port))
    except OSError as e:
        print(f"Connection failed: {e}")
        sys.exit(1)

    reader = LineReader(s)

    # Send OTA command
    s.settimeout(10)
    cmd = f"OTA {len(firmware)}\n".encode()
    s.sendall(cmd)

    # Wait for READY — echo any log lines that arrive before it
    while True:
        response = reader.readline(timeout=10)
        if response is None:
            print("Error: no response from device")
            s.close()
            sys.exit(1)
        if response.startswith("READY"):
            break
        if response.startswith("ERROR"):
            print(f"Error: {response}")
            s.close()
            sys.exit(1)
        print(response)  # log line that arrived before READY

    # Upload firmware
    print("Uploading", end="", flush=True)
    s.settimeout(60)
    sent = 0
    chunk = 1024
    start = time.time()

    while sent < len(firmware):
        s.sendall(firmware[sent : sent + chunk])
        sent += chunk
        pct = min(sent, len(firmware)) * 100 // len(firmware)
        elapsed = time.time() - start
        speed = sent / elapsed / 1024 if elapsed > 0 else 0
        print(f"\rUploading {pct:3d}%  {speed:5.1f} KB/s", end="", flush=True)

    print()

    # Wait for OK — echo any progress/log lines that arrive before it
    result = None
    while True:
        line = reader.readline(timeout=30)
        if line is None:
            print("Error: no result received")
            s.close()
            sys.exit(1)
        if line.startswith("OK"):
            result = line
            break
        if line.startswith("ERROR"):
            print(f"Failed: {line}")
            s.close()
            sys.exit(1)
        print(line)  # progress/log line

    s.close()
    elapsed = time.time() - start
    speed = len(firmware) / elapsed / 1024
    print(f"Done — {len(firmware):,} bytes in {elapsed:.1f}s ({speed:.1f} KB/s)")
    print("Device rebooting...")
    sys.exit(0)


if __name__ == "__main__":
    main()
