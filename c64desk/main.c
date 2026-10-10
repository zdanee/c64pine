/* main.c - setup, desktop window, drawing, CRT filter, event loop */
#include "c64desk.h"
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

const unsigned long PAL[16] = {
	0x000000, 0xffffff, 0x813338, 0x75cec8, 0x8e3c97, 0x56ac4d, 0x2e2c9b, 0xedf171,
	0x8e5029, 0x553800, 0xc46c71, 0x4a4a4a, 0x7b7b7b, 0xa9ff9f, 0x706deb, 0xb2b2b2 };
// sprite pens: 0 = transparent, 1-8 = C64 colours 0-7, 9 brown, 10/11 beige, dark beige
const char SPRITE_PENS[] = ".kwrcpgbynzx";
const unsigned long PEN_RGB[NPENS] = { 0, 0x000000, 0xffffff, 0x813338, 0x75cec8, 0x8e3c97, 0x56ac4d, 0x2e2c9b,
	0xedf171, 0x553800, 0xc9b68e, 0x9a8660 };

// CRT look. tweak to taste
static const double SCAN = 0.30;
static const double VIG = 0.45;
static const double CORNER = 0.035; /* of screen height */
#define BLINK_MS 330

Display *dpy;
int scr, depth;
Visual *vis;
Colormap cmap;
Window root, desk;
GC gc;
Pixmap canvas;
XftDraw *xd;
XftFont *font;
int W, H, P = 2, C = 16, bx, by, cols, rows;
int border_c = 14, screen_c = 6, text_c = 14;
char home[512], a64[512];        // $HOME, $C64PINE
int is_editor;
int is_folder;
char folder_dir[700];
int scroll_y;
static Atom A_DELETE, A_GRID;

static Pixmap picture;              // canvas + baked-in filter
static unsigned char *keep;         // brightness per pixel 0..256
static Window corners[4], overlay, bar_win;
static int bar_y = -1, composited;
static Atom A_CM, A_TYPE, A_TYPE_DESK, A_STRUT, A_STRUTP, A_DESKTOP, A_PID;
static Atom A_CLIENTS, A_EXTENTS, A_STATE, A_FULLSCREEN, A_HIDDEN, A_CURDESK, A_TYPE_DOCK, A_MOVERESIZE;
static volatile sig_atomic_t reload, next_wall, load_req;
static int user_border = 14;          // from config/c64/colors, the wallpaper may override it
static int dmg, dx0, dy0, dx1, dy1;
static long long last_paint, fix_due, refresh_at[2];




long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", a64, rel); }

/*
 * IMPORTANT: call this in every child.
 * Ignored signals survive exec(). We ignore SIGCHLD/SIGPIPE, and when
 * started from .xinitrc SIGINT/SIGQUIT come in ignored too. Without the
 * reset Xvfb can't wait for xkbcomp and Ctrl+C doesn't stop anything.
 * (took a while to find this one)
 */
