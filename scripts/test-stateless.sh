#!/usr/bin/env python3
"""Automated QEMU smoke test verifying PhantomOS stateless OverlayFS boot, auth, and sudo."""

import os
import pty
import select
import subprocess
import sys
import time


def read_until(fd: int, targets: list[bytes], timeout: float) -> tuple[bytes, bytes | None]:
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.05, deadline - time.monotonic())
        r, _, _ = select.select([fd], [], [], remaining)
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        for t in targets:
            if t in buf:
                return buf, t
    return buf, None


def login_and_run(fd: int, username: str, password: str, prompt: bytes, boot_timeout: float = 30.0) -> bytes:
    _, match = read_until(fd, [b"login:"], boot_timeout)
    if not match:
        raise RuntimeError("Timed out waiting for login prompt")

    os.write(fd, f"{username}\n".encode())
    _, match = read_until(fd, [b"Password:"], 10.0)
    if not match:
        raise RuntimeError("Timed out waiting for Password prompt")

    os.write(fd, f"{password}\n".encode())
    _, match = read_until(fd, [prompt], 10.0)
    if not match:
        raise RuntimeError(f"Timed out waiting for shell prompt {prompt!r}")
    return b""


def run_cmd(fd: int, cmd: str, marker: str, prompt: bytes) -> str:
    os.write(fd, f"{cmd}; echo {marker}:$?\n".encode())
    out, match = read_until(fd, [f"{marker}:".encode()], 10.0)
    if not match:
        raise RuntimeError(f"Command timed out: {cmd}")
    tail, _ = read_until(fd, [prompt], 5.0)
    return (out + tail).decode(errors="replace")


def main() -> int:
    repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    run_qemu = os.path.join(repo_dir, "scripts", "run-qemu.sh")

    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [run_qemu, "--stateless", "--headless", "--mem", "512M"],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
    )
    os.close(slave_fd)

    try:
        t0 = time.monotonic()
        login_and_run(master_fd, "phantom", "phantom", b"$ ")
        boot_secs = time.monotonic() - t0
        print(f"\n[test] First boot completed in {boot_secs:.2f}s (logged in as phantom)")

        id_out = run_cmd(master_fd, "id", "ID_RC", b"$ ")
        assert "uid=1000(phantom)" in id_out and "ID_RC:0" in id_out, "Not running as user phantom!"

        sudo_out = run_cmd(master_fd, "echo phantom | sudo -S id", "SUDO_RC", b"$ ")
        assert "uid=0(root)" in sudo_out and "SUDO_RC:0" in sudo_out, "sudo failed for user phantom!"

        mounts_out = run_cmd(master_fd, "cat /proc/mounts", "MNT_RC", b"$ ")
        assert " / overlay " in mounts_out, "Root filesystem is NOT OverlayFS!"
        assert "/mnt/.overlay/lower squashfs ro" in mounts_out, "Lower layer is NOT RO SquashFS!"
        assert "/var/lib/data tmpfs rw" in mounts_out, "/var/lib/data RAM disk missing!"
        assert "MNT_RC:0" in mounts_out

        # Check headless mode skipped Xorg & Display Manager
        ps_out = run_cmd(master_fd, "ps | grep -E 'Xorg|phantom-login|fluxbox' | grep -v grep || true", "PS_RC", b"$ ")
        assert "/usr/bin/Xorg" not in ps_out, "Xorg should not run in headless mode!"
        assert "/usr/bin/phantom-login" not in ps_out, "Display manager should not run in headless mode!"

        # Check RAM usage and create volatile file in user home
        ram_out = run_cmd(master_fd, "free -m", "RAM_RC", b"$ ")
        assert "RAM_RC:0" in ram_out
        probe_out = run_cmd(master_fd, "touch ~/.stateless_probe && ls -l ~/.stateless_probe", "PROBE_RC", b"$ ")
        assert "PROBE_RC:0" in probe_out

        # Reboot via sudo and verify stateless reset logging in as root
        print("\n[test] Rebooting VM via sudo to verify stateless reset & root login...")
        os.write(master_fd, b"sudo -n /sbin/reboot\n")
        login_and_run(master_fd, "root", "root", b"# ", boot_timeout=30.0)

        verify_out = run_cmd(master_fd, "test ! -e /home/phantom/.stateless_probe && echo CLEAN_STATELESS", "STATELESS_RC", b"# ")
        assert "CLEAN_STATELESS" in verify_out and "STATELESS_RC:0" in verify_out, (
            "Stateless check FAILED: /home/phantom/.stateless_probe survived reboot!"
        )

        print("\n[test] Powering off VM cleanly...")
        os.write(master_fd, b"poweroff\n")
        proc.wait(timeout=15)
        print("[test] ALL STATELESS & AUTH CHECKS PASSED!")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        os.close(master_fd)


if __name__ == "__main__":
    sys.exit(main())
