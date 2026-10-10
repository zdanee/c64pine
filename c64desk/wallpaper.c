/*
 * wallpaper.c
 *
 * Picture -> 320x200 in the 16 C64 colours, drawn into the screen area.
 *
 * config/c64/wallpaper  (c64-wallpaper writes it)
 *   image=FILE
 *   slideshow=DIR       wins over image=
 *   minutes=N
 *   border=auto|keep|0-15
 *   dither=1|0
 *
 * SIGUSR1 = next slide. Image loading is stb_image.
 */
#include "c64desk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_FAILURE_USERMSG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define WW 320
#define WH 200
#define MAXFILES 2000

static unsigned char pix[WH][WW];        // colour numbers
static int have, auto_border = -1;
static Pixmap pm;
static int pm_w, pm_h;
static char image[1024], folder[1024], border[16] = "auto", current[1024];
static int minutes = 10, dither = 1;
static long long next_change;
static char (*files)[256];
static int nfiles, pos;

int wall_active(void) { return have; }

int wall_border(void)
{
	if (!have || strcmp(border, "keep") == 0) return -1;
	if (strcmp(border, "auto") == 0) return auto_border;
	int n = atoi(border);
	return n >= 0 && n < 16 ? n : -1;
}



static void read_conf(void)
{
	char f[700], line[1200];
	image[0] = folder[0] = 0;
	snprintf(border, sizeof border, "auto");
	minutes = 10;
	dither = 1;
	path(f, sizeof f, "config/c64/wallpaper");
	FILE *fp = fopen(f, "r");
	while (fp && fgets(line, sizeof line, fp)) {
		line[strcspn(line, "\n")] = 0;
		char *v = strchr(line, '=');
		if (!v) continue;
		*v++ = 0;
		if (strcmp(line, "image") == 0) snprintf(image, sizeof image, "%s", v);
		else if (strcmp(line, "slideshow") == 0) snprintf(folder, sizeof folder, "%s", v);
		else if (strcmp(line, "minutes") == 0) minutes = atoi(v) > 0 ? atoi(v) : 10;
		else if (strcmp(line, "border") == 0) snprintf(border, sizeof border, "%s", v);
		else if (strcmp(line, "dither") == 0) dither = atoi(v) != 0;
	}
	if (fp) fclose(fp);
	if (image[0] == '~') { char t[1024]; snprintf(t, sizeof t, "%s%s", home, image + 1); snprintf(image, sizeof image, "%s", t); }
	if (folder[0] == '~') { char t[1024]; snprintf(t, sizeof t, "%s%s", home, folder + 1); snprintf(folder, sizeof folder, "%s", t); }
}

static int is_image(const char *n)
{
	const char *e = strrchr(n, '.');
	return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png") || !strcasecmp(e, ".bmp") ||
	             !strcasecmp(e, ".gif"));
}

static int cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void list_folder(void)
{
	nfiles = 0;
	if (!files) files = malloc(sizeof *files * MAXFILES);
	DIR *d = opendir(folder);
	struct dirent *de;
	while (d && (de = readdir(d)) && nfiles < MAXFILES)
		if (de->d_name[0] != '.' && is_image(de->d_name))
			snprintf(files[nfiles++], 256, "%s", de->d_name);
	if (d) closedir(d);
	qsort(files, nfiles, sizeof *files, cmp);
}

static void save_pos(void)
{
	char f[700];
	path(f, sizeof f, "cache/c64/slideshow.pos");
	FILE *fp = fopen(f, "w");
	if (fp) { fprintf(fp, "%d\n", pos); fclose(fp); }
}

static int load_pos(void)
{
	char f[700];
	int p = 0;
	path(f, sizeof f, "cache/c64/slideshow.pos");
	FILE *fp = fopen(f, "r");
	if (fp) { if (fscanf(fp, "%d", &p) != 1) p = 0; fclose(fp); }
	return p;
}




/* ---------------------------------------------------------------- */
/* colour conversion */

// "redmean" distance, way better than plain RGB distance for this
static int nearest(int r, int g, int b)
{
	int best = 0;
	long bd = -1;
	for (int i = 0; i < 16; i++) {
		int pr = PAL[i] >> 16 & 255, pg = PAL[i] >> 8 & 255, pb = PAL[i] & 255;
		long rm = (r + pr) / 2, dr = r - pr, dg = g - pg, db = b - pb;
		long d = ((512 + rm) * dr * dr >> 8) + 4 * dg * dg + ((767 - rm) * db * db >> 8);
		if (bd < 0 || d < bd) { bd = d; best = i; }
	}
	return best;
}

/* crop (cx,cy,cw,ch) of an RGB image, box-filter it down to ow x oh and
 * map to the palette with 4x4 bayer dithering. library.c uses it too */
