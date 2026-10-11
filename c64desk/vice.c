/*
 * vice.c
 *
 * The desktop's BASIC is a real C64: one x64sc, remote controlled over
 * VICE's binary monitor (127.0.0.1:6510). Its window is the "C64 SCREEN",
 * hidden until needed. Drive 8 = ~/.c64pine/c64.
 *
 * How output gets to the desktop:
 *   a small 6502 routine at $02A7 is chained into CHROUT ($0326). Everything
 *   printed to the screen is also copied into a 32 byte ring at $02DD
 *   (W=$02D8, R=$02D9, it blocks when full). We read the ring through the
 *   monitor and print it.
 *
 * Input goes in with "keyboard feed". Every monitor command pauses the
 * emulation, so when BASIC sits at READY/INPUT we just leave it paused.
 *
 * The window pops up by itself (without focus) when a program does
 * something graphical: sprites, bitmap mode, POKEs to screen/VIC, SYS ...
 */
#include "c64desk.h"
#include <ctype.h>
#include <strings.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PORT 6510
#define HOOK 0x02A7
#define VW 0x02D8
#define VR 0x02D9
#define VORIG 0x02DB       // old CHROUT vector
#define RING 0x02DD
#define RINGN 20

/* RUN/STOP: we wrap the STOP vector ($0328). Set STOPFLAG and the next
 * STOP check sees the key once. Just poking $91 doesn't work, the IRQ
 * overwrites it every jiffy. */
#define STOPHOOK 0x02F1    // 12 bytes
#define STOPFLAG 0x02FD
#define STOPORIG 0x02FE

enum { V_OFF, V_CONNECT, V_BOOT, V_READY };

static int state = V_OFF, sock = -1, active, idle_polls, polls, waiting = 1, prog_mode;
static pid_t vice_pid = -1;
static long long t_state, next_poll;
static uint32_t reqno = 1;
static unsigned char mem[1024];
static unsigned char pend[65536];                // big because of pasted listings
static int npend, skip_cr, run_pending, frozen, sound_on, poll_ms = 25, after_reset, from_snapshot;
static long long t_frozen, unload_ms;
/* idle this long -> snapshot + quit VICE, saves ~210 MB.
   config/c64/vice-unload in seconds, default 600, 0 = never */
static long long unload_after(void)
{
	char f[700];
	long sec = 600;
	path(f, sizeof f, "config/c64/vice-unload");
	FILE *fp = fopen(f, "r");
	if (fp) { if (fscanf(fp, "%ld", &sec) != 1) sec = 600; fclose(fp); }
	return sec > 0 ? sec * 1000LL : 0;
}
static unsigned char *rbuf;
static size_t rcap;

static int user_hid;                   // user closed the window -> don't pop it up again for this program
static Window vwin, fsicon;
static int shown, want_show, game, fs_on, withdrawing;
static Window refocus;                 // give focus back to this after showing the window
static char run_file[1024];            // autostart when ready
enum { ACT_NONE, ACT_FULLSCREEN, ACT_SETTINGS };
static int pend_act;
static void activate(void);
static void base_resources(void);
static void end_game(void);
static void fs_set(int on);
static void win_show(int focus);

static void dead(const char *why);

// debug: touch cache/c64/vice-debug -> cache/c64/vice.log
static FILE *dbgf;
static void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbg(const char *fmt, ...)
{
	static int checked;
	if (!checked) {
		char f[700];
		checked = 1;
		path(f, sizeof f, "cache/c64/vice-debug");
		if (access(f, F_OK) == 0) { path(f, sizeof f, "cache/c64/vice.log"); dbgf = fopen(f, "a"); }
	}
	if (!dbgf) return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(dbgf, "%lld ", now_ms() % 1000000);
	vfprintf(dbgf, fmt, ap);
	fputc('\n', dbgf);
	fflush(dbgf);
	va_end(ap);
}
static void drain(void);
static void start(void);




/***************************************************************************
 *  binary monitor protocol
 ***************************************************************************/

static int wr(const void *b, size_t n)
{
	const char *p = b;
	while (n) {
		ssize_t k = send(sock, p, n, MSG_NOSIGNAL);
		if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; }
		p += k, n -= k;
	}
	return 1;
}

static int rd(void *b, size_t n)
{
	char *p = b;
	while (n) {
		struct pollfd pf = { sock, POLLIN, 0 };
		if (poll(&pf, 1, 10000) <= 0) return 0;
		ssize_t k = recv(sock, p, n, 0);
		if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; }
		p += k, n -= k;
	}
	return 1;
}

// returns body length, -1 on error. events in between are skipped
static long request(unsigned char cmd, const unsigned char *body, uint32_t blen)
{
	if (sock < 0)
		return -1;
	if (frozen) {                              // SIGSTOPped while idle, wake it up
		kill(vice_pid, SIGCONT);
		frozen = 0;
	}
	uint32_t id = reqno++;
	dbg("req %u cmd %02x len %u state %d active %d", id, cmd, blen, state, active);
	/* must be ONE send(). VICE does a single recv per request and drops
	 * the connection on a partial one ("received 5 of 14") */
	static unsigned char msg[11 + 65536];
	if (blen > sizeof msg - 11) { dead("too long"); return -1; }
	unsigned char h[11] = { 2, 2, blen & 0xff, blen >> 8 & 0xff, blen >> 16 & 0xff, blen >> 24 & 0xff,
	                        id & 0xff, id >> 8 & 0xff, id >> 16 & 0xff, id >> 24 & 0xff, cmd };
	memcpy(msg, h, 11);
	if (blen) memcpy(msg + 11, body, blen);
	if (!wr(msg, 11 + blen)) { dead("send"); return -1; }
	for (;;) {
		unsigned char r[12];
		if (!rd(r, sizeof r) || r[0] != 2) { dead("recv"); return -1; }
		uint32_t len = r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
		uint32_t rid = r[8] | r[9] << 8 | r[10] << 16 | (uint32_t)r[11] << 24;
		if (len + 1 > rcap) {
			rcap = len + 4096;
			rbuf = realloc(rbuf, rcap);
		}
		if (len && !rd(rbuf, len)) { dead("recv body"); return -1; }
		if (rid == id) {
			if (r[7]) dbg("  err %02x", r[7]);
			return r[7] == 0 ? (long)len : -1;
		}
		dbg("  skip type %02x rid %u", r[6], rid);
	}
}

