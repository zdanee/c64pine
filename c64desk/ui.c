/* ui.c - popups: menus (+ submenus), input box, sprite chooser.
 * Only one at a time. Grabs pointer+keyboard, click outside / Esc cancels. */
#include "c64desk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { U_NONE, U_MENU, U_INPUT, U_SPRITES };

static int kind, hover = -1, blink;
static Window pw;
static XftDraw *pxd;
static int px, py, pwid, phei;
static char title[64];
static menu_cb mcb;

// open menu levels, 0 = the menu, 1.. = submenus. each is its own window
#define MAXLV 5
typedef struct { Window w; XftDraw *xd; int x, y, wid, hei, hover, rows0; UiMenu *m; } Level;
static Level lv[MAXLV];
static int nlv;
static UiMenu flat;                     // for ui_menu()
static input_cb icb;
static sprite_cb scb;
static void *cb_arg;
static char text[256];
static char names[128][64];
static int nnames, scols, cellw, cellh;
static Sprite sprites[128];
static long long next_blink;

int ui_active(void) { return kind != U_NONE; }

static void colours(unsigned long *fg, unsigned long *bg) { *fg = PAL[text_c]; *bg = PAL[screen_c]; }

static void str32(const char *s, uint32_t *out, int *n, int max)
{
	*n = 0;
	for (; *s && *n < max; s++)
		out[(*n)++] = (unsigned char)*s;
}

static void draw_str(int x, int y, const char *s, unsigned long fg, long bg)
{
	uint32_t b[128];
	int n;
	str32(s, b, &n, 128);
	draw_text32(pw, pxd, x, y, b, n, fg, bg);
}

static void redraw(void)
{
	unsigned long fg, bg;
	colours(&fg, &bg);
	XSetForeground(dpy, gc, fg);
	XFillRectangle(dpy, pw, gc, 0, 0, pwid, phei);
	XSetForeground(dpy, gc, bg);
	XFillRectangle(dpy, pw, gc, P * 2, P * 2, pwid - P * 4, phei - P * 4);
	int x0 = P * 2, y0 = P * 2;
	if (kind == U_MENU) {
		return;                                                  // menus are drawn by draw_level
	} else if (kind == U_INPUT) {
		draw_str(x0 + C, y0 + C / 2, title, fg, -1);
		int maxc = (pwid - P * 4) / C - 2, n = strlen(text), start = n > maxc - 1 ? n - (maxc - 1) : 0;
		draw_str(x0 + C, y0 + C / 2 + 2 * C, text + start, fg, -1);
		if (blink) {
			XSetForeground(dpy, gc, fg);
			XFillRectangle(dpy, pw, gc, x0 + C + (n - start) * C, y0 + C / 2 + 2 * C, C, C);
		}
		draw_str(x0 + C, y0 + C / 2 + 4 * C, "RETURN = OK   ESC = CANCEL", fg, -1);
	} else if (kind == U_SPRITES) {
		char t[80];
		snprintf(t, sizeof t, " %-*s", (pwid - P * 4) / C - 1, hover >= 0 ? names[hover] : "CHOOSE AN ICON");
		draw_str(x0, y0, t, bg, fg);
		for (int i = 0; i < nnames; i++) {
			int cx = x0 + (i % scols) * cellw, cy = y0 + C + (i / scols) * cellh;
			if (i == hover) {
				XSetForeground(dpy, gc, fg);
				XFillRectangle(dpy, pw, gc, cx, cy, cellw, cellh);
				XSetForeground(dpy, gc, bg);
				XFillRectangle(dpy, pw, gc, cx + P, cy + P, cellw - 2 * P, cellh - 2 * P);
			}
			sprite_draw(pw, gc, &sprites[i], cx + (cellw - SPW * P) / 2, cy + (cellh - SPH * P) / 2, P);
		}
	}
}


