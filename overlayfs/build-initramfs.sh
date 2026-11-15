#!/bin/bash
# ==============================================================================
# PhantomOS Buildroot Post-Image Script
# Generates a minimal initramfs.cpio.gz in ${BINARIES_DIR} containing BusyBox,
# required glibc runtime libraries, and overlayfs/init.
# ==============================================================================
set -euo pipefail

BINARIES_DIR="${1:?BINARIES_DIR argument required}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INIT_SCRIPT="${SCRIPT_DIR}/init"
STAGING_DIR="${BASE_DIR:-${BINARIES_DIR}/..}/initramfs-root"
OUTPUT_CPIO="${BINARIES_DIR}/initramfs.cpio.gz"

if [ ! -f "${INIT_SCRIPT}" ]; then
    echo "[-] Missing ${INIT_SCRIPT}" >&2
    exit 1
fi

if [ -z "${TARGET_DIR:-}" ] || [ ! -x "${TARGET_DIR}/bin/busybox" ]; then
    echo "[-] TARGET_DIR/bin/busybox not found (TARGET_DIR=${TARGET_DIR:-unset})" >&2
    exit 1
fi

READELF_BIN="$(command -v "${HOST_DIR:-/usr}/bin/x86_64-linux-readelf" || command -v readelf)"

rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}"/{bin,sbin,lib,usr/lib,proc,sys,dev,mnt/lower,mnt/upper,newroot}
ln -s lib "${STAGING_DIR}/lib64"
ln -s lib "${STAGING_DIR}/usr/lib64"

# Copy BusyBox binary
install -m 0755 "${TARGET_DIR}/bin/busybox" "${STAGING_DIR}/bin/busybox"

# Recursively copy shared libraries required by BusyBox from TARGET_DIR
copy_lib_with_deps() {
    local bin_path="$1"
    local needed lib_src lib_dest real_src

    # Ensure dynamic linker is present in /lib (accessible via /lib64 -> lib)
    for ld in "${TARGET_DIR}/lib/ld-linux-x86-64.so.2" "${TARGET_DIR}/lib64/ld-linux-x86-64.so.2"; do
        if [ -e "${ld}" ] && [ ! -e "${STAGING_DIR}/lib/ld-linux-x86-64.so.2" ]; then
            real_src="$(readlink -f "${ld}")"
            install -m 0755 "${real_src}" "${STAGING_DIR}/lib/ld-linux-x86-64.so.2"
        fi
    done

    needed="$("${READELF_BIN}" -d "${bin_path}" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p' || true)"
    for lib in ${needed}; do
        lib_src=""
        for search_dir in "${TARGET_DIR}/lib" "${TARGET_DIR}/lib64" "${TARGET_DIR}/usr/lib"; do
            if [ -e "${search_dir}/${lib}" ]; then
                lib_src="${search_dir}/${lib}"
                break
            fi
        done
        if [ -z "${lib_src}" ]; then
            echo "[-] Warning: shared library ${lib} not found in TARGET_DIR" >&2
            continue
        fi
        lib_dest="${STAGING_DIR}/lib/${lib}"
        if [ ! -e "${lib_dest}" ]; then
            real_src="$(readlink -f "${lib_src}")"
            install -m 0755 "${real_src}" "${lib_dest}"
            copy_lib_with_deps "${real_src}"
        fi
    done
}

copy_lib_with_deps "${TARGET_DIR}/bin/busybox"

# Create BusyBox applet symlinks used by /init and rescue shell
for applet in sh ash mount umount mkdir sleep usleep cat grep cut ls echo mknod blkid dmesg reboot poweroff ln chmod tr test "[" rm mv cp uname ps; do
    ln -sf /bin/busybox "${STAGING_DIR}/bin/${applet}"
done
ln -sf /bin/busybox "${STAGING_DIR}/sbin/switch_root"

# Install PID 1 /init
install -m 0755 "${INIT_SCRIPT}" "${STAGING_DIR}/init"

# Pack compressed cpio archive
(
    cd "${STAGING_DIR}"
    find . -print0 | cpio --null -o --format=newc --quiet | gzip -9 > "${OUTPUT_CPIO}"
)

echo "[+] Generated ${OUTPUT_CPIO} ($(du -h "${OUTPUT_CPIO}" | cut -f1))"
