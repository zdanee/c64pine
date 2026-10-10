# cmus (fedora has no package without rpmfusion)
SRC_WHAT="cmus 2.12 (the music player)"
SRC_VERSION=2.12.0
SRC_URL=https://github.com/cmus/cmus/archive/refs/tags/v2.12.0.tar.gz
SRC_BUILD_dnf="gcc make pkgconf-pkg-config ncurses-devel alsa-lib-devel pulseaudio-libs-devel libavformat-free-devel libswresample-free-devel libavcodec-free-devel flac-devel libvorbis-devel opusfile-devel libmad-devel libcue-devel"
SRC_RUN_dnf="ncurses-libs alsa-lib pulseaudio-libs libavformat-free libswresample-free libavcodec-free flac-libs libvorbis opusfile libmad libcue"
SRC_BINS="cmus cmus-remote"
src_make() {
	./configure prefix="$1" CONFIG_OSS=n
	make -j"$(nproc)"
	make install
	rm -rf "$1/share/man" "$1/share/doc"
}
