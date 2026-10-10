#!/bin/sh
#
# ISO installer (runs as root in the live system, offline)
#
# WIPES the chosen disk:  MBR, 512M ESP + ext4 root, 2G swapfile,
# GRUB for BIOS and UEFI (+ the removable fallback BOOTX64.EFI)
#
# unattended: C64_DISK C64_HOSTNAME C64_USER C64_PASS C64_KEYMAP C64_TZ
#             C64_AUTOLOGIN=y/n C64_YES=1 C64_REBOOT=n
# live ISO:   C64_SRC=dir to copy instead of rootfs.tar.gz, C64_EXCLUDE=boot disk
#
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
T=/mnt/c64pine
TITLE="C64PINE INSTALLER"
LOG=/tmp/c64pine-install.log
PLAIN=0
[ "${C64_YES:-}" = 1 ] && PLAIN=1
[ -f "$HERE/tui.sh" ] || PLAIN=1
[ $PLAIN = 1 ] || . "$HERE/tui.sh"

say() { printf '%s\n' "$*"; }
die() { [ $PLAIN = 1 ] || tui_end; say ""; say "?$*  ERROR"; exit 1; }

[ "$(id -u)" = 0 ] || die "RUN AS ROOT"
[ -n "${C64_SRC:-}" ] || [ -f "$HERE/rootfs.tar.gz" ] || die "rootfs.tar.gz NOT FOUND NEXT TO THE INSTALLER"



# ---- disks ----
media=${C64_EXCLUDE:-$(df "$HERE" 2>/dev/null | awk 'NR == 2 { print $1 }')}
media=$(echo "$media" | sed 's|^/dev/||; s|p\?[0-9]*$||')
DISKS=""
for b in /sys/block/*; do
	d=${b##*/}
	case "$d" in loop*|ram*|sr*|zram*|fd*|dm-*|md*) continue ;; esac
	[ "$d" = "$media" ] && continue
	gb=$(( $(cat "$b/size") / 2097152 ))
	model=$(cat "$b/device/model" 2>/dev/null | sed 's/ *$//')
	DISKS="$DISKS/dev/$d|${gb} GB  ${model:-?}
"
done
[ -n "$DISKS" ] || die "NO DISK FOUND"

KEYMAPS="us gb de fr hu it es pl cz sk se no dk fi pt ch be nl ru jp"



# ---- questions ----
if [ $PLAIN = 1 ]; then
	: "${C64_DISK:=$(echo "$DISKS" | head -1 | cut -d'|' -f1)}" "${C64_HOSTNAME:=c64pine}" "${C64_USER:=user}"
	: "${C64_KEYMAP:=us}" "${C64_TZ:=UTC}" "${C64_AUTOLOGIN:=y}"
	[ -n "${C64_PASS:-}" ] || die "C64_PASS IS NEEDED WITHOUT THE SCREENS"
else
	tui_init
	tui_view "$TITLE" "THIS ERASES ONE DISK AND INSTALLS C64PINE -- the Commodore 64 desktop on Alpine Linux -- on it.

Everything on the disk you choose next is deleted: partitions, systems, files. Other disks are not touched.

You will be asked for: the disk, a name for the computer, your user name and password, the keyboard layout, the time zone, and whether the desktop starts without asking for the password. Nothing is changed before the last question.

(ESC goes back a step; on the first screen it quits.)" || { tui_end; exit 0; }
	step=1
	while [ $step -le 8 ]; do
		case $step in
		1)	[ -n "${C64_DISK:-}" ] && { step=2; continue; }
			set --
			while IFS='|' read -r dev info; do [ -n "$dev" ] && set -- "$@" "$dev   $info"; done <<EOF