void c64ize(const unsigned char *img, int w, int h, int cx, int cy, int cw, int ch, unsigned char *out, int ow, int oh,
            int dith)
{
	static const int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	for (int y = 0; y < oh; y++) {
		int y0 = cy + (long)y * ch / oh, y1 = cy + (long)(y + 1) * ch / oh;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < ow; x++) {
			int x0 = cx + (long)x * cw / ow, x1 = cx + (long)(x + 1) * cw / ow;
			if (x1 <= x0) x1 = x0 + 1;
			long s[3] = { 0, 0, 0 }, cnt = 0;
			for (int yy = y0; yy < y1 && yy < h; yy++)
				for (int xx = x0; xx < x1 && xx < w; xx++) {
					const unsigned char *p = img + ((size_t)yy * w + xx) * 3;
					s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
					cnt++;
				}
			if (!cnt) cnt = 1;
			int r = s[0] / cnt, g = s[1] / cnt, b = s[2] / cnt;
			int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
			// +-40 looked best. not on (almost) black or white: those would get brown / grey dots
			int d = dith && hi >= 24 && lo <= 232 ? (bayer[y & 3][x & 3] - 8) * 5 : 0;
			r += d, g += d, b += d;
			out[(size_t)y * ow + x] = nearest(r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g,
			                                  b < 0 ? 0 : b > 255 ? 255 : b);
		}
	}
}

static int convert(const char *file)
{
	int w, h, n;
	unsigned char *img = stbi_load(file, &w, &h, &n, 3);
	if (!img) {
		fprintf(stderr, "c64desk: wallpaper %s: %s\n", file, stbi_failure_reason());
		return 0;
	}
	double want = (cols > 0 && rows > 0) ? (double)cols / rows : 1.6;   // aspect of the screen area
	int cw = w, ch = h, cx = 0, cy = 0;
	if ((double)w / h > want) { cw = (int)(h * want); cx = (w - cw) / 2; }
	else { ch = (int)(w / want); cy = (h - ch) / 2; }
	c64ize(img, w, h, cx, cy, cw, ch, &pix[0][0], WW, WH, dither);
	stbi_image_free(img);

	// border=auto: most common colour in the outer 8 px
	int hist[16] = { 0 };
	for (int y = 0; y < WH; y++)
		for (int x = 0; x < WW; x++)
			if (x < 8 || y < 8 || x >= WW - 8 || y >= WH - 8)
				hist[pix[y][x]]++;
	auto_border = 0;
	for (int i = 1; i < 16; i++)
		if (hist[i] > hist[auto_border]) auto_border = i;
	snprintf(current, sizeof current, "%s", file);
	return 1;
}

static void render(void)
{
	int w = cols * C, h = rows * C;
	if (pm) { XFreePixmap(dpy, pm); pm = 0; }
	if (!have || w <= 0 || h <= 0) return;
	pm = XCreatePixmap(dpy, desk, w, h, depth);
	pm_w = w, pm_h = h;
	XImage *im = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, w, h, 32, 0);
	im->data = malloc((size_t)im->bytes_per_line * h);
	for (int y = 0; y < h; y++) {
		int sy = (long)y * WH / h;
		for (int x = 0; x < w; x++)
			XPutPixel(im, x, y, PAL[pix[sy][(long)x * WW / w]]);
	}
	XPutImage(dpy, pm, gc, im, 0, 0, 0, 0, w, h);
	XDestroyImage(im);
}




// called on start, SIGHUP and resize
void wall_load(void)
{
	read_conf();
	have = 0;
	if (folder[0]) {
		list_folder();
		if (nfiles) {
			pos = load_pos() % nfiles;
			char f[1300];
			for (int tries = 0; tries < nfiles && !have; tries++) {          // skip broken files
				snprintf(f, sizeof f, "%s/%s", folder, files[pos]);
				have = convert(f);
				if (!have) pos = (pos + 1) % nfiles;
			}
			save_pos();
			next_change = now_ms() + (long long)minutes * 60000;
		}
	} else if (image[0]) {
		have = convert(image);
	}
	render();
}

void wall_draw(int x, int y, int w, int h)
{
	if (!have || !pm) return;
	int ix = bx, iy = by, iw = pm_w, ih = pm_h;
	int x0 = x > ix ? x : ix, y0 = y > iy ? y : iy;
	int x1 = (x + w < ix + iw) ? x + w : ix + iw, y1 = (y + h < iy + ih) ? y + h : iy + ih;
	if (x1 > x0 && y1 > y0)
		XCopyArea(dpy, pm, canvas, gc, x0 - ix, y0 - iy, x1 - x0, y1 - y0, x0, y0);
}


void wall_next(void)
{
	if (!folder[0]) return;
	list_folder();
	if (!nfiles) return;
	pos = (load_pos() + 1) % nfiles;
	save_pos();
	wall_load();
	desk_recolor();
}

long long wall_next_ms(void) { return (have && folder[0] && nfiles > 1) ? next_change : 0; }

void wall_tick(void)
{
	if (wall_next_ms() && now_ms() >= next_change)
		wall_next();
}
