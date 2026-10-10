/*
 * items.c -- desktop icons (= files in ~/.c64pine/Desktop)
 *
 *   *.desktop  apps   (Icon= is a sprite name)
 *   dirs       folders
 *   the rest   files, symlinks get a little arrow
 *
 * layout file: "GX GY SPRITE<TAB>NAME" in char cells, -1 = auto
 */
#include "c64desk.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>


enum { K_APP, K_DIR, K_FILE };

typedef struct {
	char name[256];
	char label[64];
	char sprite[64];
	char exec[1024];
	int kind, is_link, terminal;
	int gx, gy;
	int saved;            // pos from the layout file
	int order;            // X-C64-Order
} Item;

typedef struct { char name[256]; int gx, gy; char sprite[64]; } Lay;

static Item items[MAXITEMS];
static int nitems;
static Lay lay[MAXITEMS];
static int nlay;
static char deskdir[600];
static int ino_fd = -1, rescan_pending;

static int selected = -1, target = -1;         // target = folder under the drag
static int pressed = -1, dragging, drag_gx0, drag_gy0, off_x, off_y, press_x, press_y;
static long long last_click;
static int last_click_item = -1;


typedef struct { char name[64]; Pixmap pm, mask; int scale; } SpriteCache;
static SpriteCache cache[64];
static int ncache;
static GC mgc, bgc, pgc;

// XDND
static Atom XA_AWARE, XA_ENTER, XA_POSITION, XA_STATUS, XA_LEAVE, XA_DROP, XA_FINISHED, XA_SELECTION, XA_COPY,
	XA_MOVE, XA_URILIST;
static Window dnd_source;
static Atom dnd_action;
static int dnd_x, dnd_y;




/*======================================================================
 *  SPRITES
 *
 *  data/c64/sprites/NAME.png, 24x21 like a hardware sprite. Colours
 *  get snapped to the nearest pen. The icon theme in data/icons/c64 is
 *  generated from these (sprites.map), see sprite_theme().
 *  Old NAME.txt files (one letter per pixel) still load.
 *======================================================================*/
unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *retval_from_stbi_load);

static void sprite_path(char *out, size_t n, const char *name, const char *ext)
{
	snprintf(out, n, "%s/data/c64/sprites/%s.%s", a64, name, ext);
}

static int nearest_pen(int r, int g, int b)
{
	int best = 1;
	long bd = -1;
	for (int i = 1; i < NPENS; i++) {
		int dr = r - (int)(PEN_RGB[i] >> 16 & 0xff), dg = g - (int)(PEN_RGB[i] >> 8 & 0xff), db = b - (int)(PEN_RGB[i] & 0xff);
		long d = 3L * dr * dr + 4L * dg * dg + 2L * db * db;
		if (bd < 0 || d < bd) bd = d, best = i;
	}
	return best;
}

int sprite_load(const char *name, Sprite *s)
{
	char f[800], line[128];
	memset(s, 0, sizeof *s);
	sprite_path(f, sizeof f, name, "png");
	int w, h, n;
	unsigned char *img = stbi_load(f, &w, &h, &n, 4);
	if (img) {
		for (int y = 0; y < SPH && y < h; y++)
			for (int x = 0; x < SPW && x < w; x++) {
				unsigned char *p = img + ((size_t)y * w + x) * 4;
				s->px[y][x] = p[3] < 128 ? 0 : nearest_pen(p[0], p[1], p[2]);
			}
		stbi_image_free(img);
		return 1;
	}
	sprite_path(f, sizeof f, name, "txt");
	FILE *fp = fopen(f, "r");
	if (!fp)
		return 0;
	for (int y = 0; y < SPH && fgets(line, sizeof line, fp); y++)
		for (int x = 0; x < SPW && line[x] && line[x] != '\n'; x++) {
			const char *p = strchr(SPRITE_PENS, line[x]);
			s->px[y][x] = p ? (unsigned char)(p - SPRITE_PENS) : 0;
		}
	fclose(fp);
	return 1;
}

// padded to a square for the icon theme
static unsigned char *sprite_rgba(const Sprite *s, int k, int square, int *w, int *h)
{
	*w = SPW * k, *h = (square ? SPW : SPH) * k;
	unsigned char *b = calloc((size_t)*w * *h, 4);
	if (!b) return NULL;
	int top = square ? (SPW - SPH) / 2 : 0;
	for (int y = 0; y < SPH; y++)
		for (int x = 0; x < SPW; x++) {
			int p = s->px[y][x];
			if (!p || p >= NPENS) continue;
			for (int j = 0; j < k; j++)
				for (int i = 0; i < k; i++) {
					unsigned char *o = b + (((size_t)(y + top) * k + j) * *w + (size_t)x * k + i) * 4;
					o[0] = PEN_RGB[p] >> 16 & 0xff; o[1] = PEN_RGB[p] >> 8 & 0xff; o[2] = PEN_RGB[p] & 0xff; o[3] = 255;
				}
		}
	return b;
}

/* rebuild theme icons for one sprite (NULL = all). returns files written */
int sprite_theme(const char *name)
{
	char mf[800], line[300], icon[200], spr[64];
	snprintf(mf, sizeof mf, "%s/data/icons/c64/sprites.map", a64);
	FILE *fp = fopen(mf, "r");
	if (!fp) return 0;
	int n = 0;
	while (fgets(line, sizeof line, fp)) {
		if (line[0] == '#' || sscanf(line, "%199s %63s", icon, spr) != 2) continue;
		if (name && strcmp(name, spr)) continue;
		Sprite s;
		if (!sprite_load(spr, &s)) continue;
		static const int sizes[3] = { 24, 48, 96 };
		for (int k = 0; k < 3; k++) {
			int w, h;
			unsigned char *b = sprite_rgba(&s, k == 0 ? 1 : k == 1 ? 2 : 4, 1, &w, &h);
			char out[900];
			snprintf(out, sizeof out, "%s/data/icons/c64/%dx%d/%s.png", a64, sizes[k], sizes[k], icon);
			if (b && png_write(out, w, h, b)) n++;
			free(b);
		}
	}
	fclose(fp);
	return n;
}

int sprite_save(const char *name, const Sprite *s)
{
	char f[800];
	sprite_path(f, sizeof f, name, "png");
	int w, h;
	unsigned char *b = sprite_rgba(s, 1, 0, &w, &h);
	int ok = b && png_write(f, w, h, b);
	free(b);
	if (!ok) return 0;
	sprite_path(f, sizeof f, name, "txt");
	unlink(f);                                       // old .txt version
	sprite_theme(name);
	return 1;
}