$DISKS
EOF
			tui_menu "$TITLE" "Install to which disk? (EVERYTHING ON IT WILL BE ERASED)" "$@" || { tui_end; exit 0; }
			C64_DISK=$(echo "$DISKS" | sed -n "${TUI_RESULT}p" | cut -d'|' -f1) ;;
		2)	tui_input "$TITLE" "A name for this computer (letters, digits, -):" "${C64_HOSTNAME:-c64pine}" || { C64_DISK=""; step=1; continue; }
			case "$TUI_RESULT" in ''|*[!a-zA-Z0-9-]*) continue ;; esac
			C64_HOSTNAME=$TUI_RESULT ;;
		3)	tui_input "$TITLE" "Your user name (lower case letters, digits, - and _):" "${C64_USER:-}" || { step=2; continue; }
			case "$TUI_RESULT" in [a-z_]*) ;; *) continue ;; esac
			case "$TUI_RESULT" in *[!a-z0-9_-]*|root|sandbox) continue ;; esac
			C64_USER=$TUI_RESULT ;;
		4)	tui_input "$TITLE" "Password for $C64_USER:" "" secret || { step=3; continue; }
			p1=$TUI_RESULT
			[ -n "$p1" ] || continue
			tui_input "$TITLE" "The password again:" "" secret || continue
			[ "$TUI_RESULT" = "$p1" ] || { tui_view "$TITLE" "?MISMATCH  ERROR -- the two passwords were not the same. Again."; continue; }
			C64_PASS=$p1 ;;
		5)	set --; for k in $KEYMAPS; do set -- "$@" "$k"; done; set -- "$@" "OTHER (TYPE ITS X11 NAME)"
			tui_menu "$TITLE" "Keyboard layout:" "$@" || { step=4; continue; }
			if [ "$TUI_RESULT" -gt "$(echo $KEYMAPS | wc -w)" ]; then
				tui_input "$TITLE" "Keyboard layout (its X11 name, e.g. us, de, hu, fr-latin1 is fr):" "${C64_KEYMAP:-us}" || continue
				C64_KEYMAP=$TUI_RESULT
			else C64_KEYMAP=$(echo $KEYMAPS | cut -d' ' -f"$TUI_RESULT"); fi ;;
		6)	tui_input "$TITLE" "Time zone (Region/City, e.g. Europe/Budapest, America/New_York, UTC):" "${C64_TZ:-UTC}" || { step=5; continue; }
			[ -n "$TUI_RESULT" ] || continue
			C64_TZ=$TUI_RESULT ;;
		7)	tui_menu "$TITLE" "When the computer starts:" "THE DESKTOP STARTS AT ONCE (NO PASSWORD)" \
				"THE C64 LOGIN SCREEN ASKS FOR THE PASSWORD" || { step=6; continue; }
			[ "$TUI_RESULT" = 1 ] && C64_AUTOLOGIN=y || C64_AUTOLOGIN=n ;;
		8)	tui_view "$TITLE" "THE PLAN

  disk          $C64_DISK  -- EVERYTHING ON IT WILL BE ERASED
  computer      $C64_HOSTNAME
  user          $C64_USER
  keyboard      $C64_KEYMAP
  time zone     $C64_TZ
  start         $( [ "$C64_AUTOLOGIN" = y ] && echo "the desktop at once" || echo "the C64 login screen")

Next: type YES to erase $C64_DISK and install." || { step=7; continue; }
			tui_input "$TITLE" "Type YES (capitals) to erase $C64_DISK and install:" "" || { step=7; continue; }
			[ "$TUI_RESULT" = YES ] || { step=8; continue; } ;;
		esac
		step=$((step + 1))
	done
fi
[ -b "$C64_DISK" ] || die "NO SUCH DISK: $C64_DISK"



# ---- do it ----
part() { case "$C64_DISK" in *[0-9]) echo "${C64_DISK}p$1" ;; *) echo "${C64_DISK}$1" ;; esac; }
ESP=$(part 1) ROOT=$(part 2)

tools() {
	if ! command -v sfdisk >/dev/null || ! command -v mkfs.ext4 >/dev/null; then
		apk add --quiet --no-network e2fsprogs sfdisk >/dev/null 2>&1 || apk add --quiet e2fsprogs sfdisk
	fi
	if ! command -v mkfs.vfat >/dev/null; then
		apk add --quiet --no-network --force-non-repository "$HERE"/apks/dosfstools-*.apk   # live = ram, fine
	fi
}