void child_signals(void)
{
	static const int sigs[] = { SIGCHLD, SIGPIPE, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU };
	sigset_t none;
	for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
		signal(sigs[i], SIG_DFL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

void spawn(const char *cmd)
{
	if (fork() == 0) {
		child_signals();
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
}

void read_scale(void)
{
	char f[600], line[128];
	int pct = 100;
	path(f, sizeof f, "config/scale.env");
	FILE *fp = fopen(f, "r");
	if (fp) {
		while (fgets(line, sizeof line, fp))
			if (sscanf(line, "SCALE_PCT=%d", &pct) == 1)
				break;
		fclose(fp);
	}
	P = (2 * pct + 50) / 100;
	if (P < 1) P = 1;
	C = 8 * P;
}

void read_colors(void)
{
	char f[600];
	path(f, sizeof f, "config/c64/colors");
	FILE *fp = fopen(f, "r");
	if (fp) {
		int b, s, t;
		if (fscanf(fp, "%d %d %d", &b, &s, &t) == 3 && b >= 0 && b < 16 && s >= 0 && s < 16 && t >= 0 && t < 16)
			border_c = user_border = b, screen_c = s, text_c = t;
		fclose(fp);
	}
}


static void wall_apply_border(void)
{
	int b = wall_border();
	border_c = b >= 0 ? b : user_border;
}

void save_colors(void)
{
	char f[600];
	path(f, sizeof f, "config/c64/colors");
	FILE *fp = fopen(f, "w");
	if (fp) {
		int wb = wall_border();
		if (wb < 0 || border_c != wb) user_border = border_c;
		fprintf(fp, "%d %d %d\n", user_border, screen_c, text_c);
		fclose(fp);
	}
}

void request_rebuild(void) { reload = 1; }

XftColor xftrgb(unsigned long rgb)
{
	XftColor xc;
	XRenderColor rc = { (rgb >> 16 & 0xff) * 257, (rgb >> 8 & 0xff) * 257, (rgb & 0xff) * 257, 0xffff };
	XftColorAllocValue(dpy, vis, cmap, &rc, &xc);
	return xc;
}

void fill_rgb(unsigned long rgb, int x, int y, int w, int h)
{
	XSetForeground(dpy, gc, rgb);
	XFillRectangle(dpy, canvas, gc, x, y, w, h);
}

void fill(int c, int x, int y, int w, int h) { fill_rgb(PAL[c], x, y, w, h); }

// px,py = top left of first cell. bg < 0: transparent
void draw_text32(Drawable d, XftDraw *x, int px, int py, const uint32_t *s, int n, unsigned long rgb, long bg)
{
	if (n <= 0)
		return;
	if (bg >= 0) {
		XSetForeground(dpy, gc, (unsigned long)bg);
		XFillRectangle(dpy, d, gc, px, py, n * C, C);
	}
	XftColor xc = xftrgb(rgb);
	XftDrawString32(x, &xc, font, px, py + font->ascent, (const FcChar32 *)s, n);
	XftColorFree(dpy, vis, cmap, &xc);
}




/* ---------- damage / repaint / filter ---------- */

void damage(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	if (!dmg) {
		dx0 = x, dy0 = y, dx1 = x + w, dy1 = y + h, dmg = 1;
	} else {
		if (x < dx0) dx0 = x;
		if (y < dy0) dy0 = y;
		if (x + w > dx1) dx1 = x + w;
		if (y + h > dy1) dy1 = y + h;
	}
}

void damage_all(void) { damage(0, 0, W, H); }

static void make_keep(void)
{
	free(keep);
	keep = malloc((size_t)W * H);
	for (int y = 0; y < H; y++) {
		double s = (P > 1 && ((y - by) % P + P) % P == P - 1) ? 1 - SCAN : 1;
		for (int x = 0; x < W; x++) {
			double dx = 2.0 * x / (W - 1) - 1, dy = 2.0 * y / (H - 1) - 1, r = (dx * dx + dy * dy) / 2;
			keep[(size_t)y * W + x] = (unsigned char)(255 * s * (1 - VIG * r * r) + 0.5);
		}
	}
}

static void present(int x, int y, int w, int h)
{
	if (composited || !keep) {
		XCopyArea(dpy, canvas, picture, gc, x, y, w, h, x, y);
	} else {
		XImage *im = XGetImage(dpy, canvas, x, y, w, h, AllPlanes, ZPixmap);
		if (!im)
			return;
		int fast = im->bits_per_pixel == 32;
		for (int j = 0; j < h; j++) {
			const unsigned char *k = keep + (size_t)(y + j) * W + x;
			uint32_t *row = (uint32_t *)(im->data + (size_t)j * im->bytes_per_line);
			for (int i = 0; i < w; i++) {
				unsigned long p = fast ? row[i] : XGetPixel(im, i, j);
				unsigned f = k[i] + 1;
				p = ((p >> 16 & 0xff) * f >> 8) << 16 | ((p >> 8 & 0xff) * f >> 8) << 8 | ((p & 0xff) * f >> 8);
				if (fast) row[i] = (uint32_t)p; else XPutPixel(im, i, j, p);
			}
		}
		XPutImage(dpy, picture, gc, im, 0, 0, x, y, w, h);
		XDestroyImage(im);
	}
	XClearArea(dpy, desk, x, y, w, h, False);
}




/* ---------- folder windows ---------- */

/* header looks like the first line of a C64 directory:
 *    "NAME"   12 FILES  80123456 BLOCKS FREE
 */
static void folder_draw(int x, int y, int w, int h)
{
	fill(screen_c, bx, by, cols * C, rows * C);
	int iy = by + C, ih = (rows - 1) * C;
	int cx0 = x > bx ? x : bx, cy0 = y > iy ? y : iy;
	int cx1 = x + w < bx + cols * C ? x + w : bx + cols * C, cy1 = y + h < iy + ih ? y + h : iy + ih;
	if (cx1 > cx0 && cy1 > cy0) {
		XRectangle r = { cx0, cy0, cx1 - cx0, cy1 - cy0 };
		XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
		XftDrawSetClipRectangles(xd, 0, 0, &r, 1);
		if (lib_active()) lib_draw(cx0, cy0, cx1 - cx0, cy1 - cy0);
		else items_draw(cx0, cy0, cx1 - cx0, cy1 - cy0);
		XRectangle all = { x, y, w, h };
		XSetClipRectangles(dpy, gc, 0, 0, &all, 1, Unsorted);
		XftDrawSetClipRectangles(xd, 0, 0, &all, 1);
	}
	char name[300], right[96], line[512];
	const char *b = strrchr(folder_dir, '/');
	snprintf(name, sizeof name, "\"%s\"", strcmp(folder_dir, home) == 0 ? "HOME" : (b && b[1] ? b + 1 : folder_dir));
	struct statvfs v;
	unsigned long long blocks = statvfs(folder_dir, &v) == 0 ? (unsigned long long)v.f_bavail * v.f_frsize / 254 : 0;
	snprintf(right, sizeof right, "%d %s  %llu BLOCKS FREE", items_count(), items_count() == 1 ? "FILE" : "FILES", blocks);
	if ((int)(strlen(name) + strlen(right)) + 2 > cols)
		snprintf(right, sizeof right, "%d %s", items_count(), items_count() == 1 ? "FILE" : "FILES");
	int pad = cols - (int)strlen(name) - (int)strlen(right);
	if (pad < 1) pad = 1;
	snprintf(line, sizeof line, "%s%*s%s", name, pad, "", right);
	uint32_t u[512];
	int n = 0;
	for (int i = 0; line[i] && n < cols; i++)
		u[n++] = toupper((unsigned char)line[i]);
	draw_text32(canvas, xd, bx, by, u, n, PAL[screen_c], (long)PAL[text_c]);
	if (n < cols) fill(text_c, bx + n * C, by, (cols - n) * C, C);
}

static void set_grid_prop(void)
{
	long g[4] = { bx, by, C, scroll_y };
	XChangeProperty(dpy, desk, A_GRID, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)g, 4);
}

static void folder_scroll(int d)
{
	int max = items_bottom() + 1 - rows;
	int s = scroll_y + d;
	if (s > max) s = max;
	if (s < 0) s = 0;
	if (s != scroll_y) {
		scroll_y = s;
		set_grid_prop();
		damage_all();
	}
}

static void repaint(void)
{
	if (!dmg)
		return;
	int x = dx0 < 0 ? 0 : dx0, y = dy0 < 0 ? 0 : dy0;
	int w = (dx1 > W ? W : dx1) - x, h = (dy1 > H ? H : dy1) - y;
	dmg = 0;
	if (w <= 0 || h <= 0)
		return;
	XRectangle r = { x, y, w, h };
	XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
	XftDrawSetClipRectangles(xd, 0, 0, &r, 1);
	fill(border_c, x, y, w, h);
	if (is_folder) {
		folder_draw(x, y, w, h);
	} else {
		if (wall_active()) wall_draw(x, y, w, h);
		else fill(screen_c, bx, by, cols * C, rows * C);
		con_draw(x, y, w, h);
		items_draw(x, y, w, h);
	}
	XSetClipMask(dpy, gc, None);
	XftDrawSetClip(xd, NULL);
	present(x, y, w, h);
	last_paint = now_ms();
}




/* ---------- corners, overlay, struts ---------- */

static int in_corner(int x, int y, int r)
{
	double ddx = r - x - 0.5, ddy = r - y - 0.5;
	return x < r && y < r && ddx * ddx + ddy * ddy > (double)r * r;
}

static void make_corners(void)
{
	int r = (int)(H * CORNER + 0.5);
	if (r < 2) r = 2;
	XSetWindowAttributes a = { .override_redirect = True, .background_pixel = 0 };
	for (int k = 0; k < 4; k++) {
		int x = (k & 1) ? W - r : 0, y = (k & 2) ? H - r : 0;
		if (!corners[k])
			corners[k] = XCreateWindow(dpy, root, x, y, r, r, 0, CopyFromParent, InputOutput, CopyFromParent,
			                           CWOverrideRedirect | CWBackPixel, &a);
		else
			XMoveResizeWindow(dpy, corners[k], x, y, r, r);
		Pixmap m = XCreatePixmap(dpy, corners[k], r, r, 1);
		GC g = XCreateGC(dpy, m, 0, NULL);
		XSetForeground(dpy, g, 0);
		XFillRectangle(dpy, m, g, 0, 0, r, r);
		XSetForeground(dpy, g, 1);
		for (int y = 0; y < r; y++)
			for (int x2 = 0; x2 < r; x2++) {
				int lx = (k & 1) ? r - 1 - x2 : x2, ly = (k & 2) ? r - 1 - y : y;
				if (in_corner(lx, ly, r))
					XDrawPoint(dpy, m, g, x2, y);
			}
		XShapeCombineMask(dpy, corners[k], ShapeBounding, 0, 0, m, ShapeSet);
		XShapeCombineRectangles(dpy, corners[k], ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);   // click-through
		XFreeGC(dpy, g);
		XFreePixmap(dpy, m);
		XMapRaised(dpy, corners[k]);
	}
}

static void make_overlay(void)
{
	if (overlay) {
		XDestroyWindow(dpy, overlay);
		overlay = 0;
	}
	if (!composited)
		return;
	XVisualInfo vi;
	if (!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vi))
		return;
	Colormap cm = XCreateColormap(dpy, root, vi.visual, AllocNone);
	XSetWindowAttributes a = { .override_redirect = True, .colormap = cm, .border_pixel = 0, .background_pixel = 0 };
	overlay = XCreateWindow(dpy, root, 0, 0, W, H, 0, 32, InputOutput, vi.visual,
	                        CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel, &a);
	XShapeCombineRectangles(dpy, overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	XClassHint ch = { "c64crt", "C64crt" };
	XSetClassHint(dpy, overlay, &ch);

	Pixmap pm = XCreatePixmap(dpy, overlay, W, H, 32);
	GC g = XCreateGC(dpy, pm, 0, NULL);
	XImage *im = XCreateImage(dpy, vi.visual, 32, ZPixmap, 0, NULL, W, H, 32, 0);
	im->data = malloc((size_t)im->bytes_per_line * H);
	int r = (int)(H * CORNER + 0.5);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double k = keep ? keep[(size_t)y * W + x] / 255.0 : 1;
			int cx = x < r ? x : (x >= W - r ? W - 1 - x : -1), cy = y < r ? y : (y >= H - r ? H - 1 - y : -1);
			if (cx >= 0 && cy >= 0) {      // AA corners. shaped windows are the fallback w/o compositor
				double ddx = r - cx - 0.5, ddy = r - cy - 0.5, d = sqrt(ddx * ddx + ddy * ddy) - r;
				if (d > -1) k *= d >= 0 ? 0 : -d;
			}
			unsigned long al = (unsigned long)((1 - k) * 255 + 0.5);
			XPutPixel(im, x, y, al << 24);            // premultiplied
		}
	}
	XPutImage(dpy, pm, g, im, 0, 0, 0, 0, W, H);
	XDestroyImage(im);
	XFreeGC(dpy, g);
	XSetWindowBackgroundPixmap(dpy, overlay, pm);
	XFreePixmap(dpy, pm);
	XMapRaised(dpy, overlay);
}

