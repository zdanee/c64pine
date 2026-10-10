/*
 * console.c - the text screen and the command line
 *
 * Where a line goes:
 *   1. built-ins (LOAD"x", POKE 53280.., SYS 64738, CD, CLS, SCREEN)
 *   2. shell, if the first word is a program. TUI apps get a terminal
 *      window, GUI apps are detached, the rest runs on a pty right here
 *   3. everything else is BASIC (vice.c)
 *
 * PETSCII glyphs: Pet Me 64 maps them at U+E000 + screen code (U+E100 lowercase set)
 */
#include "c64desk.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define MAXLINES 600

typedef struct { uint32_t ch; uint8_t fg, rev; } Cell;
typedef struct { Cell *c; int n, cap; } Line;

static Line lines[MAXLINES];
static int nlines, open_line, wcol;     // open_line: last line is unfinished, output continues at wcol
static int cur_fg = -1, cur_rev, lower_set;
static char input[512];
static int inlen, blink_on = 1;
static char hist[32][512];
static int nhist, hpos = -1, typed_line = -1;


// running shell job
static pid_t job_pid = -1;
static int job_fd = -1;
static char job_cmd[600];
static int esc_state, utf_need;
static uint32_t utf_cp;
static char csi[64];
static int csilen;

// for the fake "SEARCHING FOR / LOADING" sequence
typedef struct Timer { long long due; int run; char text[1200]; struct Timer *next; } Timer;
static Timer *timers;




/* ------------------------------------------------------------------ */
/*  the log                                                            */
/* ------------------------------------------------------------------ */

static Line *cur_line(void)
{
	if (!open_line || !nlines) {
		if (nlines == MAXLINES) {
			free(lines[0].c);
			memmove(lines, lines + 1, (MAXLINES - 1) * sizeof *lines);
			nlines--;
		}
		lines[nlines] = (Line){ 0 };
		nlines++;
		open_line = 1;
		wcol = 0;
	}
	return &lines[nlines - 1];
}

#define MAXCELLS 16384             // per line. very long output without newlines gets its start chopped

static void put_cell(uint32_t ch, int fg, int rev)
{
	Line *l = cur_line();
	if (wcol >= MAXCELLS && cols > 0) {
		int drop = (wcol - MAXCELLS / 2) / cols * cols;    // whole rows, keeps wrapping intact
		memmove(l->c, l->c + drop, (l->n - drop) * sizeof *l->c);
		l->n -= drop;
		wcol -= drop;
	}
	if (wcol >= l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		while (wcol >= l->cap) l->cap *= 2;
		l->c = realloc(l->c, l->cap * sizeof *l->c);
	}
	while (l->n < wcol) l->c[l->n++] = (Cell){ ' ', (uint8_t)text_c, 0 };
	l->c[wcol] = (Cell){ ch, (uint8_t)(fg < 0 ? 255 : fg), (uint8_t)rev };
	wcol++;
	if (wcol > l->n) l->n = wcol;
	damage_all();
}

static void newline(void)
{
	cur_line();
	open_line = 0;
	damage_all();
}

void con_clear(void)
{
	for (int i = 0; i < nlines; i++)
		free(lines[i].c);
	nlines = 0;
	open_line = 0;
	damage_all();
}

void con_print(const char *s)
{
	if (open_line && lines[nlines - 1].n)
		newline();
	cur_line();
	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		put_cell(*p, -1, 0);
	newline();
}

void con_ready(void) { con_print("READY."); }


static int petscii_sc(unsigned char p)
{
	if (p >= 32 && p < 64) return p;
	if (p >= 64 && p < 96) return p - 64;
	if (p >= 96 && p < 128) return p - 32;
	if (p >= 160 && p < 192) return p - 64;
	if (p >= 192 && p < 255) return p - 128;
	if (p == 255) return 94;
	return -1;
}

