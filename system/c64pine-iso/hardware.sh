#!/bin/sh
# hardware.sh HOME [HELP_FILE]
#
# laptop stuff: firmware, NetworkManager, bluetooth, pipewire, acpid,
# touchpad, trim ... Safe to re-run, doesn't touch kernel/bootloader.
# (unlike laptop.sh!)
set -eux
H=${1:?usage: hardware.sh HOME [HELP_FILE]}
HELP=${2:-}
A=$H/.c64pine

# only the common laptop firmware, linux-firmware as a whole is several GB
apk add --no-progress intel-ucode wireless-regdb \
	linux-firmware-i915 linux-firmware-intel linux-firmware-rtlwifi linux-firmware-rtw88 linux-firmware-rtw89 \
	linux-firmware-rtl_bt linux-firmware-rtl_nic linux-firmware-ath10k linux-firmware-ath9k_htc linux-firmware-ar3k \
	linux-firmware-qca linux-firmware-brcm linux-firmware-cypress linux-firmware-mediatek
apk add --no-progress \
	networkmanager networkmanager-wifi networkmanager-tui networkmanager-cli wpa_supplicant iw \
	bluez bluetuith pipewire wireplumber pipewire-pulse pipewire-alsa pipewire-spa-bluez pipewire-tools dbus-x11 \
	brightnessctl zzz chrony kbd-bkeymaps arandr guvcview libva-intel-driver acpid
apk del --no-progress spice-vdagent 2>/dev/null || true

mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/c64.conf <<'EOF'
# no polkit, so plugdev gets to manage connections
[main]
auth-polkit=false
dhcp=internal
[device]
wifi.backend=wpa_supplicant
EOF
printf 'auto lo\niface lo inet loopback\n' > /etc/network/interfaces    # rest is NM's job

# lid/sleep -> suspend, power btn -> off
cat > /etc/acpi/handler.sh <<'EOF'
#!/bin/sh
# acpid: $1 = event (button/lid ...), $2 = device, $3 = action
case "$1" in
	button/power) poweroff ;;
	button/sleep) zzz ;;
	button/lid)   [ "$3" = close ] && zzz ;;
esac
EOF
chmod 755 /etc/acpi/handler.sh
mkdir -p /etc/acpi/events
printf 'event=.*\naction=/etc/acpi/handler.sh %%e\n' > /etc/acpi/events/anything

mkdir -p /etc/X11/xorg.conf.d
cat > /etc/X11/xorg.conf.d/30-touchpad.conf <<'EOF'
Section "InputClass"
	Identifier "touchpad"
	MatchIsTouchpad "on"
	Driver "libinput"
	Option "Tapping" "on"
	Option "DisableWhileTyping" "on"
EndSection
EOF

cat > /etc/periodic/weekly/fstrim <<'EOF'
#!/bin/sh
fstrim -a
EOF
chmod 755 /etc/periodic/weekly/fstrim

rc-update del networking boot 2>/dev/null || true
rc-update del spice-vdagentd default 2>/dev/null || true
for s in dbus acpid alsa networkmanager bluetooth chronyd crond; do rc-update add $s default; done


# ---- desktop icons ----
printf '[Desktop Entry]\nType=Application\nName=NETWORK\nExec=sh -c "c64-term -e nmtui"\nIcon=network\nX-C64-Order=125\n' > $A/Desktop/network.desktop
printf '[Desktop Entry]\nType=Application\nName=BLUETOOTH\nExec=sh -c "c64-term -e bluetuith"\nIcon=bluetooth\nX-C64-Order=126\n' > $A/Desktop/bluetooth.desktop
printf '[Desktop Entry]\nType=Application\nName=CAMERA\nExec=guvcview\nIcon=camera\nX-C64-Order=127\n' > $A/Desktop/camera.desktop
# (menu entries are already there, they appear once the programs exist)
[ -n "$HELP" ] && cp "$HELP" $A/config/help-openbox.txt
chown -R "$(stat -c %u:%g "$H")" $A
rm -rf /var/cache/apk/*
