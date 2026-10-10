C64PINE - installer for the C=64 desktop (on Alpine Linux 3.24, x86_64)

Boot this disc / USB stick (legacy BIOS or UEFI; Secure Boot off), log in as root (no password) and run
c64-install. It asks for the disk, a computer name, your user name + password, keyboard layout, time zone and
whether to log in automatically, then ERASES the chosen disk: 512 MB EFI partition + ext4 root (2 GB swap file),
GRUB for BIOS and UEFI. No network needed.

Write to a USB stick from Linux:  dd if=c64pine-*.iso of=/dev/sdX bs=4M conv=fsync   (whole stick, not sdX1)
Windows: Rufus in "DD image" mode.

C64PINE: https://github.com/zdanee/c64pine (GPL-3.0; third-party licences in CREDITS.md there).
