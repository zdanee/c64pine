/* sprite editor  (c64desk --sprite-editor [NAME])
   24x21, pens 0-11, LMB paints, RMB erases */
#include "c64desk.h"
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static Window ew;
static XftDraw *exd;
static Sprite sp, undo;
static char name[64];
static int pen = 2, painting = -1, dirty;
static int Z, gx0, gy0, px0;         // cell size, grid x/y, x of the pens
static char status[64] = "";
static const char *BUTTONS[] = { "SAVE", "SAVE AS", "LOAD", "NEW", "CLEAR", "FLIP", "UNDO" };
#define NBUTTONS 7
static int bxs[NBUTTONS], bws[NBUTTONS], brow, win_w, win_h;

static void text(int x, int y, const char *s, unsigned long fg, long bg)
{
	uint32_t b[96];
	int n = 0;
	for (; *s && n < 96; s++) b[n++] = (unsigned char)*s;
	draw_text32(ew, exd, x, y, b, n, fg, bg);
}



// desktop reloads sprites on HUP
static void notify_desktop(void)
{
	char f[700];
	int pid = 0;
	path(f, sizeof f, "cache/c64/pid");
	FILE *fp = fopen(f, "r");
	if (fp) {
		if (fscanf(fp, "%d", &pid) != 1) pid = 0;
		fclose(fp);
	}
	if (pid > 1) kill(pid, SIGHUP);
}

static void redraw(void)
{
	unsigned long fg = PAL[text_c], bg = PAL[screen_c];
	int ww = win_w, wh = win_h;
	XSetForeground(dpy, gc, bg);
	XFillRectangle(dpy, ew, gc, 0, 0, ww, wh);
	char t[96];
	snprintf(t, sizeof t, " SPRITE EDITOR: %s%s   %s", name[0] ? name : "(NEW)", dirty ? "*" : "", status);
	char line[96];
	snprintf(line, sizeof line, "%-*s", ww / C, t);
	text(0, 0, line, bg, fg);

	for (int y = 0; y < SPH; y++)
		for (int x = 0; x < SPW; x++) {
			int p = sp.px[y][x];
			XSetForeground(dpy, gc, p ? PEN_RGB[p] : bg);
			XFillRectangle(dpy, ew, gc, gx0 + x * Z, gy0 + y * Z, Z, Z);
			if (!p) {
				XSetForeground(dpy, gc, PAL[11]);
				XFillRectangle(dpy, ew, gc, gx0 + x * Z + Z / 2 - 1, gy0 + y * Z + Z / 2 - 1, 2, 2);
			}
		}
	XSetForeground(dpy, gc, fg);
	XDrawRectangle(dpy, ew, gc, gx0 - 1, gy0 - 1, SPW * Z + 1, SPH * Z + 1);
	for (int x = 8; x < SPW; x += 8) XDrawLine(dpy, ew, gc, gx0 + x * Z, gy0, gx0 + x * Z, gy0 + SPH * Z - 1);
	text(px0, gy0, "PENS", fg, -1);
	for (int i = 0; i < NPENS; i++) {
		int x = px0 + (i % 3) * 3 * C, y = gy0 + C + C / 2 + (i / 3) * 3 * C;
		XSetForeground(dpy, gc, i == pen ? fg : bg);
		XFillRectangle(dpy, ew, gc, x - P * 2, y - P * 2, 2 * C + P * 4, 2 * C + P * 4);
		XSetForeground(dpy, gc, i ? PEN_RGB[i] : bg);
		XFillRectangle(dpy, ew, gc, x, y, 2 * C, 2 * C);
		if (!i) {                                       // transparent pen
			XSetForeground(dpy, gc, fg);
			XDrawRectangle(dpy, ew, gc, x, y, 2 * C - 1, 2 * C - 1);
			XDrawLine(dpy, ew, gc, x, y, x + 2 * C - 1, y + 2 * C - 1);
		}
	}
	text(px0, gy0 + 14 * C, "PREVIEW", fg, -1);
	XSetForeground(dpy, gc, PAL[screen_c]);
	sprite_draw(ew, gc, &sp, px0, gy0 + 15 * C + C / 2, P);
	sprite_draw(ew, gc, &sp, px0 + SPW * P + C, gy0 + 15 * C + C / 2, 2 * P);
	for (int i = 0; i < NBUTTONS; i++)
		text(bxs[i], brow * C, BUTTONS[i], bg, fg);
}

static void layout(void)
{
	Z = C;
	gx0 = C;
	gy0 = 2 * C;
	px0 = gx0 + SPW * Z + 2 * C;
	brow = 2 + SPH + 1;
	int x = C;
	for (int i = 0; i < NBUTTONS; i++) {
		bxs[i] = x;
		bws[i] = strlen(BUTTONS[i]) * C;
		x += bws[i] + C;
	}
	win_w = px0 + 3 * SPW * P + 2 * C;
	if (x > win_w) win_w = x;
	win_h = (brow + 2) * C;
}



static void set_name(const char *n)
{
	size_t o = 0;
	for (; *n && o + 1 < sizeof name; n++)
		name[o++] = isalnum((unsigned char)*n) || *n == '-' || *n == '_' ? tolower((unsigned char)*n) : '-';
	name[o] = 0;
	char t[96];
	snprintf(t, sizeof t, "SPRITE EDITOR - %s", name[0] ? name : "NEW");
	XStoreName(dpy, ew, t);
}

