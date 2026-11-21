[ "${TERM:-}" = "xterm" ] && export TERM=xterm-256color

KVER="$(uname -r)"
ROOT_MODE="Read-Write"
if grep -q " / overlay " /proc/mounts 2>/dev/null; then
    ROOT_MODE="Stateless (SquashFS + OverlayFS)"
fi
MEM_INFO="$(awk '/^MemTotal:/{t=$2} /^MemFree:/{f=$2} /^Buffers:/{b=$2} /^Cached:/{c=$2} /^SReclaimable:/{s=$2} END{u=(t-f-b-c-s)/1024; if(u<0)u=0; printf "%dMB / %dMB", u, t/1024}' /proc/meminfo 2>/dev/null)"
IP_ADDR="$(ip -4 addr show eth0 2>/dev/null | awk '/inet /{print $2; exit}')"
[ -z "${IP_ADDR}" ] && IP_ADDR="offline"

clear
printf "\033[1;38;2;137;180;250mPhantomOS\033[0m  \033[38;2;100;116;139m•\033[0m  Kernel %s  \033[38;2;100;116;139m•\033[0m  %s\n" "${KVER}" "${ROOT_MODE}"
printf "\033[38;2;148;163;184mUser:\033[0m %s  \033[38;2;100;116;139m|\033[0m  \033[38;2;148;163;184mRAM:\033[0m %s  \033[38;2;100;116;139m|\033[0m  \033[38;2;148;163;184mIP:\033[0m %s  \033[38;2;100;116;139m|\033[0m  \033[38;2;137;180;250m/opt/tui-menu/menu.sh\033[0m\n\n" "$(id -un)" "${MEM_INFO}" "${IP_ADDR}"