static int io_bank = -2;            // -2 = not asked yet, -1 = none

static int mem_get_bank(unsigned start, unsigned end, unsigned char *out, int fx, int bank)
{
	unsigned char b[8] = { (unsigned char)fx, start & 0xff, start >> 8, end & 0xff, end >> 8, 0, bank & 0xff, bank >> 8 };
	long n = request(0x01, b, sizeof b);
	if (n < 2) return 0;
	unsigned len = rbuf[0] | rbuf[1] << 8;
	if (len > end - start + 1) len = end - start + 1;
	memcpy(out, rbuf + 2, len);
	return 1;
}

static int mem_get(unsigned start, unsigned end, unsigned char *out, int fx) { return mem_get_bank(start, end, out, fx, 0); }

// bank 0 doesn't always show the I/O chips at $D000, so look up "io"
static void find_io_bank(void)
{
	io_bank = -1;
	long n = request(0x82, NULL, 0);                     // banks available
	if (n < 2) return;
	int count = rbuf[0] | rbuf[1] << 8, o = 2;
	for (int i = 0; i < count && o + 4 <= n; i++) {
		int size = rbuf[o], id = rbuf[o + 1] | rbuf[o + 2] << 8, nl = rbuf[o + 3];
		if (nl == 2 && o + 4 + nl <= n && memcmp(rbuf + o + 4, "io", 2) == 0)
			io_bank = id;
		o += 1 + size;
	}
}

static int mem_set(unsigned start, const unsigned char *data, unsigned n)
{
	unsigned char b[8 + 256];
	unsigned end = start + n - 1;
	b[0] = 0; b[1] = start & 0xff; b[2] = start >> 8; b[3] = end & 0xff; b[4] = end >> 8; b[5] = 0; b[6] = 0; b[7] = 0;
	memcpy(b + 8, data, n);
	return request(0x02, b, 8 + n) >= 0;
}

static void feed(const unsigned char *t, int n)
{
	while (n > 0) {
		int k = n > 200 ? 200 : n;
		unsigned char b[256];
		b[0] = k;
		memcpy(b + 1, t, k);
		if (request(0x72, b, k + 1) < 0) return;
		t += k, n -= k;
	}
}

static void resume(void) { request(0xaa, NULL, 0); }

static void res_str(const char *name, const char *v)
{
	unsigned char b[256];
	int nl = strlen(name), vl = strlen(v), o = 0;
	b[o++] = 0;
	b[o++] = nl;
	memcpy(b + o, name, nl); o += nl;
	b[o++] = vl;
	memcpy(b + o, v, vl); o += vl;
	request(0x52, b, o);
}

static void res_int(const char *name, int v)
{
	unsigned char b[128];
	int nl = strlen(name), o = 0;
	b[o++] = 1;
	b[o++] = nl;
	memcpy(b + o, name, nl); o += nl;
	b[o++] = 4;
	b[o++] = v & 0xff; b[o++] = v >> 8 & 0xff; b[o++] = v >> 16 & 0xff; b[o++] = v >> 24 & 0xff;
	request(0x52, b, o);
}

/* Sound output alone eats 15-25% of a core (!), more than the SID emulation.
 * So it plays into "dummy" until a program might actually use the SID. */
static void sound(int on)
{
	if (on == sound_on) return;
	sound_on = on;
	res_str("SoundDeviceName", on ? "alsa" : "dummy");
}

static void snapshot_path(char *f, size_t n) { path(f, n, "cache/c64/desk-vice.vsf"); }




/***************************************************************************
 *  C64.SAV
 *
 *  The program in BASIC is saved as a listing on the desktop every time
 *  we're back at READY. (not for LOADed programs). Put back on boot.
 ***************************************************************************/
static int loaded, restore_pending;
static char pending_file[700];          // "" = C64.SAV
static unsigned char *last_prog;
static int last_len = -1;

static void sav_path(char *f, size_t n) { path(f, n, "Desktop/C64.SAV"); }

static void sav_create(void)
{
	char f[700];
	sav_path(f, sizeof f);
	if (access(f, F_OK) != 0) {
		FILE *fp = fopen(f, "w");
		if (fp) fclose(fp);
	}
}


static void sav_write(void)
{
	unsigned char p[6];
	if (loaded || !mem_get(0x2B, 0x30, p, 0)) return;
	unsigned start = p[0] | p[1] << 8, end = p[2] | p[3] << 8;
	if (start != 0x0801 || end <= start + 2 || end > 0xA000) return;          // NEW -> keep the file
	int len = end - start;
	unsigned char *prog = malloc(len);
	if (!prog || !mem_get(start, end - 1, prog, 0)) { free(prog); return; }
	if (len == last_len && last_prog && memcmp(prog, last_prog, len) == 0) { free(prog); return; }
	char f[700], tmp[720];
	sav_path(f, sizeof f);
	snprintf(tmp, sizeof tmp, "%s.tmp", f);
	FILE *fp = fopen(tmp, "w");
	if (fp) {
		basic_detok(prog, len, start, fp);
		fclose(fp);
		rename(tmp, f);
	}
	free(last_prog);
	last_prog = prog;
	last_len = len;
}

/* listing -> memory, like LOAD. file NULL = C64.SAV.
 * Other files count as LOADed, so they never overwrite C64.SAV */