static void save(void)
{
	if (!name[0]) return;
	if (sprite_save(name, &sp)) {
		dirty = 0;
		snprintf(status, sizeof status, "SAVED");
		notify_desktop();
	} else {
		snprintf(status, sizeof status, "?SAVE ERROR");
	}
	redraw();
}

static void cb_saveas(const char *t, void *a) { (void)a; if (t && t[0]) { set_name(t); save(); } }

static void cb_load(const char *n, void *a)
{
	(void)a;
	if (!n) return;
	undo = sp;
	sprite_load(n, &sp);
	set_name(n);
	dirty = 0;
	status[0] = 0;
	redraw();
}

static void paint(int x, int y, int p)
{
	int cx = (x - gx0) / Z, cy = (y - gy0) / Z;
	if (x < gx0 || y < gy0 || cx >= SPW || cy >= SPH) return;
	if (sp.px[cy][cx] == p) return;
	sp.px[cy][cx] = p;
	dirty = 1;
	status[0] = 0;
	redraw();
}

static void button(XButtonEvent *e)
{
	int x = e->x, y = e->y;
	if (x >= gx0 && y >= gy0 && x < gx0 + SPW * Z && y < gy0 + SPH * Z) {
		undo = sp;
		painting = e->button == Button3 ? 0 : pen;
		paint(x, y, painting);
		return;
	}
	for (int i = 0; i < NPENS; i++) {
		int px = px0 + (i % 3) * 3 * C, py = gy0 + C + C / 2 + (i / 3) * 3 * C;
		if (x >= px && x < px + 2 * C && y >= py && y < py + 2 * C) { pen = i; redraw(); return; }
	}
	if (y >= brow * C && y < brow * C + C)
		for (int i = 0; i < NBUTTONS; i++)
			if (x >= bxs[i] && x < bxs[i] + bws[i]) {
				switch (i) {
				case 0: if (name[0]) save(); else ui_input("SAVE AS:", "my-icon", cb_saveas, NULL); break;
				case 1: ui_input("SAVE AS:", name[0] ? name : "my-icon", cb_saveas, NULL); break;
				case 2: ui_sprites(e->x_root, e->y_root, cb_load, NULL); break;
				case 3: undo = sp; memset(&sp, 0, sizeof sp); set_name(""); dirty = 0; redraw(); break;
				case 4: undo = sp; memset(&sp, 0, sizeof sp); dirty = 1; redraw(); break;
				case 5:
					undo = sp;
					for (int j = 0; j < SPH; j++)
						for (int k = 0; k < SPW / 2; k++) {
							unsigned char t = sp.px[j][k];
							sp.px[j][k] = sp.px[j][SPW - 1 - k];
							sp.px[j][SPW - 1 - k] = t;
						}
					dirty = 1;
					redraw();
					break;
				case 6: { Sprite t = sp; sp = undo; undo = t; dirty = 1; redraw(); break; }
				}
				return;
			}
}




int editor_main(const char *n)
{
	layout();
	int ww = win_w, wh = win_h;
	ew = XCreateSimpleWindow(dpy, root, 0, 0, ww, wh, 0, 0, PAL[screen_c]);
	gc = XCreateGC(dpy, ew, 0, NULL);
	exd = XftDrawCreate(dpy, ew, vis, cmap);
	XSelectInput(dpy, ew, ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | KeyPressMask);
	XClassHint ch = { "c64sprite", "C64sprite" };
	XSetClassHint(dpy, ew, &ch);
	XSizeHints sz = { .flags = PMinSize | PMaxSize, .min_width = ww, .max_width = ww, .min_height = wh, .max_height = wh };
	XSetWMNormalHints(dpy, ew, &sz);
	Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, ew, &del, 1);
	if (n && n[0]) {
		set_name(n);
		sprite_load(name, &sp);
	} else {
		set_name("");
	}
	undo = sp;
	XMapWindow(dpy, ew);
	int fd = ConnectionNumber(dpy);
	for (;;) {
		while (XPending(dpy)) {
			XEvent e;
			XNextEvent(dpy, &e);
			if (ui_event(&e)) continue;
			if (e.xany.window != ew) continue;
			switch (e.type) {
			case Expose: if (e.xexpose.count == 0) redraw(); break;
			case ButtonPress: button(&e.xbutton); break;
			case ButtonRelease: painting = -1; break;
			case MotionNotify:
				while (XCheckTypedWindowEvent(dpy, ew, MotionNotify, &e)) ;
				if (painting >= 0 && (e.xmotion.state & (Button1Mask | Button3Mask))) paint(e.xmotion.x, e.xmotion.y, painting);
				break;
			case KeyPress: {
				KeySym ks = XLookupKeysym(&e.xkey, 0);
				if ((e.xkey.state & ControlMask) && ks == XK_s) { if (name[0]) save(); else ui_input("SAVE AS:", "my-icon", cb_saveas, NULL); }
				else if ((e.xkey.state & ControlMask) && ks == XK_z) { Sprite t = sp; sp = undo; undo = t; redraw(); }
				else if (ks >= XK_0 && ks <= XK_9) { pen = ks - XK_0; redraw(); }
				else if (ks == XK_a || ks == XK_b) { pen = 10 + (ks - XK_a); redraw(); }
				break;
			}
			case ClientMessage:
				if ((Atom)e.xclient.data.l[0] == del) return 0;
				break;
			}
		}
		ui_tick();
		XFlush(dpy);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		struct timeval tv = { 0, 100000 };
		select(fd + 1, &fds, NULL, NULL, &tv);
	}
}
