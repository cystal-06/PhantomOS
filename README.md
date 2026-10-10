# Micro-OS Backend Tester 🐧⚡

> **Đồ án cơ sở 4 - Trường Đại học Công nghệ Thông tin và Truyền thông Việt - Hàn (VKU)**  
> **Đề tài:** Phát triển hệ điều hành thu gọn dựa trên lõi Linux chuyên dụng cho kiểm thử Backend  
> **Giảng viên hướng dẫn:** ThS. Ngô Lê Quân  
> **Sinh viên thực hiện:**  
> - Nguyễn Công Đức (MSSV: 24IT340 - Lớp 24SE1)  
> - Trần Văn Tri (MSSV: 24IT283 - Lớp 24SE1)  

---

## 📌 1. Giới thiệu đề tài

Hệ điều hành siêu nhẹ (**Micro-OS**) được tinh chỉnh từ nhân Linux và bộ công cụ BusyBox/Buildroot nhằm tối ưu hóa 100% tài nguyên phần cứng cho việc kiểm thử ứng dụng Backend (Java / Spring Boot).

### 🌟 Điểm nổi bật kỹ thuật:
- **Stateless (Đóng băng hệ thống):** Khóa phân vùng gốc Read-only thông qua **OverlayFS**, tự động khôi phục trạng thái nguyên bản sau mỗi lần khởi động.
- **RAM Disk (tmpfs) Tốc độ cao:** Đẩy toàn bộ cơ sở dữ liệu tạm thời (Database) và Log lên RAM, loại bỏ hiện tượng nghẽn cổ chai I/O của ổ đĩa.
- **Tối ưu máy ảo Java (JRE):** Sử dụng `jlink` cắt gọt OpenJDK còn dưới 45MB runtime.
- **Tiêu thụ tài nguyên tối thiểu:** RAM khi nhàn rỗi (idle) dưới **50MB - 100MB**, khởi động dưới **2 giây**.

---

## 🏗️ 2. Kiến trúc hệ thống (System Architecture)

```mermaid
flowchart TD
    subgraph Boot ["Quá trình khởi động"]
        BIOS[BIOS / UEFI] --> GRUB[GRUB / Bootloader]
        GRUB --> Kernel[Linux Kernel (x86_64)]
        Kernel --> InitRAM[initramfs script]
    end

    subgraph Storage ["Cơ chế Lưu trữ Stateless"]
        InitRAM --> Lower[Lower Layer: SquashFS / Read-Only Root]
        InitRAM --> Upper[Upper Layer: tmpfs / RAM Writeable]
        Lower & Upper --> Overlay[OverlayFS Union Mount]
        Overlay --> SwitchRoot[switch_root /newroot]
    end

    subgraph Runtime ["Môi trường Chạy Backend"]
        SwitchRoot --> RamDisk[tmpfs: /var/lib/data & /var/log]
        SwitchRoot --> JRE[Minimal JRE Runtime]
        RamDisk & JRE --> BackendApp[Spring Boot Backend API :8080]
        SwitchRoot --> TUI[TUI Control Menu]
    end
```

---

## 📂 3. Cấu trúc thư mục dự án

```text
micro-backend-os/
├── configs/              # File cấu hình Buildroot và Linux Kernel
├── overlayfs/            # Script can thiệp initramfs và cơ chế đóng băng
├── rootfs-overlay/       # Thư mục nhúng trực tiếp vào Root Filesystem của OS
│   ├── etc/init.d/       # Các dịch vụ tự khởi động (Network, RAM Disk, Spring Boot)
│   └── opt/tui-menu/     # Giao diện TUI quản lý hệ thống
├── backend-sample/       # Mã nguồn RESTful API Spring Boot phục vụ kiểm thử tải
├── benchmarks/           # Bộ kịch bản Stress-test (k6 / wrk / jmeter) và vẽ đồ thị
├── scripts/              # Các script tự động hóa (build OS, chạy QEMU)
└── docs/                 # Báo cáo, slide thuyết trình và tài liệu nghiên cứu
```

---

## 🚀 4. Hướng dẫn cài đặt & Biên dịch

### Yêu cầu môi trường (Host Machine):
- Hệ điều hành: Arch Linux / Ubuntu 22.04+
- Các gói phụ thuộc: `gcc`, `make`, `bc`, `rsync`, `ncurses`, `qemu-system-x86`

### Các bước thực hiện:
```bash
# 1. Chạy script build tự động
./scripts/build-os.sh

# 2. Khởi động hệ điều hành trên máy ảo QEMU
./scripts/run-qemu.sh
```

---

## 👥 5. Phân công công việc (Task Division)

| Thành viên | Nhiệm vụ chính |
| :--- | :--- |
| **Nguyễn Công Đức** | - Tùy biến Linux Kernel & cấu hình Buildroot cơ sở<br>- Thiết lập RAM Disk (tmpfs) cho Database & tối ưu I/O<br>- Xây dựng ứng dụng Spring Boot mẫu & benchmark đối chứng |
| **Trần Văn Tri** | - Nghiên cứu & triển khai cơ chế Stateless OverlayFS<br>- Cắt tỉa và đóng gói Java Runtime tối giản (jlink)<br>- Xây dựng Menu giao diện điều khiển TUI/GUI |

---

## 📜 6. Giấy phép
Dự án được thực hiện nhằm mục đích học tập và nghiên cứu trong khuôn khổ môn học **Đồ án cơ sở 4 - VKU**.
