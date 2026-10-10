/*
 *  C64 LIBRARY -- cover flow view for folders with C64 software
 *
 *  FOLDER/.cdb is a plain tar written by c64-lib:
 *     NAME.info                key=value (title year author text source recognized tried, set_*)
 *     NAME.cover, NAME.screen  scraped
 *     NAME.setcover/setscreen  user's own, these win
 */
#include "c64desk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *retval_from_stbi_load);

#define MAXENT 512

typedef struct {
	char name[256];
	char title[128], year[16], author[128], text[1200], source[512];
	int recognized, tried;
	unsigned char *cover, *screen;          // still encoded (jpg/png)
	int cover_len, screen_len;
	Pixmap cover_pm, screen_pm;
	int cpw, cph, spw, sph;
} Entry;

static Entry ent[MAXENT];
static int nent, cur, active, scraped_once;
static long long status_until;
static char status[64];

static int ax, ay, aw, ah, cov, info_y, desc_rows, btn_y, shot_w, shot_h;
typedef struct { int x, w; const char *label; } Button;
static Button btn[5];
static int nbtn;



/* ---- data ---- */

static void free_entry(Entry *e)
{
	free(e->cover); free(e->screen);
	if (e->cover_pm) XFreePixmap(dpy, e->cover_pm);
	if (e->screen_pm) XFreePixmap(dpy, e->screen_pm);
	memset(e, 0, sizeof *e);
}

static Entry *find(const char *name)
{
	for (int i = 0; i < nent; i++)
		if (strcmp(ent[i].name, name) == 0) return &ent[i];
	return NULL;
}

static void parse_info(Entry *e, char *s, int len)
{
	char *end = s + len, *line = s;
	char set_title[128] = "", set_year[16] = "", set_author[128] = "", set_text[1200] = "";
	while (line < end) {
		char *nl = memchr(line, '\n', end - line);
		if (!nl) nl = end;
		*nl = 0;
		char *v = strchr(line, '=');
		if (v) {
			*v++ = 0;
			if (!strcmp(line, "title")) snprintf(e->title, sizeof e->title, "%s", v);
			else if (!strcmp(line, "year")) snprintf(e->year, sizeof e->year, "%s", v);
			else if (!strcmp(line, "author")) snprintf(e->author, sizeof e->author, "%s", v);
			else if (!strcmp(line, "text")) snprintf(e->text, sizeof e->text, "%s", v);
			else if (!strcmp(line, "source")) snprintf(e->source, sizeof e->source, "%s", v);
			else if (!strcmp(line, "recognized")) e->recognized = atoi(v);
			else if (!strcmp(line, "tried")) e->tried = 1;
			else if (!strcmp(line, "set_title")) snprintf(set_title, sizeof set_title, "%s", v);
			else if (!strcmp(line, "set_year")) snprintf(set_year, sizeof set_year, "%s", v);
			else if (!strcmp(line, "set_author")) snprintf(set_author, sizeof set_author, "%s", v);
			else if (!strcmp(line, "set_text")) snprintf(set_text, sizeof set_text, "%s", v);
		}
		line = nl + 1;
	}
	if (set_title[0]) snprintf(e->title, sizeof e->title, "%s", set_title), e->recognized = 1;
	if (set_year[0]) snprintf(e->year, sizeof e->year, "%s", set_year);
	if (set_author[0]) snprintf(e->author, sizeof e->author, "%s", set_author);
	if (set_text[0]) snprintf(e->text, sizeof e->text, "%s", set_text);
}

static void take(unsigned char **dst, int *dlen, const unsigned char *src, int len)
{
	free(*dst);
	*dst = malloc(len);
	if (*dst) { memcpy(*dst, src, len); *dlen = len; }
}


