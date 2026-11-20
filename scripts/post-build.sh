#!/bin/bash
set -euo pipefail

TARGET_DIR="${1:-${TARGET_DIR:?TARGET_DIR is required}}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BR_DIR="${ROOT_DIR}/buildroot-2024.02.1"

if [ -x "${BR_DIR}/board/qemu/x86_64/post-build.sh" ]; then
    "${BR_DIR}/board/qemu/x86_64/post-build.sh" "${TARGET_DIR}"
fi

STAGING_DIR="${STAGING_DIR:-${BR_DIR}/output/staging}"
HOST_DIR="${HOST_DIR:-${BR_DIR}/output/host}"
CC_BIN="${HOST_DIR}/bin/x86_64-linux-gcc"
STRIP_BIN="${HOST_DIR}/bin/x86_64-linux-strip"
PKGCFG_BIN="${HOST_DIR}/bin/pkg-config"

build_c_app() {
    local app_name="$1"
    local pkgs="$2"
    local src_file="${ROOT_DIR}/apps/${app_name}/${app_name}.c"
    local out_bin="${TARGET_DIR}/usr/bin/${app_name}"
    shift 2

    [ -f "${src_file}" ] || return 0
    if ! "${PKGCFG_BIN}" --exists ${pkgs}; then
        return 0
    fi

    echo "[post-build] Building ${app_name}..."
    mkdir -p "${TARGET_DIR}/usr/bin"
    # shellcheck disable=SC2046
    "${CC_BIN}" -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
        "${src_file}" \
        $("${PKGCFG_BIN}" --cflags --libs ${pkgs}) \
        "$@" -o "${out_bin}"

    if [ -x "${STRIP_BIN}" ]; then
        "${STRIP_BIN}" "${out_bin}"
    fi
}

if [ -x "${CC_BIN}" ] && [ -x "${PKGCFG_BIN}" ]; then
    export PKG_CONFIG_SYSROOT_DIR="${STAGING_DIR}"
    export PKG_CONFIG_LIBDIR="${STAGING_DIR}/usr/lib/pkgconfig:${STAGING_DIR}/usr/share/pkgconfig"

    build_c_app "phantom-monitor" "gtk+-2.0" -lm
    build_c_app "phantom-login" "gtk+-2.0" -lm -lcrypt -lX11
    build_c_app "phantom-panel" "gtk+-2.0 xrandr" -lm -lX11
    build_c_app "phantom-gestures" "libinput libudev x11" -lm
fi

# Install curated Wi-Fi firmware blobs if downloaded
FW_CACHE="${BR_DIR}/dl/wifi-firmware"
if [ -d "${FW_CACHE}" ] && [ -n "$(ls -A "${FW_CACHE}" 2>/dev/null)" ]; then
    mkdir -p "${TARGET_DIR}/lib/firmware"
    cp -a "${FW_CACHE}/." "${TARGET_DIR}/lib/firmware/"
fi

# Populate default user home from /etc/skel (ownership applied by Buildroot mkusers)
mkdir -p "${TARGET_DIR}/opt/firefox" "${TARGET_DIR}/root/Downloads"
if [ -d "${TARGET_DIR}/etc/skel" ]; then
    mkdir -p "${TARGET_DIR}/etc/skel/Downloads" "${TARGET_DIR}/home/phantom"
    cp -a "${TARGET_DIR}/etc/skel/." "${TARGET_DIR}/home/phantom/"
fi
