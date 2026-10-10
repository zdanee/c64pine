/*
 * c64desk - a Commodore 64 style desktop for X11
 *
 *   main.c      X setup, desktop window, drawing, CRT filter
 *   items.c     desktop icons (~/Desktop), layout, drag & drop, context menus
 *   menus.c     the desktop menu (config/c64/menus)
 *   ui.c        popup menus, input box, sprite chooser
 *   console.c   the text screen + command line (built-ins -> shell -> BASIC)
 *   vice.c      BASIC through a hidden VICE, the C64 SCREEN window
 *   editor.c    sprite/icon editor
 *   wallpaper.c pictures converted to the 16 C64 colours
 *   library.c   cover view for folders with C64 software
 *   popup.c     bar popups (clock, volume, power, notes)
 *   tile.c      super+arrow tiling
 *   basic.c     BASIC listing <-> tokenized program
 *   png.c       tiny PNG writer
 */
#ifndef C64DESK_H
#define C64DESK_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <stdint.h>
#include <stdio.h>

#define SPW 24
#define SPH 21
#define NPENS 12
#define SLOT_W 10            // icon slot in char cells
#define SLOT_H 4             // 3 for the sprite, 1 for the label
#define MAXITEMS 256

extern const unsigned long PAL[16];
extern const unsigned long PEN_RGB[NPENS];
extern const char SPRITE_PENS[];


/* main.c */
extern Display *dpy;
extern int scr, depth;
extern Visual *vis;
extern Colormap cmap;
extern Window root, desk;
extern GC gc;
extern Pixmap canvas;
extern XftDraw *xd;
extern XftFont *font;
extern int W, H, P, C, bx, by, cols, rows;
extern int border_c, screen_c, text_c;
extern char home[512], a64[512];
extern int is_editor;
extern int is_folder;                         // running as --folder DIR
extern char folder_dir[700];
extern int scroll_y;                          // in cells, folder windows only

long long now_ms(void);
void spawn(const char *cmd);
void child_signals(void);                     // call in forked children!
void path(char *out, size_t n, const char *rel);
void read_scale(void);
void read_colors(void);
void open_font(void);
void fill(int c, int x, int y, int w, int h);
void fill_rgb(unsigned long rgb, int x, int y, int w, int h);
void draw_text32(Drawable d, XftDraw *x, int px, int py, const uint32_t *s, int n, unsigned long rgb, long bg);
void damage(int x, int y, int w, int h);
void damage_all(void);
void save_colors(void);
void desk_reset_colors(void);
void desk_recolor(void);
void request_rebuild(void);
XftColor xftrgb(unsigned long rgb);


/* items.c */
typedef struct {
	unsigned char px[SPH][SPW];      // 0 = transparent, else pen number
} Sprite;

int sprite_load(const char *name, Sprite *s);
int sprite_save(const char *name, const Sprite *s);
int sprite_list(char names[][64], int max);
int sprite_theme(const char *name);
int png_write(const char *path, int w, int h, const unsigned char *rgba);
void sprite_draw(Drawable d, GC g, const Sprite *s, int x, int y, int scale);

void items_init(void);
void items_scan(void);
void items_layout_screen(void);
void items_draw(int x, int y, int w, int h);
int items_button(XButtonEvent *e);
int items_motion(XMotionEvent *e);
int items_release(XButtonEvent *e);
int items_clientmessage(XClientMessageEvent *e);
int items_selection(XSelectionEvent *e);
int items_inotify_fd(void);
void items_inotify(void);
int items_open_label(const char *label);      // LOAD"LABEL", 0 if not found
void items_desktop_menu(int x, int y);
int items_count(void);
void items_open_parent(void);
int items_bottom(void);
void items_save_layout(void);
void lay_file_of(const char *dir, char *f, size_t n);
extern int win_geom[4];                       // folder window x y w h from its layout file
extern char lay_view[16];
int is_c64_name(const char *name);


/* ui.c */
typedef void (*menu_cb)(int choice, void *arg);
typedef void (*input_cb)(const char *text, void *arg);  // NULL text = cancelled
typedef void (*sprite_cb)(const char *name, void *arg);

void ui_menu(int x, int y, const char *title, const char **labels, int n, menu_cb cb, void *arg);

// menu trees. label "-" is a separator
#define UI_MAXITEMS 48
typedef struct UiMenu UiMenu;
typedef struct { char label[64]; int id; UiMenu *sub; } UiItem;
struct UiMenu { char title[48]; UiItem it[UI_MAXITEMS]; int n; };
void ui_menu_tree(int x, int y, UiMenu *m, menu_cb cb, void *arg);
void menus_desktop(int x, int y);
void items_paste(void);
void con_enter(void);
void con_paste(const char *text);
void items_builtin(const char *name, int x, int y);
int in_path(const char *prog);
void ui_input(const char *prompt, const char *initial, input_cb cb, void *arg);
void ui_sprites(int x, int y, sprite_cb cb, void *arg);
int ui_active(void);
int ui_event(XEvent *e);      // 1 = eaten
void ui_tick(void);


/* console.c */
void con_init(void);
void con_layout(void);
void con_draw(int x, int y, int w, int h);
void con_blink(void);
void con_key(XKeyEvent *e);
void con_print(const char *utf8);
void con_put_petscii(unsigned char c);         // BASIC output
void con_banner(void);
void con_clear(void);
int con_job_fd(void);
void con_job_input(void);
void con_tick(void);
void con_ready(void);
void con_load(const char *label, const char *cmd);     // fake LOAD messages, then run cmd


/* vice.c */
int vice_running(void);
void vice_line(const char *line);
void vice_key(unsigned char petscii);
int vice_wants_keys(void);                     // program running and reading the keyboard
int vice_busy(void);
void vice_stop_key(void);
void vice_reset(void);
void vice_tick(void);
long long vice_next_ms(void);
void vice_screen_toggle(int show);
void vice_fullscreen(void);
void vice_settings(void);
void vice_run_file(const char *file);
void vice_raise_icon(void);
int vice_owns(Window w);
int vice_event(XEvent *e);
void vice_quit(void);
void vice_kill_all(void);
void vice_prepare(void);
void vice_forget(void);
void vice_load_sav(void);
void vice_load_listing(const char *file);      // NULL = C64.SAV


/* wallpaper.c */
void wall_load(void);
void c64ize(const unsigned char *img, int w, int h, int cx, int cy, int cw, int ch, unsigned char *out, int ow, int oh,
            int dither);
int wall_active(void);
int wall_border(void);                         // -1 = no preference
void wall_draw(int x, int y, int w, int h);
void wall_next(void);
void wall_tick(void);
long long wall_next_ms(void);


/* library.c */
void lib_open(void);
void lib_reload(void);
void lib_layout(void);
int lib_active(void);
void lib_set_active(int on);
int lib_count_c64(const char *dir);
void lib_draw(int x, int y, int w, int h);
void lib_go(int d);
int lib_key(KeySym k);
void lib_click(XButtonEvent *e);
void lib_tick(void);

/* popup.c, tile.c, editor.c: separate modes of the same binary */
int popup_main(int argc, char **argv);
int tile_main(const char *dir);

/* basic.c */
int basic_detok(const unsigned char *prog, int len, unsigned start, FILE *f);
int basic_crunch(const char *file, unsigned char *prog, int max, unsigned start);

int editor_main(const char *name);

#endif