static int cmpname(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int sprite_list(char names[][64], int max)
{
	char d[700];
	snprintf(d, sizeof d, "%s/data/c64/sprites", a64);
	DIR *dir = opendir(d);
	int n = 0;
	struct dirent *de;
	while (dir && (de = readdir(dir)) && n < max) {
		size_t l = strlen(de->d_name);
		if (l > 4 && l < 64 && (strcmp(de->d_name + l - 4, ".png") == 0 || strcmp(de->d_name + l - 4, ".txt") == 0)) {
			int dup = 0;
			for (int i = 0; i < n; i++) if (!strncmp(names[i], de->d_name, l - 4) && !names[i][l - 4]) dup = 1;
			if (!dup) snprintf(names[n++], 64, "%.*s", (int)(l - 4), de->d_name);
		}
	}
	if (dir) closedir(dir);
	qsort(names, n, 64, cmpname);
	return n;
}

void sprite_draw(Drawable d, GC g, const Sprite *s, int x, int y, int scale)
{
	for (int j = 0; j < SPH; j++)
		for (int i = 0; i < SPW; i++)
			if (s->px[j][i]) {
				XSetForeground(dpy, g, PEN_RGB[s->px[j][i]]);
				XFillRectangle(dpy, d, g, x + i * scale, y + j * scale, scale, scale);
			}
}

static SpriteCache *cached(const char *name)
{
	for (int i = 0; i < ncache; i++)
		if (cache[i].scale == P && strcmp(cache[i].name, name) == 0)
			return &cache[i];
	Sprite s;
	if (!sprite_load(name, &s) && !sprite_load("program", &s))
		return NULL;
	if (ncache == 64) {               // lazy, just flush everything
		for (int i = 0; i < ncache; i++) {
			XFreePixmap(dpy, cache[i].pm);
			XFreePixmap(dpy, cache[i].mask);
		}
		ncache = 0;
	}
	SpriteCache *c = &cache[ncache++];
	snprintf(c->name, sizeof c->name, "%s", name);
	c->scale = P;
	c->pm = XCreatePixmap(dpy, desk, SPW * P, SPH * P, depth);
	c->mask = XCreatePixmap(dpy, desk, SPW * P, SPH * P, 1);
	if (!mgc)
		mgc = XCreateGC(dpy, c->mask, 0, NULL);
	if (!pgc)                         // own GC, the shared one may have a clip set
		pgc = XCreateGC(dpy, c->pm, 0, NULL);
	XSetForeground(dpy, mgc, 0);
	XFillRectangle(dpy, c->mask, mgc, 0, 0, SPW * P, SPH * P);
	XSetForeground(dpy, mgc, 1);
	for (int j = 0; j < SPH; j++)
		for (int i = 0; i < SPW; i++)
			if (s.px[j][i]) {
				XFillRectangle(dpy, c->mask, mgc, i * P, j * P, P, P);
				XSetForeground(dpy, pgc, PEN_RGB[s.px[j][i]]);
				XFillRectangle(dpy, c->pm, pgc, i * P, j * P, P, P);
			}
	return c;
}

static void cache_forget(const char *name)
{
	for (int i = 0; i < ncache; i++)
		if (strcmp(cache[i].name, name) == 0)
			cache[i].name[0] = 0;
}




/*======================================================================
 *  ITEMS
 *======================================================================*/

static void quote(char *out, size_t n, const char *s)     // shell quoting
{
	size_t o = 0;
	if (n < 3) return;
	out[o++] = '\'';
	for (; *s && o + 5 < n; s++) {
		if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
		else out[o++] = *s;
	}
	out[o++] = '\'';
	out[o] = 0;
}

static const char *ext_sprite(const char *name)
{
	const char *e = strrchr(name, '.');
	static const struct { const char *ext, *sprite; } map[] = {
		{ "png", "picture" }, { "jpg", "picture" }, { "jpeg", "picture" }, { "gif", "picture" }, { "bmp", "picture" },
		{ "webp", "picture" }, { "xpm", "picture" }, { "svg", "picture" }, { "xcf", "paint" },
		{ "txt", "doc" }, { "md", "doc" }, { "log", "doc" }, { "conf", "doc" }, { "pdf", "pdf" }, { "epub", "pdf" },
		{ "djvu", "pdf" }, { "cbz", "pdf" },
		{ "doc", "writer" }, { "docx", "writer" }, { "odt", "writer" }, { "rtf", "writer" }, { "abw", "writer" },
		{ "xls", "calc" }, { "xlsx", "calc" }, { "ods", "calc" }, { "csv", "calc" }, { "gnumeric", "calc" },
		{ "d64", "floppy" }, { "g64", "floppy" }, { "prg", "floppy" }, { "t64", "floppy" }, { "p00", "floppy" },
		{ "tap", "floppy" }, { "sav", "floppy" }, { "c64", "floppy" }, { "d71", "floppy" }, { "d81", "floppy" }, { "g71", "floppy" },
		{ "x64", "floppy" }, { "crt", "program" }, { "sh", "program" }, { "html", "globe" }, { "htm", "globe" },
		{ "mp4", "video" }, { "mkv", "video" }, { "avi", "video" }, { "webm", "video" }, { "mov", "video" },
		{ "mpg", "video" }, { "mpeg", "video" }, { "m4v", "video" }, { "wmv", "video" }, { "flv", "video" },
		{ "mp3", "music" }, { "ogg", "music" }, { "oga", "music" }, { "opus", "music" }, { "wav", "music" },
		{ "flac", "music" }, { "m4a", "music" }, { "aac", "music" }, { "wma", "music" }, { "mod", "music" },
		{ "xm", "music" }, { "it", "music" }, { "s3m", "music" }, { "sid", "music" },
		{ "zip", "archive" }, { "rar", "archive" }, { "7z", "archive" }, { "arj", "archive" }, { "lzh", "archive" },
		{ "lha", "archive" }, { "cab", "archive" }, { "tar", "archive" }, { "gz", "archive" }, { "tgz", "archive" },
		{ "bz2", "archive" }, { "tbz", "archive" }, { "xz", "archive" }, { "txz", "archive" }, { "zst", "archive" },
		{ "lz", "archive" }, { "iso", "archive" },
	};
	if (e)
		for (size_t i = 0; i < sizeof map / sizeof *map; i++)
			if (strcasecmp(e + 1, map[i].ext) == 0)
				return map[i].sprite;
	return "doc";
}


static int is_wall_image(const char *name)
{
	const char *e = strrchr(name, '.');
	return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png") || !strcasecmp(e, ".bmp") ||
	             !strcasecmp(e, ".gif"));
}

static void notice(const char *msg)
{
	if (is_folder) fprintf(stderr, "c64desk: %s\n", msg);
	else con_print(msg);
}

static void item_path(const Item *it, char *out, size_t n) { snprintf(out, n, "%s/%s", deskdir, it->name); }


static int is_listing(const char *name)
{
	const char *e = strrchr(name, '.');
	return e && !strcasecmp(e, ".sav");
}


static int is_archive(const char *name)
{
	return strcmp(ext_sprite(name), "archive") == 0;
}

// xarchiver needs unrar/arj for these (non-free), bsdtar/7z can still extract
static int xarchiver_ok(const char *name)
{
	const char *e = strrchr(name, '.');
	return !e || (strcasecmp(e, ".rar") && strcasecmp(e, ".arj"));
}


static void archive(const char *args, Item *it)
{
	char p[900], q[1900], cmd[2100];
	item_path(it, p, sizeof p);
	quote(q, sizeof q, p);
	snprintf(cmd, sizeof cmd, "c64-archive %s %s", args, q);
	spawn(cmd);
}

static char pack_name[256];

static void cb_compress(int choice, void *arg)
{
	(void)arg;
	static const char *fmt[] = { "zip", "7z", "tgz", "txz" };
	for (int i = 0; i < nitems; i++)
		if (choice >= 0 && choice < 4 && strcmp(items[i].name, pack_name) == 0) {
			char a[16];
			snprintf(a, sizeof a, "c %s", fmt[choice]);
			archive(a, &items[i]);
		}
}


static void wallpaper(const char *action, Item *it)
{
	char p[900], q[1900], cmd[2100];
	item_path(it, p, sizeof p);
	quote(q, sizeof q, p);
	snprintf(cmd, sizeof cmd, "c64-wallpaper %s %s", action, q);
	spawn(cmd);
}

static int is_c64_file(const char *name)
{
	const char *e = strrchr(name, '.');
	static const char *x[] = { "d64", "g64", "d71", "g71", "d81", "prg", "c64", "t64", "p00", "tap", "crt", "x64" };
	if (e)
		for (size_t i = 0; i < sizeof x / sizeof *x; i++)
			if (strcasecmp(e + 1, x[i]) == 0)
				return 1;
	return 0;
}

int is_c64_name(const char *name) { return is_c64_file(name); }

static int sprite_exists(const char *name)
{
	char f[800], g[800];
	sprite_path(f, sizeof f, name, "png");
	sprite_path(g, sizeof g, name, "txt");
	return name[0] && (access(f, R_OK) == 0 || access(g, R_OK) == 0);
}

static void upcase(char *s) { for (; *s; s++) *s = toupper((unsigned char)*s); }

static void copystr(char *dst, size_t n, const char *src)    // snprintf warns about overlapping struct fields
{
	size_t l = strlen(src);
	if (l >= n) l = n - 1;
	memmove(dst, src, l);
	dst[l] = 0;
}