static int sav_restore_file(const char *file)
{
	char f[700];
	unsigned char prog[38000];
	if (file && file[0]) snprintf(f, sizeof f, "%s", file);
	else sav_path(f, sizeof f);
	int len = basic_crunch(f, prog, sizeof prog, 0x0801);
	if (len <= 2) return 0;
	for (int o = 0; o < len; o += 250)
		mem_set(0x0801 + o, prog + o, len - o < 250 ? len - o : 250);
	unsigned end = 0x0801 + len;
	unsigned char top[2];
	if (!mem_get(0x37, 0x38, top, 0)) return 0;
	unsigned char ptr[8] = { end & 0xff, end >> 8, end & 0xff, end >> 8, end & 0xff, end >> 8, top[0], top[1] };
	mem_set(0x2D, ptr, 8);                         // VARTAB ARYTAB STREND FRETOP, same as CLR
	free(last_prog);
	last_prog = malloc(len - 2);
	if (last_prog) memcpy(last_prog, prog, len - 2);
	last_len = -1;
	const char *b = strrchr(f, '/');
	b = b ? b + 1 : f;
	loaded = strcasecmp(b, "C64.SAV") != 0;
	char msg[300];
	snprintf(msg, sizeof msg, "SEARCHING FOR %s", b);
	for (char *c = msg; *c; c++) *c = toupper((unsigned char)*c);
	con_print(msg);
	con_print("LOADING");
	return 1;
}


// now if at READY., otherwise at the next one (stops a running program, starts VICE if needed)
void vice_load_listing(const char *file)
{
	char f[700];
	if (file && file[0]) snprintf(f, sizeof f, "%s", file);
	else sav_path(f, sizeof f);
	if (access(f, R_OK) != 0) { con_print("?FILE NOT FOUND  ERROR"); con_ready(); return; }
	if (state == V_READY && !vice_busy()) {
		if (sav_restore_file(f)) con_ready();
		else { con_print("?FILE NOT FOUND  ERROR"); con_ready(); }
		return;
	}
	snprintf(pending_file, sizeof pending_file, "%s", f);
	restore_pending = 1;
	if (state == V_READY) vice_stop_key();
	else start();
}

void vice_load_sav(void) { vice_load_listing(NULL); }




/***************************************************************************
 *  processes
 ***************************************************************************/

static pid_t run(char *const argv[], const char *display)
{
	pid_t p = fork();
	if (p == 0) {
		child_signals();
		setsid();
		if (display) setenv("DISPLAY", display, 1);
		setenv("GTK_THEME", "c64", 1);
		int fd = open("/dev/null", 2);
		if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
		execvp(argv[0], argv);
		_exit(127);
	}
	return p;
}

static int try_connect(void)
{
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
		sock = s;
		return 1;
	}
	if (s >= 0) close(s);
	return 0;
}

static void set_state(int s) { dbg("state %d -> %d", state, s); state = s; t_state = now_ms(); next_poll = now_ms(); }

static void start_vice(void);

// leftover VICE from an earlier c64desk?
static pid_t find_vice_pid(void)
{
	char cfg[700];
	path(cfg, sizeof cfg, "config/c64/desk-vice.rc");
	DIR *d = opendir("/proc");
	struct dirent *e;
	pid_t found = -1;
	while (d && (e = readdir(d)) && found < 0) {
		if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
		char f[300], buf[2048];
		snprintf(f, sizeof f, "/proc/%s/cmdline", e->d_name);
		FILE *fp = fopen(f, "r");
		if (!fp) continue;
		size_t n = fread(buf, 1, sizeof buf - 1, fp);
		fclose(fp);
		buf[n] = 0;
		for (size_t i = 0; i + 1 < n; i++) if (!buf[i]) buf[i] = ' ';
		if (strstr(buf, "x64sc") && strstr(buf, cfg)) found = atoi(e->d_name);
	}
	if (d) closedir(d);
	return found;
}

static void start(void)
{
	if (state != V_OFF)
		return;
	if (try_connect()) {                         // reuse it
		vice_pid = find_vice_pid();
		vwin = 0;
		set_state(V_BOOT);
		return;
	}
	start_vice();
}

/* Config is rewritten on every start: config/vice/vicerc + what we need
 * (no true drive emu, drive 8 = folder, no exit questions). Changes in the
 * VICE settings dialog only last for that session, on purpose. */
static void write_config(const char *cfg, const char *drive)
{
	FILE *fp = fopen(cfg, "w");
	if (!fp) return;
	fprintf(fp, "[Version]\nConfigVersion=3.10\n\n[C64SC]\n");
	char vrc[700], line[1024];
	path(vrc, sizeof vrc, "config/vice/vicerc");
	FILE *in = fopen(vrc, "r");
	int sect = 0;
	while (in && fgets(line, sizeof line, in)) {
		if (line[0] == '[') { sect = !strncmp(line, "[C64SC]", 7); continue; }
		if (sect && strchr(line, '=')) fputs(line, fp);
	}
	if (in) fclose(in);
	fprintf(fp, "SaveResourcesOnExit=0\nConfirmOnExit=0\nSpeed=100\n"
	        "Drive8Type=0\nFileSystemDevice8=1\nFSDevice8Dir=\"%s\"\nFSDeviceLongNames=1\n"
	        "VICIIFilter=0\nSoundDeviceName=\"dummy\"\n", drive);
	fclose(fp);
}

static void start_vice(void)
{
	char cfg[700], drive[700], addr[64];
	path(cfg, sizeof cfg, "config/c64/desk-vice.rc");
	path(drive, sizeof drive, "c64");
	mkdir(drive, 0755);
	snprintf(addr, sizeof addr, "ip4://127.0.0.1:%d", PORT);
	write_config(cfg, drive);
	// no -warp: 3.10 can't turn it off via the monitor anymore. boot takes ~2.5s
	char *av[] = { "x64sc", "-config", cfg, "-binarymonitor", "-binarymonitoraddress", addr, NULL };
	vice_pid = run(av, NULL);
	vwin = 0;
	sound_on = 0;
	set_state(V_CONNECT);
}

// after the user had the window: put back what we rely on
static void base_resources(void)
{
	char drive[700];
	path(drive, sizeof drive, "c64");
	res_int("Speed", 100);
	res_int("Drive8Type", 0);
	res_int("FileSystemDevice8", 1);
	res_str("FSDevice8Dir", drive);
	res_int("ConfirmOnExit", 0);
	res_int("SaveResourcesOnExit", 0);
	int on = sound_on;
	sound_on = -1;
	sound(on);
}