static void raise_top(void)
{
	if (overlay)
		XRaiseWindow(dpy, overlay);
	for (int k = 0; k < 4; k++)
		if (corners[k])
			XRaiseWindow(dpy, corners[k]);
	vice_raise_icon();
}

static int ours(Window w)
{
	if (w == desk || (w && w == overlay) || vice_owns(w))
		return 1;
	for (int k = 0; k < 4; k++)
		if (w == corners[k])
			return 1;
	return 0;
}

static void set_struts(void)
{
	long s[12] = { bx, bx, by, by, 0, H - 1, 0, H - 1, 0, W - 1, 0, W - 1 };
	XChangeProperty(dpy, desk, A_STRUTP, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)s, 12);
	XChangeProperty(dpy, desk, A_STRUT, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)s, 4);
}

static void run_hook(void)
{
	char f[600], cmd[700];
	snprintf(f, sizeof f, "%s/cache", a64);
	mkdir(f, 0755);
	snprintf(f, sizeof f, "%s/cache/c64", a64);
	mkdir(f, 0755);
	path(f, sizeof f, "cache/c64/geom");
	FILE *fp = fopen(f, "w");
	if (fp) {
		fprintf(fp, "W=%d\nH=%d\nBX=%d\nBY=%d\nPX=%d\nBORDER=#%06lx\nSCREEN=#%06lx\nTEXT=#%06lx\n",
		        W, H, bx, by, P, PAL[border_c], PAL[screen_c], PAL[text_c]);
		fclose(fp);
	}
	path(f, sizeof f, "bin/c64-hook");             // recolours bar, gtk, terminals...
	snprintf(cmd, sizeof cmd, "'%s'", f);
	if (access(f, X_OK) == 0)
		spawn(cmd);
}

void open_font(void)
{
	char pat[96];
	if (font)
		XftFontClose(dpy, font);
	snprintf(pat, sizeof pat, "Pet Me 64:pixelsize=%d:antialias=false:hinting=false", C);
	font = XftFontOpenName(dpy, scr, pat);
	if (!font) {
		fprintf(stderr, "c64desk: font %s not found\n", pat);
		exit(1);
	}
}