int in_path(const char *prog)
{
	char dirs[4096], f[1200], *sv;
	if (!prog[0]) return 1;
	if (strchr(prog, '/')) return access(prog, X_OK) == 0;
	snprintf(dirs, sizeof dirs, "%s", getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	for (char *d = strtok_r(dirs, ":", &sv); d; d = strtok_r(NULL, ":", &sv)) {
		snprintf(f, sizeof f, "%s/%s", d, prog);
		if (access(f, X_OK) == 0) return 1;
	}
	return 0;
}

/* returns 0 if the program isn't installed (TryExec, or X-C64-TryExec
   with alternatives, any one is enough) */
static int read_desktop_file(Item *it, const char *file)
{
	char line[1200];
	int ok = 1;
	FILE *fp = fopen(file, "r");
	it->order = 1000;
	if (!fp)
		return 1;
	while (fgets(line, sizeof line, fp)) {
		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, "Name=", 5) == 0) snprintf(it->label, sizeof it->label, "%s", line + 5);
		else if (strncmp(line, "Exec=", 5) == 0) snprintf(it->exec, sizeof it->exec, "%s", line + 5);
		else if (strncmp(line, "Icon=", 5) == 0 && sprite_exists(line + 5)) snprintf(it->sprite, sizeof it->sprite, "%s", line + 5);
		else if (strncmp(line, "Terminal=", 9) == 0) it->terminal = strcmp(line + 9, "true") == 0;
		else if (strncmp(line, "X-C64-Order=", 12) == 0) it->order = atoi(line + 12);
		else if (strncmp(line, "TryExec=", 8) == 0) ok = ok && in_path(line + 8);
		else if (strncmp(line, "X-C64-TryExec=", 14) == 0) {
			int any = 0;
			char *sv;
			for (char *w = strtok_r(line + 14, " ", &sv); w && !any; w = strtok_r(NULL, " ", &sv)) any = in_path(w);
			ok = ok && any;
		}
	}
	fclose(fp);
	char *p;
	while ((p = strchr(it->exec, '%')) && p[1]) {           // drop %f %u etc
		memmove(p, p + 2, strlen(p + 2) + 1);
	}
	return ok;
}

static void desktop_file_set(const char *file, const char *key, const char *value)
{
	char buf[16384] = "", line[1200];
	size_t kl = strlen(key), o = 0;
	int done = 0;
	FILE *fp = fopen(file, "r");
	while (fp && fgets(line, sizeof line, fp) && o + sizeof line < sizeof buf) {
		if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
			o += snprintf(buf + o, sizeof buf - o, "%s=%s\n", key, value);
			done = 1;
		} else {
			o += snprintf(buf + o, sizeof buf - o, "%s", line);
		}
	}
	if (fp) fclose(fp);
	if (!done)
		o += snprintf(buf + o, sizeof buf - o, "%s=%s\n", key, value);
	fp = fopen(file, "w");
	if (fp) {
		fputs(buf, fp);
		fclose(fp);
	}
}

static Lay *lay_find(const char *name, int create)
{
	for (int i = 0; i < nlay; i++)
		if (strcmp(lay[i].name, name) == 0)
			return &lay[i];
	if (!create || nlay == MAXITEMS)
		return NULL;
	Lay *l = &lay[nlay++];
	snprintf(l->name, sizeof l->name, "%s", name);
	l->gx = l->gy = -1;
	l->sprite[0] = 0;
	return l;
}

/* desktop: config/c64/layout
 * folders: config/c64/folders/PATH with / -> %, plus a line "@window X Y W H" */
void lay_file_of(const char *dir, char *f, size_t n)
{
	char d[700];
	snprintf(d, sizeof d, "%s/Desktop", a64);
	if (strcmp(dir, d) == 0) {
		path(f, n, "config/c64/layout");
		return;
	}
	char e[700];
	size_t o = 0;
	for (const char *c = dir; *c && o + 1 < sizeof e; c++)
		e[o++] = *c == '/' ? '%' : *c;
	e[o] = 0;
	snprintf(f, n, "%s/config/c64/folders/%s", a64, e);
}

int win_geom[4] = { -1, -1, -1, -1 };
char lay_view[16];                    // "icons", "library" or "" (= auto)

static void lay_load(void)
{
	char f[700], line[512];
	lay_file_of(deskdir, f, sizeof f);
	nlay = 0;
	FILE *fp = fopen(f, "r");
	while (fp && fgets(line, sizeof line, fp) && nlay < MAXITEMS) {
		line[strcspn(line, "\n")] = 0;
		if (strncmp(line, "@view ", 6) == 0) {
			snprintf(lay_view, sizeof lay_view, "%s", line + 6);
			continue;
		}
		if (strncmp(line, "@window ", 8) == 0) {
			sscanf(line + 8, "%d %d %d %d", &win_geom[0], &win_geom[1], &win_geom[2], &win_geom[3]);
			continue;
		}
		char *tab = strchr(line, '\t');
		Lay l;
		if (!tab || sscanf(line, "%d %d %63s", &l.gx, &l.gy, l.sprite) != 3)
			continue;
		snprintf(l.name, sizeof l.name, "%s", tab + 1);
		if (strcmp(l.sprite, "-") == 0) l.sprite[0] = 0;
		lay[nlay++] = l;
	}
	if (fp) fclose(fp);
}

static void lay_save(void)
{
	char f[700], tmp[720];
	lay_file_of(deskdir, f, sizeof f);
	if (is_folder) {
		char d[700];
		path(d, sizeof d, "config/c64/folders");
		mkdir(d, 0755);
	}
	snprintf(tmp, sizeof tmp, "%s.tmp", f);
	FILE *fp = fopen(tmp, "w");
	if (!fp)
		return;
	if (win_geom[2] > 0)
		fprintf(fp, "@window %d %d %d %d\n", win_geom[0], win_geom[1], win_geom[2], win_geom[3]);
	if (lay_view[0])
		fprintf(fp, "@view %s\n", lay_view);
	for (int i = 0; i < nlay; i++) {
		struct stat st;
		char p[900];
		snprintf(p, sizeof p, "%s/%s", deskdir, lay[i].name);
		if (lstat(p, &st) != 0)
			continue;
		if (lay[i].gx < 0 && !lay[i].sprite[0])
			continue;
		fprintf(fp, "%d %d %s\t%s\n", lay[i].gx, lay[i].gy, lay[i].sprite[0] ? lay[i].sprite : "-", lay[i].name);
	}
	fclose(fp);
	rename(tmp, f);
}

static int cmp_items(const void *a, const void *b)
{
	const Item *x = a, *y = b;
	if (x->kind != y->kind) return x->kind - y->kind;
	if (x->order != y->order) return x->order - y->order;
	return strcasecmp(x->label, y->label);
}

void items_save_layout(void) { lay_save(); }
int items_count(void) { return nitems; }
int items_bottom(void)
{
	int b = 0;
	for (int i = 0; i < nitems; i++)
		if (items[i].gy + SLOT_H > b) b = items[i].gy + SLOT_H;
	return b;
}

