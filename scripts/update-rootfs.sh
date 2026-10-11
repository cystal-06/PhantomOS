#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"

if [ ! -d "${BUILDROOT_DIR}" ]; then
    echo "[-] Buildroot directory not found."
    exit 1
fi

cd "${BUILDROOT_DIR}"
make target-finalize
make rootfs-ext2
echo "[+] Rootfs updated successfully in seconds!"
