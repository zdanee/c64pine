# nsxiv, for fedora
SRC_WHAT="nsxiv 34 (the picture viewer)"
SRC_VERSION=34
SRC_URL=https://codeberg.org/nsxiv/nsxiv/archive/v34.tar.gz
SRC_BUILD_dnf="gcc make pkgconf-pkg-config imlib2-devel libX11-devel libXft-devel libexif-devel giflib-devel"
SRC_RUN_dnf="imlib2 libX11 libXft libexif giflib"
SRC_BINS="nsxiv"
src_make() {
	make -j"$(nproc)" PREFIX="$1"
	make PREFIX="$1" install
	rm -rf "$1/share/man"
}
