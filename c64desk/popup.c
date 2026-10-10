/*
 * popup.c   c64desk --popup clock|volume|power|note
 *
 * The little windows of the bar. Clicking the bar item again closes it.
 *
 * calendar file: config/c64/calendar, lines "YYYY-MM-DD HH:MM TYPE TEXT"
 * anything that needs root goes through c64-root (-> c64-sys)
 */
#include "c64desk.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXB 64
typedef struct { int x, y, w, id; } Btn;

static Window win;
static Pixmap pcv;
static XftDraw *pxd;
static int pw_, ph_, ncols, nrows, kind_, regrab;
static Btn btns[MAXB];
static int nb;
static char status_[96];
enum { K_CLOCK, K_VOLUME, K_POWER, K_NOTE };



static void txt(int col, int row, const char *s, int rev, int colour)
{
	uint32_t u[128];
	int n = 0;
	for (; *s && n < 128 && col + n < ncols; s++)
		u[n++] = (*s >= 'a' && *s <= 'z') ? *s - 32 : (unsigned char)*s;
	unsigned long fg = PAL[colour < 0 ? text_c : colour], bg = PAL[screen_c];
	draw_text32(pcv, pxd, 2 * P + col * C, 2 * P + row * C, u, n, rev ? bg : fg, rev ? (long)fg : (long)bg);
}

static void button(int col, int row, const char *label, int id)
{
	txt(col, row, label, 1, -1);
	if (nb < MAXB) btns[nb++] = (Btn){ 2 * P + col * C, 2 * P + row * C, (int)strlen(label) * C, id };
}

static void hline(int row)
{
	XSetForeground(dpy, gc, PAL[text_c]);
	XFillRectangle(dpy, pcv, gc, 2 * P, 2 * P + row * C + C / 2, ncols * C, P);
}

static int hit(int x, int y)
{
	for (int i = 0; i < nb; i++)
		if (x >= btns[i].x && x < btns[i].x + btns[i].w && y >= btns[i].y && y < btns[i].y + C)
			return btns[i].id;
	return -1;
}

static void run_out(const char *cmd)      // 1st line of output -> status
{
	FILE *p = popen(cmd, "r");
	status_[0] = 0;
	if (p) {
		if (!fgets(status_, sizeof status_, p)) status_[0] = 0;
		int rc = pclose(p);
		status_[strcspn(status_, "\n")] = 0;
		if (!status_[0]) snprintf(status_, sizeof status_, rc == 0 ? "DONE" : "?FAILED");
	}
}




/* ======================= CLOCK / CALENDAR ======================= */

static int vy, vm, sy, sm, sd;                        // viewed month, selected day
static char fmt_[64];

static void cal_path(char *f, size_t n) { path(f, n, "config/c64/calendar"); }

typedef struct { char line[200]; int hh, mm; } Ev;

static int day_events(int y, int m, int d, Ev *ev, int max)
{
	char f[700], line[256], key[16];
	int n = 0;
	snprintf(key, sizeof key, "%04d-%02d-%02d ", y, m, d);
	cal_path(f, sizeof f);
	FILE *fp = fopen(f, "r");
	while (fp && fgets(line, sizeof line, fp) && n < max)
		if (!strncmp(line, key, 11)) {
			line[strcspn(line, "\n")] = 0;
			snprintf(ev[n].line, sizeof ev[n].line, "%s", line);
			if (sscanf(line + 11, "%d:%d", &ev[n].hh, &ev[n].mm) != 2) ev[n].hh = 99;
			n++;
		}
	if (fp) fclose(fp);
	for (int i = 1; i < n; i++)                        // sort by time
		for (int j = i; j > 0 && ev[j].hh * 60 + ev[j].mm < ev[j - 1].hh * 60 + ev[j - 1].mm; j--) {
			Ev t = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = t;
		}
	return n;
}

static int has_events(int y, int m, int d) { Ev e; return day_events(y, m, d, &e, 1); }

