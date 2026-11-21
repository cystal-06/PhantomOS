#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"
IMAGES_DIR="${BUILDROOT_DIR}/output/images"
KERNEL_IMAGE="${IMAGES_DIR}/bzImage"
INITRAMFS_IMAGE="${IMAGES_DIR}/initramfs.cpio.gz"
SQUASHFS_IMAGE="${IMAGES_DIR}/rootfs.squashfs"
BROWSER_IMAGE="${IMAGES_DIR}/browser.squashfs"
EXT4_IMAGE="${IMAGES_DIR}/rootfs.ext4"

BOOT_MODE="stateless"
UI_MODE="desktop"
ENABLE_BROWSER="auto"
MEM_SIZE="2048M"
CPUS="2"
USE_KVM="auto"
DEBUG_MODE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Boot Mode:
  --stateless    Boot SquashFS (RO) + tmpfs (RAM) via OverlayFS (default)
  --rw, --dev    Boot read-write rootfs.ext4 directly

Display Mode:
  --desktop      Start VirtIO GPU 1280x800 + Fluxbox desktop (default)
  --headless     Serial console only (-nographic), skips Xorg/Fluxbox
  --no-browser   Do not attach browser.squashfs addon in desktop mode

Hardware & Debug:
  --mem <SIZE>   VM RAM size (default: 2048M)
  --cpus <N>     Number of vCPUs (default: 2)
  --no-kvm       Disable /dev/kvm acceleration
  --debug        Verbose kernel & initramfs boot logs
  -h, --help     Show this help message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --stateless) BOOT_MODE="stateless" ;;
        --rw|--dev)  BOOT_MODE="rw" ;;
        --desktop)    UI_MODE="desktop" ;;
        --headless)   UI_MODE="headless" ;;
        --no-browser) ENABLE_BROWSER="no" ;;
        --mem)       MEM_SIZE="${2:?Missing value for --mem}"; shift ;;
        --cpus)      CPUS="${2:?Missing value for --cpus}"; shift ;;
        --no-kvm)    USE_KVM="no" ;;
        --debug)     DEBUG_MODE=1 ;;
        -h|--help)   usage; exit 0 ;;
        *)
            echo "[-] Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "[-] Kernel image not found at ${KERNEL_IMAGE}. Run ./scripts/build-os.sh first." >&2
    exit 1
fi

QEMU_ARGS=(
    -M pc
    -m "${MEM_SIZE}"
    -smp "${CPUS}"
    -kernel "${KERNEL_IMAGE}"
    -net nic,model=virtio
    -net user,hostfwd=tcp::8080-:8080
)

if [ "${USE_KVM}" = "auto" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_ARGS+=(-enable-kvm -cpu host)
fi

APPEND_OPTS=()
if [ "${BOOT_MODE}" = "stateless" ]; then
    if [ ! -f "${INITRAMFS_IMAGE}" ] || [ ! -f "${SQUASHFS_IMAGE}" ]; then
        echo "[-] Missing ${INITRAMFS_IMAGE} or ${SQUASHFS_IMAGE}. Run ./scripts/build-os.sh first." >&2
        exit 1
    fi
    QEMU_ARGS+=(
        -initrd "${INITRAMFS_IMAGE}"
        -drive "file=${SQUASHFS_IMAGE},if=virtio,format=raw,readonly=on"
    )
    APPEND_OPTS+=("root=/dev/vda" "rootfstype=squashfs")
else
    if [ ! -f "${EXT4_IMAGE}" ]; then
        echo "[-] Missing ${EXT4_IMAGE}. Run ./scripts/build-os.sh first." >&2
        exit 1
    fi
    QEMU_ARGS+=(-drive "file=${EXT4_IMAGE},if=virtio,format=raw")
    APPEND_OPTS+=("root=/dev/vda" "rw")
fi

if [ "${UI_MODE}" = "headless" ]; then
    QEMU_ARGS+=(-nographic)
    APPEND_OPTS+=("console=ttyS0" "headless")
else
    QEMU_ARGS+=(
        -device "virtio-vga,xres=1280,yres=800"
        -device virtio-keyboard-pci
        -device qemu-xhci
        -device usb-tablet
    )
    if [ "${ENABLE_BROWSER}" != "no" ] && [ -f "${BROWSER_IMAGE}" ]; then
        QEMU_ARGS+=(-drive "file=${BROWSER_IMAGE},if=virtio,format=raw,readonly=on")
    fi
    if [ -S "/run/user/$(id -u)/pulse/native" ]; then
        QEMU_ARGS+=(-audiodev "pa,id=snd0" -device ich9-intel-hda -device "hda-duplex,audiodev=snd0")
    else
        QEMU_ARGS+=(-audiodev "none,id=snd0" -device ich9-intel-hda -device "hda-duplex,audiodev=snd0")
    fi
    APPEND_OPTS+=("video=1280x800")
fi

if [ "${DEBUG_MODE}" -eq 1 ]; then
    APPEND_OPTS+=("phantom.debug")
else
    APPEND_OPTS+=("quiet")
fi

exec qemu-system-x86_64 "${QEMU_ARGS[@]}" -append "${APPEND_OPTS[*]}"
