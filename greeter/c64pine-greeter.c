/*
 * c64pine-greeter
 * LightDM greeter in C64 style
 *
 *     **** C64PINE LOGIN ****
 *   READY.
 *   LOGIN: _
 *
 * F1 sessions, F3 users, F7 power, Esc starts over
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <lightdm.h>
#include <glib.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const unsigned long PAL[16] = {
	0x000000, 0xffffff, 0x813338, 0x75cec8, 0x8e3c97, 0x56ac4d, 0x2e2c9b, 0xedf171,
	0x8e5029, 0x553800, 0xc46c71, 0x4a4a4a, 0x7b7b7b, 0xa9ff9f, 0x706deb, 0xb2b2b2 };
enum { BORDER = 14, SCREEN = 6, TEXT = 14 };

static Display *dpy;
static int scr, W, H, P, C, bx, by, cols, rows;
static Window win;
static Pixmap canvas;
static GC gc;
static XftDraw *xd;
static XftFont *font;
static unsigned char *keep;               // CRT filter, same as c64desk

static LightDMGreeter *greeter;
enum { S_LOGIN, S_PASSWORD, S_WAIT };
static int stage = S_LOGIN, blink = 1;
static char user[64], pass[128], message[160];
static char session[64];


enum { L_NONE, L_SESSIONS, L_USERS, L_POWER };
static int list = L_NONE, lsel, ln;
static char litem[32][80], lkey[32][64];
static int fkx[3], fkw[3];                // F-key labels, for mouse clicks




static void fill(int c, int x, int y, int w, int h)
{
	XSetForeground(dpy, gc, PAL[c]);
	XFillRectangle(dpy, canvas, gc, x, y, w, h);
}

static void text(int col, int row, const char *s, int fg, int bg)
{
	FcChar32 u[256];
	int n = 0;
	for (; *s && n < 256 && col + n < cols; s++) {
		unsigned char c = *s;
		if (c >= 0x80) continue;
		u[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
	}
	if (!n) return;
	int x = bx + col * C, y = by + row * C;
	if (bg >= 0) fill(bg, x, y, n * C, C);
	XRenderColor rc = { (PAL[fg] >> 16 & 255) * 257, (PAL[fg] >> 8 & 255) * 257, (PAL[fg] & 255) * 257, 0xffff };
	XftColor xc;
	XftColorAllocValue(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr), &rc, &xc);
	XftDrawString32(xd, &xc, font, x, y + font->ascent, u, n);
	XftColorFree(dpy, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr), &xc);
}

static void centered(int row, const char *s, int fg)
{
	int n = strlen(s);
	text((cols - n) / 2 > 0 ? (cols - n) / 2 : 0, row, s, fg, -1);
}

static const char *session_name(const char *key)
{
	static char name[80];
	snprintf(name, sizeof name, "%s", key);
	for (const GList *l = lightdm_get_sessions(); l; l = l->next) {
		LightDMSession *s = l->data;
		if (!strcmp(lightdm_session_get_key(s), key)) snprintf(name, sizeof name, "%s", lightdm_session_get_name(s));
	}
	return name;
}


static void present_area(int x0, int y0, int w, int h)
{
	XImage *im = XGetImage(dpy, canvas, x0, y0, w, h, AllPlanes, ZPixmap);
	if (!im) return;
	int fast = im->bits_per_pixel == 32;
	for (int y = 0; y < h; y++) {
		const unsigned char *k = keep + (size_t)(y0 + y) * W + x0;
		uint32_t *row = (uint32_t *)(im->data + (size_t)y * im->bytes_per_line);
		for (int x = 0; x < w; x++) {
			unsigned long p = fast ? row[x] : XGetPixel(im, x, y);
			unsigned f = k[x] + 1;
			p = ((p >> 16 & 255) * f >> 8) << 16 | ((p >> 8 & 255) * f >> 8) << 8 | ((p & 255) * f >> 8);
			if (fast) row[x] = (uint32_t)p; else XPutPixel(im, x, y, p);
		}
	}
	XPutImage(dpy, win, gc, im, 0, 0, x0, y0, w, h);
	XDestroyImage(im);
	XFlush(dpy);
}

static void present(void) { present_area(0, 0, W, H); }

static int cur_x = -1, cur_y;                  // -1 = no cursor

static int locked;          // dm-tool lock -> unlock mode

static void redraw(void)
{
	char t[160], host[64] = "C64PINE";
	fill(BORDER, 0, 0, W, H);
	fill(SCREEN, bx, by, cols * C, rows * C);
	gethostname(host, sizeof host);
	centered(1, locked ? "**** C64PINE LOCKED ****" : "**** C64PINE LOGIN ****", TEXT);
	time_t now = time(NULL);
	char d[40];
	strftime(d, sizeof d, "%d %b %Y  %H:%M", localtime(&now));
	snprintf(t, sizeof t, "%s  %s", host, d);
	centered(3, t, TEXT);
	text(0, 5, "READY.", TEXT, -1);
	snprintf(t, sizeof t, "LOGIN: %s", user);
	text(0, 6, t, TEXT, -1);
	int cur_col = strlen(t), cur_row = 6;
	if (stage >= S_PASSWORD) {
		char stars[130];
		size_t n = strlen(pass);
		memset(stars, '*', n);
		stars[n] = 0;
		snprintf(t, sizeof t, "PASSWORD: %s", stars);
		text(0, 7, t, TEXT, -1);
		cur_col = strlen(t), cur_row = 7;
	}
	if (message[0]) text(0, 9, message, TEXT, -1);
	cur_x = -1;
	if (stage != S_WAIT && list == L_NONE) {
		cur_x = bx + cur_col * C, cur_y = by + cur_row * C;
		if (blink) fill(TEXT, cur_x, cur_y, C, C);
	}


	snprintf(t, sizeof t, " F1 SESSION: %s ", session_name(session));
	const char *labels[3] = { t, " F3 USERS ", " F7 POWER " };
	int x = 0;
	for (int i = 0; i < 3; i++) {
		text(x, rows - 1, labels[i], SCREEN, TEXT);
		fkx[i] = x, fkw[i] = strlen(labels[i]);
		x += fkw[i] + 1;
	}

	// lists look like LOAD"$",8
	if (list != L_NONE) {
		static const char *head[] = { "", "SESSIONS", "USERS", "POWER" };
		int w = 34, h = ln + 4, c0 = (cols - w) / 2, r0 = (rows - h) / 2;
		if (c0 < 0) c0 = 0;
		fill(TEXT, bx + c0 * C - P, by + r0 * C - P, w * C + 2 * P, h * C + 2 * P);
		fill(SCREEN, bx + c0 * C, by + r0 * C, w * C, h * C);
		char hd[80];
		snprintf(hd, sizeof hd, "0 \"%-16s\" 64 2A", head[list]);
		snprintf(t, sizeof t, "%-*s", w, hd);
		text(c0, r0, t, SCREEN, TEXT);
		for (int i = 0; i < ln; i++) {
			snprintf(t, sizeof t, "%-3d \"%s\"", i + 1, litem[i]);
			char line[80];
			snprintf(line, sizeof line, "%-*s", w, t);
			text(c0, r0 + 1 + i, line, i == lsel ? SCREEN : TEXT, i == lsel ? TEXT : -1);
		}
		text(c0, r0 + 2 + ln, "RETURN = OK  ESC = BACK", TEXT, -1);
	}
	present();
}





static void open_list(int which)
{
	list = which, lsel = 0, ln = 0;
	if (which == L_SESSIONS) {
		for (const GList *l = lightdm_get_sessions(); l && ln < 32; l = l->next) {
			LightDMSession *s = l->data;
			snprintf(litem[ln], sizeof litem[0], "%s", lightdm_session_get_name(s));
			snprintf(lkey[ln], sizeof lkey[0], "%s", lightdm_session_get_key(s));
			if (!strcmp(lkey[ln], session)) lsel = ln;
			ln++;
		}
	} else if (which == L_USERS) {
		for (GList *l = lightdm_user_list_get_users(lightdm_user_list_get_instance()); l && ln < 32; l = l->next) {
			LightDMUser *u = l->data;
			snprintf(litem[ln], sizeof litem[0], "%s", lightdm_user_get_name(u));
			snprintf(lkey[ln], sizeof lkey[0], "%s", lightdm_user_get_name(u));
			ln++;
		}
	} else if (which == L_POWER) {
		if (lightdm_get_can_shutdown()) { snprintf(litem[ln], 80, "SHUT DOWN"); snprintf(lkey[ln++], 64, "shutdown"); }
		if (lightdm_get_can_restart()) { snprintf(litem[ln], 80, "RESTART"); snprintf(lkey[ln++], 64, "restart"); }
		if (lightdm_get_can_suspend()) { snprintf(litem[ln], 80, "SLEEP"); snprintf(lkey[ln++], 64, "suspend"); }
	}
	if (!ln) list = L_NONE;
	redraw();
}

static void user_session(const char *name)
{
	LightDMUser *u = lightdm_user_list_get_user_by_name(lightdm_user_list_get_instance(), name);
	const char *s = u ? lightdm_user_get_session(u) : NULL;
	if (s && *s) snprintf(session, sizeof session, "%s", s);
}

static void start_login(void)
{
	if (!user[0]) return;
	message[0] = 0;
	stage = S_WAIT;
	user_session(user);
	redraw();
	GError *err = NULL;
	if (lightdm_greeter_get_in_authentication(greeter)) lightdm_greeter_cancel_authentication(greeter, NULL);
	if (!lightdm_greeter_authenticate(greeter, user, &err)) {
		snprintf(message, sizeof message, "?DEVICE NOT PRESENT  ERROR");
		if (err) g_error_free(err);
		stage = S_LOGIN;
		redraw();
	}
}

static void list_choose(void)
{
	if (lsel < 0 || lsel >= ln) return;
	int which = list;
	list = L_NONE;
	if (which == L_SESSIONS) snprintf(session, sizeof session, "%s", lkey[lsel]);
	else if (which == L_USERS) {
		snprintf(user, sizeof user, "%s", lkey[lsel]);
		pass[0] = 0;
		start_login();
		return;
	} else if (which == L_POWER) {
		if (!strcmp(lkey[lsel], "shutdown")) lightdm_shutdown(NULL);
		else if (!strcmp(lkey[lsel], "restart")) lightdm_restart(NULL);
		else lightdm_suspend(NULL);
	}
	redraw();
}



/* ---- lightdm callbacks ---- */