static void cal_delete(const char *exact)
{
	char f[700], tmp[720], line[256];
	cal_path(f, sizeof f);
	snprintf(tmp, sizeof tmp, "%s.tmp", f);
	FILE *in = fopen(f, "r"), *out = fopen(tmp, "w");
	int done = 0;
	while (in && out && fgets(line, sizeof line, in)) {
		char l2[256];
		snprintf(l2, sizeof l2, "%s", line);
		l2[strcspn(l2, "\n")] = 0;
		if (!done && !strcmp(l2, exact)) { done = 1; continue; }
		fputs(line, out);
	}
	if (in) fclose(in);
	if (out) { fclose(out); rename(tmp, f); }
}

static const char *MONTHS[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER",
	"OCTOBER", "NOVEMBER", "DECEMBER" };
enum { B_PREV = 1000, B_NEXT, B_ADD, B_FORMAT, B_SET, B_NTP, B_DEL = 2000, B_DAY = 3000 };

static void read_fmt(void)
{
	char f[700];
	path(f, sizeof f, "config/c64/clock-format");
	FILE *fp = fopen(f, "r");
	fmt_[0] = 0;
	if (fp) { if (fgets(fmt_, sizeof fmt_, fp)) fmt_[strcspn(fmt_, "\n")] = 0; fclose(fp); }
	if (!fmt_[0]) snprintf(fmt_, sizeof fmt_, "TI$");
}

static void draw_clock(void)
{
	char t[64];
	time_t now = time(NULL);
	struct tm tn = *localtime(&now);
	snprintf(t, sizeof t, " %s %d", MONTHS[vm - 1], vy);
	txt(0, 0, t, 1, -1);
	for (int i = strlen(t); i < ncols; i++) txt(i, 0, " ", 1, -1);
	button(ncols - 6, 0, " < ", B_PREV);
	button(ncols - 3, 0, " > ", B_NEXT);
	txt(4, 1, "MO TU WE TH FR SA SU", 0, 3);
	struct tm first = { .tm_year = vy - 1900, .tm_mon = vm - 1, .tm_mday = 1, .tm_hour = 12 };
	mktime(&first);
	int off = (first.tm_wday + 6) % 7, days = 31;
	for (int d = 28; d <= 31; d++) {
		struct tm x = { .tm_year = vy - 1900, .tm_mon = vm - 1, .tm_mday = d, .tm_hour = 12 };
		mktime(&x);
		if (x.tm_mon != vm - 1) { days = d - 1; break; }
	}
	for (int d = 1; d <= days; d++) {
		int c = off + d - 1, col = 4 + (c % 7) * 3, row = 2 + c / 7;
		int today = vy == tn.tm_year + 1900 && vm == tn.tm_mon + 1 && d == tn.tm_mday;
		int sel = vy == sy && vm == sm && d == sd;
		snprintf(t, sizeof t, "%2d", d);
		txt(col, row, t, today || sel, sel && !today ? 3 : -1);
		if (has_events(vy, vm, d)) {
			XSetForeground(dpy, gc, PAL[7]);
			XFillRectangle(dpy, pcv, gc, 2 * P + (col + 2) * C, 2 * P + row * C + C / 2 - P, 2 * P, 2 * P);
		}
		if (nb < MAXB) btns[nb++] = (Btn){ 2 * P + col * C, 2 * P + row * C, 2 * C, B_DAY + d };
	}
	hline(8);
	snprintf(t, sizeof t, "%04d-%02d-%02d", sy, sm, sd);
	txt(0, 9, t, 0, 3);
	Ev ev[6];
	int n = day_events(sy, sm, sd, ev, 6);
	if (!n) txt(0, 10, "NOTHING PLANNED", 0, -1);
	for (int i = 0; i < n; i++) {
		txt(0, 10 + i, ev[i].line + 11, 0, -1);
		button(ncols - 3, 10 + i, " X ", B_DEL + i);
	}
	button(0, 17, " ADD ", B_ADD);
	snprintf(t, sizeof t, " FORMAT: %s ", fmt_);
	button(6, 17, t, B_FORMAT);
	button(0, 18, " SET TIME ", B_SET);
	button(11, 18, " NTP SYNC ", B_NTP);
	txt(0, 19, status_, 0, 7);
}


/* ADD: pick a type, then "HH:MM TEXT" */
static char add_type[16];
static void popup_redraw(void);