/* ---- games / C64 software ----
 * desktop C64 goes into a snapshot, the game runs in the window.
 * Closing the window quits VICE, next use restores the snapshot. */
static void do_autostart(void)
{
	char snap[700];
	snapshot_path(snap, sizeof snap);
	unsigned char b[300];
	int l = strlen(snap);
	b[0] = 0; b[1] = 0; b[2] = l;                       // no ROMs, no disks
	memcpy(b + 3, snap, l);
	if (request(0x41, b, l + 3) < 0) unlink(snap);
	l = strlen(run_file);
	if (l > 255) l = 255;
	b[0] = 1; b[1] = 0; b[2] = 0; b[3] = l;              // run, file index 0, name
	memcpy(b + 4, run_file, l);
	run_file[0] = 0;
	game = 1;
	active = 0;
	npend = 0;
	waiting = 1;
	prog_mode = 0;
	sound(1);
	res_int("Drive8Type", 1541);                         // autostart needs a real drive
	res_int("AutostartWarp", 1);                         // warp while it loads, VICE turns it off after
	if (request(0xdd, b, l + 4) < 0) {
		con_print("?FILE NOT FOUND  ERROR");
		game = 0;
		return;
	}
	resume();
	user_hid = 0;
	win_show(1);
}

void vice_run_file(const char *file)
{
	snprintf(run_file, sizeof run_file, "%s", file);
	if (game) end_game();
	if (state == V_READY) do_autostart();
	else start();                                        // autostarts at READY., see vice_tick
}

static void end_game(void)
{
	game = 0;
	if (sock >= 0) close(sock);
	sock = -1;
	if (vice_pid > 0) { kill(vice_pid, SIGCONT); kill(vice_pid, SIGTERM); }
	vice_pid = -1;
	vwin = 0;
	shown = 0;
	frozen = 0;
	fs_set(0);
	state = V_OFF;
	active = 0;
	waiting = 1;
	prog_mode = 0;
	io_bank = -2;
	if (run_file[0]) start();
}

static void dead(const char *why)
{
	dbg("DEAD %s", why);
	if (sock >= 0) close(sock);
	sock = -1;
	if ((state == V_READY || state == V_BOOT) && !game && !shown)
		con_print("?DEVICE NOT PRESENT  ERROR");
	game = 0;
	shown = 0;
	vwin = 0;
	fs_set(0);
	if (vice_pid > 0) kill(vice_pid, SIGKILL);   // TERM isn't enough when it's stuck in the monitor
	vice_pid = -1;
	frozen = 0;
	state = V_OFF;
	active = 0;
	npend = 0;
	waiting = 1;
	prog_mode = 0;
}

// signal safe, no snapshot
void vice_kill_all(void)
{
	if (vice_pid > 0) { kill(vice_pid, SIGCONT); kill(vice_pid, SIGKILL); }
}

void vice_quit(void)
{
	if (vice_pid > 0) { kill(vice_pid, SIGCONT); kill(vice_pid, SIGTERM); }
}




/***************************************************************************
 *  the CHROUT hook
 ***************************************************************************/

static void install_hook(void)
{
	static const unsigned char code[] = {
		0x48, 0x8A, 0x48,                   /* 02A7 PHA TXA PHA */
		0xA5, 0x9A, 0xC9, 0x03, 0xD0, 0x22, /* 02AA LDA $9A  CMP #3  BNE done     screen only */
		0xAE, 0xD8, 0x02, 0xE8,             /* 02B0 wait: LDX W  INX */
		0xE0, RINGN, 0x90, 0x02, 0xA2, 0x00,/* 02B4 CPX #32  BCC +2  LDX #0 */
		0xEC, 0xD9, 0x02, 0xF0, 0xF1,       /* 02BA CPX R  BEQ wait            full -> spin */
		0x8E, 0xDA, 0x02,                   /* 02BF STX temp */
		0xBA, 0xBD, 0x02, 0x01,             /* 02C2 TSX  LDA $0102,X           char from stack */
		0xAE, 0xD8, 0x02,                   /* 02C6 LDX W */
		0x9D, 0xDD, 0x02,                   /* 02C9 STA ring,X */
		0xAE, 0xDA, 0x02, 0x8E, 0xD8, 0x02, /* 02CC LDX temp  STX W */
		0x68, 0xAA, 0x68,                   /* 02D2 done: PLA TAX PLA */
		0x6C, 0xDB, 0x02,                   /* 02D5 JMP (orig) */
	};
	unsigned char v[2];
	if (!mem_get(0x0326, 0x0327, v, 0))
		return;
	if (v[0] == (HOOK & 0xff) && v[1] == HOOK >> 8)
		return;
	unsigned char vars[5] = { 0, 0, 0, v[0], v[1] };     /* W R temp orig */
	mem_set(HOOK, code, sizeof code);
	mem_set(VW, vars, sizeof vars);
	unsigned char sv[2];
	if (mem_get(0x0328, 0x0329, sv, 0) && !(sv[0] == (STOPHOOK & 0xff) && sv[1] == STOPHOOK >> 8)) {
		const unsigned char stop[] = {
			0x4E, STOPFLAG & 0xff, STOPFLAG >> 8,    /* LSR flag      bit -> carry */
			0x90, 0x04,                              /* BCC orig */
			0xA9, 0x7F, 0x85, 0x91,                  /* LDA #$7F  STA $91   stop key down */
			0x6C, STOPORIG & 0xff, STOPORIG >> 8,    /* orig: JMP (STOP) */
		};
		unsigned char st[3] = { 0, sv[0], sv[1] };
		mem_set(STOPHOOK, stop, sizeof stop);
		mem_set(STOPFLAG, st, 3);
		unsigned char ns[2] = { STOPHOOK & 0xff, STOPHOOK >> 8 };
		mem_set(0x0328, ns, 2);
	}
	unsigned char nv[2] = { HOOK & 0xff, HOOK >> 8 };
	mem_set(0x0326, nv, 2);
}




