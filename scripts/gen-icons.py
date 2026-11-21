#!/usr/bin/env python3
"""Generate lightweight, crisp GNOME 47-style PNG icons for PhantomOS."""
import math
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THEME_DIR = os.path.join(ROOT, "rootfs-overlay", "usr", "share", "icons", "PhantomGNOME")


def gear_path(cx=64, cy=64, r_out=51, r_in=40, teeth=8):
    pts = []
    step = 2 * math.pi / teeth
    for i in range(teeth):
        a0 = i * step - math.pi / 2
        a_r1 = a0 - step * 0.25
        a_t1 = a0 - step * 0.14
        a_t2 = a0 + step * 0.14
        a_r2 = a0 + step * 0.25
        pts.append((cx + r_in * math.cos(a_r1), cy + r_in * math.sin(a_r1)))
        pts.append((cx + r_out * math.cos(a_t1), cy + r_out * math.sin(a_t1)))
        pts.append((cx + r_out * math.cos(a_t2), cy + r_out * math.sin(a_t2)))
        pts.append((cx + r_in * math.cos(a_r2), cy + r_in * math.sin(a_r2)))
    d = f"M {pts[0][0]:.2f} {pts[0][1]:.2f} "
    d += " ".join(f"L {x:.2f} {y:.2f}" for x, y in pts[1:]) + " Z"
    return d


SETTINGS_SVG = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="gRim" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#9a9996"/>
      <stop offset="100%" stop-color="#5e5c64"/>
    </linearGradient>
    <linearGradient id="gTop" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#ffffff"/>
      <stop offset="55%" stop-color="#f6f5f4"/>
      <stop offset="100%" stop-color="#c0bfbc"/>
    </linearGradient>
    <linearGradient id="gInner" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#3d3846"/>
      <stop offset="100%" stop-color="#77767b"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="113" rx="38" ry="6" fill="#000000" fill-opacity="0.28"/>
  <path d="{gear_path(64, 67, 51, 39, 8)}" fill="url(#gRim)"/>
  <path d="{gear_path(64, 62, 51, 39, 8)}" fill="url(#gTop)"/>
  <circle cx="64" cy="62" r="28" fill="#d5d3cf"/>
  <circle cx="64" cy="62" r="24" fill="url(#gInner)"/>
  <circle cx="64" cy="64" r="14" fill="#ffffff" fill-opacity="0.14"/>
  <circle cx="64" cy="62" r="13" fill="#241f31"/>
  <circle cx="64" cy="62" r="7" fill="#deddda"/>
</svg>"""

FILES_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="fBack" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#1c71d8"/>
      <stop offset="100%" stop-color="#1a5fb4"/>
    </linearGradient>
    <linearGradient id="fFront" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#8ff0a4" stop-opacity="0"/>
      <stop offset="0%" stop-color="#62a0ea"/>
      <stop offset="100%" stop-color="#3584e4"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="112" rx="48" ry="6" fill="#000000" fill-opacity="0.28"/>
  <path d="M 14 34 C 14 26 20 20 28 20 L 50 20 C 55 20 58 23 61 27 L 65 32 L 104 32 C 110 32 114 36 114 42 L 114 96 C 114 104 108 110 100 110 L 28 110 C 20 110 14 104 14 96 Z" fill="url(#fBack)"/>
  <rect x="20" y="37" width="88" height="14" rx="4" fill="#f6f5f4" fill-opacity="0.88"/>
  <rect x="12" y="46" width="104" height="62" rx="10" fill="#1a5fb4"/>
  <rect x="12" y="42" width="104" height="61" rx="10" fill="url(#fFront)"/>
  <path d="M 22 42 L 106 42 C 111 42 116 46 116 51 L 116 53 C 116 48 111 44 106 44 L 22 44 C 17 44 12 48 12 53 L 12 51 C 12 46 17 42 22 42 Z" fill="#99c1f1" fill-opacity="0.7"/>
  <rect x="44" y="69" width="40" height="5" rx="2.5" fill="#1a5fb4" fill-opacity="0.35"/>
</svg>"""

HOME_FOLDER_SVG = FILES_SVG.replace(
    '<rect x="44" y="69" width="40" height="5" rx="2.5" fill="#1a5fb4" fill-opacity="0.35"/>',
    '<path d="M 64 56 L 47 70 L 52 70 L 52 84 L 61 84 L 61 75 L 67 75 L 67 84 L 76 84 L 76 70 L 81 70 Z" fill="#ffffff" fill-opacity="0.88"/>'
)

