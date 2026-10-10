#!/usr/bin/env python3
"""draws the default art (needs Pillow). output is committed, this isn't shipped.

  c64/sprites/*.png              desktop icons, 24x21
  icons/c64/ + sprites.map       icon theme made from the sprites
  themes/c64-{1,2,3}x/openbox-3  openbox theme
  /tmp/c64pine-preview.png      everything enlarged, for checking
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.join(HERE, "c64pine", "data")
WORK = "/tmp"

# pepto's "colodore" palette
C64 = ["#000000", "#ffffff", "#813338", "#75cec8", "#8e3c97", "#56ac4d", "#2e2c9b", "#edf171",
       "#8e5029", "#553800", "#c46c71", "#4a4a4a", "#7b7b7b", "#a9ff9f", "#706deb", "#b2b2b2"]
BLACK, WHITE, RED, CYAN, PURPLE, GREEN, BLUE, YELLOW, ORANGE, BROWN, LRED, DGREY, GREY, LGREEN, LBLUE, LGREY = C64
LETTERS = ".kwrcpgbynzx"              # old .txt format
# 9-11 aren't real C64 colours, but the breadbin needs beige. same table in c64desk!
PEN_RGB = C64[:8] + [BROWN, "#c9b68e", "#9a8660"]
SW, SH = 24, 21

T, K, W, R, C, P, G, B, Y, N, Z, X = range(12)


def new():
    im = Image.new("P", (SW, SH), T)
    return im, ImageDraw.Draw(im)


def box(d, x0, y0, x1, y1, fill, outline=None):
    d.rectangle((x0, y0, x1, y1), fill=fill, outline=outline)


def masked(base, layer, mask):
    # copy layer onto base where mask != T
    for y in range(SH):
        for x in range(SW):
            if layer.getpixel((x, y)) != T and mask.getpixel((x, y)) != T:
                base.putpixel((x, y), layer.getpixel((x, y)))


def terminal():
    im, d = new()
    box(d, 1, 1, 22, 14, W)                    # casing
    box(d, 2, 2, 21, 12, K)                    # bezel
    box(d, 3, 3, 20, 11, B)                    # the blue screen
    d.line((4, 4, 17, 4), fill=C)              # **** banner ****
    d.line((5, 6, 19, 6), fill=C)              # 64K RAM SYSTEM ...
    d.line((4, 8, 9, 8), fill=C)               # READY.
    box(d, 4, 9, 5, 10, C)                     # cursor
    d.point((19, 13), fill=R)                  # power LED
    box(d, 9, 15, 14, 15, W)                   # neck
    box(d, 6, 16, 17, 17, W)                   # foot
    d.line((6, 18, 17, 18), fill=K)
    return im


def floppy():
    im, d = new()
    box(d, 2, 0, 21, 20, K)
    box(d, 4, 1, 17, 6, W)                     # label
    d.line((5, 3, 16, 3), fill=B)
    d.line((5, 5, 12, 5), fill=B)
    d.point((21, 2), fill=T); d.point((21, 3), fill=T)   # write-protect notch
    d.ellipse((8, 8, 15, 13), fill=W)          # hub ring
    box(d, 10, 10, 13, 11, B)                  # hub hole
    d.point((16, 14), fill=W)                  # index hole
    box(d, 10, 16, 13, 19, P)                  # head slot (media)
    return im


def drive():                                   # a 1541
    im, d = new()
    box(d, 0, 5, 23, 16, W, K)
    d.line((1, 6, 22, 6), fill=Y)              # beige-ish top edge
    box(d, 3, 9, 20, 10, K)                    # disk slot
    box(d, 10, 8, 13, 12, K)                   # door lever
    box(d, 11, 8, 12, 12, W)
    d.point((17, 14), fill=G)                  # power LED
    box(d, 19, 14, 20, 14, R)                  # drive LED
    d.line((1, 17, 22, 17), fill=K)
    return im


def globe():
    im, d = new()
    mask, md = new()
    md.ellipse((2, 0, 21, 20), fill=K)
    d.ellipse((2, 0, 21, 20), fill=C)
    land, ld = new()
    ld.ellipse((4, 2, 10, 8), fill=G)
    ld.ellipse((6, 7, 10, 15), fill=G)
    ld.ellipse((13, 3, 19, 8), fill=G)
    ld.ellipse((14, 8, 18, 17), fill=G)
    ld.rectangle((8, 17, 15, 20), fill=G)      # antarctica peeking
    masked(im, land, mask)
    d.ellipse((2, 0, 21, 20), outline=K)
    d.point((6, 3), fill=W); d.point((5, 4), fill=W)    # shine
    return im


def writer():
    im, d = new()
    box(d, 3, 1, 16, 19, W, K)
    for y in (4, 6, 8, 10, 12, 14):
        d.line((5, y, 14 if y != 14 else 10, y), fill=B)
    d.line((21, 2, 12, 15), fill=Y, width=3)   # pencil
    box(d, 20, 1, 22, 2, R)                    # eraser
    d.point((11, 16), fill=K); d.point((12, 16), fill=K); d.point((11, 17), fill=K)   # tip
    return im


def calc():
    im, d = new()
    box(d, 1, 2, 22, 18, W, K)
    box(d, 2, 3, 21, 5, G)                     # header row
    for x in (8, 15):
        d.line((x, 3, x, 17), fill=K)
    for y in (6, 10, 14):
        d.line((2, y, 21, y), fill=K)
    box(d, 16, 11, 21, 13, Y)                  # selected cell
    d.line((3, 8, 6, 8), fill=B); d.line((10, 8, 13, 8), fill=B); d.line((3, 12, 5, 12), fill=B)
    d.line((10, 16, 13, 16), fill=R)
    return im


def paint():
    im, d = new()
    d.ellipse((0, 4, 19, 20), fill=Y, outline=K)
    d.ellipse((4, 13, 7, 16), fill=T, outline=K)          # thumb hole
    for (x, y, c) in ((4, 7, R), (9, 6, G), (14, 8, C), (14, 14, P), (9, 15, W)):
        box(d, x, y, x + 2, y + 2, c)
    d.line((22, 0, 15, 9), fill=R, width=2)    # brush handle
    box(d, 13, 9, 15, 11, K)                   # bristles
    return im


def tasks():
    im, d = new()
    box(d, 1, 1, 22, 19, K)
    box(d, 2, 2, 21, 18, B)
    for (x, top, c) in ((4, 12, G), (8, 8, Y), (12, 4, R), (16, 10, C)):
        box(d, x, top, x + 2, 16, c)
    d.line((3, 17, 20, 17), fill=W)
    return im


def help_():
    im, d = new()
    d.ellipse((2, 0, 21, 20), fill=P, outline=K)
    # a fat PETSCII-style question mark
    box(d, 8, 4, 15, 5, Y)
    box(d, 14, 5, 15, 9, Y)
    box(d, 12, 9, 15, 10, Y)
    box(d, 11, 10, 12, 12, Y)
    box(d, 7, 5, 8, 6, Y)
    box(d, 11, 15, 12, 16, Y)
    return im


def crt():
    im, d = new()
    d.line((11, 5, 6, 0), fill=K); d.line((12, 5, 17, 0), fill=K)   # rabbit ears
    box(d, 1, 5, 22, 19, P, K)
    box(d, 3, 7, 16, 17, C)
    for y in range(8, 18, 2):
        d.line((3, y, 16, y), fill=B)          # scanlines
    box(d, 18, 8, 20, 9, Y); box(d, 18, 12, 20, 13, Y)              # knobs
    d.line((4, 20, 6, 20), fill=K); d.line((17, 20, 19, 20), fill=K)  # feet
    return im


def power():
    im, d = new()
    d.ellipse((1, 0, 22, 20), fill=R, outline=K)
    d.ellipse((6, 5, 17, 16), outline=W, width=2)
    box(d, 10, 4, 13, 7, R)                    # gap at the top of the ring
    box(d, 11, 3, 12, 10, W)                   # the bar
    return im


def c64():                                     # the beige breadbin: rounded front edge, logo badge, red LED, brown keys
    im, d = new()
    d.rounded_rectangle((0, 11, 23, 19), radius=4, fill=X, outline=K)    # the rounded front edge (drawn first,
    d.line((2, 15, 21, 15), fill=Z)                                        #  the top surface overlaps its upper half)
    d.polygon([(3, 1), (20, 1), (23, 14), (0, 14)], fill=Z, outline=K)    # the sloping top
    box(d, 4, 2, 10, 4, N)                                                 # badge with a tiny white "C="
    for x, y in ((6, 2), (7, 2), (5, 3), (6, 4), (7, 4), (8, 3), (9, 3)):
        d.point((x, y), fill=W)
    d.point((18, 3), fill=R); d.point((19, 3), fill=R)                    # the single red LED
    for y, x0, x1 in ((6, 4, 16), (8, 3, 17), (10, 3, 17)):                # key rows, widening with the slope
        for x in range(x0, x1 + 1, 2):
            d.point((x, y), fill=N)
    d.line((7, 12, 14, 12), fill=N)                                        # space bar
    for y in (6, 8, 10):
        d.line((19 + (y - 6) // 4, y, 20 + (y - 6) // 4, y), fill=N)       # function keys
    return im


def network():                                 # WiFi waves over a little router
    im, d = new()
    for r in (4, 8, 12):
        d.arc((11.5 - r, 13 - r, 11.5 + r, 13 + r), 220, 320, fill=C, width=2)
    box(d, 10, 11, 13, 13, W)                  # the source dot
    box(d, 3, 15, 20, 19, W, K)                # router
    d.point((6, 17), fill=G); d.point((8, 17), fill=G); d.point((10, 17), fill=Y)
    return im


def bluetooth():                               # the rune, white on a blue capsule
    im, d = new()
    d.rounded_rectangle((5, 0, 18, 20), radius=6, fill=B, outline=W)
    d.line((11, 3, 11, 17), fill=W, width=2)
    d.line((11, 3, 15, 7), fill=W, width=2)
    d.line((15, 7, 7, 14), fill=W, width=2)
    d.line((11, 17, 15, 13), fill=W, width=2)
    d.line((15, 13, 7, 6), fill=W, width=2)
    return im


def camera():                                  # a webcam on its clip
    im, d = new()
    d.ellipse((4, 0, 19, 15), fill=K)
    d.ellipse((7, 3, 16, 12), fill=C)
    d.ellipse((9, 5, 14, 10), fill=B)
    d.point((10, 6), fill=W)                   # glint
    d.point((17, 3), fill=R)                   # LED
    box(d, 9, 15, 14, 16, K)                   # neck
    box(d, 5, 17, 18, 18, K)                   # clip
    return im


def code():                                    # opencode: </> on a dark screen
    im, d = new()
    d.rounded_rectangle((0, 1, 23, 19), radius=3, fill=K, outline=W)
    d.line((8, 6, 4, 10), fill=G, width=2); d.line((4, 10, 8, 14), fill=G, width=2)       # <
    d.line((15, 6, 19, 10), fill=G, width=2); d.line((19, 10, 15, 14), fill=G, width=2)   # >
    d.line((13, 5, 10, 15), fill=Y, width=2)                                              # /
    return im


def install():                                 # live ISO: a down arrow into a 1541
    im, d = new()
    box(d, 9, 0, 14, 6, Y, K)                    # arrow shaft
    d.polygon([(4, 7), (19, 7), (11, 13), (12, 13)], fill=Y, outline=K)   # arrow head
    box(d, 0, 12, 23, 20, W, K)                  # the drive
    box(d, 3, 15, 20, 16, K)                     # slot
    box(d, 19, 18, 20, 18, R)                    # LED
    return im


def folder():
    im, d = new()
    box(d, 1, 3, 9, 5, Y, K)                   # tab
    box(d, 1, 5, 22, 18, Y, K)
    d.line((2, 7, 21, 7), fill=K)
    return im


def doc():
    im, d = new()
    box(d, 5, 0, 18, 20, W, K)
    for y in (4, 7, 10, 13, 16):
        d.line((7, y, 16 if y != 16 else 12, y), fill=B)
    return im


def picture():
    im, d = new()
    box(d, 1, 2, 22, 18, W, K)
    box(d, 3, 4, 20, 16, C)
    box(d, 15, 5, 17, 7, Y)                    # sun
    d.polygon([(3, 16), (9, 8), (14, 16)], fill=G)
    d.polygon([(10, 16), (15, 11), (20, 16)], fill=G)
    return im


def program():                                 # executable: a cartridge
    im, d = new()
    box(d, 3, 2, 20, 18, K)
    box(d, 5, 4, 18, 11, R)
    d.line((7, 6, 16, 6), fill=W); d.line((7, 8, 13, 8), fill=W)
    for x in range(5, 19, 2):
        d.line((x, 16, x, 18), fill=Y)         # edge connector
    return im


def music():                                   # two beamed quavers
    im, d = new()
    d.polygon([(7, 2), (21, 0), (21, 3), (7, 5)], fill=W, outline=K)       # beam
    box(d, 7, 3, 8, 16, W); box(d, 20, 1, 21, 14, W)                      # stems
    d.ellipse((1, 14, 8, 20), fill=Y, outline=K)
    d.ellipse((14, 12, 21, 18), fill=Y, outline=K)
    return im


def video():                                   # a clapperboard with a play button
    im, d = new()
    d.polygon([(1, 2), (21, 0), (22, 4), (2, 6)], fill=W, outline=K)      # the clapper
    for x in (5, 10, 15, 20):
        d.line((x, 1, x - 2, 5), fill=K, width=2)
    box(d, 1, 7, 22, 20, K, W)                                             # the board
    d.polygon([(9, 9), (16, 13), (9, 17)], fill=R)                         # play
    return im


def pdf():                                     # a page with a red band
    im, d = new()
    box(d, 5, 0, 18, 20, W, K)
    for y in (3, 5, 7):
        d.line((7, y, 16, y), fill=B)
    box(d, 3, 10, 20, 15, R, K)
    for x in (6, 7, 10, 11, 14, 15):           # three tiny white blocks: "PDF"
        d.point((x, 12), fill=W); d.point((x, 13), fill=W)
    d.line((7, 17, 14, 17), fill=B)
    return im


def archive():                                 # a cardboard box with a zipper (zip, rar, 7z, tgz ...)
    im, d = new()
    d.polygon([(2, 6), (6, 1), (17, 1), (21, 6)], fill=X, outline=K)      # the lid, in perspective
    box(d, 2, 6, 21, 19, Z, K)                                            # the box
    box(d, 10, 1, 13, 19, N)                                              # the strap
    for y in range(2, 19, 2):
        d.point((11, y), fill=W); d.point((12, y + 1), fill=W)            # zipper teeth
    box(d, 9, 9, 14, 12, Y, K)                                            # the pull
    return im


SPRITES = {
    "terminal": terminal, "floppy": floppy, "drive": drive, "globe": globe, "writer": writer, "calc": calc,
    "paint": paint, "tasks": tasks, "c64": c64,
    "network": network, "bluetooth": bluetooth, "camera": camera, "code": code, "install": install, "help": help_, "crt": crt, "power": power,
    "folder": folder, "doc": doc, "picture": picture, "program": program, "music": music, "video": video, "pdf": pdf, "archive": archive,
}

# GTK icon theme names -> sprite
ICON_THEME = {
    "places": {"folder": "folder", "user-home": "folder", "user-desktop": "folder", "folder-documents": "folder",
               "folder-download": "folder", "folder-pictures": "folder", "user-trash": "floppy",
               "drive-harddisk": "drive", "network-workgroup": "globe"},
    "mimetypes": {"text-x-generic": "doc", "application-x-generic": "doc", "unknown": "doc", "text-x-script": "doc",
                  "image-x-generic": "picture", "application-x-executable": "program", "x-office-document": "writer",
                  "x-office-spreadsheet": "calc", "audio-x-generic": "music", "video-x-generic": "video",
                  "application-pdf": "pdf", "package-x-generic": "archive", "application-x-archive": "archive",
                  "application-zip": "archive", "application-x-compressed-tar": "archive",
                  "application-x-7z-compressed": "archive", "application-vnd.rar": "archive"},
    "apps": {"utilities-terminal": "terminal", "system-file-manager": "floppy", "web-browser": "globe"},
}


def to_text(im):
    return "\n".join("".join(LETTERS[im.getpixel((x, y))] for x in range(SW)) for y in range(SH)) + "\n"


def to_rgba(im, scale, square=False):
    h = SW if square else SH
    out = Image.new("RGBA", (SW, h), (0, 0, 0, 0))
    top = (h - SH) // 2
    for y in range(SH):
        for x in range(SW):
            p = im.getpixel((x, y))
            if p != T:
                out.putpixel((x, y + top), Image.new("RGB", (1, 1), PEN_RGB[p - 1]).getpixel((0, 0)) + (255,))
    return out.resize((SW * scale, h * scale), Image.NEAREST)


# ---- Openbox theme -----------------------------------------------------------------------------------------
# Buttons: 7x7 pixel art, doubled to the 14x14 px area Openbox draws button bitmaps into (see the base
# image's gen-openbox-theme.py), i.e. drawn on the same 2px "C64 pixel" as the 16px Pet Me 64 title font.
BUTTONS = {
    "close": ["##...##", ".##.##.", "..###..", "..###..", ".##.##.", "##...##", "......."],
    "iconify": [".......", ".......", ".......", ".......", ".......", "#######", "#######"],
    "max": ["#######", "#######", "#.....#", "#.....#", "#.....#", "#.....#", "#######"],
    "max_toggled": ["..#####", "..#####", "#####.#", "#...#.#", "#...###", "#...#..", "#####.."],
    # the "all desktops" button = PIN (rc.xml: on top + every desktop): an empty pin, a filled one while pinned
    "desk": ["..###..", "..#.#..", "..#.#..", ".#####.", "...#...", "...#...", "...#..."],
    "desk_toggled": ["..###..", "..###..", "..###..", ".#####.", "...#...", "...#...", "...#..."],
}


def xbm(name, rows, scale):
    rows = ["".join(c * 2 * scale for c in r) for r in rows for _ in range(2 * scale)]
    w, h = len(rows[0]), len(rows)
    data = []
    for line in rows:
        for i in range(0, w, 8):
            chunk = line[i:i + 8].ljust(8, ".")
            data.append(sum(1 << b for b, c in enumerate(chunk) if c == "#"))
    body = ", ".join("0x%02x" % b for b in data)
    return f"#define {name}_width {w}\n#define {name}_height {h}\nstatic unsigned char {name}_bits[] = {{\n {body} }};\n"


def themerc(n):
    b = 2 * n      # one "C64 pixel" at this scale
    ACT = CYAN
    return f"""! c64-{n}x: Commodore 64 Openbox theme (generated by tools/gen-assets.py)