static void cb_prompt(LightDMGreeter *g, const gchar *text_, LightDMPromptType type, gpointer data)
{
	(void)g; (void)text_; (void)data;
	if (type == LIGHTDM_PROMPT_TYPE_SECRET) {
		stage = S_PASSWORD;
		pass[0] = 0;
	} else {                              // unknown prompt, try the user name
		lightdm_greeter_respond(greeter, user, NULL);
	}
	redraw();
}

static void cb_message(LightDMGreeter *g, const gchar *text_, LightDMMessageType type, gpointer data)
{
	(void)g; (void)type; (void)data;
	snprintf(message, sizeof message, "%s", text_);
	redraw();
}

static void cb_complete(LightDMGreeter *g, gpointer data)
{
	(void)data;
	if (lightdm_greeter_get_is_authenticated(g)) {
		snprintf(message, sizeof message, "LOADING");
		redraw();
		GError *err = NULL;
		if (!lightdm_greeter_start_session_sync(g, session[0] ? session : NULL, &err)) {
			snprintf(message, sizeof message, "?LOAD ERROR (%s)", err ? err->message : "SESSION");
			if (err) g_error_free(err);
			stage = S_LOGIN, pass[0] = 0;
			redraw();
		}
		return;
	}
	snprintf(message, sizeof message, "?LOGIN ERROR");
	stage = S_LOGIN, pass[0] = 0;
	redraw();
}



