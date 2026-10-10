# PhantomOS 🐧⚡

> A minimal, stateless Linux distribution optimized for backend performance & load testing.

[![Kernel](https://img.shields.io/badge/Kernel-Linux%20x86__64-orange.svg)]()
[![Architecture](https://img.shields.io/badge/Architecture-Stateless%20OverlayFS-green.svg)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## 📌 Overview

**PhantomOS** is an ultra-lightweight, stateless micro-distribution built on top of the Linux Kernel and BusyBox/Buildroot. It is engineered specifically to provide an isolated, deterministic, and high-performance execution environment for backend applications (Java / Spring Boot, Go, Node.js).

### ✨ Key Features
- **Stateless by Design (OverlayFS):** The root filesystem is locked as Read-Only (SquashFS). All runtime state changes are held in ephemeral RAM layers and discarded upon reboot.
- **In-Memory Database & Logging (RAM Disk):** Critical paths such as `/var/lib/data` and `/var/log` are mounted directly on `tmpfs`, eliminating disk I/O bottlenecks during stress testing.
- **Trimmed Runtime Footprint:** Stripped Java Runtime Environment (JRE) via `jlink` (~40MB) and essential networking toolchains (`curl`, `iproute2`, `htop`).
- **Sub-Second Boot:** Cold-boots in under 2 seconds with an idle memory footprint of **under 60MB RAM**.

---

## 🏗️ System Architecture

```mermaid
flowchart TD
    subgraph Boot ["1. Boot Phase"]
        BIOS[BIOS / UEFI] --> GRUB[Bootloader / GRUB]
        GRUB --> Kernel[Linux Kernel]
        Kernel --> InitRAM[initramfs]
    end

    subgraph Storage ["2. Stateless Storage Stack"]
        InitRAM --> Lower[Lower Layer: SquashFS (Read-Only Root)]
        InitRAM --> Upper[Upper Layer: tmpfs (RAM Ephemeral)]
        Lower & Upper --> Overlay[OverlayFS Union Mount]
        Overlay --> SwitchRoot[switch_root /newroot]
    end

    subgraph Runtime ["3. Workload & Test Environment"]
        SwitchRoot --> RamDisk[tmpfs: /var/lib/data & /var/log]
        SwitchRoot --> JRE[Minimal JRE / Native Runtime]
        RamDisk & JRE --> BackendApp[Backend Service (:8080)]
        SwitchRoot --> TUI[TUI Management Console]
    end
```

---

## 📂 Repository Structure

```text
PhantomOS/
├── configs/              # Buildroot & Linux Kernel configuration files
├── overlayfs/            # Initramfs and stateless mount logic
├── rootfs-overlay/       # Target filesystem overlay (daemons, services, configs)
│   ├── etc/init.d/       # Init scripts (networking, ramdisk, service launcher)
│   └── opt/tui-menu/     # Terminal UI control dashboard
├── backend-sample/       # Sample RESTful backend application for load testing
├── benchmarks/           # Load testing scripts (k6, wrk) and metric collectors
├── scripts/              # Automated build and emulation scripts
└── docs/                 # Architecture notes and benchmarking reports
```

---

## 🚀 Getting Started

### Prerequisites (Host Machine)
Ensure the following packages are installed on your host system:
* **Arch Linux:** `sudo pacman -S --needed base-devel git bc rsync ncurses qemu-system-x86`
* **Ubuntu/Debian:** `sudo apt install build-essential git bc rsync libncurses-dev qemu-system-x86`

### Build PhantomOS
```bash
# Clone the repository
git clone https://github.com/<username>/PhantomOS.git
cd PhantomOS

# Run the automated build script
./scripts/build-os.sh
```

### Run on QEMU
```bash
./scripts/run-qemu.sh
```
* **Default Login:** `root`
* **Port Forwarding:** Host `localhost:8080` forwarded to VM `8080`

---

## 📄 License
This project is open-source under the [MIT License](LICENSE).