static void layout(void)
{
	cols = (W - 2 * (W * 32 / 384)) / C;          // same border ratio as the real thing (32/384, 36/272)
	rows = (H - 2 * (H * 36 / 272)) / C;
	if (cols < 20) cols = W / C;
	if (rows < 10) rows = H / C;
	bx = (W - cols * C) / 2;
	by = (H - rows * C) / 2;
}


// screen size / scale / files changed
static void rebuild(int hook)
{
	if (is_folder) {
		XWindowAttributes wa;
		XGetWindowAttributes(dpy, desk, &wa);
		W = wa.width, H = wa.height;
	} else {
		W = DisplayWidth(dpy, scr);
		H = DisplayHeight(dpy, scr);
	}
	read_scale();
	read_colors();
	open_font();
	if (is_folder) {
		cols = (W - C) / C, rows = (H - C) / C;
		if (cols < 1) cols = 1;
		if (rows < 1) rows = 1;
		bx = (W - cols * C) / 2, by = C / 2;
		if (lib_active()) lib_layout();
	} else {
		layout();
		wall_load();
		wall_apply_border();
		XMoveResizeWindow(dpy, desk, 0, 0, W, H);
	}
	if (canvas) {
		XftDrawDestroy(xd);
		XFreePixmap(dpy, canvas);
		XFreePixmap(dpy, picture);
	}
	canvas = XCreatePixmap(dpy, desk, W, H, depth);
	picture = XCreatePixmap(dpy, desk, W, H, depth);
	xd = XftDrawCreate(dpy, canvas, vis, cmap);
	if (!is_folder) {
		make_keep();
		con_layout();
	}
	items_scan();
	items_layout_screen();
	if (is_folder) folder_scroll(0);
	set_grid_prop();
	damage_all();
	repaint();
	XSetWindowBackgroundPixmap(dpy, desk, picture);
	XClearWindow(dpy, desk);
	if (is_folder)
		return;
	set_struts();
	make_corners();
	make_overlay();
	raise_top();
	bar_win = 0;
	bar_y = -1;
	if (hook)
		run_hook();
}


void desk_recolor(void)
{
	wall_apply_border();
	damage_all();
	run_hook();
}

void desk_reset_colors(void)
{
	save_colors();
	damage_all();
	run_hook();
}




/* ---------- the bar ---------- */

#define MAXTRACK 128
typedef struct { Window w; int top, bottom, resizing; } Track;
static Track track[MAXTRACK];
static int ntrack;

static int bar_h(void) { return 10 * P; }                     // keep in sync with c64-bar!
static int bar_y_normal(void) { return (by - bar_h()) / 2; }
static int bar_y_high(void) { return 6 * P; }
static int reserve(void) { return bar_y_high() + bar_h() + 2 * P; }

static long *prop32(Window w, Atom a, Atom type, unsigned long *n)
{
	Atom at;
	int fmt;
	unsigned long after;
	unsigned char *d = NULL;
	*n = 0;
	if (XGetWindowProperty(dpy, w, a, 0, 1024, False, type, &at, &fmt, n, &after, &d) != Success || !d || fmt != 32) {
		if (d) XFree(d);
		*n = 0;
		return NULL;
	}
	return (long *)d;
}

static int has_atom(Window w, Atom prop, Atom value)
{
	unsigned long n;
	long *v = prop32(w, prop, XA_ATOM, &n);
	int r = 0;
	for (unsigned long i = 0; i < n; i++)
		if ((Atom)v[i] == value) r = 1;
	if (v) XFree(v);
	return r;
}

static int is_class(Window w, const char *cls)
{
	XClassHint ch;
	int r = 0;
	if (XGetClassHint(dpy, w, &ch)) {
		r = ch.res_class && strcmp(ch.res_class, cls) == 0;
		if (ch.res_name) XFree(ch.res_name);
		if (ch.res_class) XFree(ch.res_class);
	}
	return r;
}

static Window find_bar(void)
{
	Window r, par, *kids = NULL, found = 0;
	unsigned int nk;
	if (!XQueryTree(dpy, root, &r, &par, &kids, &nk))
		return 0;
	for (unsigned int i = 0; i < nk && !found; i++) {
		if (is_class(kids[i], "Bar")) { found = kids[i]; break; }
		Window *gk = NULL;
		unsigned int ng;
		if (XQueryTree(dpy, kids[i], &r, &par, &gk, &ng)) {
			for (unsigned int j = 0; j < ng && !found; j++)
				if (is_class(gk[j], "Bar")) found = gk[j];
			if (gk) XFree(gk);
		}
	}
	if (kids) XFree(kids);
	return found;
}

static Track *tracked(Window w)
{
	for (int i = 0; i < ntrack; i++)
		if (track[i].w == w) return &track[i];
	if (ntrack == MAXTRACK) ntrack = 0;
	track[ntrack] = (Track){ w, -1, -1, 0 };
	return &track[ntrack++];
}

static int buttons_down(void)
{
	Window r, c;
	int rx, ry, wx, wy;
	unsigned int mask = 0;
	XQueryPointer(dpy, root, &r, &c, &rx, &ry, &wx, &wy, &mask);
	return mask & (Button1Mask | Button2Mask | Button3Mask);
}

static void moveresize(Window w, int y, int height)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = A_MOVERESIZE;
	e.xclient.format = 32;
	e.xclient.data.l[0] = NorthWestGravity | 1 << 9 | (height > 0 ? 1 << 11 : 0) | 2 << 12;
	e.xclient.data.l[2] = y;
	e.xclient.data.l[4] = height;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

/* Windows that reach into the bar's row push the bar up into the border.
 * Nothing may stay in the strip above it. */
