# VICE from source: fedora (no rpmfusion) and debian/ubuntu, whose package
# ships without the ROMs. we only keep the C64 bits
SRC_WHAT="VICE 3.10 (the C64 emulator; 10-30 minutes)"
SRC_VERSION=3.10

SRC_URL="https://downloads.sourceforge.net/project/vice-emu/releases/vice-3.10.tar.gz https://sourceforge.net/projects/vice-emu/files/releases/vice-3.10.tar.gz/download"
SRC_SHA256=8e5bac18cbcb9f192380ad3ef881f8790f5b75c41d7b3da65d831985d864d6d1
SRC_BUILD_dnf="gcc gcc-c++ make pkgconf-pkg-config flex bison xa dos2unix texinfo file gtk3-devel glew-devel alsa-lib-devel pulseaudio-libs-devel libpng-devel giflib-devel libcurl-devel libevdev-devel"
SRC_RUN_dnf="gtk3 glew alsa-lib pulseaudio-libs libpng giflib libcurl libevdev"
SRC_BUILD_apt="build-essential pkg-config flex bison xa65 dos2unix texinfo file libgtk-3-dev libglew-dev libasound2-dev libpulse-dev libpng-dev libgif-dev libcurl4-openssl-dev libevdev-dev"
SRC_RUN_apt="libgtk-3-0t64/libgtk-3-0 libglew2.2/libglew2.1 libasound2t64/libasound2 libpulse0 libpng16-16t64/libpng16-16 libgif7 libcurl4t64/libcurl4 libevdev2"
SRC_BINS="x64sc c1541 petcat"
src_make() {   # $1 = where to install
	./configure --prefix="$1" --enable-gtk3ui --disable-pdf-docs --disable-html-docs --without-oss
	make -j"$(nproc)"
	make install
	for f in "$1"/bin/*; do case " $SRC_BINS " in *" ${f##*/} "*) ;; *) rm -f "$f" ;; esac; done
	for d in C128 CBM-II DTV PET PLUS4 SCPU64 VIC20 C64DTV; do rm -rf "$1/share/vice/$d"; done
	rm -rf "$1/share/man" "$1/share/info" "$1/share/doc"
}
