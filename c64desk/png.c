/* png.c - minimal PNG writer for the icons
 *
 * No zlib: the pixels go into "stored" deflate blocks (= uncompressed).
 * Files get bigger but every reader takes them and icons are tiny anyway. */
#include "c64desk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static uint32_t crc_table[256];

static uint32_t crc(const unsigned char *b, size_t n, uint32_t c)
{
	if (!crc_table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
	return c;
}

static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *fp, const char *type, const unsigned char *d, uint32_t n)
{
	unsigned char h[8];
	be32(h, n);
	memcpy(h + 4, type, 4);
	fwrite(h, 1, 8, fp);
	if (n) fwrite(d, 1, n, fp);
	uint32_t c = crc(h + 4, 4, 0xffffffffu);   // crc covers type + data
	c = crc(d, n, c) ^ 0xffffffffu;
	be32(h, c);
	fwrite(h, 1, 4, fp);
}



// rgba = w*h*4 bytes. writes path.tmp then renames, returns 1 if ok
int png_write(const char *path, int w, int h, const unsigned char *rgba)
{
	size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
	unsigned char *raw = malloc(raw_n);
	if (!raw) return 0;
	for (int y = 0; y < h; y++) {
		raw[y * (1 + w * 4)] = 0;                                   // filter byte
		memcpy(raw + y * (1 + w * 4) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
	}

	/* zlib stream: header, stored blocks of max 64k, adler32 */
	size_t blocks = raw_n / 65535 + 1, zn = 2 + raw_n + blocks * 5 + 4;
	unsigned char *z = malloc(zn);
	if (!z) { free(raw); return 0; }
	size_t o = 0;
	z[o++] = 0x78; z[o++] = 0x01;
	for (size_t p = 0; p < raw_n || p == 0; ) {
		size_t len = raw_n - p > 65535 ? 65535 : raw_n - p;
		z[o++] = p + len >= raw_n;                                   // BFINAL
		z[o++] = len & 0xff; z[o++] = len >> 8;
		z[o++] = ~len & 0xff; z[o++] = (~len >> 8) & 0xff;
		memcpy(z + o, raw + p, len);
		o += len, p += len;
		if (!raw_n) break;
	}
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	be32(z + o, b << 16 | a);
	o += 4;

	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *fp = fopen(tmp, "wb");
	int ok = 0;
	if (fp) {
		static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
		unsigned char ihdr[13];
		be32(ihdr, w); be32(ihdr + 4, h);
		ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8 bit RGBA */
		fwrite(sig, 1, 8, fp);
		chunk(fp, "IHDR", ihdr, 13);
		chunk(fp, "IDAT", z, o);
		chunk(fp, "IEND", NULL, 0);
		ok = fclose(fp) == 0 && rename(tmp, path) == 0;
	}
	free(raw);
	free(z);
	return ok;
}