static void cb_add_text(const char *text, void *arg)
{
	(void)arg;
	regrab = 1;
	if (!text || !text[0]) return;
	int hh = -1, mm = -1, k = 0;
	if (sscanf(text, "%d:%d %n", &hh, &mm, &k) < 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
		snprintf(status_, sizeof status_, "?TIME FIRST, LIKE 14:30");
		return;
	}
	char f[700];
	cal_path(f, sizeof f);
	FILE *fp = fopen(f, "a");
	if (fp) { fprintf(fp, "%04d-%02d-%02d %02d:%02d %s %s\n", sy, sm, sd, hh, mm, add_type, text + k); fclose(fp); }
	status_[0] = 0;
}

static void cb_add_type(int choice, void *arg)
{
	(void)arg;
	static const char *types[] = { "APPOINTMENT", "MEETING", "EVENT", "ALARM" };
	if (choice < 0 || choice > 3) { regrab = 1; return; }
	snprintf(add_type, sizeof add_type, "%s", types[choice]);
	ui_input("TIME AND TEXT (14:30 DENTIST):", "", cb_add_text, NULL);
}

static void cb_format(const char *text, void *arg)
{
	(void)arg;
	regrab = 1;
	if (!text) return;
	char f[700];
	path(f, sizeof f, "config/c64/clock-format");
	FILE *fp = fopen(f, "w");
	if (fp) { fprintf(fp, "%s\n", text[0] ? text : "TI$"); fclose(fp); }
	read_fmt();
	snprintf(status_, sizeof status_, "THE BAR SHOWS IT IN A MOMENT");
}

static void cb_settime(const char *text, void *arg)
{
	(void)arg;
	regrab = 1;
	if (!text || !text[0]) return;
	char cmd[200];
	if (strchr(text, '\'')) return;
	snprintf(cmd, sizeof cmd, "c64-root time '%s' 2>&1", text);
	run_out(cmd);
}

static void clock_click(int id)
{
	static const char *types[] = { "APPOINTMENT", "MEETING", "EVENT", "ALARM (BEEPS)" };
	if (id == B_PREV) { if (--vm < 1) { vm = 12; vy--; } }
	else if (id == B_NEXT) { if (++vm > 12) { vm = 1; vy++; } }
	else if (id >= B_DAY) { sy = vy, sm = vm, sd = id - B_DAY; }
	else if (id >= B_DEL) {
		Ev ev[6];
		int n = day_events(sy, sm, sd, ev, 6);
		if (id - B_DEL < n) cal_delete(ev[id - B_DEL].line);
	}
	else if (id == B_ADD) {
		XWindowAttributes wa;
		XGetWindowAttributes(dpy, win, &wa);
		ui_menu(wa.x + 2 * C, wa.y + 10 * C, "ADD", types, 4, cb_add_type, NULL);
	}
	else if (id == B_FORMAT) ui_input("FORMAT (HH:MM:SS, YYYY-MM-DD HH:MM, TI$):", fmt_, cb_format, NULL);
	else if (id == B_SET) {
		char now[32];
		time_t t = time(NULL);
		strftime(now, sizeof now, "%Y-%m-%d %H:%M", localtime(&t));
		ui_input("NEW DATE AND TIME:", now, cb_settime, NULL);
	}
	else if (id == B_NTP) {
		snprintf(status_, sizeof status_, "SYNCING...");
		popup_redraw();
		XFlush(dpy);
		run_out("timeout 25 c64-root ntp 2>&1 | tail -1");
	}
}




/* ======================= VOLUME ======================= */

static int vol = -1, muted;

static void vol_read(void)
{
	FILE *p = popen("c64-volume get 2>/dev/null", "r");          // "40" or "40 MUTE"
	char line[64] = "";
	vol = -1, muted = 0;
	if (p && fgets(line, sizeof line, p) && line[0] >= '0' && line[0] <= '9') {
		vol = atoi(line);
		muted = strstr(line, "MUTE") != NULL;
	}
	if (p) pclose(p);
}

static void vol_set(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	char cmd[64];
	snprintf(cmd, sizeof cmd, "c64-volume set %d", v);
	if (system(cmd)) {}
	vol = v, muted = 0;
}

enum { B_MUTE = 100 };
static int bar_x0, bar_w, bar_y;