void items_scan(void)
{
	char sel[256] = "";
	for (int i = 0; i < ncache; i++) {            // sprites may have been edited
		XFreePixmap(dpy, cache[i].pm);
		XFreePixmap(dpy, cache[i].mask);
	}
	ncache = 0;
	if (selected >= 0 && selected < nitems)
		snprintf(sel, sizeof sel, "%s", items[selected].name);
	lay_load();
	nitems = 0;
	DIR *dir = opendir(deskdir);
	struct dirent *de;
	while (dir && (de = readdir(dir)) && nitems < MAXITEMS) {
		if (de->d_name[0] == '.')
			continue;
		char p[900];
		struct stat st, lst;
		snprintf(p, sizeof p, "%s/%s", deskdir, de->d_name);
		if (lstat(p, &lst) != 0)
			continue;
		Item *it = &items[nitems];
		memset(it, 0, sizeof *it);
		snprintf(it->name, sizeof it->name, "%s", de->d_name);
		it->is_link = S_ISLNK(lst.st_mode);
		int isdir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
		size_t l = strlen(it->name);
		it->order = 1000;
		if (!isdir && l > 8 && strcmp(it->name + l - 8, ".desktop") == 0) {
			it->kind = K_APP;
			if (!read_desktop_file(it, p))
				continue;
			if (!it->label[0]) {
				copystr(it->label, sizeof it->label, it->name);
				if (l - 8 < sizeof it->label) it->label[l - 8] = 0;
			}
			if (!it->sprite[0]) snprintf(it->sprite, sizeof it->sprite, "program");
		} else if (isdir) {
			it->kind = K_DIR;
			copystr(it->label, sizeof it->label, it->name);
			snprintf(it->sprite, sizeof it->sprite, "folder");
		} else {
			it->kind = K_FILE;
			copystr(it->label, sizeof it->label, it->name);
			snprintf(it->sprite, sizeof it->sprite, "%s", ext_sprite(it->name));
		}
		upcase(it->label);
		Lay *ly = lay_find(it->name, 0);
		if (ly && ly->sprite[0] && sprite_exists(ly->sprite))
			snprintf(it->sprite, sizeof it->sprite, "%s", ly->sprite);
		it->gx = ly ? ly->gx : -1;
		it->gy = ly ? ly->gy : -1;
		it->saved = it->gx >= 0;
		nitems++;
	}
	if (dir) closedir(dir);
	qsort(items, nitems, sizeof *items, cmp_items);
	selected = -1;
	for (int i = 0; i < nitems; i++)
		if (sel[0] && strcmp(items[i].name, sel) == 0)
			selected = i;
	pressed = -1;
	dragging = 0;
	target = -1;
}

static int overlap(int ax, int ay, int bx_, int by_)
{
	return ax < bx_ + SLOT_W && bx_ < ax + SLOT_W && ay < by_ + SLOT_H && by_ < ay + SLOT_H;
}

static int grid_rows(void) { return is_folder ? 100000 : rows; }
static int grid_top(void) { return is_folder ? 1 : 0; }

static int free_at(int gx, int gy, int except)
{
	if (gx < 0 || gy < grid_top() || gx + SLOT_W > cols || gy + SLOT_H > grid_rows())
		return 0;
	for (int i = 0; i < nitems; i++)
		if (i != except && items[i].gx >= 0 && overlap(gx, gy, items[i].gx, items[i].gy))
			return 0;
	return 1;
}


/* saved positions (clamped to the screen), the rest goes in columns
 * from the right, under the banner */
void items_layout_screen(void)
{
	for (int i = 0; i < nitems; i++) {
		Item *it = &items[i];
		if (!it->saved) { it->gx = -1; continue; }
		if (it->gx + SLOT_W > cols) it->gx = cols - SLOT_W;
		if (it->gy + SLOT_H > grid_rows()) it->gy = grid_rows() - SLOT_H;
		if (it->gx < 0) it->gx = 0;
		if (it->gy < grid_top()) it->gy = grid_top();
	}
	if (is_folder) {                  // folder windows fill rows from top left instead
		int x = 0, y = 1;
		for (int i = 0; i < nitems; i++) {
			if (items[i].gx >= 0)
				continue;
			while (!free_at(x, y, i)) {
				x += SLOT_W;
				if (x + SLOT_W > cols) { x = 0; y += SLOT_H + 1; }
			}
			items[i].gx = x, items[i].gy = y;
		}
		return;
	}
	int x = cols - SLOT_W, y = 5;
	for (int i = 0; i < nitems; i++) {
		if (items[i].gx >= 0)
			continue;
		for (int guard = 0; guard < 2000; guard++) {
			if (free_at(x, y, i)) {
				items[i].gx = x;
				items[i].gy = y;
				break;
			}
			y += SLOT_H + 1;
			if (y + SLOT_H > rows) {
				y = 5;
				x -= SLOT_W;
				if (x < 0) { x = 0; y = 0; items[i].gx = 0; items[i].gy = 0; break; }
			}
		}
		y += SLOT_H + 1;
		if (y + SLOT_H > rows) { y = 5; x -= SLOT_W; if (x < 0) x = 0; }
	}
}

static void item_rect(const Item *it, int *x, int *y, int *w, int *h)
{
	*x = bx + it->gx * C;
	*y = by + (it->gy - scroll_y) * C;
	*w = SLOT_W * C;
	*h = SLOT_H * C;
}

static void damage_item(int i)
{
	int x, y, w, h;
	if (i < 0 || i >= nitems)
		return;
	item_rect(&items[i], &x, &y, &w, &h);
	damage(x, y, w, h);
}

void items_draw(int rx, int ry, int rw, int rh)
{
	if (!bgc)
		bgc = XCreateGC(dpy, desk, 0, NULL);
	for (int i = 0; i < nitems; i++) {
		Item *it = &items[i];
		int x, y, w, h;
		item_rect(it, &x, &y, &w, &h);
		if (x >= rx + rw || y >= ry + rh || x + w <= rx || y + h <= ry)
			continue;
		SpriteCache *c = cached(it->sprite);
		int sx = x + (w - SPW * P) / 2;
		if (c) {
			XSetClipMask(dpy, bgc, c->mask);
			XSetClipOrigin(dpy, bgc, sx, y);
			XCopyArea(dpy, c->pm, canvas, bgc, 0, 0, SPW * P, SPH * P, sx, y);
			XSetClipMask(dpy, bgc, None);
		}
		if (it->is_link) {
			fill(1, sx, y + (SPH - 5) * P, 5 * P, 5 * P);
			fill(0, sx + P, y + (SPH - 4) * P, 3 * P, P);
			fill(0, sx + 3 * P, y + (SPH - 4) * P, P, 3 * P);
		}
		uint32_t lab[16];
		int n = 0, len = strlen(it->label);
		for (int k = 0; k < len && n < SLOT_W; k++)
			lab[n++] = (unsigned char)it->label[k];
		if (len > SLOT_W) lab[SLOT_W - 1] = lab[SLOT_W - 2] = '.';
		int lx = x + (SLOT_W - n) / 2 * C, ly = y + 3 * C;
		if (i == selected || i == target)
			draw_text32(canvas, xd, lx, ly, lab, n, PAL[screen_c], PAL[text_c]);
		else
			draw_text32(canvas, xd, lx, ly, lab, n, PAL[text_c], (long)PAL[screen_c]);
	}
}

static int item_at(int px, int py, int except)
{
	for (int i = nitems - 1; i >= 0; i--) {
		int x, y, w, h;
		if (i == except)
			continue;
		item_rect(&items[i], &x, &y, &w, &h);

		int sx = x + (w - SPW * P) / 2, n = strlen(items[i].label);
		if (n > SLOT_W) n = SLOT_W;
		int lx = x + (SLOT_W - n) / 2 * C;
		if ((px >= sx && px < sx + SPW * P && py >= y && py < y + SPH * P) ||
		    (px >= lx && px < lx + n * C && py >= y + 3 * C && py < y + 4 * C))
			return i;
	}
	return -1;
}




/*======================================================================
 *  ACTIONS + MENUS
 *======================================================================*/


static void item_command(const Item *it, char *cmd, size_t n)
{
	char p[900], q[1900];
	item_path(it, p, sizeof p);
	quote(q, sizeof q, p);
	if (it->kind == K_APP) {
		if (it->terminal) snprintf(cmd, n, "c64-term -e %s", it->exec);
		else snprintf(cmd, n, "%s", it->exec);
	} else if (it->kind == K_DIR) {
		snprintf(cmd, n, "c64desk --folder %s", q);
	} else if (is_listing(it->name)) {
		snprintf(cmd, n, "c64desk --load %s", q);
	} else if (is_c64_file(it->name)) {
		snprintf(cmd, n, "c64-run %s", q);
	} else {
		snprintf(cmd, n, "xdg-open %s", q);
	}
}