void con_put_petscii(unsigned char c)
{
	static const struct { unsigned char code, colour; } cols_[] = {
		{ 5, 1 }, { 28, 2 }, { 30, 5 }, { 31, 6 }, { 129, 8 }, { 144, 0 }, { 149, 9 }, { 150, 10 }, { 151, 11 },
		{ 152, 12 }, { 153, 13 }, { 154, 14 }, { 155, 15 }, { 156, 4 }, { 158, 7 }, { 159, 3 } };
	for (size_t i = 0; i < sizeof cols_ / sizeof *cols_; i++)
		if (cols_[i].code == c) { cur_fg = cols_[i].colour; return; }
	switch (c) {
	case 13: case 141: newline(); cur_rev = 0; return;
	case 147: con_clear(); return;
	case 18: cur_rev = 1; return;
	case 146: cur_rev = 0; return;
	case 14: lower_set = 1; return;
	case 142: lower_set = 0; return;
	case 20:                                                   // DEL
		if (open_line && wcol > 0) { wcol--; lines[nlines - 1].n = wcol; damage_all(); }
		return;
	case 29: put_cell(' ', cur_fg, 0); return;                // crsr right
	}
	int sc = petscii_sc(c);
	if (sc < 0)
		return;
	uint32_t base = lower_set ? 0xE100 : 0xE000;
	put_cell(base + sc + (cur_rev ? 128 : 0), cur_fg, 0);
}

void con_banner(void)
{
	char rel[32] = "", line[256], l1[128], l2[128];
	unsigned long total = 0, avail = 0;
	FILE *fp = fopen("/etc/alpine-release", "r");
	if (fp) {
		if (fgets(rel, sizeof rel, fp)) rel[strcspn(rel, "\n")] = 0;
		fclose(fp);
	}
	char *dot = strchr(rel, '.');
	if (dot && (dot = strchr(dot + 1, '.'))) *dot = 0;
	if ((fp = fopen("/proc/meminfo", "r"))) {
		while (fgets(line, sizeof line, fp)) {
			sscanf(line, "MemTotal: %lu", &total);
			sscanf(line, "MemAvailable: %lu", &avail);
		}
		fclose(fp);
	}
	if (rel[0]) snprintf(l1, sizeof l1, "**** C64PINE ON ALPINE LINUX %s ****", rel);
	else {                                       // "ON DEBIAN 13" etc
		char id[40] = "LINUX", ver[40] = "", *q;
		if ((fp = fopen("/etc/os-release", "r"))) {
			while (fgets(line, sizeof line, fp)) {
				line[strcspn(line, "\n")] = 0;
				char *v = strchr(line, '=');
				if (!v) continue;
				*v++ = 0;
				if (*v == '"') { v++; if ((q = strchr(v, '"'))) *q = 0; }
				if (strcmp(line, "ID") == 0) snprintf(id, sizeof id, "%s", v);
				else if (strcmp(line, "VERSION_ID") == 0) { snprintf(ver, sizeof ver, " %s", v); if ((q = strchr(ver, '.'))) *q = 0; }
			}
			fclose(fp);
		}
		for (q = id; *q; q++) *q = toupper((unsigned char)*q);
		snprintf(l1, sizeof l1, "**** C64PINE ON %s%s ****", id, ver);
	}
	snprintf(l2, sizeof l2, "%luM RAM SYSTEM  %llu BASIC BYTES FREE", (total + 512) / 1024, (unsigned long long)avail * 1024);
	if ((int)strlen(l2) > cols)
		snprintf(l2, sizeof l2, "%luM RAM  %llu BYTES FREE", (total + 512) / 1024, (unsigned long long)avail * 1024);
	con_print("");
	snprintf(line, sizeof line, "%*s%s", (int)((cols - (int)strlen(l1)) / 2 > 0 ? (cols - strlen(l1)) / 2 : 0), "", l1);
	con_print(line);
	con_print("");
	snprintf(line, sizeof line, "%*s%s", (int)((cols - (int)strlen(l2)) / 2 > 0 ? (cols - strlen(l2)) / 2 : 0), "", l2);
	con_print(line);
	con_print("");
	con_ready();
}

