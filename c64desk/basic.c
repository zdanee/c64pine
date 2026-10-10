/* basic.c
 *
 * BASIC V2 program <-> text listing, used for C64.SAV.
 * Non-ASCII PETSCII is written as {N} (decimal), like petcat does.
 * Crunching follows the ROM's rules: keyword table order, nothing in quotes,
 * nothing after REM, DATA until the next ':'
 */
#include "c64desk.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const KW[] = {
	"END", "FOR", "NEXT", "DATA", "INPUT#", "INPUT", "DIM", "READ", "LET", "GOTO", "RUN", "IF", "RESTORE", "GOSUB",
	"RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE", "VERIFY", "DEF", "POKE", "PRINT#", "PRINT", "CONT", "LIST",
	"CLR", "CMD", "SYS", "OPEN", "CLOSE", "GET", "NEW", "TAB(", "TO", "FN", "SPC(", "THEN", "NOT", "STEP", "+", "-",
	"*", "/", "^", "AND", "OR", ">", "=", "<", "SGN", "INT", "ABS", "USR", "FRE", "POS", "SQR", "RND", "LOG", "EXP",
	"COS", "SIN", "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$", "LEFT$", "RIGHT$", "MID$", "GO",
};
#define NKW ((int)(sizeof KW / sizeof *KW))      // $80-$CB

static void put_char(FILE *f, unsigned char c)
{
	if ((c >= 0x20 && c <= 0x5B) || c == 0x5D) fputc(c, f);
	else fprintf(f, "{%d}", c);
}



// returns number of lines
int basic_detok(const unsigned char *p, int len, unsigned start, FILE *f)
{
	int o = 0, lines = 0;
	while (o + 4 <= len) {
		unsigned link = p[o] | p[o + 1] << 8;
		if (!link) break;
		unsigned num = p[o + 2] | p[o + 3] << 8;
		int i = o + 4, quote = 0;
		fprintf(f, "%u ", num);
		while (i < len && p[i]) {
			unsigned char c = p[i++];
			if (c == '"') quote = !quote;
			if (!quote && c >= 0x80 && c < 0x80 + NKW) fputs(KW[c - 0x80], f);
			else if (!quote && c == 0xFF) fputs("{255}", f);       /* pi */
			else put_char(f, c);
		}
		fputc('\n', f);
		lines++;
		if (link <= start + o || link - start > (unsigned)len) break;        // broken link chain
		o = link - start;
	}
	return lines;
}



/* one line, without the line number -> tokens */
static int crunch_line(const char *s, unsigned char *out, int max)
{
	unsigned char t[512];
	int n = 0;
	for (const char *q = s; *q && n < (int)sizeof t - 1; q++) {          // resolve {N} + uppercase first
		if (*q == '{') {
			char *e;
			long v = strtol(q + 1, &e, 10);
			if (e != q + 1 && *e == '}' && v >= 0 && v < 256) { t[n++] = (unsigned char)v; q = e; continue; }
		}
		unsigned char c = (unsigned char)*q;
		if (c == '\r' || c == '\n') continue;
		t[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
	}
	int o = 0, quote = 0, rem = 0, data = 0;
	for (int i = 0; i < n && o < max - 1;) {
		unsigned char c = t[i];
		if (rem || c >= 0x80) { out[o++] = c; i++; continue; }
		if (c == '"') { quote = !quote; out[o++] = c; i++; continue; }
		if (quote) { out[o++] = c; i++; continue; }
		if (data) { if (c == ':') data = 0; out[o++] = c; i++; continue; }
		if (c == '?') { out[o++] = 0x99; i++; continue; }
		if (c == ' ' || (c >= '0' && c <= '9') || c == ':' || c == ';') { out[o++] = c; i++; continue; }
		int k;
		for (k = 0; k < NKW; k++) {
			size_t l = strlen(KW[k]);
			if (i + (int)l <= n && memcmp(t + i, KW[k], l) == 0) break;
		}
		if (k < NKW) {
			out[o++] = 0x80 + k;
			i += strlen(KW[k]);
			if (k == 0x8F - 0x80) rem = 1;
			if (k == 0x83 - 0x80) data = 1;
			continue;
		}
		out[o++] = c;
		i++;
	}
	return o;
}




/* listing file -> program bytes, linked for address `start`.
 * returns length incl. the final 00 00, or 0 if there were no lines */
int basic_crunch(const char *file, unsigned char *prog, int max, unsigned start)
{
	FILE *f = fopen(file, "r");
	if (!f) return 0;
	char line[1024];
	int len = 0, lines = 0;
	long last = -1;
	while (fgets(line, sizeof line, f)) {
		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		if (!isdigit((unsigned char)*s)) continue;
		long num = strtol(s, &s, 10);
		if (num < 0 || num > 63999 || num <= last) continue;
		if (*s == ' ') s++;
		unsigned char body[512];
		int bl = crunch_line(s, body, sizeof body);
		if (len + 4 + bl + 1 + 2 > max) break;
		unsigned link = start + len + 4 + bl + 1;
		prog[len] = link & 0xff; prog[len + 1] = link >> 8;
		prog[len + 2] = num & 0xff; prog[len + 3] = num >> 8;
		memcpy(prog + len + 4, body, bl);
		prog[len + 4 + bl] = 0;
		len += 4 + bl + 1;
		last = num;
		lines++;
	}
	fclose(f);
	if (!lines) return 0;
	prog[len++] = 0;
	prog[len++] = 0;
	return len;
}
