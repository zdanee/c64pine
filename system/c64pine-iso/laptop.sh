#!/bin/sh
#
# !!! IMAGE BUILDING ONLY !!!
# removes linux-virt and syslinux, never run this on an installed system
#
# Turns the VM rootfs into the one the ISO installs: real kernel, grub,
# runlevels, doas with password, + hardware.sh. Runs chrooted.
#
set -eux
[ -n "${HELP_FILE:-}" ] || : "${HOST:?}"
HERE=$(cd "$(dirname "$0")" && pwd)
H=/home/sandbox
if [ ! -f "$HERE/hardware.sh" ]; then wget -q "$HOST/hardware.sh" -O "$HERE/hardware.sh"; fi
if [ -z "${HELP_FILE:-}" ]; then HELP_FILE=$HERE/help-laptop.txt; wget -q "$HOST/help-laptop.txt" -O "$HELP_FILE"; fi

# firmware first, otherwise linux-lts pulls in the whole multi-GB linux-firmware
sh "$HERE/hardware.sh" $H "$HELP_FILE"


# ---- kernel + grub ----
# grub's trigger would probe the root device, which is a dir here. install.sh
# runs grub-mkconfig itself and turns it back on
grep -q '^disable_trigger=1' /etc/update-grub.conf 2>/dev/null || echo disable_trigger=1 >> /etc/update-grub.conf
apk add --no-progress linux-lts mkinitfs grub grub-bios grub-efi efibootmgr e2fsprogs dosfstools sfdisk
apk del --no-progress linux-virt syslinux 2>/dev/null || true
grep -q 'nvme keymap' /etc/mkinitfs/mkinitfs.conf || sed -i 's/^features="\(.*\)"/features="\1 nvme keymap"/' /etc/mkinitfs/mkinitfs.conf
mkinitfs "$(ls /lib/modules | grep -- -lts | head -1)"

cat > /etc/default/grub <<'EOF'
GRUB_DISTRIBUTOR="C64pine"
GRUB_TIMEOUT=3
GRUB_DISABLE_SUBMENU=y
GRUB_DISABLE_RECOVERY=true
GRUB_CMDLINE_LINUX_DEFAULT="modules=sd-mod,usb-storage,ext4,nvme rootfstype=ext4 quiet"
EOF
cat > /etc/grub.d/06_c64 <<'EOF'
#!/bin/sh
# the boot menu in C64 colours
cat <<'GRUB'
set color_normal=light-blue/blue
set color_highlight=blue/light-blue
set menu_color_normal=light-blue/blue
set menu_color_highlight=blue/light-blue
GRUB
EOF
chmod 755 /etc/grub.d/06_c64

# doas with password, except power stuff
cat > /etc/doas.d/wheel.conf <<'EOF'
permit persist :wheel
permit nopass :wheel cmd poweroff
permit nopass :wheel cmd reboot
permit nopass :wheel cmd zzz
permit nopass :wheel cmd /usr/local/sbin/c64-sys
EOF
chmod 0400 /etc/doas.d/wheel.conf

# runlevels like setup-alpine makes them
for s in devfs dmesg; do rc-update add $s sysinit; done
for s in modules sysctl hostname bootmisc syslog hwclock swap seedrng localmount; do rc-update add $s boot; done
for s in mount-ro killprocs savecache; do rc-update add $s shutdown; done

chown -R sandbox:sandbox $H
ls /usr/libexec/pipewire-launcher
echo "files naming /home/sandbox (the installer rewrites them):"; grep -rl /home/sandbox $H || true
rm -rf /var/cache/apk/*
