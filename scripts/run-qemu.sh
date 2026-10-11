#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"
KERNEL_IMAGE="${BUILDROOT_DIR}/output/images/bzImage"
ROOTFS_IMAGE="${BUILDROOT_DIR}/output/images/rootfs.ext4"

if [ ! -f "$KERNEL_IMAGE" ] || [ ! -f "$ROOTFS_IMAGE" ]; then
    echo "[-] System images not found."
    exit 1
fi

qemu-system-x86_64 \
    -M pc \
    -m 2048M \
    -smp 2 \
    -kernel "${KERNEL_IMAGE}" \
    -drive file="${ROOTFS_IMAGE}",if=virtio,format=raw \
    -append "root=/dev/vda quiet" \
    -vga virtio \
    -device virtio-mouse-pci \
    -device virtio-keyboard-pci \
    -device usb-tablet \
    -net nic,model=virtio \
    -net user,hostfwd=tcp::8080-:8080