DESKTOP_FOLDER_SVG = FILES_SVG.replace(
    '<rect x="44" y="69" width="40" height="5" rx="2.5" fill="#1a5fb4" fill-opacity="0.35"/>',
    '<rect x="48" y="60" width="32" height="22" rx="3" fill="none" stroke="#ffffff" stroke-opacity="0.88" stroke-width="3.5"/><line x1="56" y1="87" x2="72" y2="87" stroke="#ffffff" stroke-opacity="0.88" stroke-width="3.5" stroke-linecap="round"/>'
)

TERMINAL_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="tFrame" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#77767b"/>
      <stop offset="100%" stop-color="#3d3846"/>
    </linearGradient>
    <linearGradient id="tScr" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#241f31"/>
      <stop offset="100%" stop-color="#16141f"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="112" rx="48" ry="6" fill="#000000" fill-opacity="0.28"/>
  <rect x="12" y="24" width="104" height="84" rx="13" fill="#241f31"/>
  <rect x="12" y="20" width="104" height="83" rx="13" fill="url(#tFrame)"/>
  <rect x="18" y="26" width="92" height="71" rx="8" fill="url(#tScr)"/>
  <path d="M 32 45 L 48 56 L 32 67" fill="none" stroke="#ffffff" stroke-width="6.5" stroke-linecap="round" stroke-linejoin="round"/>
  <line x1="54" y1="68" x2="76" y2="68" stroke="#62a0ea" stroke-width="6.5" stroke-linecap="round"/>
  <rect x="18" y="26" width="92" height="3" rx="1.5" fill="#ffffff" fill-opacity="0.10"/>
</svg>"""

MONITOR_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="mFrame" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#77767b"/>
      <stop offset="100%" stop-color="#3d3846"/>
    </linearGradient>
    <linearGradient id="mScr" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#1b2230"/>
      <stop offset="100%" stop-color="#121620"/>
    </linearGradient>
    <linearGradient id="mLine" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0%" stop-color="#2ec27e"/>
      <stop offset="65%" stop-color="#57e389"/>
      <stop offset="100%" stop-color="#62a0ea"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="112" rx="48" ry="6" fill="#000000" fill-opacity="0.28"/>
  <rect x="12" y="24" width="104" height="84" rx="13" fill="#241f31"/>
  <rect x="12" y="20" width="104" height="83" rx="13" fill="url(#mFrame)"/>
  <rect x="18" y="26" width="92" height="71" rx="8" fill="url(#mScr)"/>
  <line x1="18" y1="49" x2="110" y2="49" stroke="#ffffff" stroke-opacity="0.06" stroke-width="1.5"/>
  <line x1="18" y1="73" x2="110" y2="73" stroke="#ffffff" stroke-opacity="0.06" stroke-width="1.5"/>
  <line x1="48" y1="26" x2="48" y2="97" stroke="#ffffff" stroke-opacity="0.06" stroke-width="1.5"/>
  <line x1="78" y1="26" x2="78" y2="97" stroke="#ffffff" stroke-opacity="0.06" stroke-width="1.5"/>
  <path d="M 22 65 L 40 65 L 48 47 L 58 83 L 70 38 L 80 74 L 88 61 L 106 61" fill="none" stroke="#57e389" stroke-opacity="0.28" stroke-width="10" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M 22 65 L 40 65 L 48 47 L 58 83 L 70 38 L 80 74 L 88 61 L 106 61" fill="none" stroke="url(#mLine)" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
  <circle cx="70" cy="38" r="4" fill="#ffffff"/>
</svg>"""