partition() {
	for p in $(grep "^$C64_DISK" /proc/mounts | cut -d' ' -f2 | sort -r); do umount "$p"; done
	swapoff -a 2>/dev/null || true
	dd if=/dev/zero of="$C64_DISK" bs=1M count=8 2>/dev/null
	sfdisk --quiet --wipe always "$C64_DISK" <<EOF
label: dos
size=512MiB, type=ef
type=83, bootable
EOF
	sleep 2; mdev -s 2>/dev/null || true; sleep 1
	[ -b "$ROOT" ] || { echo "the partitions did not show up"; return 1; }
	mkfs.vfat -F 32 -n EFI "$ESP" >/dev/null
	mkfs.ext4 -F -q -L c64pine "$ROOT"
}

copy_system() {
	mkdir -p $T
	for m in ext4 vfat nls_cp437 nls_iso8859_1; do modprobe $m 2>/dev/null || true; done   # not autoloaded in the live system!
	mount -t ext4 "$ROOT" $T
	if [ -n "${C64_SRC:-}" ]; then cp -a "$C64_SRC"/. $T/
	else gzip -dc "$HERE/rootfs.tar.gz" | tar -xpf - -C $T; fi
	mkdir -p $T/boot/efi
	mount -t vfat "$ESP" $T/boot/efi
}

set_up() {
	uuid() { blkid "$1" | sed -n 's/.* UUID="\([^"]*\)".*/\1/p'; }
	cat > $T/etc/fstab <<EOF
UUID=$(uuid "$ROOT")	/	ext4	rw,relatime	0 1
UUID=$(uuid "$ESP")	/boot/efi	vfat	rw,relatime,fmask=0077,dmask=0077	0 2
/swapfile	none	swap	sw	0 0
tmpfs	/tmp	tmpfs	nosuid,nodev	0 0
EOF
	dd if=/dev/zero of=$T/swapfile bs=1M count=2048 2>/dev/null
	chmod 600 $T/swapfile
	mkswap $T/swapfile >/dev/null
	echo "$C64_HOSTNAME" > $T/etc/hostname
	printf '127.0.0.1\tlocalhost %s\n::1\t\tlocalhost %s\n' "$C64_HOSTNAME" "$C64_HOSTNAME" > $T/etc/hosts
	if [ -f "$T/usr/share/zoneinfo/$C64_TZ" ]; then
		cp "$T/usr/share/zoneinfo/$C64_TZ" $T/etc/localtime; echo "$C64_TZ" > $T/etc/timezone
	fi
	mkdir -p $T/etc/X11/xorg.conf.d
	cat > $T/etc/X11/xorg.conf.d/00-keyboard.conf <<EOF
Section "InputClass"
	Identifier "keyboard"
	MatchIsKeyboard "on"
	Option "XkbLayout" "$C64_KEYMAP"
EndSection
EOF
}

