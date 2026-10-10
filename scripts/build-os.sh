#!/bin/bash
# ========================================================
# Script tự động nạp cấu hình và biên dịch Micro-OS
# ========================================================
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"
CONFIG_FILE="${DIR}/configs/micro_os_defconfig"

if [ ! -d "${BUILDROOT_DIR}" ]; then
    echo "[-] Chưa tìm thấy thư mục Buildroot tại ${BUILDROOT_DIR}"
    exit 1
fi

cd "${BUILDROOT_DIR}"

if [ -f "${CONFIG_FILE}" ]; then
    echo "[+] Nạp cấu hình từ ${CONFIG_FILE}..."
    make defconfig BR2_DEFCONFIG="${CONFIG_FILE}"
else
    echo "[!] Chưa có file configs/micro_os_defconfig, sử dụng qemu_x86_64_defconfig mặc định..."
    make qemu_x86_64_defconfig
fi

echo "[+] Bắt đầu biên dịch hệ điều hành (dùng $(nproc) luồng CPU)..."
make -j"$(nproc)"

echo "[+] Biên dịch hoàn tất! File ảnh nằm tại ${BUILDROOT_DIR}/output/images/"