! active window = cyan frame + title with blue text (light blue would melt into the border); inactive = grey

border.width: {b}
padding.width: {b}
padding.height: {b}
window.client.padding.width: {b}
window.client.padding.height: {b}
window.handle.width: 0
menu.overlap.x: 0
menu.overlap.y: 0

window.active.border.color: {ACT}
window.inactive.border.color: {GREY}
window.active.client.color: {ACT}
window.inactive.client.color: {GREY}

window.active.title.bg: flat solid
window.active.title.bg.color: {ACT}
window.active.title.separator.color: {ACT}
window.inactive.title.bg: flat solid
window.inactive.title.bg.color: {GREY}
window.inactive.title.separator.color: {GREY}

window.active.label.bg: parentrelative
window.active.label.text.color: {BLUE}
window.active.label.text.justify: left
window.inactive.label.bg: parentrelative
window.inactive.label.text.color: {DGREY}
window.inactive.label.text.justify: left

window.active.handle.bg: flat solid
window.active.handle.bg.color: {ACT}
window.inactive.handle.bg: flat solid
window.inactive.handle.bg.color: {GREY}
window.active.grip.bg: flat solid
window.active.grip.bg.color: {ACT}
window.inactive.grip.bg: flat solid
window.inactive.grip.bg.color: {GREY}

