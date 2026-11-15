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
MONITOR_SRC="${ROOT_DIR}/apps/phantom-monitor/phantom-monitor.c"

if [ -f "${MONITOR_SRC}" ] && [ -x "${CC_BIN}" ] && [ -x "${PKGCFG_BIN}" ]; then
    export PKG_CONFIG_SYSROOT_DIR="${STAGING_DIR}"
    export PKG_CONFIG_LIBDIR="${STAGING_DIR}/usr/lib/pkgconfig:${STAGING_DIR}/usr/share/pkgconfig"

    if "${PKGCFG_BIN}" --exists gtk+-2.0; then
        echo "[post-build] Building phantom-monitor..."
        mkdir -p "${TARGET_DIR}/usr/bin"
        # shellcheck disable=SC2046
        "${CC_BIN}" -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
            "${MONITOR_SRC}" \
            $("${PKGCFG_BIN}" --cflags --libs gtk+-2.0) \
            -lm -o "${TARGET_DIR}/usr/bin/phantom-monitor"

        if [ -x "${STRIP_BIN}" ]; then
            "${STRIP_BIN}" "${TARGET_DIR}/usr/bin/phantom-monitor"
        fi
    fi
fi