BROWSER_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <radialGradient id="bGlobe" cx="42%" cy="35%" r="65%">
      <stop offset="0%" stop-color="#62a0ea"/>
      <stop offset="55%" stop-color="#3584e4"/>
      <stop offset="100%" stop-color="#1a5fb4"/>
    </radialGradient>
    <linearGradient id="bSweep" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#ffbe6f"/>
      <stop offset="45%" stop-color="#ff7800"/>
      <stop offset="100%" stop-color="#e01b24"/>
    </linearGradient>
    <linearGradient id="bTail" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#9141ac"/>
      <stop offset="100%" stop-color="#e66100"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="113" rx="40" ry="6" fill="#000000" fill-opacity="0.28"/>
  <circle cx="64" cy="64" r="48" fill="#1a5fb4"/>
  <circle cx="64" cy="61" r="48" fill="url(#bGlobe)"/>
  <ellipse cx="64" cy="61" rx="22" ry="47" fill="none" stroke="#99c1f1" stroke-opacity="0.35" stroke-width="2.5"/>
  <line x1="16" y1="61" x2="112" y2="61" stroke="#99c1f1" stroke-opacity="0.35" stroke-width="2.5"/>
  <path d="M 24 88 C 12 68 18 35 44 21 C 28 38 30 62 46 72 C 58 80 82 74 96 52 C 98 78 78 104 48 104 C 36 104 29 97 24 88 Z" fill="url(#bTail)"/>
  <path d="M 32 32 C 54 10 92 14 106 44 C 115 62 104 92 78 102 C 95 85 95 56 76 44 C 62 35 42 38 32 32 Z" fill="url(#bSweep)"/>
  <ellipse cx="52" cy="36" rx="18" ry="8" transform="rotate(-22 52 36)" fill="#ffffff" fill-opacity="0.25"/>
</svg>"""

EDITOR_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="ePaper" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#ffffff"/>
      <stop offset="100%" stop-color="#deddda"/>
    </linearGradient>
    <linearGradient id="ePen" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#f6d32d"/>
      <stop offset="55%" stop-color="#ff7800"/>
      <stop offset="100%" stop-color="#c64600"/>
    </linearGradient>
  </defs>
  <ellipse cx="60" cy="113" rx="40" ry="6" fill="#000000" fill-opacity="0.26"/>
  <rect x="24" y="18" width="74" height="94" rx="10" fill="#9a9996"/>
  <rect x="24" y="14" width="74" height="93" rx="10" fill="url(#ePaper)"/>
  <rect x="24" y="14" width="74" height="16" rx="10" fill="#3584e4"/>
  <rect x="24" y="22" width="74" height="8" fill="#3584e4"/>
  <rect x="35" y="44" width="44" height="5" rx="2.5" fill="#9a9996"/>
  <rect x="35" y="58" width="50" height="5" rx="2.5" fill="#c0bfbc"/>
  <rect x="35" y="72" width="42" height="5" rx="2.5" fill="#c0bfbc"/>
  <rect x="35" y="86" width="32" height="5" rx="2.5" fill="#62a0ea"/>
  <g transform="translate(86,58) rotate(36)">
    <rect x="-8" y="-26" width="16" height="44" rx="3" fill="url(#ePen)"/>
    <rect x="-8" y="-28" width="16" height="9" rx="3" fill="#f66151"/>
    <polygon points="-8,18 8,18 0,31" fill="#f6d32d"/>
    <polygon points="-3,26 3,26 0,31" fill="#241f31"/>
  </g>
</svg>"""

BACKEND_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="kBadge" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#62a0ea"/>
      <stop offset="100%" stop-color="#1c71d8"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="113" rx="36" ry="6" fill="#000000" fill-opacity="0.28"/>
  <g transform="translate(64,65) rotate(45)">
    <rect x="-38" y="-38" width="76" height="76" rx="16" fill="#1a5fb4"/>
  </g>
  <g transform="translate(64,60) rotate(45)">
    <rect x="-38" y="-38" width="76" height="76" rx="16" fill="url(#kBadge)"/>
  </g>
  <polygon points="64,33 88,46 64,59 40,46" fill="#ffffff" fill-opacity="0.95"/>
  <polygon points="40,46 64,59 64,87 40,74" fill="#deddda" fill-opacity="0.92"/>
  <polygon points="88,46 64,59 64,87 88,74" fill="#99c1f1" fill-opacity="0.95"/>
</svg>"""

TRASH_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="trBody" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#deddda"/>
      <stop offset="100%" stop-color="#9a9996"/>
    </linearGradient>
  </defs>
  <ellipse cx="64" cy="113" rx="34" ry="6" fill="#000000" fill-opacity="0.26"/>
  <path d="M 32 36 L 96 36 L 88 104 C 87 109 83 111 78 111 L 50 111 C 45 111 41 109 40 104 Z" fill="url(#trBody)"/>
  <rect x="26" y="27" width="76" height="11" rx="5" fill="#f6f5f4"/>
  <rect x="50" y="18" width="28" height="9" rx="4" fill="#c0bfbc"/>
  <line x1="48" y1="50" x2="50" y2="96" stroke="#5e5c64" stroke-width="4.5" stroke-linecap="round"/>
  <line x1="64" y1="50" x2="64" y2="96" stroke="#5e5c64" stroke-width="4.5" stroke-linecap="round"/>
  <line x1="80" y1="50" x2="78" y2="96" stroke="#5e5c64" stroke-width="4.5" stroke-linecap="round"/>
</svg>"""

