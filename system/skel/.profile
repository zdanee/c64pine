# ISO / VM user
export PATH="$HOME/.opencode/bin:$HOME/.c64pine/bin:$PATH"
export ENV="$HOME/.c64pine/config/c64/shell/ashrc"
export EDITOR=vi
[ -n "${XDG_RUNTIME_DIR:-}" ] || { export XDG_RUNTIME_DIR="/tmp/runtime-$(id -u)"; mkdir -p -m 700 "$XDG_RUNTIME_DIR" 2>/dev/null; }

# tty1 autologin -> X
if [ -z "${DISPLAY:-}" ] && [ "$(tty)" = /dev/tty1 ]; then
	exec startx
fi
