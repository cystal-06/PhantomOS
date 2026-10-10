#!/bin/sh
# ========================================================
# TUI Control Menu cho Micro-OS Backend Tester
# ========================================================

show_menu() {
    while true; do
        CHOICE=$(dialog --clear \
                --backtitle "PhantomOS Backend Tester v1.0" \
                --title " BẢNG ĐIỀU KHIỂN HỆ THỐNG " \
                --menu "Dùng phím mũi tên và Enter để chọn:" 15 60 6 \
                1 "Thông tin IP & Cấu hình mạng" \
                2 "Trạng thái ứng dụng Spring Boot" \
                3 "Kiểm tra dung lượng RAM Disk (/var/lib/data)" \
                4 "Giám sát tài nguyên hệ thống (htop)" \
                5 "Khởi động lại (Reset sạch rác - Stateless)" \
                6 "Thoát ra Shell Terminal" \
                2>&1 >/dev/tty)

        clear
        case "$CHOICE" in
            1)
                ip addr show
                read -p "Nhấn Enter để quay lại menu..." _
                ;;
            2)
                ps | grep java || echo "Spring Boot hiện chưa chạy."
                read -p "Nhấn Enter để quay lại menu..." _
                ;;
            3)
                df -h /var/lib/data
                read -p "Nhấn Enter để quay lại menu..." _
                ;;
            4)
                htop
                ;;
            5)
                reboot
                ;;
            6)
                break
                ;;
        esac
    done
}

if command -v dialog >/dev/null 2>&1; then
    show_menu
else
    echo "Hệ thống chưa cài đặt dialog. Vui lòng sử dụng CLI thông thường."
fi