static void open_item(int i)
{
	char cmd[2200];
	if (!is_folder && items[i].kind == K_FILE && is_listing(items[i].name)) {
		char p[900];
		item_path(&items[i], p, sizeof p);
		vice_load_listing(p);
		return;
	}
	item_command(&items[i], cmd, sizeof cmd);
	if (is_folder) spawn(cmd);
	else con_load(items[i].label, cmd);
}

int items_open_label(const char *label)
{
	size_t n = strlen(label);
	int wild = n && label[n - 1] == '*';
	if (wild) n--;
	for (int i = 0; i < nitems; i++)
		if (wild ? strncasecmp(items[i].label, label, n) == 0 : strcasecmp(items[i].label, label) == 0) {
			open_item(i);
			return 1;
		}
	return 0;
}

static void save_position(int i)
{
	Lay *l = lay_find(items[i].name, 1);
	if (!l) return;
	l->gx = items[i].gx;
	l->gy = items[i].gy;
	items[i].saved = 1;
	lay_save();
}

static void set_sprite(int i, const char *sprite)
{
	Lay *l = lay_find(items[i].name, 1);
	if (!l) return;
	snprintf(l->sprite, sizeof l->sprite, "%s", sprite);
	lay_save();
	if (items[i].kind == K_APP) {
		char p[900];
		item_path(&items[i], p, sizeof p);
		desktop_file_set(p, "Icon", sprite);
	}
	snprintf(items[i].sprite, sizeof items[i].sprite, "%s", sprite);
	cache_forget(sprite);
	damage_item(i);
}

// NAME, NAME 2, NAME 3 ...
static void unique_name(const char *want, char *out, size_t n)
{
	char p[900];
	struct stat st;
	snprintf(out, n, "%s", want);
	for (int k = 2; k < 1000; k++) {
		snprintf(p, sizeof p, "%s/%s", deskdir, out);
		if (lstat(p, &st) != 0)
			return;
		const char *dot = strrchr(want, '.');
		if (dot && dot != want) snprintf(out, n, "%.*s %d%s", (int)(dot - want), want, k, dot);
		else snprintf(out, n, "%s %d", want, k);
	}
}

// position is stored before the file exists so the scan puts it there
static void place_new(const char *name, int px, int py)
{
	int gx = (px - bx) / C - SLOT_W / 2, gy = (py - by) / C - 1 + scroll_y;
	if (gx < 0) gx = 0;
	if (gy < grid_top()) gy = grid_top();
	if (gx + SLOT_W > cols) gx = cols - SLOT_W;
	if (gy + SLOT_H > grid_rows()) gy = grid_rows() - SLOT_H;
	Lay *l = lay_find(name, 1);
	if (!l) return;
	l->gx = gx;
	l->gy = gy;
	lay_save();
}

static void rescan(void)
{
	items_scan();
	items_layout_screen();
	damage_all();
}



static int menu_x, menu_y;
static char menu_name[256];

static int find_by_name(const char *name)
{
	for (int i = 0; i < nitems; i++)
		if (strcmp(items[i].name, name) == 0)
			return i;
	return -1;
}

static void cb_rename(const char *text, void *arg)
{
	(void)arg;
	int i = find_by_name(menu_name);
	if (!text || !text[0] || i < 0)
		return;
	char from[900], to[900];
	item_path(&items[i], from, sizeof from);
	if (items[i].kind == K_APP) {
		desktop_file_set(from, "Name", text);
	} else if (!strchr(text, '/')) {
		snprintf(to, sizeof to, "%s/%s", deskdir, text);
		if (rename(from, to) == 0) {
			Lay *l = lay_find(menu_name, 0);
			if (l) { snprintf(l->name, sizeof l->name, "%s", text); lay_save(); }
		} else {
			notice("?FILE EXISTS  ERROR");
		}
	}
	rescan();
}

static void cb_command(const char *text, void *arg)
{
	(void)arg;
	int i = find_by_name(menu_name);
	if (!text || !text[0] || i < 0)
		return;
	char p[900];
	item_path(&items[i], p, sizeof p);
	desktop_file_set(p, "Exec", text);
	rescan();
}

static void cb_sprite(const char *name, void *arg)
{
	(void)arg;
	int i = find_by_name(menu_name);
	if (name && i >= 0)
		set_sprite(i, name);
}

static void sanitize(const char *in, char *out, size_t n)
{
	size_t o = 0;
	for (; *in && o + 1 < n; in++)
		out[o++] = isalnum((unsigned char)*in) ? tolower((unsigned char)*in) : '-';
	out[o] = 0;
}

static void edit_icon(int i)
{
	char mine[64], s[48], cmd[256];
	sanitize(items[i].label, s, sizeof s);
	if (strncmp(items[i].sprite, "my-", 3) == 0) {
		snprintf(mine, sizeof mine, "%s", items[i].sprite);
	} else {
		Sprite sp;
		snprintf(mine, sizeof mine, "my-%s", s);
		if (!sprite_exists(mine)) {
			sprite_load(items[i].sprite, &sp);
			sprite_save(mine, &sp);
		}
		set_sprite(i, mine);
	}
	snprintf(cmd, sizeof cmd, "c64desk --sprite-editor '%s'", mine);
	spawn(cmd);
}

static void trash(int i)
{
	char p[900], q[1900], cmd[2000];
	item_path(&items[i], p, sizeof p);
	if (items[i].kind == K_APP || items[i].is_link) {
		unlink(p);
	} else {
		quote(q, sizeof q, p);
		snprintf(cmd, sizeof cmd, "gio trash %s 2>/dev/null || { mkdir -p \"$C64PINE/data/Trash/files\" && mv %s \"$C64PINE/data/Trash/files/\"; }", q, q);
		spawn(cmd);
	}
	rescan();
}

static void cb_item_menu(int choice, void *arg)
{
	(void)arg;
	int i = find_by_name(menu_name);
	if (i < 0 || choice < 0)
		return;
	Item *it = &items[i];
	char initial[256], p[900], q[1900], cmd[2100];
	/* XXX order must match item_menu()! */
	int k = 0;
	if (choice == k++) { open_item(i); return; }
	if (it->kind == K_DIR && choice == k++) {
		item_path(it, p, sizeof p);
		quote(q, sizeof q, p);
		snprintf(cmd, sizeof cmd, "cd %s && exec c64-term", q);
		spawn(cmd);
		return;
	}
	if (it->kind == K_DIR && choice == k++) {
		item_path(it, p, sizeof p);
		quote(q, sizeof q, p);
		snprintf(cmd, sizeof cmd, "env GTK_THEME=c64 pcmanfm %s", q);
		spawn(cmd);
		return;
	}
	if (it->kind == K_DIR && choice == k++) { wallpaper("slideshow", it); return; }
	if (it->kind != K_DIR && it->kind != K_APP && is_wall_image(it->name) && choice == k++) { wallpaper("set", it); return; }
	if (it->kind == K_FILE && is_listing(it->name) && choice == k++) {
		item_path(it, p, sizeof p);
		quote(q, sizeof q, p);
		snprintf(cmd, sizeof cmd, "c64-term -t LISTING -e less %s", q);
		spawn(cmd);
		return;
	}
	if (it->kind == K_FILE && is_archive(it->name)) {
		if (choice == k++) { archive("x", it); return; }
		if (choice == k++) { archive("l", it); return; }
		if (xarchiver_ok(it->name) && choice == k++) {
			item_path(it, p, sizeof p);
			quote(q, sizeof q, p);
			snprintf(cmd, sizeof cmd, "env GTK_THEME=c64 xarchiver %s", q);
			spawn(cmd);
			return;
		}
	}
	if (it->kind != K_APP && choice == k++) {
		static const char *fl[] = { "ZIP", "7Z", "TAR.GZ", "TAR.XZ" };
		snprintf(pack_name, sizeof pack_name, "%s", it->name);
		ui_menu(menu_x, menu_y, "COMPRESS AS", fl, 4, cb_compress, NULL);
		return;
	}
	if (choice == k++) {
		snprintf(initial, sizeof initial, "%s", it->kind == K_APP ? it->label : it->name);
		ui_input("NEW NAME:", initial, cb_rename, NULL);
		return;
	}
	if (choice == k++) { ui_sprites(menu_x, menu_y, cb_sprite, NULL); return; }
	if (choice == k++) { edit_icon(i); return; }
	if (it->kind == K_APP && choice == k++) {
		ui_input("COMMAND:", it->exec, cb_command, NULL);
		return;
	}
	if (choice == k++) { trash(i); return; }
}

