#!/bin/sh
#
#   curl -fsSL https://raw.githubusercontent.com/zdanee/c64pine/main/get.sh | sh
#
# Downloads to a temp dir and runs the installer. Nothing is changed
# before you say yes in the installer.
#
#   C64PINE_REF=v1.4        tag or branch
#   C64PINE_SRC=/some/dir   local copy
#   GITHUB_TOKEN=...         private fork
set -eu
REPO=zdanee/c64pine
REF=${C64PINE_REF:-main}

say() { printf '%s\n' "$*" >&2; }
[ "$(id -u)" != 0 ] || { say "Run this as your normal user, not as root: the installer asks for root itself when needed."; exit 1; }
( : </dev/tty ) 2>/dev/null || { say "The installer asks you questions: run this in a terminal."; exit 1; }

if [ -n "${C64PINE_SRC:-}" ]; then
	exec "$C64PINE_SRC/installer/c64pine-install" "$@" </dev/tty
fi

for t in tar gzip; do command -v $t >/dev/null 2>&1 || { say "This needs '$t' (install it with your package manager)."; exit 1; }; done
if command -v curl >/dev/null 2>&1; then get() { curl -fsSL ${2:+-H "$2"} "$1"; }
elif command -v wget >/dev/null 2>&1; then get() { wget -qO- ${2:+--header="$2"} "$1"; }
else say "This needs curl or wget."; exit 1; fi

D=$(mktemp -d "${TMPDIR:-/tmp}/c64pine-get.XXXXXX")
say "**** C64PINE ****"
say "Downloading $REPO ($REF) into $D ..."
if [ -n "${GITHUB_TOKEN:-}" ]; then
	get "https://api.github.com/repos/$REPO/tarball/$REF" "Authorization: Bearer $GITHUB_TOKEN"
else
	get "https://codeload.github.com/$REPO/tar.gz/$REF"
fi | tar xz -C "$D" --strip-components=1 || { say "?DOWNLOAD  ERROR (is the name '$REF' right? a private repository needs GITHUB_TOKEN)"; exit 1; }

# stdin is the pipe from curl, give the installer the terminal
exec "$D/installer/c64pine-install" "$@" </dev/tty