void con_init(void)
{
	if (chdir(home) != 0) {}
}

void con_layout(void)
{
	static int first = 1;
	if (first) {             // banner needs cols
		first = 0;
		con_banner();
	}
}




/* drawing.  lines wrap at cols, the last one also carries the input + cursor */
static int line_rows(int n) { return n <= 0 ? 1 : (n + cols - 1) / cols; }

static void cursor_pos(int *ccol, int *crow);

static void draw_cells(int row, const Cell *c, int n)
{
	int i = 0;
	while (i < n) {
		int j = i;
		uint32_t buf[256];
		int fg = c[i].fg, rev = c[i].rev;
		while (j < n && c[j].fg == fg && c[j].rev == rev && j - i < 256) {
			buf[j - i] = c[j].ch;
			j++;
		}
		unsigned long col = PAL[fg == 255 ? text_c : fg];
		if (rev) draw_text32(canvas, xd, bx + i * C, by + row * C, buf, j - i, PAL[screen_c], (long)col);
		else draw_text32(canvas, xd, bx + i * C, by + row * C, buf, j - i, col, wall_active() ? (long)PAL[screen_c] : -1);
		i = j;
	}
}


static int input_cells(Cell *out, int max)
{
	int n = 0;
	for (int i = 0; i < inlen && n < max; i++)
		out[n++] = (Cell){ (unsigned char)input[i], 255, 0 };
	return n;
}

void con_draw(int x, int y, int w, int h)
{
	(void)x; (void)w;
	static Cell *last;
	static int lastcap;
	int nl = nlines, lastn = 0;

	int need = (open_line && nl ? lines[nl - 1].n : 0) + inlen + 1;
	if (need > lastcap) {
		lastcap = need + 1024;
		last = realloc(last, lastcap * sizeof *last);
	}
	if (open_line && nl) {
		Line *l = &lines[nl - 1];
		memcpy(last, l->c, l->n * sizeof *last);
		lastn = l->n;
		nl--;
	}
	lastn += input_cells(last + lastn, lastcap - lastn);
	int total = line_rows(lastn + 1);                     // +1 for the cursor
	int first = nl;
	while (first > 0 && total + line_rows(lines[first - 1].n) <= rows) {
		first--;
		total += line_rows(lines[first].n);
	}
	int skip = total > rows ? total - rows : 0;           // current line alone is taller than the screen
	int row = 0;
	int r0 = (y - by) / C - 1, r1 = (y + h - by) / C + 1;
	for (int i = first; i <= nl; i++) {
		const Cell *c = i < nl ? lines[i].c : last;
		int n = i < nl ? lines[i].n : lastn;
		int nr = i < nl ? line_rows(n) : line_rows(n + 1);
		for (int k = 0; k < nr; k++, row++) {
			int vr = row - skip;
			if (vr < 0 || vr >= rows || vr < r0 || vr > r1)
				continue;
			int off = k * cols, cnt = n - off < cols ? n - off : cols;
			if (cnt > 0)
				draw_cells(vr, c + off, cnt);
		}
	}
	int cc, cr;
	cursor_pos(&cc, &cr);
	if (blink_on && cr >= 0 && cr < rows)
		fill(text_c, bx + cc * C, by + cr * C, C, C);
}

static void cursor_pos(int *ccol, int *crow)
{
	int nl = nlines, lastn = inlen;
	if (open_line && nl) { lastn += lines[nl - 1].n; nl--; }
	int total = line_rows(lastn + 1), first = nl;
	while (first > 0 && total + line_rows(lines[first - 1].n) <= rows) {
		first--;
		total += line_rows(lines[first].n);
	}
	int above = 0;
	for (int i = first; i < nl; i++) above += line_rows(lines[i].n);
	int skip = total > rows ? total - rows : 0;
	*ccol = lastn % cols;
	*crow = above + lastn / cols - skip;
}