static void read_cdb(void)
{
	char f[900];
	snprintf(f, sizeof f, "%s/.cdb", folder_dir);
	FILE *fp = fopen(f, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	unsigned char *t = n > 0 ? malloc(n) : NULL;
	if (!t || fread(t, 1, n, fp) != (size_t)n) { free(t); fclose(fp); return; }
	fclose(fp);
	char longname[512] = "";
	for (long o = 0; o + 512 <= n;) {
		const unsigned char *h = t + o;
		if (!h[0]) break;
		long size = strtol((const char *)h + 124, NULL, 8);
		char type = h[156];
		char name[512];
		if (longname[0]) { snprintf(name, sizeof name, "%s", longname); longname[0] = 0; }
		else if (!memcmp(h + 257, "ustar", 5) && h[345]) snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
		else snprintf(name, sizeof name, "%.100s", h);
		const unsigned char *data = h + 512;
		if (o + 512 + size > n) break;
		if (type == 'L') snprintf(longname, sizeof longname, "%.*s", (int)(size < 511 ? size : 511), data);
		else if (type == '0' || type == 0) {
			char *base = name;
			if (!strncmp(base, "./", 2)) base += 2;
			char *dot = strrchr(base, '.');
			if (dot) {
				*dot = 0;
				Entry *e = find(base);
				if (e) {
					if (!strcmp(dot + 1, "info")) {
						char *s = malloc(size + 1);
						if (s) { memcpy(s, data, size); s[size] = 0; parse_info(e, s, size); free(s); }
					}
					else if (!strcmp(dot + 1, "cover") && !e->cover) take(&e->cover, &e->cover_len, data, size);
					else if (!strcmp(dot + 1, "setcover")) take(&e->cover, &e->cover_len, data, size);
					else if (!strcmp(dot + 1, "screen") && !e->screen) take(&e->screen, &e->screen_len, data, size);
					else if (!strcmp(dot + 1, "setscreen")) take(&e->screen, &e->screen_len, data, size);
				}
			}
		}
		o += 512 + (size + 511) / 512 * 512;
	}
	free(t);
}

static int cmp_ent(const void *a, const void *b) { return strcasecmp(((const Entry *)a)->name, ((const Entry *)b)->name); }


void lib_reload(void)
{
	char keep[256];
	snprintf(keep, sizeof keep, "%s", nent ? ent[cur].name : "");
	for (int i = 0; i < nent; i++) free_entry(&ent[i]);
	nent = 0;
	DIR *d = opendir(folder_dir);
	struct dirent *de;
	while (d && (de = readdir(d)) && nent < MAXENT)
		if (de->d_name[0] != '.' && is_c64_name(de->d_name))
			snprintf(ent[nent++].name, sizeof ent[0].name, "%s", de->d_name);
	if (d) closedir(d);
	qsort(ent, nent, sizeof *ent, cmp_ent);
	read_cdb();
	cur = 0;
	for (int i = 0; i < nent; i++)
		if (!strcmp(ent[i].name, keep)) cur = i;
	status[0] = 0;
	// first visit: scrape the unknown ones in the background. inotify on .cdb redraws when it's done
	int missing = 0;
	for (int i = 0; i < nent; i++)
		if (!ent[i].recognized && !ent[i].tried) missing = 1;
	if (missing && !scraped_once && active) {
		char q[1300], cmd[1500];
		scraped_once = 1;
		snprintf(q, sizeof q, "'%s'", folder_dir);
		snprintf(cmd, sizeof cmd, "c64-lib scrape %s >/dev/null 2>&1", q);
		spawn(cmd);
		snprintf(status, sizeof status, "SEARCHING...");
		status_until = now_ms() + 60000;
	}
	damage_all();
}

int lib_count_c64(const char *dir)
{
	int n = 0;
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d)))
		if (de->d_name[0] != '.' && is_c64_name(de->d_name)) n++;
	if (d) closedir(d);
	return n;
}

int lib_active(void) { return active; }


void lib_open(void)
{
	if (strcmp(lay_view, "icons") != 0 && lib_count_c64(folder_dir) > 0) {
		active = 1;
		lib_layout();
		lib_reload();
	}
}