static void draw_volume(void)
{
	char t[64];
	if (vol < 0) { txt(0, 0, " NO SOUND CARD ", 1, -1); return; }
	snprintf(t, sizeof t, " VOLUME %d%%%s", vol, muted ? " (MUTED)" : "");
	txt(0, 0, t, 1, -1);
	for (int i = strlen(t); i < ncols; i++) txt(i, 0, " ", 1, -1);
	int cells = ncols - 2, full = cells * vol / 100;
	bar_x0 = 2 * P + C, bar_w = cells * C, bar_y = 2 * P + 2 * C;
	XSetForeground(dpy, gc, PAL[text_c]);
	XFillRectangle(dpy, pcv, gc, bar_x0 - P, bar_y - P, bar_w + 2 * P, C + 2 * P);
	XSetForeground(dpy, gc, PAL[screen_c]);
	XFillRectangle(dpy, pcv, gc, bar_x0, bar_y, bar_w, C);
	XSetForeground(dpy, gc, PAL[muted ? 11 : text_c]);
	XFillRectangle(dpy, pcv, gc, bar_x0, bar_y, full * C, C);
	button(0, 4, muted ? " UNMUTE " : " MUTE ", B_MUTE);
	txt(ncols - 14, 4, "WHEEL / ARROWS", 0, 3);
}

static void vol_from_x(int x)
{
	int v = (x - bar_x0) * 100 / (bar_w ? bar_w : 1);
	if (v != vol) vol_set(v);
}




/* ======================= POWER ======================= */

#define CPUF "/sys/devices/system/cpu"
static int confirm = -1;
enum { B_GOV = 200, B_TURBO = 300, B_PROF = 400, B_SLEEP = 500, B_REBOOT, B_OFF, B_LOCK = 600 };
static int can_lock = -1;   // -1 = not asked yet

static int rd(const char *f, char *out, size_t n)
{
	FILE *fp = fopen(f, "r");
	out[0] = 0;
	if (!fp) return 0;
	if (!fgets(out, n, fp)) out[0] = 0;
	fclose(fp);
	out[strcspn(out, "\n")] = 0;
	return out[0] != 0;
}

static char govs[8][24], profs[8][24];
static int ngov, nprof;

static int split(const char *s, char out[][24], int max)
{
	int n = 0;
	char b[256];
	snprintf(b, sizeof b, "%s", s);
	for (char *t = strtok(b, " "); t && n < max; t = strtok(NULL, " ")) snprintf(out[n++], 24, "%s", t);
	return n;
}