void con_blink(void)
{
	int cc, cr;
	blink_on = !blink_on;
	cursor_pos(&cc, &cr);
	damage(bx + cc * C, by + cr * C, C, C);
}




static void after(int ms, int run, const char *text)
{
	Timer *t = calloc(1, sizeof *t), **pp = &timers;
	t->due = now_ms() + ms;
	t->run = run;
	snprintf(t->text, sizeof t->text, "%s", text);
	while (*pp && (*pp)->due <= t->due) pp = &(*pp)->next;
	t->next = *pp;
	*pp = t;
}

void con_load(const char *label, const char *cmd)
{
	char line[128];
	snprintf(line, sizeof line, "SEARCHING FOR %s", label);
	after(150, 0, line);
	after(600, 0, "LOADING");
	after(900, 1, cmd);
	after(950, 0, "READY.");
}




/* ------------------------------------------------------------------ */
/*  shell jobs                                                         */
/* ------------------------------------------------------------------ */

static const char *TUI[] = { "mc", "htop", "top", "vim", "vi", "nvim", "nano", "less", "more", "man", "nmtui", "bluetuith",
	"links", "lynx", "w3m", "ssh", "alsamixer", "ncdu", "tmux", "screen", "watch", "sh", "ash", "bash", "zsh", "python",
	"python3", "lua", "node", "ftp", "telnet", "doas", "su", "sudo", "passwd", "fdisk", "cfdisk", "crontab", "info",
	"vimtutor", "emacs", "mutt", "irssi", "nmcli-tui", "iftop", "nethogs", "btop", "opencode", "opencode2", NULL };

static int in_list(const char *w, const char **l)
{
	for (; *l; l++)
		if (strcmp(w, *l) == 0) return 1;
	return 0;
}

static int find_exe(const char *word, char *out, size_t n)
{
	if (strchr(word, '/')) {
		snprintf(out, n, "%s", word);
		return access(word, X_OK) == 0;
	}
	const char *pv = getenv("PATH");
	if (!pv) pv = "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";
	while (*pv) {
		size_t k = strcspn(pv, ":");
		snprintf(out, n, "%.*s/%s", (int)k, pv, word);
		struct stat st;
		if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0)
			return 1;
		pv += k + (pv[k] == ':');
	}
	return 0;
}

/* GUI program? Yes if a .desktop file runs it or it links libX11/gtk/qt.
 * Crude but works well enough. */
static int is_gui(const char *word, const char *exe)
{
	DIR *d = opendir("/usr/share/applications");
	struct dirent *de;
	char p[600], line[600];
	int gui = 0;
	while (d && !gui && (de = readdir(d))) {
		if (!strstr(de->d_name, ".desktop")) continue;
		snprintf(p, sizeof p, "/usr/share/applications/%s", de->d_name);
		FILE *fp = fopen(p, "r");
		while (fp && fgets(line, sizeof line, fp)) {
			if (strncmp(line, "Exec=", 5) == 0) {
				char w[256] = "";
				sscanf(line + 5, "%255s", w);
				const char *b = strrchr(w, '/');
				if (strcmp(b ? b + 1 : w, word) == 0) gui = 1;
				break;
			}
		}
		if (fp) fclose(fp);
	}
	if (d) closedir(d);
	if (gui)
		return 1;
	FILE *fp = fopen(exe, "rb");
	if (!fp)
		return 0;
	static char buf[1 << 17];
	size_t n = fread(buf, 1, sizeof buf, fp);
	fclose(fp);
	if (n < 4 || memcmp(buf, "\177ELF", 4) != 0)
		return 0;
	static const char *libs[] = { "libX11.so", "libgtk-3.so", "libgtk-4.so", "libQt5Gui", "libQt6Gui", "libSDL2", NULL };
	for (int i = 0; libs[i]; i++)
		for (size_t k = 0; k + strlen(libs[i]) < n; k++)
			if (buf[k] == libs[i][0] && memcmp(buf + k, libs[i], strlen(libs[i])) == 0)
				return 1;
	return 0;
}