void lib_set_active(int on)
{
	active = on && lib_count_c64(folder_dir) > 0;
	snprintf(lay_view, 16, "%s", on ? "library" : "icons");
	items_save_layout();
	if (active) lib_reload();
	damage_all();
}




/* ---- pictures ---- */

static Pixmap to_pixmap(const unsigned char *idx, int w, int h, int scale, int *pw, int *ph)
{
	*pw = w * scale, *ph = h * scale;
	Pixmap pm = XCreatePixmap(dpy, desk, *pw, *ph, depth);
	XImage *im = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, *pw, *ph, 32, 0);
	im->data = malloc((size_t)im->bytes_per_line * *ph);
	for (int y = 0; y < *ph; y++)
		for (int x = 0; x < *pw; x++)
			XPutPixel(im, x, y, PAL[idx[(y / scale) * w + x / scale]]);
	XPutImage(dpy, pm, gc, im, 0, 0, 0, 0, *pw, *ph);
	XDestroyImage(im);
	return pm;
}

// max 320x320 "C64 pixels", less in small windows
static void make_cover(Entry *e)
{
	if (e->cover_pm || !e->cover) return;
	int w, h, n;
	unsigned char *img = stbi_load_from_memory(e->cover, e->cover_len, &w, &h, &n, 3);
	if (!img) return;
	int box = cov / P;
	if (box > 320) box = 320;
	int ow = w >= h ? box : box * w / h, oh = h >= w ? box : box * h / w;
	if (ow < 1) ow = 1;
	if (oh < 1) oh = 1;
	unsigned char *idx = malloc((size_t)ow * oh);
	c64ize(img, w, h, 0, 0, w, h, idx, ow, oh, 1);
	stbi_image_free(img);
	e->cover_pm = to_pixmap(idx, ow, oh, P, &e->cpw, &e->cph);
	free(idx);
}


static void make_screen(Entry *e)
{
	if (e->screen_pm || !e->screen || shot_w <= 0) return;
	int w, h, n;
	unsigned char *img = stbi_load_from_memory(e->screen, e->screen_len, &w, &h, &n, 3);
	if (!img) return;
	int ow = shot_w, oh = shot_w * h / w;
	if (oh > shot_h) { oh = shot_h; ow = shot_h * w / h; }
	if (ow < 1 || oh < 1) { stbi_image_free(img); return; }
	Pixmap pm = XCreatePixmap(dpy, desk, ow, oh, depth);
	XImage *im = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, ow, oh, 32, 0);
	im->data = malloc((size_t)im->bytes_per_line * oh);
	for (int y = 0; y < oh; y++)
		for (int x = 0; x < ow; x++) {
			const unsigned char *p = img + ((size_t)(y * h / oh) * w + x * w / ow) * 3;
			XPutPixel(im, x, y, (unsigned long)p[0] << 16 | p[1] << 8 | p[2]);
		}
	XPutImage(dpy, pm, gc, im, 0, 0, 0, 0, ow, oh);
	XDestroyImage(im);
	stbi_image_free(img);
	e->screen_pm = pm, e->spw = ow, e->sph = oh;
}




void lib_layout(void)
{
	ax = bx, ay = by + C, aw = cols * C, ah = (rows - 1) * C;
	btn_y = by + (rows - 1) * C;
	desc_rows = rows >= 24 ? 6 : rows >= 18 ? 4 : 2;
	info_y = btn_y - C - (2 + desc_rows) * C;
	cov = info_y - ay - C;
	if (cov > aw / 2) cov = aw / 2;
	if (cov < 4 * C) cov = 4 * C;
	shot_h = desc_rows * C;
	shot_w = shot_h * 8 / 5;
	if (shot_w > aw / 3) shot_w = aw / 3;
	for (int i = 0; i < nent; i++) {
		if (ent[i].cover_pm) XFreePixmap(dpy, ent[i].cover_pm);
		if (ent[i].screen_pm) XFreePixmap(dpy, ent[i].screen_pm);
		ent[i].cover_pm = ent[i].screen_pm = 0;
	}
}



