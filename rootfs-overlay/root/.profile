[ "${TERM:-}" = "xterm" ] && export TERM=xterm-256color

KVER="$(uname -r)"
ROOT_MODE="Read-Write (ext4)"
if grep -q " / overlay " /proc/mounts 2>/dev/null; then
    ROOT_MODE="Stateless (SquashFS + OverlayFS)"
fi
MEM_INFO="$(awk '/^MemTotal:/{t=$2} /^MemFree:/{f=$2} /^Buffers:/{b=$2} /^Cached:/{c=$2} /^SReclaimable:/{s=$2} END{u=(t-f-b-c-s)/1024; if(u<0)u=0; printf "%dMB used / %dMB total", u, t/1024}' /proc/meminfo 2>/dev/null)"
IP_ADDR="$(ip -4 addr show eth0 2>/dev/null | awk '/inet /{print $2; exit}')"
[ -z "${IP_ADDR}" ] && IP_ADDR="pending"

clear
cat << "BANNER"
  ____  _                 _                 ___  ____  
 |  _ \| |__   __ _ _ __ | |_ ___  _ __ ___ / _ \/ ___| 
 | |_) | '_ \ / _` | '_ \| __/ _ \| '_ ` _ \ | | \___ \ 
 |  __/| | | | (_| | | | | || (_) | | | | | | |_| |___) |
 |_|   |_| |_|\__,_|_| |_|\__\___/|_| |_| |_|\___/|____/ 
BANNER
echo " >> User: $(id -un) (uid=$(id -u)) | Kernel: ${KVER} | Mode: ${ROOT_MODE}"
echo " >> RAM: ${MEM_INFO} | eth0: ${IP_ADDR}"
echo " >> Type /opt/tui-menu/menu.sh to open Backend Lab TUI"
echo ""
