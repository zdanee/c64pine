/*
 * tile.c -- super+arrows window tiling   (c64desk --tile left|right|up|down)
 *
 * left/right: half of the blue screen, press again for half of the whole screen
 * up: maximize, again: full screen under the bar
 * down: restore
 *
 * state + original geometry are stored on the window (_C64_TILE)
 */
#include "c64desk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { T_NONE, T_LEFT, T_LEFT_BIG, T_RIGHT, T_RIGHT_BIG, T_MAX, T_MAX_BIG };

static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }

static long *prop(Window w, Atom a, Atom type, unsigned long *n)
{
	Atom t;
	int f;
	unsigned long after;
	unsigned char *d = NULL;
	*n = 0;
	if (XGetWindowProperty(dpy, w, a, 0, 64, False, type, &t, &f, n, &after, &d) != Success || !d || f != 32) {
		if (d) XFree(d);
		*n = 0;
		return NULL;
	}
	return (long *)d;
}

static void wm_state(Window w, int add, const char *s1, const char *s2)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = atom("_NET_WM_STATE");
	e.xclient.format = 32;
	e.xclient.data.l[0] = add ? 1 : 0;
	e.xclient.data.l[1] = atom(s1);
	e.xclient.data.l[2] = s2 ? atom(s2) : 0;
	e.xclient.data.l[3] = 2;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}



static void frame_rect(Window w, const long *ext, long *r)
{
	XWindowAttributes wa;
	XGetWindowAttributes(dpy, w, &wa);
	int cx, cy;
	Window c;
	XTranslateCoordinates(dpy, w, root, 0, 0, &cx, &cy, &c);
	r[0] = cx - ext[0], r[1] = cy - ext[2], r[2] = wa.width + ext[0] + ext[1], r[3] = wa.height + ext[2] + ext[3];
}

// x,y = frame corner, but w,h are the client size (EWMH)
static void request(Window w, int x, int y, int fw, int fh, const long *ext)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = atom("_NET_MOVERESIZE_WINDOW");
	e.xclient.format = 32;
	e.xclient.data.l[0] = NorthWestGravity | 1 << 8 | 1 << 9 | 1 << 10 | 1 << 11 | 2 << 12;
	e.xclient.data.l[1] = x;
	e.xclient.data.l[2] = y;
	e.xclient.data.l[3] = fw - ext[0] - ext[1];
	e.xclient.data.l[4] = fh - ext[2] - ext[3];
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
	XSync(dpy, False);
}

static void extents(Window w, long *ext)
{
	unsigned long n;
	long *e = prop(w, atom("_NET_FRAME_EXTENTS"), XA_CARDINAL, &n);
	if (e && n == 4) memcpy(ext, e, 4 * sizeof *ext);
	if (e) XFree(e);
}

/*
 * Ask, then check where the WM really put it. Terminals with size
 * increments end up a few pixels off, so we correct a couple of times.
 * Extents are re-read every round because openbox drops the side
 * borders of maximized windows.
 */
static void place(Window w, int x, int y, int fw, int fh)
{
	int rx = x, ry = y, rw = fw, rh = fh, last[4] = { 9999, 9999, 9999, 9999 };
	long ext[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < 6; i++) {
		extents(w, ext);
		request(w, rx, ry, rw, rh, ext);
		struct timespec ts = { 0, 80000000 };
		nanosleep(&ts, NULL);
		long r[4];
		extents(w, ext);
		frame_rect(w, ext, r);
		if (getenv("C64TILE_DEBUG"))
			fprintf(stderr, "want %d %d %d %d req %d %d %d %d got %ld %ld %ld %ld ext %ld %ld %ld %ld\n", x, y, fw, fh,
			        rx, ry, rw, rh, r[0], r[1], r[2], r[3], ext[0], ext[1], ext[2], ext[3]);
		int dx = x - r[0], dy = y - r[1];
		int over_r = (r[0] + r[2]) - (x + fw), over_b = (r[1] + r[3]) - (y + fh);
		int short_ = over_r < -3 * C || over_b < -3 * C;
		if (!dx && !dy && over_r <= 0 && over_b <= 0 && !short_) break;
		// same error twice = WM does it differently, compensate.
		// otherwise it was probably still busy, just ask again
		if (dx == last[0] && dy == last[1] && over_r == last[2] && over_b == last[3]) {
			rx += dx, ry += dy;
			if (over_r > 0) rw -= over_r;
			if (over_b > 0) rh -= over_b;
		}
		last[0] = dx, last[1] = dy, last[2] = over_r, last[3] = over_b;
	}
}