static void item_menu(int i, int x, int y)
{
	const char *labels[16];
	int n = 0;
	Item *it = &items[i];
	labels[n++] = "OPEN";
	if (it->kind == K_DIR) labels[n++] = "OPEN IN TERMINAL";
	if (it->kind == K_DIR) labels[n++] = "OPEN IN FILE MANAGER";
	if (it->kind == K_DIR) labels[n++] = "WALLPAPER SLIDESHOW";
	if (it->kind != K_DIR && it->kind != K_APP && is_wall_image(it->name)) labels[n++] = "SET AS WALLPAPER";
	if (it->kind == K_FILE && is_listing(it->name)) labels[n++] = "READ LISTING";
	if (it->kind == K_FILE && is_archive(it->name)) {
		labels[n++] = "EXTRACT HERE";
		labels[n++] = "LIST CONTENTS";
		if (xarchiver_ok(it->name)) labels[n++] = "OPEN IN ARCHIVER";
	}
	if (it->kind != K_APP) labels[n++] = "COMPRESS...";
	labels[n++] = "RENAME...";
	labels[n++] = "CHANGE ICON...";
	labels[n++] = "EDIT ICON...";
	if (it->kind == K_APP) labels[n++] = "EDIT COMMAND...";
	labels[n++] = (it->kind == K_APP || it->is_link) ? "REMOVE" : "MOVE TO TRASH";
	snprintf(menu_name, sizeof menu_name, "%s", it->name);
	menu_x = x, menu_y = y;
	ui_menu(x, y, it->label, labels, n, cb_item_menu, NULL);
}