static void in_terminal(const char *cmd)
{
	char q[1400], c[1600], cwd[600];
	if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "%s", home);
	size_t o = 0;
	q[o++] = '\'';
	for (const char *s = cmd; *s && o + 6 < sizeof q; s++) {
		if (*s == '\'') { memcpy(q + o, "'\\''", 4); o += 4; } else q[o++] = *s;
	}
	q[o++] = '\'';
	q[o] = 0;
	snprintf(c, sizeof c, "exec c64-term -e sh -c %s", q);
	spawn(c);
}

static void start_job(const char *cmd)
{
	struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
	int fd;
	pid_t pid = forkpty(&fd, NULL, NULL, &ws);
	if (pid < 0) {
		con_print("?OUT OF MEMORY  ERROR");
		con_ready();
		return;
	}
	if (pid == 0) {
		char c[16], r[16];
		snprintf(c, sizeof c, "%d", cols);
		snprintf(r, sizeof r, "%d", rows);
		setenv("TERM", "dumb", 1);
		setenv("COLUMNS", c, 1);
		setenv("LINES", r, 1);
		setenv("PAGER", "cat", 1);
		child_signals();
		struct termios tio;                      // no echo, we already show the typed line
		if (tcgetattr(0, &tio) == 0) {
			tio.c_lflag &= ~(ECHO | ECHONL);
			tcsetattr(0, TCSANOW, &tio);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	job_pid = pid;
	job_fd = fd;
	snprintf(job_cmd, sizeof job_cmd, "%s", cmd);
	esc_state = 0;
	utf_need = 0;
	cur_fg = -1;
	cur_rev = 0;
	lower_set = 0;
}

static void end_job(int ready)
{
	if (job_fd >= 0) close(job_fd);
	job_fd = -1;
	job_pid = -1;
	cur_fg = -1;
	cur_rev = 0;
	if (open_line && lines[nlines - 1].n) newline();
	open_line = 0;
	if (ready) con_ready();
}

// job turned out to be a fullscreen thing after all -> move it to a terminal
static void to_terminal(void)
{
	kill(-job_pid, SIGKILL);
	char cmd[600];
	snprintf(cmd, sizeof cmd, "%s", job_cmd);
	if (open_line) lines[nlines - 1].n = 0;
	end_job(0);
	con_print("OPENING A TERMINAL WINDOW");
	in_terminal(cmd);
	con_ready();
}

static int ansi_colour(int code)
{
	static const int map[8] = { 0, 10, 13, 7, 3, 4, 3, 1 };   // ANSI -> C64 colours
	if (code >= 30 && code <= 37) return map[code - 30];
	if (code >= 90 && code <= 97) return map[code - 90];
	return -1;
}

static void job_bytes(const unsigned char *b, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned char c = b[i];
		if (esc_state == 1) {
			if (c == '[') { esc_state = 2; csilen = 0; }
			else if (c == ']') esc_state = 3;
			else esc_state = 0;
			continue;
		}
		if (esc_state == 2) {                        // CSI
			if (c >= 0x40 && c <= 0x7e) {
				csi[csilen] = 0;
				esc_state = 0;
				if (c == 'm') {
					char *s = csi;
					if (!*s) { cur_fg = -1; cur_rev = 0; }
					while (*s) {
						int v = atoi(s);
						if (v == 0 || v == 39) cur_fg = v == 0 ? (cur_rev = 0, -1) : -1;
						else if (v == 7) cur_rev = 1;
						else if (v == 27) cur_rev = 0;
						else if (ansi_colour(v) >= 0) cur_fg = ansi_colour(v);
						s = strchr(s, ';');
						if (!s) break;
						s++;
					}
				} else if (c == 'K') {
					if (open_line) { lines[nlines - 1].n = wcol; damage_all(); }
				} else if (strchr("HfJABCDrlhd", c)) {
					to_terminal();                    // cursor positioning or alt screen
					return;
				}
			} else if (csilen < (int)sizeof csi - 1) {
				csi[csilen++] = c;
			}
			continue;
		}
		if (esc_state == 3) {                        // OSC, ends with BEL or ESC backslash
			if (c == 7) esc_state = 0;
			else if (c == 27) esc_state = 1;
			continue;
		}
		if (utf_need) {
			if ((c & 0xc0) == 0x80) {
				utf_cp = utf_cp << 6 | (c & 0x3f);
				if (--utf_need == 0) put_cell(utf_cp, cur_fg, cur_rev);
				continue;
			}
			utf_need = 0;
		}
		if (c == 27) { esc_state = 1; continue; }
		if (c == '\n') { newline(); continue; }
		if (c == '\r') { cur_line(); wcol = 0; continue; }
		if (c == '\b') { if (wcol > 0) wcol--; continue; }
		if (c == '\t') { cur_line(); do put_cell(' ', -1, 0); while (wcol % 8); continue; }
		if (c < 32 || c == 127) continue;
		if (c >= 0xc0) {
			utf_need = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
			utf_cp = c & (0x3f >> utf_need);
			continue;
		}
		if (c >= 0x80) continue;
		put_cell(c, cur_fg, cur_rev);
	}
}

int con_job_fd(void) { return job_fd; }

void con_job_input(void)
{
	unsigned char buf[4096];
	for (int k = 0; k < 16 && job_fd >= 0; k++) {
		ssize_t n = read(job_fd, buf, sizeof buf);
		if (n > 0) { job_bytes(buf, n); continue; }
		if (n < 0 && errno == EAGAIN) return;
		end_job(1);
		return;
	}
}




/* ------------------------------------------------------------------ */
/*  dispatch                                                           */
/* ------------------------------------------------------------------ */

static const char *BASIC_WORDS[] = { "END", "FOR", "NEXT", "DATA", "INPUT", "DIM", "READ", "LET", "GOTO", "RUN", "IF",
	"RESTORE", "GOSUB", "RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE", "VERIFY", "DEF", "POKE", "PRINT", "CONT",
	"LIST", "CLR", "CMD", "SYS", "OPEN", "CLOSE", "GET", "NEW", "VERIFY", NULL };

static void upper(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; s[i] && i + 1 < n; i++) d[i] = toupper((unsigned char)s[i]);
	d[i] = 0;
}