window.active.button.unpressed.bg: parentrelative
window.active.button.unpressed.image.color: {BLUE}
window.active.button.hover.bg: flat solid
window.active.button.hover.bg.color: {BLUE}
window.active.button.hover.image.color: {ACT}
window.active.button.pressed.bg: flat solid
window.active.button.pressed.bg.color: {WHITE}
window.active.button.pressed.image.color: {BLUE}
window.active.button.toggled.unpressed.bg: parentrelative
window.active.button.toggled.unpressed.image.color: {BLUE}
window.active.button.toggled.hover.bg: flat solid
window.active.button.toggled.hover.bg.color: {BLUE}
window.active.button.toggled.hover.image.color: {ACT}
window.active.button.toggled.pressed.bg: flat solid
window.active.button.toggled.pressed.bg.color: {WHITE}
window.active.button.toggled.pressed.image.color: {BLUE}
window.active.button.disabled.bg: parentrelative
window.active.button.disabled.image.color: {GREY}

window.inactive.button.unpressed.bg: parentrelative
window.inactive.button.unpressed.image.color: {DGREY}
window.inactive.button.hover.bg: flat solid
window.inactive.button.hover.bg.color: {DGREY}
window.inactive.button.hover.image.color: {GREY}
window.inactive.button.pressed.bg: flat solid
window.inactive.button.pressed.bg.color: {WHITE}
window.inactive.button.pressed.image.color: {DGREY}
window.inactive.button.toggled.unpressed.bg: parentrelative
window.inactive.button.toggled.unpressed.image.color: {DGREY}
window.inactive.button.toggled.hover.bg: flat solid
window.inactive.button.toggled.hover.bg.color: {DGREY}
window.inactive.button.toggled.hover.image.color: {GREY}
window.inactive.button.toggled.pressed.bg: flat solid
window.inactive.button.toggled.pressed.bg.color: {WHITE}
window.inactive.button.toggled.pressed.image.color: {DGREY}
window.inactive.button.disabled.bg: parentrelative
window.inactive.button.disabled.image.color: {LGREY}

