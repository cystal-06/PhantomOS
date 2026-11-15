#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"
IMAGES_DIR="${BUILDROOT_DIR}/output/images"

if [ ! -d "${BUILDROOT_DIR}" ] || [ ! -f "${BUILDROOT_DIR}/.config" ]; then
    echo "[-] Buildroot directory or .config not found. Run ./scripts/build-os.sh first." >&2
    exit 1
fi

cd "${BUILDROOT_DIR}"
echo "[+] Updating rootfs images (target-finalize + ext4 + squashfs + initramfs)..."
make target-finalize HOST_CFLAGS="-O2 -std=gnu17"
make rootfs-ext2 HOST_CFLAGS="-O2 -std=gnu17"
make rootfs-squashfs HOST_CFLAGS="-O2 -std=gnu17"

echo "[+] Rootfs updated successfully:"
ls -lh "${IMAGES_DIR}"/{initramfs.cpio.gz,rootfs.squashfs,rootfs.ext4} 2>/dev/null || true