static int builtin(const char *line)
{
	char u[512];
	int a, b;
	upper(u, line, sizeof u);
	char *e = u + strlen(u);
	while (e > u && e[-1] == ' ') *--e = 0;
	if (strncmp(u, "LOAD", 4) == 0) {
		char *q1 = strchr(u, '"');
		if (!q1) return 0;
		char *q2 = strchr(q1 + 1, '"');
		char name[128];
		snprintf(name, sizeof name, "%.*s", (int)(q2 ? q2 - q1 - 1 : (long)strlen(q1 + 1)), q1 + 1);
		size_t nl = strlen(name);                     // LOAD"X.SAV" = listing
		if (nl > 4 && strcasecmp(name + nl - 4, ".SAV") == 0) {
			char f[1200], dirs[2][700];
			snprintf(dirs[0], sizeof dirs[0], "%s/Desktop", a64);
			if (!getcwd(dirs[1], sizeof dirs[1])) dirs[1][0] = 0;
			for (int d = 0; d < 2; d++) {
				DIR *dp = opendir(dirs[d]);
				struct dirent *de;
				while (dp && (de = readdir(dp)))
					if (strcasecmp(de->d_name, name) == 0) {
						snprintf(f, sizeof f, "%s/%s", dirs[d], de->d_name);
						closedir(dp);
						vice_load_listing(f);
						return 1;
					}
				if (dp) closedir(dp);
			}
			con_print("?FILE NOT FOUND  ERROR");
			con_ready();
			return 1;
		}
		return items_open_label(name);                   // no icon? then BASIC loads it from drive 8
	}
	if ((sscanf(u, "POKE %d , %d", &a, &b) == 2 || sscanf(u, "POKE%d,%d", &a, &b) == 2) &&
	    (a == 53280 || a == 53281 || a == 646)) {
		b &= 15;
		if (a == 53280) border_c = b;
		else if (a == 53281) screen_c = b;
		else text_c = b;
		desk_reset_colors();
		con_ready();
		return 1;
	}
	if (strncmp(u, "SYS", 3) == 0 && atoi(u + 3 + strspn(u + 3, " ")) == 64738) {
		con_clear();
		con_banner();
		vice_reset();
		return 1;
	}
	if (strcmp(u, "CLS") == 0) { con_clear(); return 1; }
	if (strcmp(u, "SCREEN") == 0) { vice_screen_toggle(1); con_ready(); return 1; }
	if (strcmp(u, "CD") == 0 || strncmp(u, "CD ", 3) == 0) {
		const char *d = line + 2;
		while (*d == ' ') d++;
		char p[700];
		if (!*d) snprintf(p, sizeof p, "%s", home);
		else if (*d == '~') snprintf(p, sizeof p, "%s%s", home, d + 1);
		else snprintf(p, sizeof p, "%s", d);
		if (chdir(p) != 0) con_print("?DIRECTORY NOT FOUND  ERROR");
		con_ready();
		return 1;
	}
	return 0;
}