menu.border.width: {b}
menu.border.color: {LBLUE}
menu.separator.color: {LBLUE}
menu.separator.width: {b}
menu.separator.padding.width: 0
menu.separator.padding.height: {b}
menu.title.bg: flat solid
menu.title.bg.color: {LBLUE}
menu.title.text.color: {BLUE}
menu.title.text.justify: left
menu.items.bg: flat solid
menu.items.bg.color: {BLUE}
menu.items.text.color: {LBLUE}
menu.items.disabled.text.color: {GREY}
menu.items.active.bg: flat solid
menu.items.active.bg.color: {LBLUE}
menu.items.active.text.color: {BLUE}
menu.items.active.disabled.text.color: {GREY}

osd.border.width: {b}
osd.border.color: {LBLUE}
osd.bg: flat solid
osd.bg.color: {BLUE}
osd.label.bg: parentrelative
osd.label.text.color: {LBLUE}
osd.hilight.bg: flat solid
osd.hilight.bg.color: {LBLUE}
osd.unhilight.bg: flat solid
osd.unhilight.bg.color: {GREY}
"""


def main():
    sprites = {name: f() for name, f in SPRITES.items()}

    d = os.path.join(HOME, "c64", "sprites")
    os.makedirs(d, exist_ok=True)
    for name, im in sprites.items():
        to_rgba(im, 1).save(os.path.join(d, name + ".png"))
        txt = os.path.join(d, name + ".txt")
        if os.path.exists(txt):
            os.remove(txt)

    base = os.path.join(HOME, "icons", "c64")
    dirs = []
    for size, scale in ((24, 1), (48, 2), (96, 4)):
        for ctx, icons in ICON_THEME.items():
            sub = f"{size}x{size}/{ctx}"
            dirs.append((sub, size, ctx))
            os.makedirs(os.path.join(base, sub), exist_ok=True)
            for icon, sprite in icons.items():
                to_rgba(sprites[sprite], scale, square=True).save(os.path.join(base, sub, icon + ".png"))
    with open(os.path.join(base, "sprites.map"), "w") as f:
        f.write("# THEME-ICON SPRITE: the icon theme's pictures are these sprites (data/c64/sprites/SPRITE.png), at 24,\n"
                "# 48 and 96 pixels. c64desk renews them when a sprite is saved in the icon editor (c64desk --icons: all).\n")
        for ctx, icons in ICON_THEME.items():
            for icon, sprite in icons.items():
                f.write(f"{ctx}/{icon} {sprite}\n")
    ctxname = {"places": "Places", "mimetypes": "MimeTypes", "apps": "Applications"}
    with open(os.path.join(base, "index.theme"), "w") as f:
        f.write("[Icon Theme]\nName=c64\nComment=8-colour C64 sprites (tools/gen-assets.py)\nInherits=Adwaita,hicolor\n")
        f.write("Directories=" + ",".join(s for s, _, _ in dirs) + "\n")
        for sub, size, ctx in dirs:
            f.write(f"\n[{sub}]\nSize={size}\nMinSize={size // 2}\nMaxSize={size * 2}\nContext={ctxname[ctx]}\nType=Scalable\n")

    for n in (1, 2, 3):
        d = os.path.join(HOME, "themes", f"c64-{n}x", "openbox-3")
        os.makedirs(d, exist_ok=True)
        rc = themerc(n)
        open(os.path.join(d, "themerc"), "w").write(rc)
        # the template c64-colors renders when the desktop colours change (presets, POKE 53280/53281/646):
        # active frame/title = @TITLE@, screen = @SCREEN@, text = @TEXT@
        tpl = rc.replace(CYAN, "@TITLE@").replace(BLUE, "@SCREEN@").replace(LBLUE, "@TEXT@")
        open(os.path.join(d, "themerc.in"), "w").write(tpl)
        for name, rows in BUTTONS.items():
            open(os.path.join(d, f"{name}.xbm"), "w").write(xbm(name, rows, n))

    # preview sheet: sprites on the blue screen, 4x
    os.makedirs(WORK, exist_ok=True)
    cols = 8
    sheet = Image.new("RGB", (cols * 26 * 4, ((len(sprites) + cols - 1) // cols) * 23 * 4 + 40), BLUE)
    for i, im in enumerate(sprites.values()):
        x, y = (i % cols) * 26 * 4 + 4, (i // cols) * 23 * 4 + 4
        sheet.paste(to_rgba(im, 4), (x, y), to_rgba(im, 4))
    bx = 4
    for name, rows in BUTTONS.items():
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == "#":
                    ImageDraw.Draw(sheet).rectangle((bx + x * 4, sheet.height - 34 + y * 4, bx + x * 4 + 3, sheet.height - 31 + y * 4), fill=LBLUE)
        bx += 40
    sheet.save(os.path.join(WORK, "c64pine-preview.png"))
    print("sprites:", ", ".join(sprites), "-> preview", os.path.join(WORK, "c64pine-preview.png"))


if __name__ == "__main__":
    main()
