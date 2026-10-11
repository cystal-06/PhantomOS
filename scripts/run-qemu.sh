#!/bin/bash
# ========================================================
# Script chạy thử nghiệm PhantomOS trên máy ảo QEMU
# ========================================================
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"
KERNEL_IMAGE="${BUILDROOT_DIR}/output/images/bzImage"
ROOTFS_IMAGE="${BUILDROOT_DIR}/output/images/rootfs.ext4"

if [ ! -f "$KERNEL_IMAGE" ] || [ ! -f "$ROOTFS_IMAGE" ]; then
    echo "[-] Chưa tìm thấy file ảnh hệ thống!"
    echo "    Vui lòng biên dịch trước bằng lệnh: make trong thư mục ${BUILDROOT_DIR}"
    exit 1
fi

echo "[+] Khởi động PhantomOS trên QEMU..."
echo "    - RAM: 2048 MB"
echo "    - Port Forward: localhost:8080 -> VM:8080 (Spring Boot)"
echo "    - Để thoát: Nhấn Ctrl+A rồi nhấn X"
echo ""

qemu-system-x86_64 \
    -M pc \
    -m 2048M \
    -smp 2 \
    -kernel "${KERNEL_IMAGE}" \
    -drive file="${ROOTFS_IMAGE}",if=virtio,format=raw \
    -append "root=/dev/vda console=ttyS0 quiet" \
    -net nic,model=virtio \
    -net user,hostfwd=tcp::8080-:8080 \
    -nographic