static void draw_level(int l)
{
	Level *v = &lv[l];
	unsigned long fg, bg;
	colours(&fg, &bg);
	Window w0 = pw;
	XftDraw *x0d = pxd;
	pw = v->w, pxd = v->xd;
	XSetForeground(dpy, gc, fg);
	XFillRectangle(dpy, v->w, gc, 0, 0, v->wid, v->hei);
	XSetForeground(dpy, gc, bg);
	XFillRectangle(dpy, v->w, gc, P * 2, P * 2, v->wid - P * 4, v->hei - P * 4);
	int cols = (v->wid - P * 4) / C, x0 = P * 2, y0 = P * 2;
	char t[80];
	if (v->rows0) {
		snprintf(t, sizeof t, " %-*.*s", cols - 1, cols - 1, v->m->title);
		draw_str(x0, y0, t, bg, fg);
	}
	for (int i = 0; i < v->m->n; i++) {
		int y = y0 + (i + v->rows0) * C;
		if (!strcmp(v->m->it[i].label, "-")) {
			XSetForeground(dpy, gc, fg);
			XFillRectangle(dpy, v->w, gc, x0 + C / 2, y + C / 2 - P / 2, v->wid - P * 4 - C, P > 0 ? P : 1);
			continue;
		}
		int on = i == v->hover, sub = v->m->it[i].sub != NULL, room = cols - (sub ? 3 : 1);
		snprintf(t, sizeof t, " %-*.*s%s", room, room, v->m->it[i].label, sub ? "> " : "");
		draw_str(x0, y, t, on ? bg : fg, on ? (long)fg : (long)bg);
	}
	pw = w0, pxd = x0d;
}