/* ---- input ---- */

static void key(XKeyEvent *e)
{
	char buf[16];
	KeySym ks;
	int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
	if (list != L_NONE) {
		if (ks == XK_Escape) { list = L_NONE; redraw(); }
		else if (ks == XK_Up && lsel > 0) { lsel--; redraw(); }
		else if (ks == XK_Down && lsel < ln - 1) { lsel++; redraw(); }
		else if (ks == XK_Return || ks == XK_KP_Enter) list_choose();
		return;
	}
	if (ks == XK_F1) { open_list(L_SESSIONS); return; }
	if (ks == XK_F3) { open_list(L_USERS); return; }
	if (ks == XK_F7) { open_list(L_POWER); return; }
	if (ks == XK_Escape) {
		if (lightdm_greeter_get_in_authentication(greeter)) lightdm_greeter_cancel_authentication(greeter, NULL);
		stage = S_LOGIN, user[0] = pass[0] = message[0] = 0;
		redraw();
		return;
	}
	if (stage == S_WAIT) return;
	char *f = stage == S_LOGIN ? user : pass;
	size_t max = stage == S_LOGIN ? sizeof user : sizeof pass;
	if (ks == XK_Return || ks == XK_KP_Enter) {
		if (stage == S_LOGIN) start_login();
		else { stage = S_WAIT; message[0] = 0; redraw(); lightdm_greeter_respond(greeter, pass, NULL); memset(pass, 0, sizeof pass); }
		return;
	}
	if (ks == XK_BackSpace) {
		size_t l = strlen(f);
		if (l) f[l - 1] = 0;
		else if (stage == S_PASSWORD) {
			lightdm_greeter_cancel_authentication(greeter, NULL);
			stage = S_LOGIN;
		}
	} else if (n == 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
		size_t l = strlen(f);
		if (l + 1 < max) { f[l] = buf[0]; f[l + 1] = 0; }
	}
	message[0] = 0;
	redraw();
}