/***************************************************************************
 *  the VICE window ("C64 SCREEN")
 *
 *  Minimized + skip taskbar while hidden. openbox starts it minimized
 *  (rc.xml) so it doesn't flash, and its close button runs
 *  "c64desk --c64 hide" instead of closing VICE.
 *  Don't withdraw it: openbox won't take back a window that was withdrawn
 *  while minimized.
 ***************************************************************************/

static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }

static long prop_card(Window w, const char *name)
{
	Atom t; int f; unsigned long n, after; unsigned char *d = NULL; long v = -1;
	if (XGetWindowProperty(dpy, w, atom(name), 0, 1, False, XA_CARDINAL, &t, &f, &n, &after, &d) == Success && d) {
		if (n) v = (long)*(unsigned long *)d;
		XFree(d);
	}
	return v;
}

static int prop_is(Window w, const char *name)
{
	Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
	int ok = XGetWindowProperty(dpy, w, atom(name), 0, 1, False, AnyPropertyType, &t, &f, &n, &after, &d) == Success && t != None;
	if (d) XFree(d);
	return ok;
}


static int is_vice_main(Window w)
{
	if (prop_card(w, "_NET_WM_PID") != vice_pid) return 0;
	XClassHint ch = { 0 };
	int ok = 0;
	if (XGetClassHint(dpy, w, &ch)) {
		ok = ch.res_class && (!strcmp(ch.res_class, "X64sc") || !strcmp(ch.res_class, "C64screen"));
		XFree(ch.res_name); XFree(ch.res_class);
	}
	Window tr;
	if (!ok || XGetTransientForHint(dpy, w, &tr)) return 0;
	char *name = NULL;
	ok = XFetchName(dpy, w, &name) && name && !strncmp(name, "VICE", 4);
	if (name) XFree(name);
	return ok;
}

static void key_to_vice(const char *keys);
static void win_away(void);

static void find_vwin(void)
{
	if (vwin || vice_pid <= 0) return;
	Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, root, atom("_NET_CLIENT_LIST"), 0, 4096, False, XA_WINDOW, &t, &f, &n, &after, &d) == Success && d) {
		for (unsigned long i = 0; i < n && !vwin; i++)
			if (is_vice_main(((Window *)d)[i])) vwin = ((Window *)d)[i];
		XFree(d);
	}
	if (!vwin) {             // withdrawn by an older c64desk, or no EWMH WM
		int ewmh = prop_is(root, "_NET_SUPPORTING_WM_CHECK");
		Window r, par, *ch = NULL;
		unsigned int nc = 0;
		XWindowAttributes wa;
		XClassHint cl;
		if (XQueryTree(dpy, root, &r, &par, &ch, &nc)) {
			for (unsigned int i = 0; i < nc && !vwin; i++) {
				if (!is_vice_main(ch[i])) continue;
				int ours = 0;
				if (XGetClassHint(dpy, ch[i], &cl)) {
					ours = cl.res_class && !strcmp(cl.res_class, "C64screen");
					XFree(cl.res_name); XFree(cl.res_class);
				}
				if (ours || (!ewmh && XGetWindowAttributes(dpy, ch[i], &wa) && wa.map_state == IsViewable)) vwin = ch[i];
			}
			if (ch) XFree(ch);
		}
	}
	if (!vwin) return;
	dbg("VICE window %lx", vwin);
	XClassHint ch = { "c64screen", "C64screen" };
	XSetClassHint(dpy, vwin, &ch);
	XSelectInput(dpy, vwin, StructureNotifyMask | PropertyChangeMask);
	win_away();
	if (want_show) {
		win_show(want_show == 2);
		if (pend_act == ACT_FULLSCREEN) key_to_vice("alt+d");
		if (pend_act == ACT_SETTINGS) key_to_vice("alt+o");
		pend_act = ACT_NONE;
	}
}


static void net_state(int add, const char *a, const char *b)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = vwin;
	e.xclient.message_type = atom("_NET_WM_STATE");
	e.xclient.format = 32;
	e.xclient.data.l[0] = add;
	e.xclient.data.l[1] = atom(a);
	e.xclient.data.l[2] = b ? atom(b) : 0;
	e.xclient.data.l[3] = 2;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

static void activate_window(Window w)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = atom("_NET_ACTIVE_WINDOW");
	e.xclient.format = 32;
	e.xclient.data.l[0] = 2;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}


static void win_away(void)
{
	withdrawing = 1;
	net_state(1, "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER");
	XIconifyWindow(dpy, vwin, scr);
	XFlush(dpy);
	shown = 0;
}

// focus=0: show it but keep the keyboard on the desktop
static void win_show(int focus)
{
	if (!vwin) {
		want_show = focus ? 2 : want_show ? want_show : 1;
		start();
		return;
	}
	want_show = 0;
	dbg("win_show %lx focus %d", vwin, focus);
	if (frozen && vice_pid > 0) { kill(vice_pid, SIGCONT); frozen = 0; }
	if (focus) XDeleteProperty(dpy, vwin, atom("_NET_WM_USER_TIME"));
	else {
		long t0 = 0;
		XChangeProperty(dpy, vwin, atom("_NET_WM_USER_TIME"), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&t0, 1);
	}
	withdrawing = 0;
	net_state(0, "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER");
	if (focus) activate_window(vwin);
	else {
		Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
		refocus = desk;
		if (XGetWindowProperty(dpy, root, atom("_NET_ACTIVE_WINDOW"), 0, 1, False, XA_WINDOW, &t, &f, &n, &after, &d) == Success && d) {
			if (n && *(Window *)d) refocus = *(Window *)d;
			XFree(d);
		}
		XMapRaised(dpy, vwin);
	}
	XFlush(dpy);                                        // "shown" gets set in vice_event
	if (!game && state == V_READY) activate();
}


static void win_hide(void)
{
	if (!vwin) return;
	dbg("win_hide %lx", vwin);
	win_away();
	fs_set(0);
	if (game) end_game();
	else if (state == V_READY) base_resources();
}

// fake real key presses for VICE's hotkeys (alt+d, alt+o)
static void key_to_vice(const char *keys)
{
	char cmd[200];
	snprintf(cmd, sizeof cmd, "xdotool windowactivate --sync %lu key --clearmodifiers %s", vwin, keys);
	spawn(cmd);
}