static void dispatch(const char *line)
{
	const char *s = line;
	while (*s == ' ') s++;
	if (!*s)
		return;
	if (builtin(s))
		return;
	char word[256] = "", exe[700];
	sscanf(s, "%255s", word);
	// line numbers and UPPERCASE keywords are BASIC even if a program with that name exists
	int kw = 0;
	char w[32];
	int k = 0;
	while (s[k] && isalpha((unsigned char)s[k]) && k < 31) { w[k] = s[k]; k++; }
	w[k] = 0;
	if (k && !strpbrk(w, "abcdefghijklmnopqrstuvwxyz"))
		for (int i = 0; BASIC_WORDS[i]; i++)
			if (strcmp(w, BASIC_WORDS[i]) == 0) kw = 1;
	if (!isdigit((unsigned char)s[0]) && !kw && s[0] != '?' && find_exe(word, exe, sizeof exe)) {
		const char *b = strrchr(word, '/');
		b = b ? b + 1 : word;
		size_t l = strlen(s);
		if (l && s[l - 1] == '&') { spawn(s); con_ready(); return; }
		if (in_list(b, TUI)) { con_print("OPENING A TERMINAL WINDOW"); in_terminal(s); con_ready(); return; }
		if (is_gui(b, exe)) { spawn(s); con_ready(); return; }
		start_job(s);
		return;
	}
	if (typed_line >= 0 && typed_line < nlines)            // uppercase like a real C64
		for (int i = 0; i < lines[typed_line].n; i++)
			if (lines[typed_line].c[i].ch < 128) lines[typed_line].c[i].ch = toupper((int)lines[typed_line].c[i].ch);
	damage_all();
	vice_line(s);
}




/* ------------------------------------------------------------------ */
/*  keyboard                                                           */
/* ------------------------------------------------------------------ */

static unsigned char key_petscii(KeySym ks, const char *buf, int n)
{
	switch (ks) {
	case XK_Return: case XK_KP_Enter: return 13;
	case XK_BackSpace: case XK_Delete: return 20;
	case XK_Up: return 145;
	case XK_Down: return 17;
	case XK_Left: return 157;
	case XK_Right: return 29;
	case XK_Home: return 19;
	case XK_F1: return 133;
	case XK_F3: return 134;
	case XK_F5: return 135;
	case XK_F7: return 136;
	}
	if (n != 1) return 0;
	unsigned char c = buf[0];
	if (c >= 'a' && c <= 'z') return c - 32;
	if (c >= 'A' && c <= 'Z') return c + 128;            // shift = graphics chars
	if (c >= 32 && c < 96) return c;
	return 0;
}