static void draw_power(void)
{
	char t[128], v[256], v2[64];
	int row = 0;
	txt(0, row, " POWER", 1, -1);
	for (int i = 6; i < ncols; i++) txt(i, row, " ", 1, -1);
	row += 2;

	// battery
	int bats = 0;
	for (int b = 0; b < 4; b++) {
		char f[128];
		snprintf(f, sizeof f, "/sys/class/power_supply/BAT%d/capacity", b);
		if (!rd(f, v, sizeof v)) continue;
		bats++;
		snprintf(f, sizeof f, "/sys/class/power_supply/BAT%d/status", b);
		rd(f, v2, sizeof v2);
		snprintf(t, sizeof t, "BATTERY %s%%  %s", v, v2);
		txt(0, row++, t, 0, -1);
		long now_ = 0, rate = 0;
		snprintf(f, sizeof f, "/sys/class/power_supply/BAT%d/energy_now", b);
		if (rd(f, v, sizeof v)) now_ = atol(v);
		snprintf(f, sizeof f, "/sys/class/power_supply/BAT%d/power_now", b);
		if (rd(f, v, sizeof v)) rate = atol(v);
		if (rate > 0 && now_ > 0 && !strcmp(v2, "Discharging")) {
			long min = now_ * 60 / rate;
			snprintf(t, sizeof t, "ABOUT %ld:%02ld LEFT", min / 60, min % 60);
			txt(0, row++, t, 0, 3);
		}
	}
	if (!bats) txt(0, row++, "NO BATTERY", 0, -1);
	row++;
	int settings = 0;

	if (rd(CPUF "/cpu0/cpufreq/scaling_available_governors", v, sizeof v)) {
		char cur[32];
		rd(CPUF "/cpu0/cpufreq/scaling_governor", cur, sizeof cur);
		ngov = split(v, govs, 8);
		txt(0, row++, "CPU SPEED:", 0, 3);
		int col = 0;
		for (int i = 0; i < ngov; i++) {
			snprintf(t, sizeof t, "%s%s ", strcmp(govs[i], cur) ? " " : "*", govs[i]);
			if (col + (int)strlen(t) > ncols) { col = 0; row++; }
			if (strcmp(govs[i], cur)) button(col, row, t, B_GOV + i); else txt(col, row, t, 0, 7);
			col += strlen(t) + 1;
		}
		row += 2;
		settings++;
	}

	int turbo = -1;
	if (rd(CPUF "/intel_pstate/no_turbo", v, sizeof v)) turbo = atoi(v) ? 0 : 1;
	else if (rd(CPUF "/cpufreq/boost", v, sizeof v)) turbo = atoi(v) ? 1 : 0;
	if (turbo >= 0) {
		txt(0, row, "TURBO BOOST:", 0, 3);
		button(13, row, turbo ? " ON  (CLICK: OFF) " : " OFF (CLICK: ON) ", B_TURBO);
		row += 2;
		settings++;
	}
	// acpi platform profile
	if (rd("/sys/firmware/acpi/platform_profile_choices", v, sizeof v)) {
		char cur[32];
		rd("/sys/firmware/acpi/platform_profile", cur, sizeof cur);
		nprof = split(v, profs, 8);
		txt(0, row++, "POWER PROFILE:", 0, 3);
		int col = 0;
		for (int i = 0; i < nprof; i++) {
			snprintf(t, sizeof t, "%s%s ", strcmp(profs[i], cur) ? " " : "*", profs[i]);
			if (col + (int)strlen(t) > ncols) { col = 0; row++; }
			if (strcmp(profs[i], cur)) button(col, row, t, B_PROF + i); else txt(col, row, t, 0, 7);
			col += strlen(t) + 1;
		}
		row += 2;
		settings++;
	}
	if (!settings) { txt(0, row++, "NO POWER SETTINGS ON THIS PC", 0, -1); row++; }
	hline(row++);
	button(0, row, confirm == B_SLEEP ? " SURE? " : " SLEEP ", B_SLEEP);
	button(9, row, confirm == B_REBOOT ? " SURE? " : " RESTART ", B_REBOOT);
	button(19, row, confirm == B_OFF ? " SURE? " : " SHUT DOWN ", B_OFF);
	row++;
	if (can_lock < 0) {        // SIGCHLD is ignored -> system() would lose the exit status
		void (*old)(int) = signal(SIGCHLD, SIG_DFL);
		can_lock = system("c64-power can-lock") == 0;
		signal(SIGCHLD, old);
	}
	if (can_lock) { row++; button(0, row, " LOCK THE SCREEN ", B_LOCK); row++; }
	txt(0, row, status_, 0, 7);
}

static void power_click(int id)
{
	char cmd[160];
	if (id == B_LOCK) { spawn("c64-power lock"); exit(0); }
	if (id >= B_SLEEP && id <= B_OFF) {
		if (confirm != id) { confirm = id; return; }
		static const char *c[] = { "c64-power sleep", "c64-power reboot", "c64-power off" };
		spawn(c[id - B_SLEEP]);
		exit(0);
	}
	confirm = -1;
	if (id >= B_PROF && id < B_PROF + nprof) snprintf(cmd, sizeof cmd, "c64-root profile %s 2>&1", profs[id - B_PROF]);
	else if (id >= B_GOV && id < B_GOV + ngov) snprintf(cmd, sizeof cmd, "c64-root governor %s 2>&1", govs[id - B_GOV]);
	else if (id == B_TURBO) {
		char v[16];
		int on = rd(CPUF "/intel_pstate/no_turbo", v, sizeof v) ? atoi(v) : !(rd(CPUF "/cpufreq/boost", v, sizeof v) && atoi(v));
		snprintf(cmd, sizeof cmd, "c64-root turbo %s 2>&1", on ? "on" : "off");
	} else return;
	run_out(cmd);
}




/* ======================= NOTE ======================= */

