#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_VERSION="2024.02.1"
BUILDROOT_DIR="${DIR}/buildroot-${BUILDROOT_VERSION}"
CONFIG_FILE="${DIR}/configs/phantomos_defconfig"
IMAGES_DIR="${BUILDROOT_DIR}/output/images"

for cmd in wget tar make gcc cpio gzip rsync bc; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[-] Missing required host command: ${cmd}" >&2
        exit 1
    fi
done

if [ ! -d "${BUILDROOT_DIR}" ]; then
    echo "[+] Downloading Buildroot ${BUILDROOT_VERSION}..."
    wget -c "https://buildroot.org/downloads/buildroot-${BUILDROOT_VERSION}.tar.xz" -O "${DIR}/buildroot-${BUILDROOT_VERSION}.tar.xz"
    echo "[+] Extracting Buildroot..."
    tar -xf "${DIR}/buildroot-${BUILDROOT_VERSION}.tar.xz" -C "${DIR}"
    rm -f "${DIR}/buildroot-${BUILDROOT_VERSION}.tar.xz"
fi

cd "${BUILDROOT_DIR}"

if [ -f "${CONFIG_FILE}" ]; then
    echo "[+] Loading configuration from ${CONFIG_FILE}..."
    make defconfig BR2_DEFCONFIG="${CONFIG_FILE}"
else
    echo "[!] Missing ${CONFIG_FILE}, falling back to qemu_x86_64_defconfig..."
    make qemu_x86_64_defconfig
fi

echo "[+] Building PhantomOS using $(nproc) CPU threads..."
make -j"$(nproc)" HOST_CFLAGS="-O2 -std=gnu17"

echo "[+] Build complete! Generated images in ${IMAGES_DIR}:"
ls -lh "${IMAGES_DIR}"/{bzImage,initramfs.cpio.gz,rootfs.squashfs,rootfs.ext4} 2>/dev/null || ls -lh "${IMAGES_DIR}"
