/* menus.c - desktop right-click menu from config/c64/menus
 * (file format is described in the file itself. [LOAD] is c64-launch's business) */
#include "c64desk.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>

#define MAXMENUS 48
#define MAXACT 512
enum { A_CMD, A_BUILTIN, A_DESKTOP, A_WINDOW };
typedef struct { int kind; char cmd[400]; long arg; } Action;

static UiMenu menus[MAXMENUS];
static int nmenus;
static Action acts[MAXACT];
static int nacts;

typedef struct { int indent; char text[460]; } Line;
static Line lines[600];
static int nlines;

static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }



static void read_section(const char *section)
{
	char f[700], buf[600], want[64];
	nlines = 0;
	path(f, sizeof f, "config/c64/menus");
	FILE *fp = fopen(f, "r");
	if (!fp) return;
	snprintf(want, sizeof want, "[%s]", section);
	int in = 0;
	while (fgets(buf, sizeof buf, fp) && nlines < 600) {
		buf[strcspn(buf, "\r\n")] = 0;
		if (buf[0] == '#') continue;
		if (buf[0] == '[') { in = !strncmp(buf, want, strlen(want)); continue; }
		if (!in) continue;
		int ind = 0;
		while (buf[ind] == ' ') ind++;
		if (!buf[ind]) continue;
		lines[nlines].indent = ind / 2;
		snprintf(lines[nlines].text, sizeof lines[nlines].text, "%s", buf + ind);
		nlines++;
	}
	fclose(fp);
}

static void trim(char *s)
{
	int n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
	char *p = s;
	while (isspace((unsigned char)*p)) p++;
	memmove(s, p, strlen(p) + 1);
}

/* Is the program behind a command installed?  Looks through
   env VAR=x, sh -c '...' and c64-term -e. Hidden if not. */
static int cmd_installed(const char *cmd)
{
	char w[8][128];
	int n = 0;
	const char *s = cmd;
	while (*s && n < 8) {
		while (*s == ' ') s++;
		if (!*s) break;
		int k = 0;
		if (*s == '"' || *s == '\'') {                                   // quoted: take first word inside
			char q = *s++;
			while (*s && *s != q && *s != ' ' && k < 127) w[n][k++] = *s++;
			while (*s && *s != q) s++;
			if (*s) s++;
		} else
			while (*s && *s != ' ' && k < 127) w[n][k++] = *s++;
		w[n][k] = 0;
		n++;
	}
	int i = 0, k;
	do {
		k = i;
		if (i < n && !strcmp(w[i], "env")) i++;
		while (i < n && strchr(w[i], '=')) i++;
		if (i + 1 < n && !strcmp(w[i], "sh") && !strcmp(w[i + 1], "-c")) i += 2;
		if (i + 1 < n && !strcmp(w[i], "c64-term") && !strcmp(w[i + 1], "-e")) i += 2;
	} while (i != k);
	if (i >= n) return 1;
	return in_path(w[i]);
}

static int add_act(int kind, const char *cmd, long arg)
{
	if (nacts >= MAXACT) return -1;
	acts[nacts].kind = kind;
	snprintf(acts[nacts].cmd, sizeof acts[nacts].cmd, "%s", cmd ? cmd : "");
	acts[nacts].arg = arg;
	return nacts++;
}

static void add_item(UiMenu *m, const char *label, int id, UiMenu *sub)
{
	if (m->n >= UI_MAXITEMS) return;
	snprintf(m->it[m->n].label, sizeof m->it[m->n].label, "%s", label);
	for (char *c = m->it[m->n].label; *c; c++) *c = toupper((unsigned char)*c);
	m->it[m->n].id = id;
	m->it[m->n].sub = sub;
	m->n++;
}

static UiMenu *new_menu(const char *title)
{
	if (nmenus >= MAXMENUS) return NULL;
	UiMenu *m = &menus[nmenus++];
	memset(m, 0, sizeof *m);
	snprintf(m->title, sizeof m->title, "%s", title);
	return m;
}

static char *prop_str(Window w, const char *name, Atom type)
{
	Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, w, atom(name), 0, 1024, False, type, &t, &f, &n, &after, &d) != Success || !d) return NULL;
	return (char *)d;
}

static long prop_card(Window w, const char *name)
{
	Atom t; int f; unsigned long n, after; unsigned char *d = NULL; long v = -1;
	if (XGetWindowProperty(dpy, w, atom(name), 0, 1, False, XA_CARDINAL, &t, &f, &n, &after, &d) == Success && d) {
		if (n) v = (long)*(unsigned long *)d;
		XFree(d);
	}
	return v;
}



