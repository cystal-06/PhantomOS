#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_CACHE="${DIR}/buildroot-2024.02.1/dl/wifi-firmware"
BASE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"

# Curated basic Wi-Fi firmware blobs for real laptops & USB adapters
# (Keeps disk & RAM tiny instead of fetching the 360MB+ full linux-firmware archive)
FW_FILES=(
    # Realtek PCIe (rtw88 / rtw89)
    "rtw88/rtw8822c_fw.bin"
    "rtw88/rtw8822c_wow_fw.bin"
    "rtw88/rtw8822b_fw.bin"
    "rtw88/rtw8821c_fw.bin"
    "rtw88/rtw8723d_fw.bin"
    "rtw89/rtw8852a_fw.bin"
    "rtw89/rtw8852b_fw-1.bin"
    "rtw89/rtw8852b_fw.bin"
    "rtw89/rtw8852c_fw.bin"
    # Intel iwlwifi (7265D, 3168, 8265, 9260, AX200, AX201, AX210)
    "iwlwifi-7265D-29.ucode"
    "iwlwifi-3168-29.ucode"
    "iwlwifi-8265-36.ucode"
    "iwlwifi-9000-pu-b0-jf-b0-46.ucode"
    "iwlwifi-9260-th-b0-jf-b0-46.ucode"
    "iwlwifi-cc-a0-77.ucode"
    "iwlwifi-QuZ-a0-hr-b0-77.ucode"
    "iwlwifi-ty-a0-gf-a0-77.ucode"
    "iwlwifi-ty-a0-gf-a0.pnvm"
    # MediaTek (MT7921 / MT7922)
    "mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin"
    "mediatek/WIFI_RAM_CODE_MT7922_1.bin"
    "mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin"
    "mediatek/WIFI_RAM_CODE_MT7961_1.bin"
    # Qualcomm Atheros (QCA6174 / QCA9377)
    "ath10k/QCA6174/hw3.0/firmware-6.bin"
    "ath10k/QCA6174/hw3.0/board-2.bin"
    "ath10k/QCA9377/hw1.0/firmware-6.bin"
    "ath10k/QCA9377/hw1.0/board-2.bin"
    # Realtek USB / Legacy (rtlwifi)
    "rtlwifi/rtl8723befw.bin"
    "rtlwifi/rtl8821aefw.bin"
)

mkdir -p "${FW_CACHE}"
FORCE=0
if [ "${1:-}" = "--refresh" ]; then
    FORCE=1
fi

echo "[+] Fetching essential Wi-Fi firmware blobs into ${FW_CACHE}..."
ok_cnt=0
fail_cnt=0
for rel in "${FW_FILES[@]}"; do
    dst="${FW_CACHE}/${rel}"
    mkdir -p "$(dirname "${dst}")"
    if [ "${FORCE}" -eq 0 ] && [ -s "${dst}" ]; then
        ok_cnt=$((ok_cnt + 1))
        continue
    fi
    if curl -fsSL --retry 2 --connect-timeout 8 "${BASE_URL}/${rel}?h=20240115" -o "${dst}.tmp"; then
        mv "${dst}.tmp" "${dst}"
        ok_cnt=$((ok_cnt + 1))
    else
        rm -f "${dst}.tmp"
        echo "    [warn] skipped unavailable blob: ${rel}" >&2
        fail_cnt=$((fail_cnt + 1))
    fi
done

echo "[+] Wi-Fi firmware ready (${ok_cnt} files, skipped ${fail_cnt}): $(du -sh "${FW_CACHE}" | awk '{print $1}')"