static char note[256], note_title[32] = "REMINDER";
static int note_timeout;                  // secs, 0 = wait for OK
static void draw_note(void)
{
	char t[40];
	snprintf(t, sizeof t, " %s", note_title);
	txt(0, 0, t, 1, -1);
	for (int i = strlen(t); i < ncols; i++) txt(i, 0, " ", 1, -1);
	const char *s = note;
	int row = 2;
	while (*s && row < nrows - 2) {           // word wrap
		while (*s == ' ') s++;
		int n = strlen(s) > (size_t)ncols ? ncols : (int)strlen(s);
		if (s[n]) {
			int b = n;
			while (b > 0 && s[b] != ' ') b--;
			if (b > 0) n = b;
		}
		char l[64];
		snprintf(l, sizeof l, "%.*s", n, s);
		txt(0, row++, l, 0, -1);
		s += n;
	}
	button(ncols - 4, nrows - 1, " OK ", 1);
}





static void popup_redraw(void)
{
	nb = 0;
	XSetForeground(dpy, gc, PAL[text_c]);
	XFillRectangle(dpy, pcv, gc, 0, 0, pw_, ph_);
	XSetForeground(dpy, gc, PAL[screen_c]);
	XFillRectangle(dpy, pcv, gc, P, P, pw_ - 2 * P, ph_ - 2 * P);
	if (kind_ == K_CLOCK) draw_clock();
	else if (kind_ == K_VOLUME) draw_volume();
	else if (kind_ == K_POWER) draw_power();
	else draw_note();
	XCopyArea(dpy, pcv, win, gc, 0, 0, pw_, ph_, 0, 0);
}

static void grab(void)
{
	for (int i = 0; i < 50; i++) {
		if (XGrabPointer(dpy, win, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
		                 GrabModeAsync, None, None, CurrentTime) == GrabSuccess)
			break;
		struct timespec ts = { 0, 10000000 };
		nanosleep(&ts, NULL);
	}
	XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}

