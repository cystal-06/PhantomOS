#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDROOT_DIR="${DIR}/buildroot-2024.02.1"
DL_DIR="${BUILDROOT_DIR}/dl/firefox"
ARCHIVE="${DL_DIR}/firefox-esr-latest.tar.xz"
OUT_SQUASHFS="${BUILDROOT_DIR}/output/images/browser.squashfs"
URL="https://download.mozilla.org/?product=firefox-esr-latest&os=linux64&lang=en-US"

MKSQUASHFS="$(command -v mksquashfs || true)"
if [ -x "${BUILDROOT_DIR}/output/host/bin/mksquashfs" ]; then
    MKSQUASHFS="${BUILDROOT_DIR}/output/host/bin/mksquashfs"
fi
if [ -z "${MKSQUASHFS}" ]; then
    echo "[-] mksquashfs not found. Build OS first or install squashfs-tools." >&2
    exit 1
fi

mkdir -p "${DL_DIR}" "$(dirname "${OUT_SQUASHFS}")"
if [ ! -f "${ARCHIVE}" ] || [ "${1:-}" = "--refresh" ]; then
    echo "[+] Downloading Firefox ESR (Linux x86_64)..."
    curl -fSL "${URL}" -o "${ARCHIVE}.tmp"
    mv "${ARCHIVE}.tmp" "${ARCHIVE}"
else
    echo "[+] Using cached Firefox archive: ${ARCHIVE}"
fi

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

echo "[+] Extracting and stripping bloat..."
tar -xf "${ARCHIVE}" -C "${STAGE_DIR}"
FF_ROOT="${STAGE_DIR}/firefox"

rm -f \
    "${FF_ROOT}/crashreporter" \
    "${FF_ROOT}/crashhelper" \
    "${FF_ROOT}/minidump-analyzer" \
    "${FF_ROOT}/pingsender" \
    "${FF_ROOT}/updater" \
    "${FF_ROOT}/updater.ini" \
    "${FF_ROOT}/precomplete" \
    "${FF_ROOT}/removed-files" \
    "${FF_ROOT}/Throbber-small.gif"

mkdir -p "${FF_ROOT}/defaults/pref" "${FF_ROOT}/distribution"

cat > "${FF_ROOT}/defaults/pref/phantom-ram.js" <<'EOF'
// PhantomOS ultra-low-RAM & stateless profile defaults
pref("browser.cache.disk.enable", false);
pref("browser.cache.disk_cache_ssl", false);
pref("browser.cache.offline.enable", false);
pref("browser.cache.memory.enable", true);
pref("browser.cache.memory.capacity", 32768);
pref("media.memory_cache_max_size", 16384);
pref("browser.sessionhistory.max_entries", 8);
pref("browser.sessionstore.interval", 60000);
pref("browser.tabs.unloadOnLowMemory", true);
pref("dom.ipc.processCount", 1);
pref("fission.autostart", false);
pref("gfx.webrender.software", true);
pref("browser.shell.checkDefaultBrowser", false);
pref("browser.download.folderList", 1);
pref("browser.download.useDownloadDir", true);
pref("toolkit.telemetry.enabled", false);
pref("toolkit.telemetry.unified", false);
pref("datareporting.healthreport.uploadEnabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("app.update.enabled", false);
pref("app.update.auto", false);
pref("network.prefetch-next", false);
pref("network.dns.disablePrefetch", true);
pref("browser.startup.homepage", "file:///opt/firefox/phantom-start.html");
EOF

cat > "${FF_ROOT}/distribution/policies.json" <<'EOF'
{
  "policies": {
    "DisableAppUpdate": true,
    "DisableFirefoxStudies": true,
    "DisablePocket": true,
    "DisableTelemetry": true,
    "DontCheckDefaultBrowser": true,
    "OverrideFirstRunPage": "",
    "OverridePostUpdatePage": "",
    "FirefoxHome": {
      "Search": true,
      "TopSites": false,
      "SponsoredTopSites": false,
      "Highlights": false,
      "Pocket": false,
      "SponsoredPocket": false,
      "Snippets": false
    }
  }
}
EOF

cat > "${FF_ROOT}/phantom-start.html" <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>PhantomOS Browser</title>
<style>
  body { margin: 0; font-family: sans-serif; background: #11111b; color: #cdd6f4; display: flex; min-height: 100vh; align-items: center; justify-content: center; }
  .card { background: #1e1e2e; border: 1px solid #313244; border-radius: 14px; padding: 28px 34px; max-width: 560px; width: 90%; box-shadow: 0 12px 32px rgba(0,0,0,0.45); }
  h1 { margin: 0 0 6px; font-size: 24px; color: #89b4fa; }
  p { margin: 0 0 18px; color: #a6adc8; font-size: 14px; }
  .grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
  a { display: block; padding: 11px 14px; background: #181825; border: 1px solid #313244; border-radius: 8px; color: #cdd6f4; text-decoration: none; font-weight: 600; font-size: 14px; }
  a:hover { border-color: #89b4fa; color: #89b4fa; }
  .note { margin-top: 16px; font-size: 12px; color: #9399b2; }
</style>
</head>
<body>
  <div class="card">
    <h1>PhantomOS Web Browser</h1>
    <p>Stateless Firefox ESR • Low-RAM Profile (1 Content Process • No Disk Cache)</p>
    <div class="grid">
      <a href="https://www.youtube.com">YouTube</a>
      <a href="https://www.google.com">Google Search</a>
      <a href="https://github.com">GitHub</a>
      <a href="http://localhost:8080">Local Backend (:8080)</a>
    </div>
    <div class="note">Downloads are saved to <code>~/Downloads</code> (ephemeral tmpfs overlay).</div>
  </div>
</body>
</html>
EOF

echo "[+] Building compressed SquashFS module: ${OUT_SQUASHFS}..."
rm -f "${OUT_SQUASHFS}"
"${MKSQUASHFS}" "${FF_ROOT}" "${OUT_SQUASHFS}" -comp zstd -Xcompression-level 19 -all-root -noappend >/dev/null
ls -lh "${OUT_SQUASHFS}"
