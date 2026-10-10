# Credits and licences

C64pine's own code and art (everything in this repository except what is listed below) is licensed under the
**GNU General Public License v3.0** (see `LICENSE`): c64desk, the scripts, the dotfiles, the generated themes and icon
theme, the sprites, the vfetch logo and the installer.

## Included in this repository

| What | Where | Author | Licence |
|---|---|---|---|
| **Pet Me 64**, **Pet Me 64 2Y** fonts | `fonts/` | Kreative Software (Rebecca G. Bettencourt), [kreativekorp.com](https://www.kreativekorp.com/software/fonts/) | Kreative Software Relay Fonts Free Use License (`fonts/FreeLicense.txt`): free redistribution, unmodified, with the licence and credit; may not be sold. **Not** covered by the GPL. |
| **stb_image** v2.30 | `c64desk/stb_image.h` | Sean Barrett and contributors, [github.com/nothings/stb](https://github.com/nothings/stb) | public domain (Unlicense) or MIT, at your choice (see the end of the file) |
| Colodore palette | colour values in the sources | Philip "pepto" Timmermann | colour measurements (facts) |

## Built from source by `install/alpine.sh` (not included; fetched at the pinned commits)

| Program | Source | Licence |
|---|---|---|
| st-bonk (terminal) | https://gitlab.com/feloniousbonk/st-bonk (based on suckless st) | MIT/X Consortium |
| dmenu (kota's fork) | https://git.sr.ht/~kota/dmenu | MIT/X Consortium |
| dprint | https://git.sr.ht/~kota/dprint | GPL-3.0 |
| lemonbar-xft | https://github.com/krypt-n/bar | MIT-style |
| vfetch | https://codeberg.org/exotya/vfetch (+ `patches/vfetch-c64-logo.patch`) | ISC-style |

## Installed from Alpine Linux's repositories

Everything else (the kernel, X.Org, Openbox, picom, PCManFM, LibreWolf, GIMP, AbiWord, Gnumeric, cmus, mpv, nsxiv,
zathura, FFmpeg, 7-Zip, libarchive, xarchiver, NetworkManager, PipeWire, ...) comes as Alpine packages under their own licences; Alpine's package sources:
https://gitlab.alpinelinux.org/alpine/aports .

**VICE** (C64 emulator, GPL-2.0, https://vice-emu.sourceforge.io) comes from Alpine's edge/testing repository. It
ships the original Commodore 64 ROM images, which are not free software; their copyright is held by Cloanto. VICE and
Alpine redistribute them; this repository does not contain them.

**opencode** (MIT, https://opencode.ai) is installed with its official installer at install time.

**The C64 LIBRARY** (`home/sh/c64lib.sh`) looks titles up at run time on Wikipedia (text: CC BY-SA 4.0; the cover and
screenshot pictures under their own terms, often fair use) and Wikidata (CC0), and keeps the result only in the user's
own folders (`.cdb`). None of that data is part of this repository or the ISOs.

## The ISO releases

The installer ISOs on the releases page are the official Alpine "standard" ISO with C64pine added. They contain
Alpine packages (many under the GPL) and VICE with its ROMs. The corresponding source: this repository (C64pine's own
code and the exact build scripts), the upstream projects above at the pinned commits, and Alpine's aports
(https://gitlab.alpinelinux.org/alpine/aports, branch `3.24-stable`; VICE: `master`, `testing/vice`) for the binary
packages, whose versions are listed in the ISO's `/lib/apk/db/installed` (in `c64/rootfs.tar.gz`).

## Trademarks

"Commodore" and the Commodore "C=" logo are trademarks of their respective owner. C64pine is an unofficial fan
project, not affiliated with or endorsed by the trademark owner. The vfetch logo and the C64 icon are original pixel
art in the style of the logo and the computer.