static void check_windows(void)
{
	if (bar_y_normal() <= bar_y_high())
		return;
	unsigned long n, nd;
	long *cl = prop32(root, A_CLIENTS, XA_WINDOW, &n);
	long *cd = prop32(root, A_CURDESK, XA_CARDINAL, &nd);
	long cur = nd ? cd[0] : 0;
	int overlap = 0, held = buttons_down(), later = 0;
	for (unsigned long i = 0; i < n; i++) {
		Window w = cl[i];
		XWindowAttributes wa;
		if (w == desk || !XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable || wa.override_redirect)
			continue;
		if (has_atom(w, A_TYPE, A_TYPE_DOCK) || has_atom(w, A_TYPE, A_TYPE_DESK) || is_class(w, "Bar"))
			continue;
		if (has_atom(w, A_STATE, A_FULLSCREEN) || has_atom(w, A_STATE, A_HIDDEN))
			continue;
		unsigned long k;
		long *dk = prop32(w, A_DESKTOP, XA_CARDINAL, &k);
		long d = k ? dk[0] : cur;
		if (dk) XFree(dk);
		if (d != cur && d != 0xFFFFFFFFL)
			continue;
		long *ext = prop32(w, A_EXTENTS, XA_CARDINAL, &k);
		long et = k == 4 ? ext[2] : 0, eb = k == 4 ? ext[3] : 0;
		if (ext) XFree(ext);
		int cx, cy;
		Window ch;
		XTranslateCoordinates(dpy, w, root, 0, 0, &cx, &cy, &ch);
		int top = cy - et, bottom = cy + wa.height + eb;
		if (wa.width >= W && wa.height >= H)
			continue;
		Track *t = tracked(w);
		if (t->top >= 0 && top != t->top)
			t->resizing = bottom == t->bottom;
		t->top = top;
		t->bottom = bottom;
		if (top < bar_y_normal() + bar_h())
			overlap = 1;
		if (top < reserve()) {
			if (held) {
				later = 1;
			} else {
				int nh = wa.height - (reserve() - top);
				moveresize(w, reserve(), t->resizing && nh >= 4 * C ? nh : 0);
				t->top = -1;
			}
		}
	}
	if (cl) XFree(cl);
	if (cd) XFree(cd);
	fix_due = later ? now_ms() + 150 : 0;

	XWindowAttributes ba;
	if (!bar_win || !XGetWindowAttributes(dpy, bar_win, &ba))
		bar_win = find_bar(), bar_y = -1;
	if (bar_win) {
		int y = overlap ? bar_y_high() : bar_y_normal();
		if (y != bar_y) {
			XMoveWindow(dpy, bar_win, bx, y);
			bar_y = y;
		}
	}
}




/* ---------- main ---------- */

static void check_compositor(void)
{
	int c = XGetSelectionOwner(dpy, A_CM) != None;
	if (c != composited) {
		composited = c;
		damage_all();
		repaint();
		make_overlay();
		raise_top();
		if (c) {       // picom sometimes misses the first frame at login, repaint once more later
			refresh_at[0] = now_ms() + 1000;
			refresh_at[1] = now_ms() + 3000;
		}
	}
}

static void on_hup(int sig) { (void)sig; reload = 1; }
static void on_usr1(int sig) { (void)sig; next_wall = 1; }
static void on_usr2(int sig) { (void)sig; load_req = 1; }

/* Talking to the running desktop: write cache/c64/load-request, send SIGUSR2.
 *   c64desk --load FILE.sav
 *   c64desk --c64 show|hide|fullscreen|settings|run FILE|menu
 */
static int send_request(const char *line)
{
	char f[700], pidf[700];
	path(f, sizeof f, "cache/c64/load-request");
	FILE *fp = fopen(f, "w");
	if (!fp) return 1;
	fprintf(fp, "%s\n", line);
	fclose(fp);
	int pid = 0;
	path(pidf, sizeof pidf, "cache/c64/pid");
	if ((fp = fopen(pidf, "r"))) { if (fscanf(fp, "%d", &pid) != 1) pid = 0; fclose(fp); }
	if (pid <= 0 || kill(pid, SIGUSR2) != 0) { fprintf(stderr, "c64desk: the desktop is not running\n"); return 1; }
	return 0;
}

static int load_main(const char *file)
{
	char rp[PATH_MAX];
	if (!realpath(file, rp)) { fprintf(stderr, "c64desk: no such file: %s\n", file); return 1; }
	return send_request(rp);
}

static int c64_main(const char *cmd, const char *file)
{
	char line[PATH_MAX + 32], rp[PATH_MAX];
	if (!strcmp(cmd, "run")) {
		if (!file || !realpath(file, rp)) { fprintf(stderr, "c64desk: no such file: %s\n", file ? file : ""); return 1; }
		snprintf(line, sizeof line, "@run %s", rp);
	} else if (!strcmp(cmd, "show") || !strcmp(cmd, "hide") || !strcmp(cmd, "fullscreen") || !strcmp(cmd, "settings") ||
	           !strcmp(cmd, "menu")) {
		snprintf(line, sizeof line, "@%s", cmd);
	} else {
		fprintf(stderr, "c64desk --c64 show | hide | fullscreen | settings | menu | run FILE\n");
		return 2;
	}
	return send_request(line);
}

// other clients' windows can disappear while we look at them (e.g. the bar restarts on resize)
static int x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

/* X is gone or we got TERM: take the hidden VICE + its Xvfb with us,
   nobody else would under a login manager */
static int x_gone(Display *d) { (void)d; vice_kill_all(); _exit(0); }
static void on_term(int s) { (void)s; vice_kill_all(); _exit(143); }   // non-zero so the respawn loop restarts us

static void write_pid(void)
{
	char f[600];
	path(f, sizeof f, "cache/c64");
	mkdir(f, 0755);
	path(f, sizeof f, "cache/c64/pid");
	FILE *fp = fopen(f, "w");
	if (fp) {
		fprintf(fp, "%d\n", (int)getpid());
		fclose(fp);
	}
}


static void folder_close(void)
{
	int x, y;
	Window c;
	XTranslateCoordinates(dpy, desk, root, 0, 0, &x, &y, &c);
	Atom t;
	int f;
	unsigned long n, after;
	unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, desk, XInternAtom(dpy, "_NET_FRAME_EXTENTS", False), 0, 4, False, XA_CARDINAL, &t, &f,
	                       &n, &after, &d) == Success && d && n == 4) {
		long *e = (long *)d;
		x -= e[0], y -= e[2];
	}
	if (d) XFree(d);
	win_geom[0] = x, win_geom[1] = y, win_geom[2] = W, win_geom[3] = H;
	items_save_layout();
	exit(0);
}

