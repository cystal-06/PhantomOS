export LANG=C.UTF-8
export LC_ALL=C.UTF-8
[ "${TERM:-}" = "xterm" ] && export TERM=xterm-256color

KVER="$(uname -r)"
ROOT_MODE="Read-Write (ext4)"
if grep -q " / overlay " /proc/mounts 2>/dev/null; then
    ROOT_MODE="Stateless (SquashFS + OverlayFS)"
fi
MEM_INFO="$(awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf "%dMB used / %dMB total", (t-a)/1024, t/1024}' /proc/meminfo 2>/dev/null)"
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
echo " >> Kernel: ${KVER} | Mode: ${ROOT_MODE}"
echo " >> RAM: ${MEM_INFO} | eth0: ${IP_ADDR}"
echo " >> Type /opt/tui-menu/menu.sh to open Backend Lab TUI"
echo ""