static int maximized(Window w)
{
	unsigned long n;
	long *st = prop(w, atom("_NET_WM_STATE"), XA_ATOM, &n);
	int m = 0;
	for (unsigned long i = 0; i < n; i++)
		if ((Atom)st[i] == atom("_NET_WM_STATE_MAXIMIZED_VERT") || (Atom)st[i] == atom("_NET_WM_STATE_MAXIMIZED_HORZ")) m = 1;
	if (st) XFree(st);
	return m;
}

int tile_main(const char *dir)
{
	read_scale();                                        // need C
	unsigned long n;
	long *a = prop(root, atom("_NET_ACTIVE_WINDOW"), XA_WINDOW, &n);
	Window w = n ? (Window)a[0] : 0;
	if (a) XFree(a);
	if (!w) return 1;

	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr), bxx = 0, byy = 0, px = 1;
	char f[700], line[128];
	path(f, sizeof f, "cache/c64/geom");
	FILE *fp = fopen(f, "r");
	while (fp && fgets(line, sizeof line, fp)) {
		sscanf(line, "BX=%d", &bxx);
		sscanf(line, "BY=%d", &byy);
		sscanf(line, "PX=%d", &px);
	}
	if (fp) fclose(fp);
	int top = 18 * px;                                   // bar height, see reserve() in main.c
	const int M = 4;                                     // margin for the BIG variants
	long ext[4] = { 0, 0, 0, 0 };
	long *e = prop(w, atom("_NET_FRAME_EXTENTS"), XA_CARDINAL, &n);
	if (e && n == 4) memcpy(ext, e, sizeof ext);
	if (e) XFree(e);
	long cur[4];
	frame_rect(w, ext, cur);
	// state, x, y, w, h
	Atom A_TILE = atom("_C64_TILE");
	long st[5] = { T_NONE, 0, 0, 0, 0 };
	long *s = prop(w, A_TILE, XA_CARDINAL, &n);
	if (s && n == 5) memcpy(st, s, sizeof st);
	if (s) XFree(s);
	if (st[0] == T_NONE) memcpy(st + 1, cur, sizeof cur);

	// the monitor the window is on. not the primary one: no bar there, borders by the C64's proportions
	int ox, oy, px0, py0, pw, ph;
	monitor_rect(cur[0] + cur[2] / 2, cur[1] + cur[3] / 2, &ox, &oy, &sw, &sh);
	monitor_rect(-1, 0, &px0, &py0, &pw, &ph);
	if (ox != px0 || oy != py0) {
		bxx = sw * 32 / 384, byy = sh * 36 / 272, top = 0;
	}


	int ix = ox + bxx, iy = oy + byy, iw = sw - 2 * bxx, ih = sh - 2 * byy;           // inner (blue) area
	int next = st[0];
	if (!strcmp(dir, "left")) next = st[0] == T_LEFT ? T_LEFT_BIG : T_LEFT;
	else if (!strcmp(dir, "right")) next = st[0] == T_RIGHT ? T_RIGHT_BIG : T_RIGHT;
	else if (!strcmp(dir, "up")) next = st[0] == T_MAX ? T_MAX_BIG : T_MAX;
	else if (!strcmp(dir, "down")) next = T_NONE;
	else return 2;

	/* unmaximize first or openbox keeps the size. It restores the old
	 * size asynchronously, so wait or it overwrites ours */

	if (maximized(w)) {
		wm_state(w, 0, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
		XSync(dpy, False);
		for (int i = 0; i < 40 && maximized(w); i++) {
			struct timespec ts = { 0, 10000000 };
			nanosleep(&ts, NULL);
		}
		struct timespec ts = { 0, 150000000 };
		nanosleep(&ts, NULL);
	}
	switch (next) {
	case T_LEFT: place(w, ix, iy, iw / 2, ih); break;
	case T_RIGHT: place(w, ix + iw - iw / 2, iy, iw / 2, ih); break;
	case T_LEFT_BIG: place(w, ox + M, oy + top + M, sw / 2 - M - M / 2, sh - top - 2 * M); break;
	case T_RIGHT_BIG: place(w, ox + sw / 2 + M / 2, oy + top + M, sw - sw / 2 - M - M / 2, sh - top - 2 * M); break;
	case T_MAX: wm_state(w, 1, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"); break;
	case T_MAX_BIG: place(w, ox + M, oy + top + M, sw - 2 * M, sh - top - 2 * M); break;
	case T_NONE: place(w, st[1], st[2], st[3], st[4]); break;
	}
	st[0] = next;
	if (next == T_NONE) XDeleteProperty(dpy, w, A_TILE);
	else XChangeProperty(dpy, w, A_TILE, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)st, 5);
	XSync(dpy, False);
	return 0;
}