TEXT_FILE_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <ellipse cx="64" cy="113" rx="36" ry="5.5" fill="#000000" fill-opacity="0.22"/>
  <path d="M 28 20 C 28 15 32 12 37 12 L 78 12 L 100 34 L 100 104 C 100 109 96 112 91 112 L 37 112 C 32 112 28 109 28 104 Z" fill="#c0bfbc"/>
  <path d="M 28 18 C 28 13 32 10 37 10 L 78 10 L 100 32 L 100 101 C 100 106 96 109 91 109 L 37 109 C 32 109 28 106 28 101 Z" fill="#ffffff"/>
  <path d="M 78 10 L 78 27 C 78 30 80 32 83 32 L 100 32 Z" fill="#deddda"/>
  <rect x="40" y="44" width="44" height="5" rx="2.5" fill="#9a9996"/>
  <rect x="40" y="58" width="48" height="5" rx="2.5" fill="#c0bfbc"/>
  <rect x="40" y="72" width="46" height="5" rx="2.5" fill="#c0bfbc"/>
  <rect x="40" y="86" width="30" height="5" rx="2.5" fill="#62a0ea"/>
</svg>"""

ICONS = [
    ("apps", "preferences-system", SETTINGS_SVG),
    ("apps", "system-file-manager", FILES_SVG),
    ("apps", "utilities-terminal", TERMINAL_SVG),
    ("apps", "applications-system", MONITOR_SVG),
    ("apps", "utilities-system-monitor", MONITOR_SVG),
    ("apps", "web-browser", BROWSER_SVG),
    ("apps", "accessories-text-editor", EDITOR_SVG),
    ("apps", "system-software-install", BACKEND_SVG),
    ("places", "folder", FILES_SVG),
    ("places", "user-home", HOME_FOLDER_SVG),
    ("places", "user-desktop", DESKTOP_FOLDER_SVG),
    ("places", "user-trash", TRASH_SVG),
    ("mimetypes", "text-x-generic", TEXT_FILE_SVG),
]

SIZES = [24, 48, 64]


def main():
    index_path = os.path.join(THEME_DIR, "index.theme")
    os.makedirs(THEME_DIR, exist_ok=True)
    dirs = []
    for sz in SIZES:
        for cat in ("apps", "places", "mimetypes"):
            os.makedirs(os.path.join(THEME_DIR, f"{sz}x{sz}", cat), exist_ok=True)
            dirs.append(f"{sz}x{sz}/{cat}")

    with open(index_path, "w") as f:
        f.write("[Icon Theme]\n")
        f.write("Name=PhantomGNOME\n")
        f.write("Comment=Lightweight GNOME 47 Adwaita-inspired icon theme for PhantomOS\n")
        f.write("Inherits=Adwaita,hicolor\n")
        f.write("Directories=" + ",".join(dirs) + "\n\n")
        for sz in SIZES:
            for cat in ("apps", "places", "mimetypes"):
                ctx = "Applications" if cat == "apps" else ("Places" if cat == "places" else "MimeTypes")
                f.write(f"[{sz}x{sz}/{cat}]\nSize={sz}\nContext={ctx}\nType=Fixed\n\n")

    for cat, name, svg in ICONS:
        svg_bytes = svg.encode("utf-8")
        for sz in SIZES:
            out_png = os.path.join(THEME_DIR, f"{sz}x{sz}", cat, f"{name}.png")
            subprocess.run(
                ["rsvg-convert", "-w", str(sz), "-h", str(sz), "-o", out_png],
                input=svg_bytes,
                check=True,
            )

    # Also build preview strip at /tmp/icons_preview.png
    previews = []
    for name in (
        "preferences-system", "system-file-manager", "utilities-terminal",
        "applications-system", "web-browser", "accessories-text-editor", "system-software-install"
    ):
        previews.append(os.path.join(THEME_DIR, "64x64", "apps", f"{name}.png"))
    subprocess.run(
        ["magick", "montage"] + previews + ["-background", "#181825", "-tile", "7x1", "-geometry", "72x72+8+8", "/tmp/icons_preview.png"],
        check=True,
    )
    print(f"Generated {len(ICONS) * len(SIZES)} icons into {THEME_DIR}")


if __name__ == "__main__":
    main()