static void open_popup(int x, int y, int w, int h)
{
	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	if (x + w > sw) x = sw - w;
	if (y + h > sh) y = sh - h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	px = x, py = y, pwid = w, phei = h;
	XSetWindowAttributes a = { .override_redirect = True, .background_pixel = PAL[screen_c],
	                           .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask };
	pw = XCreateWindow(dpy, root, x, y, w, h, 0, CopyFromParent, InputOutput, CopyFromParent,
	                   CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
	pxd = XftDrawCreate(dpy, pw, vis, cmap);
	XMapRaised(dpy, pw);
	for (int i = 0; i < 50; i++) {                 // retry, someone else may still hold the grab
		if (XGrabPointer(dpy, pw, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
		                 GrabModeAsync, None, None, CurrentTime) == GrabSuccess)
			break;
		struct timespec ts = { 0, 10000000 };
		nanosleep(&ts, NULL);
	}
	XGrabKeyboard(dpy, pw, True, GrabModeAsync, GrabModeAsync, CurrentTime);
	redraw();
}

static void close_popup(void)
{
	if (!kind)
		return;
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	if (kind == U_MENU) {
		for (int l = nlv - 1; l >= 0; l--) { XftDrawDestroy(lv[l].xd); XDestroyWindow(dpy, lv[l].w); }
		nlv = 0;
	} else {
		XftDrawDestroy(pxd);
		XDestroyWindow(dpy, pw);
	}
	XFlush(dpy);
	kind = U_NONE;
	hover = -1;
}

static int menu_width(UiMenu *m, int with_title)
{
	int w = with_title ? (int)strlen(m->title) : 0;
	for (int i = 0; i < m->n; i++) {
		int l = strlen(m->it[i].label) + (m->it[i].sub ? 2 : 0);
		if (l > w) w = l;
	}
	return (w > 34 ? 34 : w) + 2;
}

static int selectable(UiMenu *m, int i) { return i >= 0 && i < m->n && strcmp(m->it[i].label, "-") != 0; }


static void open_level(int l, int x, int y, UiMenu *m)
{
	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	Level *v = &lv[l];
	v->m = m;
	v->rows0 = l == 0;
	v->hover = -1;
	v->wid = menu_width(m, l == 0) * C + 4 * P;
	v->hei = (m->n + v->rows0) * C + 4 * P;
	if (l > 0 && x + v->wid > sw) x = lv[l - 1].x - v->wid + 2 * P;          // flip to the left side
	if (x + v->wid > sw) x = sw - v->wid;
	if (y + v->hei > sh) y = sh - v->hei;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	v->x = x, v->y = y;
	XSetWindowAttributes a = { .override_redirect = True, .background_pixel = PAL[screen_c],
	                           .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask };
	v->w = XCreateWindow(dpy, root, x, y, v->wid, v->hei, 0, CopyFromParent, InputOutput, CopyFromParent,
	                     CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
	v->xd = XftDrawCreate(dpy, v->w, vis, cmap);
	XMapRaised(dpy, v->w);
	nlv = l + 1;
	draw_level(l);
}

static void close_levels_from(int l)
{
	while (nlv > l) {
		nlv--;
		XftDrawDestroy(lv[nlv].xd);
		XDestroyWindow(dpy, lv[nlv].w);
	}
}


static void open_sub(int l, int i)
{
	UiMenu *sub = lv[l].m->it[i].sub;
	if (nlv > l + 1 && lv[l + 1].m == sub) return;
	close_levels_from(l + 1);
	if (l + 1 >= MAXLV || !sub) return;
	open_level(l + 1, lv[l].x + lv[l].wid - 2 * P, lv[l].y + (i + lv[l].rows0) * C, sub);
}

void ui_menu_tree(int x, int y, UiMenu *m, menu_cb cb, void *arg)
{
	close_popup();
	kind = U_MENU;
	mcb = cb, cb_arg = arg, hover = -1;
	open_level(0, x, y, m);
	pw = lv[0].w, pxd = lv[0].xd, px = lv[0].x, py = lv[0].y, pwid = lv[0].wid, phei = lv[0].hei;
	for (int i = 0; i < 50; i++) {
		if (XGrabPointer(dpy, pw, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
		                 GrabModeAsync, None, None, CurrentTime) == GrabSuccess)
			break;
		struct timespec ts = { 0, 10000000 };
		nanosleep(&ts, NULL);
	}
	XGrabKeyboard(dpy, pw, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}



void ui_menu(int x, int y, const char *t, const char **l, int n, menu_cb cb, void *arg)
{
	snprintf(flat.title, sizeof flat.title, "%s", t);
	flat.n = n > UI_MAXITEMS ? UI_MAXITEMS : n;
	for (int i = 0; i < flat.n; i++) {
		snprintf(flat.it[i].label, sizeof flat.it[i].label, "%s", l[i]);
		flat.it[i].id = i;
		flat.it[i].sub = NULL;
	}
	ui_menu_tree(x, y, &flat, cb, arg);
}

void ui_input(const char *prompt, const char *initial, input_cb cb, void *arg)
{
	close_popup();
	kind = U_INPUT;
	snprintf(title, sizeof title, "%s", prompt);
	snprintf(text, sizeof text, "%s", initial ? initial : "");
	icb = cb, cb_arg = arg, blink = 1;
	next_blink = now_ms() + 330;
	int w = 40;
	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	if (w * C + 4 * P > sw) w = sw / C - 1;
	int wid = w * C + 4 * P, hei = 6 * C + 4 * P;
	open_popup((sw - wid) / 2, (sh - hei) / 2, wid, hei);
}

void ui_sprites(int x, int y, sprite_cb cb, void *arg)
{
	close_popup();
	nnames = sprite_list(names, 128);
	for (int i = 0; i < nnames; i++)
		sprite_load(names[i], &sprites[i]);
	kind = U_SPRITES;
	scb = cb, cb_arg = arg, hover = -1;
	cellw = SPW * P + 2 * C;
	cellh = SPH * P + C;
	scols = 6;
	int srows = (nnames + scols - 1) / scols;
	if (srows < 1) srows = 1;
	open_popup(x, y, scols * cellw + 4 * P, C + srows * cellh + 4 * P);
}

static int item_under(int x, int y)
{
	x -= 2 * P;
	y -= 2 * P;
	if (kind == U_SPRITES) {
		if (x < 0 || y < C) return -1;
		int i = ((y - C) / cellh) * scols + x / cellw;
		return x / cellw < scols && i < nnames ? i : -1;
	}
	return -1;
}

static void finish_menu(int choice)
{
	menu_cb cb = mcb;
	void *a = cb_arg;
	close_popup();
	if (cb) cb(choice, a);
}

static void finish_sprite(int choice)
{
	sprite_cb cb = scb;
	void *a = cb_arg;
	char n[64] = "";
	if (choice >= 0) snprintf(n, sizeof n, "%s", names[choice]);
	close_popup();
	if (cb) cb(choice >= 0 ? n : NULL, a);
}

static void finish_input(int ok)
{
	input_cb cb = icb;
	void *a = cb_arg;
	char t[256];
	snprintf(t, sizeof t, "%s", text);
	close_popup();
	if (cb) cb(ok ? t : NULL, a);
}

// -1 if none
static int level_at(int rx, int ry, int *item)
{
	for (int l = nlv - 1; l >= 0; l--) {
		Level *v = &lv[l];
		if (rx < v->x || ry < v->y || rx >= v->x + v->wid || ry >= v->y + v->hei) continue;
		int r = (ry - v->y - 2 * P) / C - v->rows0;
		*item = (ry - v->y - 2 * P >= 0 && r >= 0 && r < v->m->n) ? r : -1;
		return l;
	}
	*item = -1;
	return -1;
}

static int first_selectable(UiMenu *m) { for (int i = 0; i < m->n; i++) if (selectable(m, i)) return i; return -1; }

static int menu_event(XEvent *e)
{
	switch (e->type) {
	case Expose:
		for (int l = 0; l < nlv; l++) if (e->xexpose.window == lv[l].w && e->xexpose.count == 0) draw_level(l);
		return 1;
	case MotionNotify: {
		int i, l = level_at(e->xmotion.x_root, e->xmotion.y_root, &i);
		if (l < 0) return 1;
		if (!selectable(lv[l].m, i)) i = -1;
		if (i >= 0 && lv[l].m->it[i].sub) open_sub(l, i);
		else close_levels_from(l + 1);
		if (lv[l].hover != i) { lv[l].hover = i; draw_level(l); }
		return 1;
	}
	case ButtonPress: {
		int i, l = level_at(e->xbutton.x_root, e->xbutton.y_root, &i);
		if (l < 0) finish_menu(-1);
		return 1;
	}
	case ButtonRelease: {
		if (e->xbutton.button == Button4 || e->xbutton.button == Button5) return 1;
		int i, l = level_at(e->xbutton.x_root, e->xbutton.y_root, &i);
		if (l >= 0 && selectable(lv[l].m, i)) {
			if (lv[l].m->it[i].sub) open_sub(l, i);
			else finish_menu(lv[l].m->it[i].id);
		}
		return 1;
	}
	case KeyPress: {
		KeySym ks = XLookupKeysym(&e->xkey, 0);
		int d = nlv - 1;
		Level *v = &lv[d];
		if (ks == XK_Escape || ks == XK_Left) {
			if (d > 0) { close_levels_from(d); draw_level(d - 1); }
			else if (ks == XK_Escape) finish_menu(-1);
			return 1;
		}
		if (ks == XK_Down || ks == XK_Up) {
			int dir = ks == XK_Down ? 1 : -1, i = v->hover;
			for (int k = 0; k < v->m->n; k++) {
				i = i < 0 ? (dir > 0 ? 0 : v->m->n - 1) : (i + dir + v->m->n) % v->m->n;
				if (selectable(v->m, i)) break;
			}
			v->hover = i;
			draw_level(d);
			return 1;
		}
		if ((ks == XK_Right || ks == XK_Return || ks == XK_KP_Enter) && selectable(v->m, v->hover)) {
			UiItem *it = &v->m->it[v->hover];
			if (it->sub) {
				open_sub(d, v->hover);
				if (nlv > d + 1) { lv[d + 1].hover = first_selectable(it->sub); draw_level(d + 1); }
			} else if (ks != XK_Right) finish_menu(it->id);
		}
		return 1;
	}
	}
	return 0;
}


/*****************************************************************
 * events
 *****************************************************************/

int ui_event(XEvent *e)
{
	if (!kind)
		return 0;
	if (kind == U_MENU) {
		int ours = 0;
		for (int l = 0; l < nlv; l++) if (e->xany.window == lv[l].w) ours = 1;
		if (!ours && e->type != KeyPress && e->type != ButtonPress && e->type != ButtonRelease && e->type != MotionNotify)
			return 0;
		return menu_event(e);
	}
	if (e->xany.window != pw && e->type != KeyPress && e->type != ButtonPress && e->type != ButtonRelease && e->type != MotionNotify)
		return 0;
	switch (e->type) {
	case Expose:
		if (e->xexpose.count == 0) redraw();
		return 1;
	case MotionNotify: {
		int inside = e->xmotion.window == pw;
		int h = inside ? item_under(e->xmotion.x, e->xmotion.y) : -1;
		if (h != hover) { hover = h; redraw(); }
		return 1;
	}
	case ButtonPress:
		if (e->xbutton.window != pw || e->xbutton.x < 0 || e->xbutton.y < 0 || e->xbutton.x >= pwid || e->xbutton.y >= phei) {
			if (kind == U_MENU) finish_menu(-1);
			else if (kind == U_SPRITES) finish_sprite(-1);
			return 1;                                      // input box ignores outside clicks

		}
		return 1;
	case ButtonRelease:
		if (e->xbutton.window == pw && e->xbutton.button != Button4 && e->xbutton.button != Button5) {
			int h = item_under(e->xbutton.x, e->xbutton.y);
			if (h >= 0) {
				if (kind == U_MENU) finish_menu(h);
				else if (kind == U_SPRITES) finish_sprite(h);
			}
		}
		return 1;
	case KeyPress: {
		char buf[16];
		KeySym ks;
		int n = XLookupString(&e->xkey, buf, sizeof buf, &ks, NULL);
		if (ks == XK_Escape) {
			if (kind == U_MENU) finish_menu(-1);
			else if (kind == U_SPRITES) finish_sprite(-1);
			else finish_input(0);
			return 1;
		}
		if (kind == U_INPUT) {
			size_t l = strlen(text);
			if (ks == XK_Return || ks == XK_KP_Enter) { finish_input(1); return 1; }
			if (ks == XK_BackSpace) { if (l) text[l - 1] = 0; }
			else if (n == 1 && (unsigned char)buf[0] >= 32 && l + 1 < sizeof text) { text[l] = buf[0]; text[l + 1] = 0; }
			blink = 1;
			redraw();
			return 1;
		}
		int max = nnames;
		int step = kind == U_SPRITES ? (ks == XK_Up || ks == XK_Down ? scols : 1) : 1;
		if (ks == XK_Down || ks == XK_Right) { hover = hover < 0 ? 0 : (hover + step < max ? hover + step : hover); redraw(); }
		else if (ks == XK_Up || ks == XK_Left) { hover = hover < 0 ? 0 : (hover - step >= 0 ? hover - step : hover); redraw(); }
		else if ((ks == XK_Return || ks == XK_KP_Enter) && hover >= 0) finish_sprite(hover);
		return 1;
	}
	}
	return 0;
}

void ui_tick(void)
{
	if (kind == U_INPUT && now_ms() >= next_blink) {
		blink = !blink;
		next_blink = now_ms() + 330;
		redraw();
	}
}