the_user() {
	for m in dev proc sys; do mount --rbind /$m $T/$m; done
	[ -d /sys/firmware/efi/efivars ] && mount -t efivarfs efivarfs $T/sys/firmware/efi/efivars 2>/dev/null || true

	chroot $T /bin/sh -e -s "$C64_USER" "$C64_KEYMAP" "$C64_AUTOLOGIN" <<'CHROOT'
U=$1 KM=$2 AUTO=$3
mv /home/sandbox /home/.template
deluser sandbox 2>/dev/null || true
adduser -D -s /bin/ash "$U"
for g in wheel audio video input plugdev lp; do addgroup "$U" $g 2>/dev/null || true; done
rm -rf "/home/$U"; mv /home/.template "/home/$U"
grep -rl /home/sandbox "/home/$U" 2>/dev/null | while read -r f; do sed -i "s#/home/sandbox#/home/$U#g" "$f"; done
chown -R "$U:$U" "/home/$U"
passwd -l root >/dev/null
L=/etc/lightdm/lightdm.conf.d/60-c64pine-autologin.conf
if [ -f $L ]; then                  # lightdm
	if [ "$AUTO" = y ] || [ "$AUTO" = Y ]; then sed -i "s/^autologin-user=.*/autologin-user=$U/" $L; else rm -f $L; fi
elif [ "$AUTO" = y ] || [ "$AUTO" = Y ]; then   # old images: tty1 autologin
	sed -i "s/login -f sandbox/login -f $U/" /usr/local/sbin/autologin
else
	sed -i 's|^tty1::.*|tty1::respawn:/sbin/getty 38400 tty1|' /etc/inittab
fi
# console keymap (X has its own)
if [ -f "/usr/share/bkeymaps/$KM/$KM.bmap.gz" ]; then
	mkdir -p /etc/keymap
	cp "/usr/share/bkeymaps/$KM/$KM.bmap.gz" /etc/keymap/
	echo "KEYMAP=/etc/keymap/$KM.bmap.gz" > /etc/conf.d/loadkmap
	rc-update add loadkmap boot >/dev/null 2>&1 || true
fi
CHROOT
	printf '%s:%s\n' "$C64_USER" "$C64_PASS" | chroot $T chpasswd
}

boot_loader() {
	chroot $T grub-install --target=i386-pc --recheck "$C64_DISK" >/dev/null 2>&1 || echo "(legacy BIOS boot: grub-install failed)"
	chroot $T grub-install --target=x86_64-efi --efi-directory=/boot/efi --removable >/dev/null 2>&1 || echo "(UEFI fallback loader: grub-install failed)"
	if [ -d /sys/firmware/efi ]; then
		chroot $T grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=c64pine >/dev/null 2>&1 || true
	fi
	chroot $T grub-mkconfig -o /boot/grub/grub.cfg >/dev/null 2>&1 || { echo "grub-mkconfig failed"; return 1; }
	grep -q vmlinuz-lts $T/boot/grub/grub.cfg || { echo "no kernel in the boot menu"; return 1; }
	sed -i '/^disable_trigger=1/d' $T/etc/update-grub.conf 2>/dev/null || true   # re-enable, was off for the build
	[ -d $T/sys/firmware/efi/efivars ] && umount $T/sys/firmware/efi/efivars 2>/dev/null || true
	for m in dev proc sys; do umount -l $T/$m; done
	umount $T/boot/efi
	umount $T
	sync
}

set -- "LOADING THE TOOLS|tools" "ERASING $C64_DISK, NEW PARTITIONS|partition" \
	"$( [ -n "${C64_SRC:-}" ] && echo "COPYING THE SYSTEM" || echo "UNPACKING THE SYSTEM") (A FEW MINUTES)|copy_system" \
	"SETTING UP: NAME, TIME ZONE, KEYBOARD, SWAP|set_up" "THE USER $C64_USER|the_user" "THE BOOT LOADER|boot_loader"
if [ $PLAIN = 1 ]; then
	say ""; say "    **** $TITLE ****"; say ""
	for s; do
		say "${s%%|*}"
		eval "${s#*|}" || die "${s%%|*}"
	done
	say ""; say "READY."; say " C64PINE IS INSTALLED ON $C64_DISK. REMOVE THE USB STICK / DISC AND REBOOT."
	if [ "${C64_REBOOT:-}" != n ]; then
		printf 'reboot now? y/n [y]: '
		read -r r || r=n
		[ "${r:-y}" = n ] || reboot
	fi
	exit 0
fi

set +e
if ! tui_steps "$TITLE" "$LOG" "$@"; then
	tui_view "$TITLE" "?$TUI_FAILED  ERROR

The log: $LOG (its end is above). Nothing else was changed than $C64_DISK."
	tui_end; exit 1
fi
tui_menu "$TITLE" "READY. C64PINE IS INSTALLED ON $C64_DISK.
Take out the USB stick / disc, then:" "RESTART THE COMPUTER NOW" "STAY HERE" || TUI_RESULT=2
tui_end
[ "$TUI_RESULT" = 1 ] && [ "${C64_REBOOT:-}" != n ] && reboot
exit 0