static void button(XButtonEvent *e)
{
	int col = (e->x - bx) / C, row = (e->y - by) / C;
	if (list != L_NONE) {
		int w = 34, h = ln + 4, c0 = (cols - w) / 2, r0 = (rows - h) / 2;
		int i = row - r0 - 1;
		if (col >= c0 && col < c0 + w && i >= 0 && i < ln) { lsel = i; list_choose(); }
		else { list = L_NONE; redraw(); }
		return;
	}
	if (row == rows - 1)
		for (int i = 0; i < 3; i++)
			if (col >= fkx[i] && col < fkx[i] + fkw[i]) open_list(i == 0 ? L_SESSIONS : i == 1 ? L_USERS : L_POWER);
}

static gboolean x_events(GIOChannel *ch, GIOCondition cond, gpointer data)
{
	(void)ch; (void)cond; (void)data;
	while (XPending(dpy)) {
		XEvent e;
		XNextEvent(dpy, &e);
		if (e.type == KeyPress) key(&e.xkey);
		else if (e.type == ButtonPress) button(&e.xbutton);
		else if (e.type == Expose && e.xexpose.count == 0) redraw();
	}
	return TRUE;
}

static gboolean tick(gpointer data)
{
	(void)data;
	blink = !blink;
	if (cur_x >= 0) {                              // only redraw the cursor
		fill(blink ? TEXT : SCREEN, cur_x, cur_y, C, C);
		present_area(cur_x, cur_y, C, C);
	}
	return TRUE;
}