static void cb_new_shortcut(const char *text, void *arg)
{
	(void)arg;
	if (!text || !text[0])
		return;
	char p[900], name[256], target_[900];
	struct stat st;
	if (text[0] == '~') snprintf(target_, sizeof target_, "%s%s", home, text + 1);
	else snprintf(target_, sizeof target_, "%s", text);
	if (target_[0] == '/' && stat(target_, &st) == 0) {          // path -> symlink
		const char *b = strrchr(target_, '/');
		unique_name(b && b[1] ? b + 1 : "SHORTCUT", name, sizeof name);
		snprintf(p, sizeof p, "%s/%s", deskdir, name);
		if (symlink(target_, p) != 0) { notice("?SHORTCUT  ERROR"); return; }
	} else {                                                      // command -> .desktop
		char word[64] = "", label[64], fn[80];
		sscanf(text, "%63s", word);
		const char *b = strrchr(word, '/');
		snprintf(label, sizeof label, "%s", b ? b + 1 : word);
		upcase(label);
		snprintf(fn, sizeof fn, "%s.desktop", b ? b + 1 : word);
		unique_name(fn, name, sizeof name);
		snprintf(p, sizeof p, "%s/%s", deskdir, name);
		FILE *fp = fopen(p, "w");
		if (!fp) { notice("?SHORTCUT  ERROR"); return; }
		fprintf(fp, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nIcon=program\n", label, text);
		fclose(fp);
	}
	place_new(name, menu_x, menu_y);
	rescan();
}

static void cb_new_folder(const char *text, void *arg)
{
	(void)arg;
	char p[900], name[256];
	if (!text || !text[0] || strchr(text, '/'))
		return;
	unique_name(text, name, sizeof name);
	snprintf(p, sizeof p, "%s/%s", deskdir, name);
	if (mkdir(p, 0755) != 0) { notice("?FOLDER  ERROR"); return; }
	place_new(name, menu_x, menu_y);
	rescan();
}

static void cb_new_file(const char *text, void *arg)
{
	(void)arg;
	char p[900], name[256];
	if (!text || !text[0] || strchr(text, '/'))
		return;
	unique_name(text, name, sizeof name);
	snprintf(p, sizeof p, "%s/%s", deskdir, name);
	FILE *fp = fopen(p, "w");
	if (!fp) { notice("?FILE  ERROR"); return; }
	fclose(fp);
	place_new(name, menu_x, menu_y);
	rescan();
}

// @new-shortcut etc from menus.c
void items_builtin(const char *name, int x, int y)
{
	menu_x = x, menu_y = y;
	if (!strcmp(name, "new-shortcut")) ui_input("COMMAND, OR A FILE/FOLDER PATH:", "", cb_new_shortcut, NULL);
	else if (!strcmp(name, "new-folder")) ui_input("FOLDER NAME:", "NEW FOLDER", cb_new_folder, NULL);
	else if (!strcmp(name, "new-file")) ui_input("FILE NAME:", "NEW FILE.TXT", cb_new_file, NULL);
	else if (!strcmp(name, "line-up")) {                         // just forget the saved positions
		for (int i = 0; i < nlay; i++) lay[i].gx = lay[i].gy = -1;
		lay_save();
		rescan();
	}
}

static void cb_folder_menu(int choice, void *arg)
{
	(void)arg;
	char q[1300], cmd[1400];
	quote(q, sizeof q, deskdir);
	switch (choice) {
	case 0: ui_input("FOLDER NAME:", "NEW FOLDER", cb_new_folder, NULL); break;
	case 1: ui_input("FILE NAME:", "NEW FILE.TXT", cb_new_file, NULL); break;
	case 2:
		for (int i = 0; i < nlay; i++) lay[i].gx = lay[i].gy = -1;
		lay_save();
		rescan();
		break;
	case 3: snprintf(cmd, sizeof cmd, "env GTK_THEME=c64 pcmanfm %s", q); spawn(cmd); break;
	case 4: snprintf(cmd, sizeof cmd, "cd %s && exec c64-term", q); spawn(cmd); break;
	case 5: {
		char p[700];
		snprintf(p, sizeof p, "%s", deskdir);
		char *sl = strrchr(p, '/');
		if (sl && sl != p) *sl = 0;
		else snprintf(p, sizeof p, "/");
		quote(q, sizeof q, p);
		snprintf(cmd, sizeof cmd, "c64desk --folder %s", q);
		spawn(cmd);
		break;
	}
	case 6: snprintf(cmd, sizeof cmd, "c64-wallpaper slideshow %s", q); spawn(cmd); break;
	case 7: lib_set_active(!lib_active()); rescan(); break;
	}
}

void items_open_parent(void) { cb_folder_menu(5, NULL); }

void items_desktop_menu(int x, int y)
{
	if (is_folder) {
		static const char *fl[8] = { "NEW FOLDER...", "NEW TEXT FILE...", "LINE UP ICONS", "OPEN IN FILE MANAGER",
			"OPEN IN TERMINAL", "OPEN ENCLOSING FOLDER", "WALLPAPER SLIDESHOW", "LIBRARY VIEW" };
		fl[7] = lib_active() ? "ICON VIEW" : "C64 LIBRARY VIEW";
		menu_x = x, menu_y = y;
		ui_menu(x, y, "FOLDER", fl, lib_count_c64(deskdir) > 0 ? 8 : 7, cb_folder_menu, NULL);
		return;
	}
	menus_desktop(x, y);
}




/*======================================================================
 *  MOUSE
 *======================================================================*/

int items_button(XButtonEvent *e)
{
	int i = item_at(e->x, e->y, -1);
	if (e->button == Button3) {
		if (i >= 0) {
			int old = selected;
			selected = i;
			damage_item(old);
			damage_item(i);
			item_menu(i, e->x_root, e->y_root);
		} else {
			items_desktop_menu(e->x_root, e->y_root);
		}
		return 1;
	}
	if (e->button != Button1)
		return 0;
	XSetInputFocus(dpy, desk, RevertToPointerRoot, e->time);
	int old = selected;
	selected = i;
	damage_item(old);
	damage_item(i);
	if (i < 0)
		return 0;
	long long t = now_ms();
	if (i == last_click_item && t - last_click < 400) {
		open_item(i);
		last_click_item = -1;
		pressed = -1;
		return 1;
	}
	last_click_item = i;
	last_click = t;
	pressed = i;
	press_x = e->x, press_y = e->y;
	drag_gx0 = items[i].gx, drag_gy0 = items[i].gy;
	off_x = e->x - (bx + items[i].gx * C);
	off_y = e->y - (by + (items[i].gy - scroll_y) * C);
	return 1;
}

int items_motion(XMotionEvent *e)
{
	if (pressed < 0)
		return 0;
	if (!dragging && abs(e->x - press_x) < 5 && abs(e->y - press_y) < 5)
		return 1;
	dragging = 1;
	Item *it = &items[pressed];
	int gx = (e->x - off_x - bx + C / 2) / C, gy = (e->y - off_y - by + C / 2) / C + scroll_y;
	if (e->x - off_x - bx + C / 2 < 0) gx = 0;
	if (e->y - off_y - by + C / 2 < 0) gy = scroll_y;
	if (gx + SLOT_W > cols) gx = cols - SLOT_W;
	if (gy + SLOT_H > grid_rows()) gy = grid_rows() - SLOT_H;
	if (gy < grid_top()) gy = grid_top();
	int t = item_at(e->x, e->y, pressed);
	if (t >= 0 && items[t].kind != K_DIR) t = -1;
	if (t != target) {
		damage_item(target);
		target = t;
		damage_item(target);
	}
	if (gx != it->gx || gy != it->gy) {
		damage_item(pressed);
		it->gx = gx, it->gy = gy;
		damage_item(pressed);
	}
	return 1;
}

static void move_into(int from, int folder)
{
	char src[900], dst[1200];
	item_path(&items[from], src, sizeof src);
	snprintf(dst, sizeof dst, "%s/%s/%s", deskdir, items[folder].name, items[from].name);
	struct stat st;
	if (lstat(dst, &st) == 0) {
		notice("?FILE EXISTS  ERROR");
		return;
	}
	if (rename(src, dst) != 0)
		notice("?MOVE  ERROR");
}

/* another c64desk window (desktop / folder) under the pointer?
 * gives back its dir and grid so we can drop icons into it */
static Window window_under(int rx, int ry, char *dir, size_t n, long grid[4])
{
	Atom A_DIR = XInternAtom(dpy, "_C64_FOLDER", False), A_GRID = XInternAtom(dpy, "_C64_GRID", False);
	Window w = root, child = 0;
	int x, y;
	for (int depth_ = 0; depth_ < 8; depth_++) {
		if (!XTranslateCoordinates(dpy, root, w, rx, ry, &x, &y, &child) || !child)
			break;
		w = child;
		Atom t;
		int f;
		unsigned long nn, after;
		unsigned char *d = NULL;
		if (XGetWindowProperty(dpy, w, A_DIR, 0, 1024, False, AnyPropertyType, &t, &f, &nn, &after, &d) == Success && d) {
			snprintf(dir, n, "%s", (char *)d);
			XFree(d);
			d = NULL;
			if (w == desk) return 0;
			int gx, gy;
			Window c2;
			XTranslateCoordinates(dpy, root, w, rx, ry, &gx, &gy, &c2);
			grid[0] = gx, grid[1] = gy;
			if (XGetWindowProperty(dpy, w, A_GRID, 0, 4, False, XA_CARDINAL, &t, &f, &nn, &after, &d) == Success && d && nn == 4) {
				long *g = (long *)d;      // bx by C scroll
				int cgx = (gx - g[0]) / g[2] - SLOT_W / 2, cgy = (gy - g[1]) / g[2] - 1 + g[3];
				grid[0] = cgx < 0 ? 0 : cgx, grid[1] = cgy < 1 ? 1 : cgy;
			} else {
				grid[0] = grid[1] = -1;
			}
			if (d) XFree(d);
			return w;
		}
	}
	return 0;
}


static int move_to_dir(int i, const char *dir, int gx, int gy)
{
	char src[900], dst[1300], f[800];
	item_path(&items[i], src, sizeof src);
	snprintf(dst, sizeof dst, "%s/%s", dir, items[i].name);
	struct stat st;
	if (strcmp(dir, deskdir) == 0) return 0;
	if (strncmp(dir, src, strlen(src)) == 0 && (dir[strlen(src)] == '/' || !dir[strlen(src)])) {
		notice("?CANNOT MOVE A FOLDER INTO ITSELF  ERROR");
		return 0;
	}
	if (lstat(dst, &st) == 0) { notice("?FILE EXISTS  ERROR"); return 0; }
	if (gx >= 0) {
		lay_file_of(dir, f, sizeof f);
		char d[700];
		path(d, sizeof d, "config/c64/folders");
		mkdir(d, 0755);
		FILE *fp = fopen(f, "a");
		if (fp) { fprintf(fp, "%d %d %s\t%s\n", gx, gy, "-", items[i].name); fclose(fp); }
	}
	if (rename(src, dst) != 0) {                         // EXDEV, let mv deal with it
		char q[1900], qd[1900], cmd[4000];
		quote(q, sizeof q, src);
		quote(qd, sizeof qd, dir);
		snprintf(cmd, sizeof cmd, "mv -n %s %s/", q, qd);
		spawn(cmd);
	}
	Lay *l = lay_find(items[i].name, 0);
	if (l) { l->gx = l->gy = -1; l->sprite[0] = 0; }
	return 1;
}

int items_release(XButtonEvent *e)
{
	if (pressed < 0)
		return 0;
	int i = pressed;
	pressed = -1;
	if (!dragging)
		return 1;
	dragging = 0;
	char odir[700];
	long grid[4];
	if (window_under(e->x_root, e->y_root, odir, sizeof odir, grid)) {
		damage_item(target);
		target = -1;
		if (move_to_dir(i, odir, grid[0], grid[1])) {
			rescan();
		} else {
			damage_item(i);
			items[i].gx = drag_gx0, items[i].gy = drag_gy0;
			damage_item(i);
		}
		return 1;
	}
	if (target >= 0) {
		int t = target;
		target = -1;
		damage_item(t);
		move_into(i, t);
		rescan();
		return 1;
	}
	if (!free_at(items[i].gx, items[i].gy, i)) {      // taken -> snap back
		damage_item(i);
		items[i].gx = drag_gx0, items[i].gy = drag_gy0;
		damage_item(i);
		return 1;
	}
	save_position(i);
	return 1;
}




/*======================================================================
 *  XDND (drops from other apps)
 *======================================================================*/

static void dnd_reply(Atom type, long l1, Atom action)
{
	XEvent r = { 0 };
	r.xclient.type = ClientMessage;
	r.xclient.window = dnd_source;
	r.xclient.message_type = type;
	r.xclient.format = 32;
	r.xclient.data.l[0] = desk;
	r.xclient.data.l[1] = l1;
	if (type == XA_STATUS)
		r.xclient.data.l[4] = action;
	else
		r.xclient.data.l[2] = action;
	XSendEvent(dpy, dnd_source, False, NoEventMask, &r);
}

int items_clientmessage(XClientMessageEvent *e)
{
	if (e->message_type == XA_ENTER) {
		dnd_source = e->data.l[0];
		return 1;
	}
	if (e->message_type == XA_POSITION) {
		dnd_source = e->data.l[0];
		dnd_x = e->data.l[2] >> 16, dnd_y = e->data.l[2] & 0xffff;
		dnd_action = e->data.l[4] == (long)XA_MOVE ? XA_MOVE : XA_COPY;
		int t = item_at(dnd_x, dnd_y, -1);
		if (t >= 0 && items[t].kind != K_DIR) t = -1;
		if (t != target) {
			damage_item(target);
			target = t;
			damage_item(target);
		}
		dnd_reply(XA_STATUS, 1 | 2, dnd_action);
		return 1;
	}
	if (e->message_type == XA_LEAVE) {
		damage_item(target);
		target = -1;
		return 1;
	}
	if (e->message_type == XA_DROP) {
		dnd_source = e->data.l[0];
		XConvertSelection(dpy, XA_SELECTION, XA_URILIST, XA_SELECTION, desk, e->data.l[2]);
		return 1;
	}
	return 0;
}

static int unhex(int c) { return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10) & 15; }