/* ---- drawing ---- */

static void text_at(int x, int y, const char *s, int maxc, unsigned long fg, long bg)
{
	uint32_t u[256];
	int n = 0;
	for (; *s && n < maxc && n < 256; s++) {
		unsigned char c = *s;
		if (c >= 0x80) continue;                         // font has no glyphs for these anyway
		u[n++] = c >= 'a' && c <= 'z' ? c - 32 : c;
	}
	draw_text32(canvas, xd, x, y, u, n, fg, bg);
}

static void centered(int y, const char *s, unsigned long fg)
{
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x80) n++;
	int maxc = aw / C;
	if (n > maxc) n = maxc;
	text_at(ax + (aw - n * C) / 2, y, s, maxc, fg, (long)PAL[screen_c]);
}

// unknown files get a big floppy sprite
static void draw_cover(Entry *e, int cx)
{
	int top = ay + C / 2;
	make_cover(e);
	if (e->cover_pm) {
		int x = cx - e->cpw / 2, y = top + (cov - e->cph) / 2;
		int x0 = x < ax ? ax : x, x1 = x + e->cpw > ax + aw ? ax + aw : x + e->cpw;
		if (x1 > x0) XCopyArea(dpy, e->cover_pm, canvas, gc, x0 - x, 0, x1 - x0, e->cph, x0, y);
		return;
	}
	Sprite s;
	if (!sprite_load("floppy", &s)) return;
	int scale = cov / SPW;
	if (scale < 1) scale = 1;
	int x = cx - SPW * scale / 2, y = top + (cov - SPH * scale) / 2;
	XRectangle r = { ax, ay, aw, ah };
	XSetClipRectangles(dpy, gc, 0, 0, &r, 1, Unsorted);
	sprite_draw(canvas, gc, &s, x, y, scale);
	XSetClipMask(dpy, gc, None);
}

void lib_draw(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
	fill(screen_c, ax, ay, aw, ah);
	if (!nent) {
		centered(ay + ah / 2, "NO C64 FILES HERE", PAL[text_c]);
		return;
	}
	Entry *e = &ent[cur];
	int mid = ax + aw / 2;
	if (cur > 0) draw_cover(&ent[cur - 1], ax);                    // neighbours, half visible
	if (cur + 1 < nent) draw_cover(&ent[cur + 1], ax + aw);
	draw_cover(e, mid);


	char t[300], name[256];
	snprintf(name, sizeof name, "%s", e->name);
	char *dot = strrchr(name, '.');
	if (dot) *dot = 0;
	centered(info_y, e->title[0] ? e->title : name, PAL[text_c]);
	if (e->year[0] || e->author[0])
		snprintf(t, sizeof t, "%s%s%s", e->year, e->year[0] && e->author[0] ? "  " : "", e->author);
	else
		snprintf(t, sizeof t, "%s", e->name);
	centered(info_y + C, t, PAL[3]);


	/* screenshot left, text right */
	int ty = info_y + 2 * C + C / 2, tx = ax;
	make_screen(e);
	if (e->screen_pm) {
		XCopyArea(dpy, e->screen_pm, canvas, gc, 0, 0, e->spw, e->sph, ax, ty);
		tx = ax + e->spw + C;
	}
	const char *s = e->text[0] ? e->text : e->recognized ? "" :
	                e->tried ? "NOT FOUND ONLINE. SET YOUR OWN TITLE, PICTURES AND TEXT, OR SCRAPE AGAIN." :
	                "NO INFORMATION YET.";
	int dc = (ax + aw - tx) / C, row = 0;
	while (*s && row < desc_rows && dc > 4) {
		int n = (int)strlen(s), take_ = n;
		if (take_ > dc) {
			take_ = dc;
			while (take_ > 0 && s[take_] != ' ') take_--;
			if (take_ == 0) take_ = dc;
		}
		char line[256];
		snprintf(line, sizeof line, "%.*s", take_, s);
		if (row == desc_rows - 1 && take_ < n) {
			int l = strlen(line);
			if (l > dc - 3) l = dc - 3;
			snprintf(line + l, sizeof line - l, "...");
		}
		text_at(tx, ty + row * C, line, dc, PAL[text_c], (long)PAL[screen_c]);
		s += take_;
		while (*s == ' ') s++;
		row++;
	}


	// buttons
	static const char *labels[] = { " RUN ", " SEARCH ", " SCRAPE ", " SET ", " ICONS " };
	int bx_ = ax;
	nbtn = 0;
	for (int i = 0; i < 5; i++) {
		int n = strlen(labels[i]);
		text_at(bx_, btn_y, labels[i], n, PAL[screen_c], (long)PAL[text_c]);
		btn[nbtn++] = (Button){ bx_, n * C, labels[i] };
		bx_ += (n + 1) * C;
	}
	char pos[64];
	if (status[0] && now_ms() < status_until) snprintf(pos, sizeof pos, "%s %d/%d", status, cur + 1, nent);
	else snprintf(pos, sizeof pos, "%d/%d", cur + 1, nent);
	int pn = strlen(pos);
	if (bx_ + (pn + 1) * C <= ax + aw) text_at(ax + aw - pn * C, btn_y, pos, pn, PAL[text_c], (long)PAL[screen_c]);
}