int popup_main(int argc, char **argv)
{
	const char *k = argc > 2 ? argv[2] : "clock";
	kind_ = !strcmp(k, "volume") ? K_VOLUME : !strcmp(k, "power") ? K_POWER : !strcmp(k, "note") ? K_NOTE : K_CLOCK;
	char pidf[700];
	path(pidf, sizeof pidf, "cache/c64/popup.pid");
	FILE *fp = fopen(pidf, "r");
	int old = 0;
	if (fp) { if (fscanf(fp, "%d", &old) != 1) old = 0; fclose(fp); }
	if (kind_ != K_NOTE && old > 0 && kill(old, 0) == 0) {          // already open -> toggle off
		kill(old, SIGTERM);
		return 0;
	}
	if (kind_ != K_NOTE && (fp = fopen(pidf, "w"))) { fprintf(fp, "%d\n", (int)getpid()); fclose(fp); }
	if (kind_ == K_NOTE) {                  // note [--title T] [--timeout S] TEXT...

		int i = 3;
		for (; i + 1 < argc && argv[i][0] == '-' && argv[i][1] == '-'; i += 2)
			if (!strcmp(argv[i], "--title")) snprintf(note_title, sizeof note_title, "%s", argv[i + 1]);
			else if (!strcmp(argv[i], "--timeout")) note_timeout = atoi(argv[i + 1]);
		for (int o = 0, f = i; i < argc && o < (int)sizeof note - 2; i++)
			o += snprintf(note + o, sizeof note - o, "%s%s", i > f ? " " : "", argv[i]);
	}

	read_scale();
	read_colors();
	open_font();
	time_t now = time(NULL);
	struct tm tn = *localtime(&now);
	vy = sy = tn.tm_year + 1900, vm = sm = tn.tm_mon + 1, sd = tn.tm_mday;
	read_fmt();
	if (kind_ == K_VOLUME) vol_read();
	ncols = kind_ == K_CLOCK ? 30 : kind_ == K_VOLUME ? 28 : kind_ == K_POWER ? 32 : 30;
	nrows = kind_ == K_CLOCK ? 20 : kind_ == K_VOLUME ? 5 : kind_ == K_POWER ? 24 : 7;
	pw_ = ncols * C + 4 * P, ph_ = nrows * C + 4 * P;
	Window r, c;
	int rx, ry, wx, wy;
	unsigned int mask;
	XQueryPointer(dpy, root, &r, &c, &rx, &ry, &wx, &wy, &mask);
	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	int x = kind_ == K_NOTE ? (sw - pw_) / 2 : rx - pw_ / 2, y = kind_ == K_NOTE ? sh / 4 : ry + C;
	if (x + pw_ > sw) x = sw - pw_;
	if (y + ph_ > sh) y = sh - ph_;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	XSetWindowAttributes a = { .override_redirect = True, .background_pixel = PAL[screen_c],
	                           .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask };
	win = XCreateWindow(dpy, root, x, y, pw_, ph_, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
	desk = win;
	gc = XCreateGC(dpy, win, 0, NULL);
	pcv = XCreatePixmap(dpy, win, pw_, ph_, depth);
	pxd = XftDrawCreate(dpy, pcv, vis, cmap);
	XClassHint ch = { "c64popup", "C64popup" };
	XSetClassHint(dpy, win, &ch);
	XMapRaised(dpy, win);
	if (kind_ != K_NOTE) grab();
	popup_redraw();
	if (kind_ == K_NOTE && strncmp(note, "ALARM", 5) == 0)
		spawn("for i in 1 2 3; do mpv --really-quiet --no-video 'av://lavfi:sine=f=880:d=0.25' >/dev/null 2>&1; sleep 0.15; done");

	int dragging = 0, fd = ConnectionNumber(dpy);
	time_t started = time(NULL);
	for (;;) {
		if (note_timeout > 0 && time(NULL) - started >= note_timeout) goto out;
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ui_active()) {
				ui_event(&ev);
				if (!ui_active()) regrab = 1;
				continue;
			}
			if (ev.type == Expose) XCopyArea(dpy, pcv, win, gc, 0, 0, pw_, ph_, 0, 0);
			else if (ev.type == KeyPress) {
				KeySym ks = XLookupKeysym(&ev.xkey, 0);
				if (ks == XK_Escape || ks == XK_q) goto out;
				if (kind_ == K_VOLUME && (ks == XK_Left || ks == XK_Down)) vol_set(vol - 5);
				if (kind_ == K_VOLUME && (ks == XK_Right || ks == XK_Up)) vol_set(vol + 5);
				if (kind_ == K_CLOCK && ks == XK_Left) clock_click(B_PREV);
				if (kind_ == K_CLOCK && ks == XK_Right) clock_click(B_NEXT);
				if (kind_ == K_NOTE && ks == XK_Return) goto out;
				popup_redraw();
			} else if (ev.type == ButtonPress) {
				int bxp = ev.xbutton.x, byp = ev.xbutton.y;
				if (ev.xbutton.window != win || bxp < 0 || byp < 0 || bxp >= pw_ || byp >= ph_) goto out;
				if (kind_ == K_VOLUME && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)) {
					vol_set(vol + (ev.xbutton.button == Button4 ? 5 : -5));
				} else if (ev.xbutton.button == Button1) {
					if (kind_ == K_VOLUME && byp >= bar_y - P && byp < bar_y + C + P && bxp >= bar_x0 - C && bxp < bar_x0 + bar_w + C) {
						dragging = 1;
						vol_from_x(bxp);
					} else {
						int id = hit(bxp, byp);
						if (id >= 0) {
							if (kind_ == K_CLOCK) clock_click(id);
							else if (kind_ == K_POWER) power_click(id);
							else if (kind_ == K_NOTE) goto out;
							else if (id == B_MUTE) {
								if (system(muted ? "c64-volume unmute" : "c64-volume mute")) {}
								vol_read();
							}
						}
					}
				}
				popup_redraw();
			} else if (ev.type == MotionNotify && dragging) {
				while (XCheckTypedEvent(dpy, MotionNotify, &ev))
					;
				vol_from_x(ev.xmotion.x);
				popup_redraw();
			} else if (ev.type == ButtonRelease) {
				dragging = 0;
			}
		}
		if (regrab && !ui_active()) {
			regrab = 0;
			XRaiseWindow(dpy, win);
			if (kind_ != K_NOTE) grab();
			popup_redraw();
		}
		ui_tick();
		XFlush(dpy);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		struct timeval tv = { 0, 300000 };
		select(fd + 1, &fds, NULL, NULL, &tv);
	}
out:
	unlink(pidf);
	return 0;
}