// RETURN
void con_enter(void)
{
	char line[512];
	snprintf(line, sizeof line, "%s", input);

	for (int i = 0; i < inlen; i++) put_cell((unsigned char)input[i], -1, 0);
	typed_line = nlines - 1;
	newline();
	inlen = 0;
	input[0] = 0;
	hpos = -1;
	if (line[0] && (!nhist || strcmp(hist[(nhist - 1) % 32], line) != 0))
		snprintf(hist[nhist++ % 32], sizeof hist[0], "%s", line);
	if (job_fd >= 0) {
		size_t l = strlen(line);
		line[l] = '\n';
		if (write(job_fd, line, l + 1)) {}
	} else if (vice_busy()) {
		vice_line(line);
	} else {
		dispatch(line);
	}
}

/* paste = typing it in, newline = RETURN. So BASIC listings just work */
void con_paste(const char *t)
{
	for (; *t; t++) {
		unsigned char c = *t;
		if (c == '\n') { con_enter(); continue; }
		if (c == '\r') continue;
		if (c == '\t') c = ' ';
		if (c < 32 || c == 127 || c >= 0x80) continue;
		if (inlen < (int)sizeof input - 1) { input[inlen++] = c; input[inlen] = 0; }
	}
	blink_on = 1;
	damage_all();
}

void con_key(XKeyEvent *e)
{
	char buf[16];
	KeySym ks;
	int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
	int ctrl = e->state & ControlMask;
	// RUN/STOP
	if (ks == XK_Escape || ks == XK_Pause || (ctrl && (ks == XK_c || ks == XK_C))) {
		if (job_fd >= 0) { char c = 3; if (write(job_fd, &c, 1)) {} kill(-job_pid, SIGINT); }
		else if (vice_busy()) vice_stop_key();
		else { inlen = 0; input[0] = 0; damage_all(); }
		return;
	}
	if (ctrl && (ks == XK_l || ks == XK_L)) { con_clear(); return; }
	if (ctrl && (ks == XK_d || ks == XK_D) && job_fd >= 0) { char c = 4; if (write(job_fd, &c, 1)) {} return; }
	if (vice_wants_keys() && job_fd < 0) {                                // e.g. GET loops
		unsigned char p = key_petscii(ks, buf, n);
		if (p) vice_key(p);
		return;
	}
	if ((ctrl && (ks == XK_v || ks == XK_V)) || ((e->state & ShiftMask) && ks == XK_Insert)) {
		items_paste();
		return;
	}
	if (ks == XK_Return || ks == XK_KP_Enter) {
		con_enter();
	} else if (ks == XK_BackSpace) {
		if (inlen) input[--inlen] = 0;
	} else if (ks == XK_Up || ks == XK_Down) {                           // history

		int count = nhist < 32 ? nhist : 32;
		if (!count) return;
		if (ks == XK_Up) hpos = hpos < 0 ? 0 : (hpos + 1 < count ? hpos + 1 : hpos);
		else hpos = hpos > 0 ? hpos - 1 : -1;
		if (hpos < 0) input[0] = 0;
		else snprintf(input, sizeof input, "%s", hist[(nhist - 1 - hpos) % 32]);
		inlen = strlen(input);
	} else if (n == 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127 && inlen < (int)sizeof input - 1) {
		input[inlen++] = buf[0];
		input[inlen] = 0;
	} else {
		return;
	}
	blink_on = 1;
	damage_all();
}

void con_tick(void)
{
	long long t = now_ms();
	while (timers && timers->due <= t) {
		Timer *tm = timers;
		timers = tm->next;
		if (tm->run) spawn(tm->text);
		else con_print(tm->text);
		free(tm);
	}
}