// @desktops - desktops + their windows
static void add_desktops(UiMenu *m)
{
	long nd = prop_card(root, "_NET_NUMBER_OF_DESKTOPS"), cur = prop_card(root, "_NET_CURRENT_DESKTOP");
	if (nd < 1) nd = 1;
	Atom t; int f; unsigned long n = 0, after; unsigned char *d = NULL;
	XGetWindowProperty(dpy, root, atom("_NET_CLIENT_LIST"), 0, 1024, False, XA_WINDOW, &t, &f, &n, &after, &d);
	Window *cl = (Window *)d;
	Atom skip = atom("_NET_WM_STATE_SKIP_TASKBAR");
	for (long k = 0; k < nd && k < 16; k++) {
		char lab[64];
		snprintf(lab, sizeof lab, "%s DESKTOP %ld", k == cur ? "*" : " ", k + 1);
		add_item(m, lab, add_act(A_DESKTOP, NULL, k), NULL);
		for (unsigned long i = 0; cl && i < n; i++) {
			long wd = prop_card(cl[i], "_NET_WM_DESKTOP");
			if (wd != k && wd != -1 && wd != 0xFFFFFFFF) continue;
			if (wd == -1 && k != 0) continue;
			Atom *st = NULL; unsigned long ns = 0;
			unsigned char *sd = NULL;
			int skipped = 0;
			if (XGetWindowProperty(dpy, cl[i], atom("_NET_WM_STATE"), 0, 32, False, XA_ATOM, &t, &f, &ns, &after, &sd) == Success && sd) {
				st = (Atom *)sd;
				for (unsigned long j = 0; j < ns; j++) if (st[j] == skip) skipped = 1;
				XFree(sd);
			}
			if (skipped || cl[i] == desk) continue;
			char *name = prop_str(cl[i], "_NET_WM_NAME", atom("UTF8_STRING"));
			if (!name) name = prop_str(cl[i], "WM_NAME", XA_STRING);
			if (!name) continue;
			char wl[64];
			snprintf(wl, sizeof wl, "    %.56s", name);                  // not uppercased on purpose
			XFree(name);
			if (m->n < UI_MAXITEMS) {
				snprintf(m->it[m->n].label, sizeof m->it[m->n].label, "%s", wl);
				m->it[m->n].id = add_act(A_WINDOW, NULL, (long)cl[i]);
				m->it[m->n].sub = NULL;
				m->n++;
			}
		}
	}
	if (d) XFree(d);
}

static const struct { const char *name, *label; } builtins[] = {
	{ "new-shortcut", "NEW SHORTCUT..." }, { "new-folder", "NEW FOLDER..." }, { "new-file", "NEW TEXT FILE..." },
	{ "line-up", "LINE UP ICONS" },
};



/* recursive, one call per submenu level */
static void build(UiMenu *m, int *i, int level, int depth)
{
	while (*i < nlines && lines[*i].indent >= level) {
		Line *ln = &lines[*i];
		if (ln->indent > level) { (*i)++; continue; }
		char t[460];
		snprintf(t, sizeof t, "%s", ln->text);
		(*i)++;
		if (!strcmp(t, "-") || !strcmp(t, "---")) {
			if (m->n && strcmp(m->it[m->n - 1].label, "-")) add_item(m, "-", -1, NULL);
			continue;
		}
		if (t[0] == '@') {
			const char *b = t + 1;
			if (!strcmp(b, "desktops")) add_desktops(m);
			else if (!strcmp(b, "programs") && depth < 4) {
				Line save[600];
				int ns = nlines;
				memcpy(save, lines, sizeof(Line) * ns);
				read_section("PROGRAMS");
				int j = 0;
				build(m, &j, 0, depth + 1);
				memcpy(lines, save, sizeof(Line) * ns);
				nlines = ns;
			} else
				for (size_t k = 0; k < sizeof builtins / sizeof *builtins; k++)
					if (!strcmp(b, builtins[k].name)) add_item(m, builtins[k].label, add_act(A_BUILTIN, b, 0), NULL);
			continue;
		}
		size_t L = strlen(t);
		if (L >= 2 && t[L - 1] == '>') {
			t[L - 1] = 0;
			trim(t);
			UiMenu *sub = new_menu(t);
			if (!sub) continue;
			build(sub, i, level + 1, depth);
			while (sub->n && !strcmp(sub->it[sub->n - 1].label, "-")) sub->n--;
			if (sub->n) add_item(m, t, -1, sub);                        // drop empty submenus
			continue;
		}
		char *eq = strstr(t, " = ");
		if (!eq) continue;
		*eq = 0;
		char *cmd = eq + 3;
		trim(t);
		trim(cmd);
		if (!cmd_installed(cmd)) continue;
		add_item(m, t, add_act(A_CMD, cmd, 0), NULL);
	}
	while (m->n && !strcmp(m->it[m->n - 1].label, "-")) m->n--;
}

static int menu_x, menu_y;

static void run_action(int id, void *arg)
{
	(void)arg;
	if (id < 0 || id >= nacts) return;
	Action *a = &acts[id];
	switch (a->kind) {
	case A_CMD: spawn(a->cmd); break;
	case A_BUILTIN: items_builtin(a->cmd, menu_x, menu_y); break;
	case A_DESKTOP: case A_WINDOW: {
		XEvent e = { 0 };
		e.xclient.type = ClientMessage;
		e.xclient.format = 32;
		e.xclient.data.l[0] = 2;
		if (a->kind == A_DESKTOP) {
			e.xclient.window = root;
			e.xclient.message_type = atom("_NET_CURRENT_DESKTOP");
			e.xclient.data.l[0] = a->arg;
		} else {
			e.xclient.window = (Window)a->arg;
			e.xclient.message_type = atom("_NET_ACTIVE_WINDOW");
		}
		XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XFlush(dpy);
		break;
	}
	}
}




// re-read every time, cheap enough and the MENUS app may have changed it
void menus_desktop(int x, int y)
{
	nmenus = nacts = 0;
	read_section("DESKTOP");
	UiMenu *m = new_menu("DESKTOP");
	int i = 0;
	build(m, &i, 0, 0);
	if (!m->n) {                                 // no/empty menu file

		add_item(m, "NEW FOLDER...", add_act(A_BUILTIN, "new-folder", 0), NULL);
		add_item(m, "MENUS", add_act(A_CMD, "c64-menus", 0), NULL);
	}
	menu_x = x, menu_y = y;
	ui_menu_tree(x, y, m, run_action, NULL);
}