/* fullscreen + the little "back to window" button top right */
static void fs_draw(void)
{
	int s = 2 * C, t = P > 1 ? P : 1;
	XSetForeground(dpy, gc, PAL[6]);
	XFillRectangle(dpy, fsicon, gc, 0, 0, s, s);
	XSetForeground(dpy, gc, PAL[14]);
	for (int i = 0; i < t; i++) XDrawRectangle(dpy, fsicon, gc, i, i, s - 1 - 2 * i, s - 1 - 2 * i);
	int x = s / 4, y = s / 4 + t, w = s / 2, h = s / 2 - t;
	XFillRectangle(dpy, fsicon, gc, x, y, w, 2 * t + 1);
	for (int i = 0; i < t; i++) XDrawRectangle(dpy, fsicon, gc, x + i, y + i, w - 1 - 2 * i, h - 1 - 2 * i);
}

static void fs_set(int on)
{
	if (on == fs_on) return;
	fs_on = on;
	if (!on) { if (fsicon) XUnmapWindow(dpy, fsicon); return; }
	int s = 2 * C;
	if (!fsicon) {
		XSetWindowAttributes a = { 0 };
		a.override_redirect = True;
		a.background_pixel = PAL[6];
		a.event_mask = ExposureMask | ButtonPressMask;
		int mx, my, mw, mh;
		monitor_rect(-1, 0, &mx, &my, &mw, &mh);
		fsicon = XCreateWindow(dpy, root, mx + mw - s - 2 * P, my + 2 * P, s, s, 0, CopyFromParent, InputOutput,
		                       CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
		XClassHint ch = { "c64desk", "C64desk" };
		XSetClassHint(dpy, fsicon, &ch);
	}
	XMapRaised(dpy, fsicon);
}

void vice_raise_icon(void) { if (fs_on && fsicon) XRaiseWindow(dpy, fsicon); }
int vice_owns(Window w) { return w && (w == fsicon); }

static int is_fullscreen(void)
{
	Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
	int on = 0;
	Atom fsa = atom("_NET_WM_STATE_FULLSCREEN");
	if (XGetWindowProperty(dpy, vwin, atom("_NET_WM_STATE"), 0, 32, False, XA_ATOM, &t, &f, &n, &after, &d) == Success && d) {
		for (unsigned long i = 0; i < n; i++) if (((Atom *)d)[i] == fsa) on = 1;
		XFree(d);
	}
	return on;
}





void vice_screen_toggle(int show)
{
	if (!show) {
		user_hid = 1;
		win_hide();
		return;
	}
	user_hid = 0;
	start();
	win_show(1);
}

void vice_fullscreen(void)
{
	user_hid = 0;
	if (!vwin) { pend_act = ACT_FULLSCREEN; vice_screen_toggle(1); return; }
	if (!shown) win_show(1);
	key_to_vice("alt+d");
}

void vice_settings(void)
{
	user_hid = 0;
	if (!vwin) { pend_act = ACT_SETTINGS; vice_screen_toggle(1); return; }
	if (!shown) win_show(1);
	key_to_vice("alt+o");
}

static void auto_panel(void)
{
	if (!user_hid && !shown && !game)
		win_show(0);
}

int vice_event(XEvent *e)
{
	if (fsicon && e->xany.window == fsicon) {
		if (e->type == Expose) fs_draw();
		else if (e->type == ButtonPress && vwin) key_to_vice("alt+d");
		return 1;
	}
	if (!vwin || e->xany.window != vwin)
		return 0;
	dbg("vwin event %d", e->type);
	if (e->type == MapNotify || e->type == UnmapNotify ||
	    (e->type == PropertyNotify && e->xproperty.atom == atom("WM_STATE"))) {
		// trust WM_STATE, map/unmap also happen on reparent and fullscreen
		int was = shown;
		long st = -1;
		Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
		Atom wms = atom("WM_STATE");
		if (XGetWindowProperty(dpy, vwin, wms, 0, 2, False, wms, &t, &f, &n, &after, &d) == Success && d) {
			if (n) st = *(long *)d;
			XFree(d);
		}
		if (st < 0) st = e->type == MapNotify ? 1 : e->type == UnmapNotify ? 3 : -1;   // no WM
		if (st == 1 && !was) {
			shown = 1;
			if (refocus) { activate_window(refocus); refocus = 0; }
			if (frozen && vice_pid > 0) { kill(vice_pid, SIGCONT); frozen = 0; }
			if (!game && state == V_READY) activate();
		} else if (st == 3 && was) {
			shown = 0;
			if (game && !withdrawing && vice_pid > 0 && kill(vice_pid, SIGSTOP) == 0) frozen = 1;   // pause minimized games
			withdrawing = 0;
		}
	} else if (e->type == DestroyNotify) {
		vwin = 0;
		shown = 0;
		fs_set(0);
	} else if (e->type == PropertyNotify && e->xproperty.atom == atom("_NET_WM_STATE")) {
		fs_set(is_fullscreen());
	}
	return 1;
}




/***************************************************************************
 *  input
 ***************************************************************************/

static void queue(const unsigned char *t, int n)
{
	if (npend + n <= (int)sizeof pend) {
		memcpy(pend + npend, t, n);
		npend += n;
	}
}

static void activate(void)
{
	if (game) return;
	if (state == V_READY && npend) {
		feed(pend, npend);
		npend = 0;
	}
	waiting = 0;
	idle_polls = 0;
	poll_ms = 25;
	if (state == V_READY && !active) {
		active = 1;
		resume();
	}
	next_poll = now_ms() + 20;
}

/* Does the program need the real screen? Scans the tokens for SYS and
 * POKEs into screen RAM / VIC. Heuristic, but good enough */
static int program_graphics(void)
{
	unsigned char zp[2], prog[16384];
	if (!mem_get(0x2D, 0x2E, zp, 0)) return 0;
	unsigned end = zp[0] | zp[1] << 8;
	if (end <= 0x0803) return 0;
	if (end > 0x0801 + sizeof prog) end = 0x0801 + sizeof prog;
	if (!mem_get(0x0801, end - 1, prog, 0)) return 0;
	unsigned n = end - 0x0801;
	for (unsigned i = 0; i < n; i++) {
		if (prog[i] == 0x9E) return 1;                                 // SYS
		if (prog[i] == 0x97) {                                         // POKE
			unsigned j = i + 1;
			while (j < n && prog[j] == ' ') j++;
			long a = 0;
			int digits = 0;
			while (j < n && prog[j] >= '0' && prog[j] <= '9') { a = a * 10 + prog[j++] - '0'; digits++; }
			if (digits && ((a >= 1024 && a < 2024) || (a >= 55296 && a < 56296) || (a >= 53248 && a < 53295)))
				return 1;
			if (!digits) return 1;                                     // POKE A,.. could be anything
		}
	}
	return 0;
}

// same idea for the SID ($D400-$D41C)
static int program_sound(void)
{
	unsigned char zp[2], prog[16384];
	if (!mem_get(0x2D, 0x2E, zp, 0)) return 1;
	unsigned end = zp[0] | zp[1] << 8;
	if (end <= 0x0803) return 0;
	if (end > 0x0801 + sizeof prog) end = 0x0801 + sizeof prog;
	if (!mem_get(0x0801, end - 1, prog, 0)) return 1;
	unsigned n = end - 0x0801;
	int quote = 0;
	for (unsigned i = 0; i < n; i++) {
		if (prog[i] == '"') quote = !quote;
		if (quote) continue;
		if (prog[i] == 0x9E) return 1;                                 // SYS
		if (prog[i] == 0x97) {                                         // POKE
			unsigned j = i + 1;
			while (j < n && prog[j] == ' ') j++;
			long a = 0;
			int digits = 0;
			while (j < n && prog[j] >= '0' && prog[j] <= '9') { a = a * 10 + prog[j++] - '0'; digits++; }
			if (!digits || (a >= 54272 && a <= 54300)) return 1;
		}
	}
	return 0;
}

void vice_line(const char *line)
{
	if (game) { con_print("?GAME RUNNING  ERROR (CLOSE ITS WINDOW)"); con_ready(); return; }
	unsigned char t[600];
	int n = 0;
	for (const char *s = line; *s && n < 590; s++) {
		unsigned char c = *s;
		if (c >= 'a' && c <= 'z') c -= 32;
		else if (c >= 96 || c < 32) continue;
		t[n++] = c;
	}
	t[n++] = 13;
	sav_create();
	{
		const unsigned char *k = t;
		while (*k == ' ') k++;
		if (!(*k >= '0' && *k <= '9')) {                 // direct mode LOAD/NEW
			if (memcmp(k, "LOAD", 4) == 0) loaded = 1;
			if (memcmp(k, "NEW", 3) == 0) loaded = 0;
		}
	}
	if (state == V_READY && mem[STOPFLAG]) {
		unsigned char zero = 0;
		mem_set(STOPFLAG, &zero, 1);
		mem[STOPFLAG] = 0;
	}
	start();
	queue(t, n);
	skip_cr = 1;
	if (n >= 4 && memcmp(t, "RUN", 3) == 0) run_pending = 1;
	if (state == V_READY)
		activate();
}

void vice_key(unsigned char p)
{
	if (game) return;
	queue(&p, 1);
	activate();
}

void vice_stop_key(void)
{
	dbg("STOP key state %d", state);
	if (state != V_READY || game)
		return;
	unsigned char one = 1;
	mem_set(STOPFLAG, &one, 1);
	drain();                          // program may be blocked on a full ring
	active = 0;
	activate();
}

void vice_reset(void)
{
	if (state != V_READY && state != V_BOOT)
		return;
	unsigned char r = 0;
	request(0xcc, &r, 1);
	/* old READY. and hook vector stay in RAM until the reboot is done (~2.5s)
	 * so blank the screen, otherwise the boot check sees the old one */
	unsigned char sp[250];
	memset(sp, 32, sizeof sp);
	for (int a = 0x0400; a < 0x07E8; a += 250)
		mem_set(a, sp, 250);
	after_reset = 1;
	resume();
	active = 0;
	set_state(V_BOOT);
}

int vice_busy(void)
{
	if (game) return 0;
	if (state == V_READY) return !(waiting && !prog_mode);
	return state != V_OFF && npend > 0;
}

int vice_wants_keys(void) { return state == V_READY && prog_mode && !waiting && !game; }

int vice_running(void) { return state == V_READY; }

long long vice_next_ms(void)
{
	if (state != V_OFF && vice_pid > 0 && !vwin)
		return now_ms() + 200;
	if (state == V_READY && frozen && !shown && !game && unload_ms)
		return t_frozen + unload_ms + 1;
	if (state == V_OFF || (state == V_READY && (!active || game)))
		return 0;
	return next_poll;
}




/***************************************************************************
 *  polling
 ***************************************************************************/

static int screen_has_ready(void)
{
	static const unsigned char ready[] = { 18, 5, 1, 4, 25, 46 };   // READY. (screen codes)
	unsigned char s[1000];
	if (!mem_get(0x0400, 0x07E7, s, 0)) return 0;
	for (int i = 0; i + 6 <= 1000; i++)
		if (memcmp(s + i, ready, 6) == 0) return 1;
	return 0;
}

// ring -> desktop. mem[] = $0000-$03FF. returns 1 if there was anything
static int drain_mem(void)
{
	int w = mem[VW], r = mem[VR], got = 0;
	if (w >= RINGN || r >= RINGN)
		return 0;
	while (r != w) {
		unsigned char c = mem[RING + r];
		// cursor movement = needs the real screen. not crsr right, BASIC prints that after every number
		if (prog_mode && (c == 17 || c == 145 || c == 157 || c == 19))
			auto_panel();
		if (!(skip_cr && c == 13))
			con_put_petscii(c);
		skip_cr = 0;
		r = (r + 1) % RINGN;
		got++;
	}
	if (got) {
		unsigned char rr = r;
		mem_set(VR, &rr, 1);
	}
	return got;
}

static void drain(void)
{
	if (mem_get(VW, RING + RINGN - 1, mem + VW, 0))
		drain_mem();
}

/* each poll stops+restarts the emulation, which is more expensive than
 * the emulation itself. 25ms while output flows, 100ms when quiet */
static void poll_ready(void)
{
	if (!mem_get(0x0000, 0x03FF, mem, 0))
		return;
	if (mem[0x326] != (HOOK & 0xff) || mem[0x327] != HOOK >> 8) {      // RESTORE/reset killed our hook
		install_hook();
		mem_get(0x0000, 0x03FF, mem, 0);
	}
	int got = drain_mem();
	dbg("poll got %d waiting %d prog %d CC %d C6 %d 9D %02x pc? idle %d", got, mem[0xCC] == 0 && mem[0xC6] == 0, !(mem[0x9D] & 0x80), mem[0xCC], mem[0xC6], mem[0x9D], idle_polls);
	poll_ms = got > RINGN / 2 ? 10 : got ? 25 : poll_ms * 2 > 100 ? 100 : poll_ms * 2;
	waiting = mem[0xCC] == 0 && mem[0xC6] == 0;
	int was_prog = prog_mode;
	prog_mode = !(mem[0x9D] & 0x80);
	if ((prog_mode && !was_prog) || (run_pending && !npend && mem[0xC6] == 0)) {   // program started
		run_pending = 0;
		user_hid = 0;
		if (program_graphics()) auto_panel();
		if (program_sound()) sound(1);
	}
	if (prog_mode && !waiting && (++polls % 4) == 0) {
		unsigned char v[8];                                              // $D011-$D018
		if (io_bank == -2) find_io_bank();
		if (io_bank >= 0 && mem_get_bank(0xD011, 0xD018, v, 0, io_bank) &&
		    ((v[0] & 0x60) || v[4] || (v[5] & 0x10) || (v[7] != 0x14 && v[7] != 0x15 && v[7] != 0x16 && v[7] != 0x17)))
			auto_panel();
	}
	if (npend) { feed(pend, npend); npend = 0; idle_polls = 0; }
	if (waiting && !got && !npend) {
		if (++idle_polls == 4) {                                         // waiting for input
			if (!prog_mode && restore_pending) {
				restore_pending = 0;
				if (sav_restore_file(pending_file)) con_ready();
				pending_file[0] = 0;
			}
			if (!prog_mode) sav_write();
			sound(0);
		}
		if (idle_polls >= 4 && !shown) {
			active = 0;
			if (vice_pid > 0 && kill(vice_pid, SIGSTOP) == 0) {          // 0% cpu when idle
				frozen = 1;
				t_frozen = now_ms();
				unload_ms = unload_after();
			}
			return;
		}
	} else {
		idle_polls = 0;
	}
	resume();
}


static void unload(void)
{
	char snap[700];
	snapshot_path(snap, sizeof snap);
	unsigned char b[300];
	int l = strlen(snap);
	b[0] = 0; b[1] = 0; b[2] = l;
	memcpy(b + 3, snap, l);
	if (request(0x41, b, l + 3) < 0) unlink(snap);
	if (sock >= 0) close(sock);
	sock = -1;
	if (vice_pid > 0) { kill(vice_pid, SIGCONT); kill(vice_pid, SIGTERM); }
	vice_pid = -1;
	vwin = 0;
	frozen = 0;
	state = V_OFF;
	active = 0;
	waiting = 1;
	prog_mode = 0;
	io_bank = -2;
}

void vice_forget(void)
{
	char snap[700];
	snapshot_path(snap, sizeof snap);
	unlink(snap);
}

void vice_tick(void)
{
	long long t = now_ms();
	if (state != V_OFF && vice_pid > 0 && !vwin)
		find_vwin();
	if (state == V_READY && frozen && !shown && !game && unload_ms && t - t_frozen > unload_ms) {
		unload();
		return;
	}
	if (state == V_OFF || t < next_poll)
		return;
	switch (state) {
	case V_CONNECT:
		if (try_connect()) set_state(V_BOOT);
		else if (t - t_state > 15000) { con_print("?VICE NOT PRESENT  ERROR"); vice_quit(); state = V_OFF; }
		next_poll = t + 150;
		break;
	case V_BOOT: {
		char snap[700];
		snapshot_path(snap, sizeof snap);
		if (access(snap, F_OK) == 0) {                   // was unloaded while idle
			from_snapshot = 1;
			unsigned char b[300];
			int l = strlen(snap);
			b[0] = l;
			memcpy(b + 1, snap, l);
			request(0x42, b, l + 1);
			unlink(snap);
		}
		unsigned char v[2] = { 0, 0 }, kb[2] = { 1, 1 };
		int hooked = mem_get(0x0326, 0x0327, v, 0) && v[0] == (HOOK & 0xff) && v[1] == HOOK >> 8;
		/* READY. on screen isn't enough, the editor has to wait for keys too
		 * ($CC=0, $C6 empty) or the fed line can get lost */
		int waiting_keys = mem_get(0xC6, 0xC6, kb, 0) && mem_get(0xCC, 0xCC, kb + 1, 0) && kb[0] == 0 && kb[1] == 0;
		if ((hooked && !after_reset) || (screen_has_ready() && waiting_keys)) {
			after_reset = 0;
			install_hook();
			if (sock < 0) break;
			if (!from_snapshot || restore_pending) {   // fresh boot: restore C64.SAV
				if (sav_restore_file(restore_pending ? pending_file : NULL) && restore_pending && !npend) con_ready();
			}
			from_snapshot = restore_pending = 0;
			pending_file[0] = 0;
			if (sock < 0) break;
			set_state(V_READY);
			active = 0;
			waiting = 1;
			activate();
			if (run_file[0]) do_autostart();

		} else {
			resume();
			next_poll = t + 150;
			if (t - t_state > 20000) dead("boot");
		}
		break;
	}
	case V_READY:
		if (sock < 0) { state = V_OFF; break; }
		if (active && !game) {
			poll_ready();
			next_poll = now_ms() + poll_ms;
		}
		break;
	}
}

void vice_prepare(void) { start(); }
