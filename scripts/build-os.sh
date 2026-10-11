#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_VERSION="2024.02.1"
BUILDROOT_DIR="${DIR}/buildroot-${BUILDROOT_VERSION}"
CONFIG_FILE="${DIR}/configs/phantomos_defconfig"

if [ ! -d "${BUILDROOT_DIR}" ]; then
    echo "[+] Downloading Buildroot ${BUILDROOT_VERSION}..."
    wget -c "https://buildroot.org/downloads/buildroot-${BUILDROOT_VERSION}.tar.xz" -O "${DIR}/buildroot-${BUILDROOT_VERSION}.tar.xz"
    echo "[+] Extracting Buildroot..."
    tar -xf "${DIR}/buildroot-${BUILDROOT_VERSION}.tar.xz" -C "${DIR}"
    rm -f "${DIR}/buildroot-${BUILDROOT_VERSION}.tar.xz"
fi

cd "${BUILDROOT_DIR}"

if [ -f "${CONFIG_FILE}" ]; then
    echo "[+] Nạp cấu hình từ ${CONFIG_FILE}..."
    make defconfig BR2_DEFCONFIG="${CONFIG_FILE}"
else
    echo "[!] Chưa có file configs/phantomos_defconfig, sử dụng qemu_x86_64_defconfig mặc định..."
    make qemu_x86_64_defconfig
fi

echo "[+] Bắt đầu biên dịch hệ điều hành (dùng $(nproc) luồng CPU)..."
make -j"$(nproc)"

echo "[+] Biên dịch hoàn tất! File ảnh nằm tại ${BUILDROOT_DIR}/output/images/"