// already open? raise that one instead
static int folder_raise_existing(void)
{
	Atom A_CL = XInternAtom(dpy, "_NET_CLIENT_LIST", False), A_DIR = XInternAtom(dpy, "_C64_FOLDER", False);
	Atom t;
	int f, found = 0;
	unsigned long n, after;
	unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, root, A_CL, 0, 4096, False, XA_WINDOW, &t, &f, &n, &after, &d) != Success || !d)
		return 0;
	Window *w = (Window *)d;
	for (unsigned long i = 0; i < n && !found; i++) {
		unsigned char *p = NULL;
		unsigned long pn;
		if (XGetWindowProperty(dpy, w[i], A_DIR, 0, 1024, False, AnyPropertyType, &t, &f, &pn, &after, &p) == Success && p) {
			if (strcmp((char *)p, folder_dir) == 0) {
				XEvent e = { 0 };
				e.xclient.type = ClientMessage;
				e.xclient.window = w[i];
				e.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
				e.xclient.format = 32;
				e.xclient.data.l[0] = 2;
				XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
				XFlush(dpy);
				found = 1;
			}
			XFree(p);
		}
	}
	XFree(d);
	return found;
}

static int folder_main(const char *dir)
{
	char rp[PATH_MAX];
	if (!realpath(dir, rp)) {
		fprintf(stderr, "c64desk: no such folder: %s\n", dir);
		return 1;
	}
	snprintf(folder_dir, sizeof folder_dir, "%s", rp);
	if (folder_raise_existing())
		return 0;
	is_folder = 1;
	signal(SIGHUP, on_hup);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	A_GRID = XInternAtom(dpy, "_C64_GRID", False);
	A_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	read_scale();
	read_colors();

	// last size, or 4x3 icons
	char lf[800], line[512];
	lay_file_of(folder_dir, lf, sizeof lf);
	FILE *fp = fopen(lf, "r");
	char view[16] = "";
	while (fp && fgets(line, sizeof line, fp)) {
		if (strncmp(line, "@window ", 8) == 0)
			sscanf(line + 8, "%d %d %d %d", &win_geom[0], &win_geom[1], &win_geom[2], &win_geom[3]);
		if (strncmp(line, "@view ", 6) == 0)
			sscanf(line + 6, "%15s", view);
	}
	if (fp) fclose(fp);
	int n = 0;
	DIR *dd = opendir(folder_dir);
	struct dirent *de;
	while (dd && (de = readdir(dd)))
		if (de->d_name[0] != '.') n++;
	if (dd) closedir(dd);
	int ic = n < 2 ? 2 : n > 4 ? 4 : n, ir = (n + 3) / 4;
	if (ir < 1) ir = 1;
	if (ir > 3) ir = 3;
	int w = (ic * SLOT_W + 1) * C, h = (1 + ir * (SLOT_H + 1) + 1) * C;
	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	if (strcmp(view, "icons") && lib_count_c64(folder_dir) > 0)  // library needs more room
		w = sw * 3 / 4, h = sh * 4 / 5;
	if (win_geom[2] > 0) w = win_geom[2], h = win_geom[3];
	if (w > sw) w = sw;
	if (h > sh) h = sh;

	XSetWindowAttributes a = { .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	                                         KeyPressMask | StructureNotifyMask, .background_pixel = PAL[border_c] };
	desk = XCreateWindow(dpy, root, win_geom[2] > 0 ? win_geom[0] : 0, win_geom[2] > 0 ? win_geom[1] : 0, w, h, 0,
	                     CopyFromParent, InputOutput, CopyFromParent, CWEventMask | CWBackPixel, &a);
	gc = XCreateGC(dpy, desk, 0, NULL);
	XClassHint ch = { "c64folder", "C64folder" };
	XSetClassHint(dpy, desk, &ch);
	char title[300];
	const char *b = strrchr(folder_dir, '/');
	snprintf(title, sizeof title, "%s", strcmp(folder_dir, home) == 0 ? "HOME" : (b && b[1] ? b + 1 : folder_dir));
	for (char *c = title; *c; c++) *c = toupper((unsigned char)*c);
	XStoreName(dpy, desk, title);
	XChangeProperty(dpy, desk, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False), 8,
	                PropModeReplace, (unsigned char *)title, strlen(title));
	XSetWMProtocols(dpy, desk, &A_DELETE, 1);
	XWMHints wh = { .flags = InputHint, .input = True };
	XSetWMHints(dpy, desk, &wh);
	XSizeHints sz = { .flags = PMinSize | PResizeInc | PBaseSize | (win_geom[2] > 0 ? USPosition | USSize : 0),
	                  .x = win_geom[0], .y = win_geom[1], .width = w, .height = h,
	                  .min_width = (2 * SLOT_W + 1) * C, .min_height = (2 + SLOT_H + 1) * C,
	                  .width_inc = C, .height_inc = C, .base_width = C, .base_height = C };
	XSetWMNormalHints(dpy, desk, &sz);
	long pid = getpid();
	XChangeProperty(dpy, desk, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)&pid, 1);
	items_init();
	XMapWindow(dpy, desk);
	rebuild(0);
	lib_open();

	int fd = ConnectionNumber(dpy);
	for (;;) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ui_event(&ev))
				continue;
			if (ev.type == ConfigureNotify && ev.xconfigure.window == desk) {
				if (ev.xconfigure.width != W || ev.xconfigure.height != H) {
					while (XCheckTypedWindowEvent(dpy, desk, ConfigureNotify, &ev))
						;
					rebuild(0);
				}
			} else if (ev.type == ButtonPress && ev.xbutton.window == desk) {
				if (lib_active()) {
					if (ev.xbutton.button == Button4) lib_go(-1);
					else if (ev.xbutton.button == Button5) lib_go(1);
					else if (ev.xbutton.button == Button3) items_desktop_menu(ev.xbutton.x_root, ev.xbutton.y_root);
					else if (ev.xbutton.button == Button1) lib_click(&ev.xbutton);
				} else if (ev.xbutton.button == Button4) folder_scroll(-2);
				else if (ev.xbutton.button == Button5) folder_scroll(2);
				else items_button(&ev.xbutton);
			} else if (ev.type == MotionNotify && ev.xmotion.window == desk) {
				while (XCheckTypedWindowEvent(dpy, desk, MotionNotify, &ev))
					;
				if (!lib_active()) items_motion(&ev.xmotion);
			} else if (ev.type == ButtonRelease && ev.xbutton.window == desk) {
				if (!lib_active()) items_release(&ev.xbutton);
			} else if (ev.type == KeyPress && ev.xkey.window == desk) {
				KeySym k = XLookupKeysym(&ev.xkey, 0);
				if (lib_active() && lib_key(k))
					;
				else if (k == XK_Escape || ((ev.xkey.state & ControlMask) && k == XK_w))
					folder_close();
				else if (k == XK_BackSpace || ((ev.xkey.state & Mod1Mask) && k == XK_Up))
					items_open_parent();
				else if (k == XK_Page_Up || k == XK_Up) folder_scroll(k == XK_Up ? -1 : -(rows - 2));
				else if (k == XK_Page_Down || k == XK_Down) folder_scroll(k == XK_Down ? 1 : rows - 2);
			} else if (ev.type == ClientMessage && ev.xclient.window == desk) {
				if ((Atom)ev.xclient.data.l[0] == A_DELETE) folder_close();
				else items_clientmessage(&ev.xclient);
			} else if (ev.type == SelectionNotify && ev.xselection.requestor == desk) {
				items_selection(&ev.xselection);
			}
		}
		if (reload) {
			reload = 0;
			rebuild(0);
		}
		ui_tick();
		lib_tick();
		if (dmg) repaint();
		XFlush(dpy);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int maxfd = fd, inf = items_inotify_fd();
		if (inf >= 0) { FD_SET(inf, &fds); if (inf > maxfd) maxfd = inf; }
		struct timeval tv = { 0, 200000 };
		if (select(maxfd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR)
			break;
		if (inf >= 0 && FD_ISSET(inf, &fds)) {
			items_inotify();
			if (lib_active()) lib_reload();
			damage(0, 0, W, C + by);              // file count in the header
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	snprintf(home, sizeof home, "%s", getenv("HOME") ? getenv("HOME") : ".");
	if (getenv("C64PINE") && getenv("C64PINE")[0]) snprintf(a64, sizeof a64, "%s", getenv("C64PINE"));
	else snprintf(a64, sizeof a64, "%s/.c64pine", home);
	if (argc > 1 && strcmp(argv[1], "--icons") == 0) {   // rebuild data/icons/c64, works without X
		int k = sprite_theme(argc > 2 ? argv[2] : NULL);
		printf("%d icon files written\n", k);
		return k > 0 ? 0 : 1;
	}
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "c64desk: cannot open display\n");
		return 1;
	}
	XSetErrorHandler(x_error);
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	depth = DefaultDepth(dpy, scr);
	if (argc > 1 && strcmp(argv[1], "--sprite-editor") == 0) {
		is_editor = 1;
		read_scale();
		read_colors();
		open_font();
		return editor_main(argc > 2 ? argv[2] : NULL);
	}
	if (argc > 1 && strcmp(argv[1], "--popup") == 0)
		return popup_main(argc, argv);
	if (argc > 2 && strcmp(argv[1], "--load") == 0)
		return load_main(argv[2]);
	if (argc > 2 && strcmp(argv[1], "--c64") == 0)
		return c64_main(argv[2], argc > 3 ? argv[3] : NULL);
	if (argc > 2 && strcmp(argv[1], "--tile") == 0)
		return tile_main(argv[2]);
	if (argc > 2 && strcmp(argv[1], "--folder") == 0)
		return folder_main(argv[2]);
	write_pid();
	XSetIOErrorHandler(x_gone);
	signal(SIGTERM, on_term);
	signal(SIGHUP, on_hup);
	signal(SIGUSR1, on_usr1);         // next slide
	signal(SIGUSR2, on_usr2);         // load-request

	char cm[32];
	snprintf(cm, sizeof cm, "_NET_WM_CM_S%d", scr);
	A_CM = XInternAtom(dpy, cm, False);
	A_TYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	A_TYPE_DESK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
	A_STRUT = XInternAtom(dpy, "_NET_WM_STRUT", False);
	A_STRUTP = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
	A_DESKTOP = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
	A_PID = XInternAtom(dpy, "_NET_WM_PID", False);
	A_CLIENTS = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	A_EXTENTS = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
	A_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
	A_FULLSCREEN = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	A_HIDDEN = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
	A_CURDESK = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
	A_TYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	A_MOVERESIZE = XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", False);
	A_GRID = XInternAtom(dpy, "_C64_GRID", False);

	W = DisplayWidth(dpy, scr);
	H = DisplayHeight(dpy, scr);
	XSetWindowAttributes a = { .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
	                                         KeyPressMask, .background_pixel = PAL[border_c] };
	desk = XCreateWindow(dpy, root, 0, 0, W, H, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask | CWBackPixel, &a);
	gc = XCreateGC(dpy, desk, 0, NULL);
	XClassHint ch = { "c64desk", "C64desk" };
	XSetClassHint(dpy, desk, &ch);
	XStoreName(dpy, desk, "c64desk");
	XWMHints wh = { .flags = InputHint, .input = True };
	XSetWMHints(dpy, desk, &wh);
	XSizeHints sh = { .flags = USPosition | USSize, .x = 0, .y = 0, .width = W, .height = H };
	XSetWMNormalHints(dpy, desk, &sh);
	XChangeProperty(dpy, desk, A_TYPE, XA_ATOM, 32, PropModeReplace, (unsigned char *)&A_TYPE_DESK, 1);
	long all = 0xFFFFFFFF, pid = getpid();
	XChangeProperty(dpy, desk, A_DESKTOP, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&all, 1);
	XChangeProperty(dpy, desk, A_PID, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);

	composited = XGetSelectionOwner(dpy, A_CM) != None;
	int fx_ev, fx_err, rr_ev, rr_err;
	XFixesQueryExtension(dpy, &fx_ev, &fx_err);
	XFixesSelectSelectionInput(dpy, root, A_CM, XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask |
	                           XFixesSelectionClientCloseNotifyMask);
	XRRQueryExtension(dpy, &rr_ev, &rr_err);
	XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
	XSelectInput(dpy, root, SubstructureNotifyMask | PropertyChangeMask);

	items_init();
	con_init();
	vice_forget();
	rebuild(1);
	XMapWindow(dpy, desk);
	XLowerWindow(dpy, desk);
	if (composited) {
		refresh_at[0] = now_ms() + 1000;
		refresh_at[1] = now_ms() + 3000;
	}

	long long next_blink = now_ms() + BLINK_MS;
	int fd = ConnectionNumber(dpy);
	for (;;) {
		int need_raise = 0, need_check = 0;
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if ((ev.type == ConfigureNotify || ev.type == MapNotify || ev.type == UnmapNotify) && !ours(ev.xany.window))
				need_check = 1;
			else if (ev.type == PropertyNotify && ev.xproperty.window == root &&
			         (ev.xproperty.atom == A_CURDESK || ev.xproperty.atom == A_CLIENTS))
				need_check = 1;
			if (ui_event(&ev) || vice_event(&ev))
				continue;
			if (ev.type == ButtonPress && ev.xbutton.window == desk) {
				if (!items_button(&ev.xbutton) && ev.xbutton.button == Button1)
					XSetInputFocus(dpy, desk, RevertToPointerRoot, ev.xbutton.time);
			} else if (ev.type == MotionNotify && ev.xmotion.window == desk) {
				while (XCheckTypedWindowEvent(dpy, desk, MotionNotify, &ev))
					;
				items_motion(&ev.xmotion);
			} else if (ev.type == ButtonRelease && ev.xbutton.window == desk) {
				items_release(&ev.xbutton);
			} else if (ev.type == KeyPress && ev.xkey.window == desk) {
				con_key(&ev.xkey);
			} else if (ev.type == ClientMessage && ev.xclient.window == desk) {
				items_clientmessage(&ev.xclient);
			} else if (ev.type == SelectionNotify && ev.xselection.requestor == desk) {
				items_selection(&ev.xselection);
			} else if (ev.type == MapNotify && !ours(ev.xmap.window)) {
				need_raise = 1;
			} else if (ev.type == ConfigureNotify && !ours(ev.xconfigure.window) && ev.xconfigure.event == root &&
			           ours(ev.xconfigure.above) && ev.xconfigure.above != desk) {
				need_raise = 1;
			} else if (ev.type == rr_ev + RRScreenChangeNotify) {
				XRRUpdateConfiguration(&ev);
				if (DisplayWidth(dpy, scr) != W || DisplayHeight(dpy, scr) != H)
					rebuild(1);
			} else if (ev.type == fx_ev + XFixesSelectionNotify) {
				check_compositor();
			}
		}
		if (reload) {
			reload = 0;
			rebuild(1);
		}
		if (load_req) {
			load_req = 0;
			char f[700], file[1024];
			path(f, sizeof f, "cache/c64/load-request");
			FILE *fp = fopen(f, "r");
			if (fp) {
				if (fgets(file, sizeof file, fp)) {
					file[strcspn(file, "\n")] = 0;
					if (file[0] != '@') vice_load_listing(file);
					else if (!strncmp(file, "@run ", 5)) vice_run_file(file + 5);
					else if (!strcmp(file, "@show")) vice_screen_toggle(1);
					else if (!strcmp(file, "@hide")) vice_screen_toggle(0);
					else if (!strcmp(file, "@fullscreen")) vice_fullscreen();
					else if (!strcmp(file, "@settings")) vice_settings();
					else if (!strcmp(file, "@menu")) {
						Window r, c; int rx = 0, ry = 0, wx, wy; unsigned int mk;
						XQueryPointer(dpy, root, &r, &c, &rx, &ry, &wx, &wy, &mk);
						menus_desktop(rx, ry);
					}
				}
				fclose(fp);
				unlink(f);
			}
		}
		if (next_wall) {
			next_wall = 0;
			wall_next();
		}
		wall_tick();
		if (need_raise)
			raise_top();
		if (need_check || (fix_due && now_ms() >= fix_due))
			check_windows();

		long long t = now_ms();
		for (int i = 0; i < 2; i++)
			if (refresh_at[i] && t >= refresh_at[i]) {
				refresh_at[i] = 0;
				damage_all();
				if (overlay)
					XClearArea(dpy, overlay, 0, 0, 0, 0, True);
			}
		if (t >= next_blink) {
			next_blink = t + BLINK_MS;
			con_blink();
		}
		con_tick();
		vice_tick();
		ui_tick();
		// cap repaints at ~30/s while output streams in

		long long wait_paint = 0;
		if (dmg) {
			long long since = now_ms() - last_paint;
			if (since >= 33) repaint();
			else wait_paint = 33 - since;
		}
		XFlush(dpy);

		long long due = next_blink;
		long long v = vice_next_ms();
		if (v && v < due) due = v;
		long long wt = wall_next_ms();
		if (wt && wt < due) due = wt;
		if (fix_due && fix_due < due) due = fix_due;
		for (int i = 0; i < 2; i++)
			if (refresh_at[i] && refresh_at[i] < due) due = refresh_at[i];
		long long wait = due - now_ms();
		if (wait_paint && wait_paint < wait) wait = wait_paint;
		if (wait < 0) wait = 0;
		struct timeval tv = { wait / 1000, (wait % 1000) * 1000 };
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int maxfd = fd, jf = con_job_fd(), inf = items_inotify_fd();
		if (jf >= 0) { FD_SET(jf, &fds); if (jf > maxfd) maxfd = jf; }
		if (inf >= 0) { FD_SET(inf, &fds); if (inf > maxfd) maxfd = inf; }
		if (select(maxfd + 1, &fds, NULL, NULL, &tv) < 0) {
			if (errno != EINTR)
				break;
			continue;
		}
		if (jf >= 0 && FD_ISSET(jf, &fds))
			con_job_input();
		if (inf >= 0 && FD_ISSET(inf, &fds))
			items_inotify();
	}
	vice_quit();
	return 0;
}