static void act(const char *what)
{
	if (!nent) return;
	char f[1300], q[1400], qd[1400], cmd[3000];
	snprintf(f, sizeof f, "%s/%s", folder_dir, ent[cur].name);
	snprintf(q, sizeof q, "'%s'", f);
	snprintf(qd, sizeof qd, "'%s'", folder_dir);
	if (strchr(f, '\'')) return;                                     // TODO quote properly
	if (!strcmp(what, "run")) snprintf(cmd, sizeof cmd, "c64-run %s", q);
	else snprintf(cmd, sizeof cmd, "c64-lib %s %s '%s'%s", what, qd, ent[cur].name,
	              !strcmp(what, "scrape") ? " --force" : "");
	spawn(cmd);
	if (!strcmp(what, "scrape")) {
		snprintf(status, sizeof status, "SEARCHING...");
		status_until = now_ms() + 60000;
		damage_all();
	}
}

void lib_go(int d)
{
	int n = cur + d;
	if (n < 0) n = 0;
	if (n >= nent) n = nent - 1;
	if (n != cur && n >= 0) { cur = n; damage_all(); }
}

int lib_key(KeySym k)
{
	switch (k) {
	case XK_Left: case XK_Up: lib_go(-1); return 1;
	case XK_Right: case XK_Down: lib_go(1); return 1;
	case XK_Page_Up: lib_go(-5); return 1;
	case XK_Page_Down: lib_go(5); return 1;
	case XK_Home: lib_go(-MAXENT); return 1;
	case XK_End: lib_go(MAXENT); return 1;
	case XK_Return: case XK_KP_Enter: act("run"); return 1;
	}
	return 0;
}

void lib_click(XButtonEvent *e)
{
	static long long last;
	long long t = now_ms();
	if (e->y >= btn_y && e->y < btn_y + C) {
		for (int i = 0; i < nbtn; i++)
			if (e->x >= btn[i].x && e->x < btn[i].x + btn[i].w) {
				switch (i) {
				case 0: act("run"); break;
				case 1: act("search"); break;
				case 2: act("scrape"); break;
				case 3: act("set"); break;
				case 4: lib_set_active(0); break;
				}
				return;
			}
		return;
	}
	if (e->y >= ay && e->y < info_y) {
		int mid = ax + aw / 2;
		if (e->x < mid - cov / 2) lib_go(-1);
		else if (e->x > mid + cov / 2) lib_go(1);
		else if (t - last < 400) act("run");                    // dbl click

		last = t;
	}
}

void lib_tick(void)
{
	if (status[0] && now_ms() >= status_until) { status[0] = 0; damage_all(); }
}