/* ---- paste ----
 * files on the clipboard -> copy (or move if cut) to the desktop
 * text -> typed at the prompt
 */
static Atom XA_CLIPBOARD, XA_TARGETS, XA_PASTE, XA_GNOMEFILES, XA_UTF8;
static int paste_step;                     // 1 = waiting for TARGETS, 2 = for the data
static Atom paste_type;

void items_paste(void)
{
	paste_step = 1;
	XConvertSelection(dpy, XA_CLIPBOARD, XA_TARGETS, XA_PASTE, desk, CurrentTime);
}


static void paste_files(char *s)
{
	int cut = 0;
	char *save = NULL;
	for (char *line = strtok_r(s, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		if (!strcmp(line, "cut")) { cut = 1; continue; }
		if (strncmp(line, "file://", 7) != 0) continue;
		char p[900];
		size_t o = 0;
		const char *u = line + 7;
		if (*u != '/') u = strchr(u, '/');
		for (; u && *u && o + 1 < sizeof p; u++) {
			if (*u == '%' && isxdigit((unsigned char)u[1]) && isxdigit((unsigned char)u[2])) {
				p[o++] = unhex(u[1]) * 16 + unhex(u[2]);
				u += 2;
			} else p[o++] = *u;
		}
		p[o] = 0;
		const char *base = strrchr(p, '/');
		char dst[1300];
		snprintf(dst, sizeof dst, "%s%s", deskdir, base ? base : "/");
		if (!o || !strcmp(p, dst)) continue;
		char qs[1900], qd[1900], cmd[4000];
		quote(qs, sizeof qs, p);
		quote(qd, sizeof qd, deskdir);
		snprintf(cmd, sizeof cmd, "%s %s %s/", cut ? "mv --" : "cp -r --", qs, qd);
		spawn(cmd);
	}
}

static int paste_selection(XSelectionEvent *e)
{
	Atom type; int fmt; unsigned long n, after; unsigned char *d = NULL;
	if (e->property == None) { paste_step = 0; return 1; }
	if (XGetWindowProperty(dpy, desk, e->property, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &after, &d) != Success || !d) {
		paste_step = 0;
		return 1;
	}
	if (paste_step == 1) {                                     // pick the best target
		Atom want = None, *a = (Atom *)d;
		Atom pref[] = { XA_GNOMEFILES, XA_URILIST, XA_UTF8, XA_STRING };
		for (int k = 0; k < 4 && want == None; k++)
			for (unsigned long i = 0; i < n; i++) if (a[i] == pref[k]) { want = pref[k]; break; }
		XFree(d);
		if (want == None) { paste_step = 0; return 1; }
		paste_type = want;
		paste_step = 2;
		XConvertSelection(dpy, XA_CLIPBOARD, want, XA_PASTE, desk, CurrentTime);
		return 1;
	}
	paste_step = 0;
	if (paste_type == XA_GNOMEFILES || paste_type == XA_URILIST) paste_files((char *)d);
	else con_paste((const char *)d);
	XFree(d);
	return 1;
}

int items_selection(XSelectionEvent *e)
{
	if (e->selection == XA_CLIPBOARD && paste_step) return paste_selection(e);
	if (e->selection != XA_SELECTION)
		return 0;
	int t = target, ok = 0;
	target = -1;
	damage_item(t);
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *d = NULL;
	if (e->property != None &&
	    XGetWindowProperty(dpy, desk, e->property, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &after, &d) == Success && d) {
		char *s = (char *)d, *line;
		char *save = NULL;
		for (line = strtok_r(s, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
			if (strncmp(line, "file://", 7) != 0)
				continue;
			char p[900];
			size_t o = 0;
			const char *u = line + 7;
			if (*u != '/') u = strchr(u, '/');          // file://host/path
			for (; u && *u && o + 1 < sizeof p; u++) {
				if (*u == '%' && isxdigit((unsigned char)u[1]) && isxdigit((unsigned char)u[2])) {
					p[o++] = unhex(u[1]) * 16 + unhex(u[2]);
					u += 2;
				} else {
					p[o++] = *u;
				}
			}
			p[o] = 0;
			if (!p[0])
				continue;
			const char *b = strrchr(p, '/');
			b = b ? b + 1 : p;
			char q[1900], qd[1900], cmd[4000], name[256], dst[1200];
			quote(q, sizeof q, p);
			if (t >= 0) {                                   // dropped on a folder icon
				snprintf(dst, sizeof dst, "%s/%s", deskdir, items[t].name);
				quote(qd, sizeof qd, dst);
				snprintf(cmd, sizeof cmd, "%s %s %s/", dnd_action == XA_MOVE ? "mv -n" : "cp -rn", q, qd);
				spawn(cmd);
			} else if (strncmp(p, deskdir, strlen(deskdir)) == 0 && strchr(p + strlen(deskdir) + 1, '/') == NULL) {
				place_new(b, dnd_x, dnd_y);              // already here, just move it
			} else {                                        // elsewhere -> symlink
				unique_name(b, name, sizeof name);
				snprintf(dst, sizeof dst, "%s/%s", deskdir, name);
				if (symlink(p, dst) == 0)
					place_new(name, dnd_x, dnd_y);
			}
			ok = 1;
		}
		XFree(d);
	}
	dnd_reply(XA_FINISHED, ok, ok ? dnd_action : None);
	rescan();
	return 1;
}




/*======================================================================
 *  init + inotify
 *======================================================================*/


void items_init(void)
{
	if (is_folder) snprintf(deskdir, sizeof deskdir, "%s", folder_dir);
	else snprintf(deskdir, sizeof deskdir, "%s/Desktop", a64);
	mkdir(deskdir, 0755);
	XChangeProperty(dpy, desk, XInternAtom(dpy, "_C64_FOLDER", False), XA_STRING, 8, PropModeReplace,
	                (unsigned char *)deskdir, strlen(deskdir));
	XA_AWARE = XInternAtom(dpy, "XdndAware", False);
	XA_ENTER = XInternAtom(dpy, "XdndEnter", False);
	XA_POSITION = XInternAtom(dpy, "XdndPosition", False);
	XA_STATUS = XInternAtom(dpy, "XdndStatus", False);
	XA_LEAVE = XInternAtom(dpy, "XdndLeave", False);
	XA_DROP = XInternAtom(dpy, "XdndDrop", False);
	XA_FINISHED = XInternAtom(dpy, "XdndFinished", False);
	XA_SELECTION = XInternAtom(dpy, "XdndSelection", False);
	XA_COPY = XInternAtom(dpy, "XdndActionCopy", False);
	XA_MOVE = XInternAtom(dpy, "XdndActionMove", False);
	XA_URILIST = XInternAtom(dpy, "text/uri-list", False);
	XA_CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
	XA_TARGETS = XInternAtom(dpy, "TARGETS", False);
	XA_PASTE = XInternAtom(dpy, "C64DESK_PASTE", False);
	XA_GNOMEFILES = XInternAtom(dpy, "x-special/gnome-copied-files", False);
	XA_UTF8 = XInternAtom(dpy, "UTF8_STRING", False);
	long version = 5;
	XChangeProperty(dpy, desk, XA_AWARE, XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
	ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino_fd >= 0)
		inotify_add_watch(ino_fd, deskdir, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB);
}

int items_inotify_fd(void) { return ino_fd; }

void items_inotify(void)
{
	char buf[4096];
	while (read(ino_fd, buf, sizeof buf) > 0)
		rescan_pending = 1;
	if (rescan_pending && !dragging) {
		rescan_pending = 0;
		rescan();
	}
}