int main(void)
{
	if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "c64pine-greeter: no display\n"); return 1; }
	scr = DefaultScreen(dpy);
	W = DisplayWidth(dpy, scr), H = DisplayHeight(dpy, scr);
	P = H >= 1400 ? 4 : H >= 1000 ? 3 : H >= 600 ? 2 : 1;
	C = 8 * P;
	cols = (W - 2 * (W * 32 / 384)) / C, rows = (H - 2 * (H * 36 / 272)) / C;
	bx = (W - cols * C) / 2, by = (H - rows * C) / 2;
	char pat[96];
	snprintf(pat, sizeof pat, "Pet Me 64:pixelsize=%d:antialias=false:hinting=false", C);
	font = XftFontOpenName(dpy, scr, pat);
	if (!font) font = XftFontOpenName(dpy, scr, "monospace:pixelsize=16");

	keep = malloc((size_t)W * H);
	for (int y = 0; y < H; y++) {
		double s = (P > 1 && ((y - by) % P + P) % P == P - 1) ? 0.7 : 1;
		for (int x = 0; x < W; x++) {
			double dx = 2.0 * x / (W - 1) - 1, dy = 2.0 * y / (H - 1) - 1, r = (dx * dx + dy * dy) / 2;
			keep[(size_t)y * W + x] = (unsigned char)(255 * s * (1 - 0.45 * r * r) + 0.5);
		}
	}

	XSetWindowAttributes a = { .override_redirect = True, .background_pixel = PAL[BORDER],
	                           .event_mask = ExposureMask | KeyPressMask | ButtonPressMask };
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
	gc = XCreateGC(dpy, win, 0, NULL);
	canvas = XCreatePixmap(dpy, win, W, H, DefaultDepth(dpy, scr));
	xd = XftDrawCreate(dpy, canvas, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));
	XMapRaised(dpy, win);
	XDefineCursor(dpy, RootWindow(dpy, scr), XCreateFontCursor(dpy, 68));   // XC_left_ptr
	for (int i = 0; i < 100; i++) {
		if (XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
		usleep(20000);
	}

	greeter = lightdm_greeter_new();
	g_signal_connect(greeter, LIGHTDM_GREETER_SIGNAL_SHOW_PROMPT, G_CALLBACK(cb_prompt), NULL);
	g_signal_connect(greeter, LIGHTDM_GREETER_SIGNAL_SHOW_MESSAGE, G_CALLBACK(cb_message), NULL);
	g_signal_connect(greeter, LIGHTDM_GREETER_SIGNAL_AUTHENTICATION_COMPLETE, G_CALLBACK(cb_complete), NULL);
	GError *err = NULL;
	if (!lightdm_greeter_connect_to_daemon_sync(greeter, &err)) {
		fprintf(stderr, "c64pine-greeter: %s\n", err ? err->message : "no LightDM");
		return 1;
	}
	// default session: C64PINE if installed
	const char *hint = lightdm_greeter_get_default_session_hint(greeter);
	snprintf(session, sizeof session, "%s", hint ? hint : "");
	for (const GList *l = lightdm_get_sessions(); l; l = l->next)
		if (!strcmp(lightdm_session_get_key(l->data), "c64pine")) snprintf(session, sizeof session, "c64pine");
	const char *su = lightdm_greeter_get_select_user_hint(greeter);
	if (su && *su) snprintf(user, sizeof user, "%s", su);
	/* locked: skip straight to the password */
	locked = lightdm_greeter_get_lock_hint(greeter);
	if (locked && !user[0])      // dm-tool lock doesn't say who

		for (GList *l = lightdm_user_list_get_users(lightdm_user_list_get_instance()); l; l = l->next)
			if (lightdm_user_get_logged_in(l->data)) { snprintf(user, sizeof user, "%s", lightdm_user_get_name(l->data)); break; }
	if (locked && user[0]) start_login();

	GIOChannel *ch = g_io_channel_unix_new(ConnectionNumber(dpy));
	g_io_add_watch(ch, G_IO_IN, x_events, NULL);
	g_timeout_add(330, tick, NULL);
	redraw();
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
