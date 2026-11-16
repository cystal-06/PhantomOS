# PhantomOS global POSIX shell environment
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export TZ="Asia/Ho_Chi_Minh"
export LANG="C.UTF-8"
export LC_ALL="C.UTF-8"
export PAGER="less"

if [ -x /usr/bin/vi ]; then
    export EDITOR="/usr/bin/vi"
fi

if [ -n "${PS1:-}" ]; then
    if [ "$(id -u)" -eq 0 ]; then
        export PS1='[\u@\h \W]# '
    else
        export PS1='[\u@\h \W]\$ '
    fi
fi
