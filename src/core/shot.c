// Screenshots of the surfaces themselves, with their transparency (for the README gallery and CI): with
// JELLY_SHOT_DIR set, each surface writes <dir>/<name>.png once, JELLY_SHOT_AT seconds after start (default 5).
// The pixels come straight from GL, so no compositor or screen capture is involved. A tiny PNG writer (stored
// deflate blocks, no compression library) keeps it dependency-free.
#include "gl.h"
#include "shot.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const unsigned char *p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t k = i;
      for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
      crc_table[i] = k;
    }
  c ^= 0xffffffffu;
  while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
  return c ^ 0xffffffffu;
}
static void be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
  unsigned char h[8]; be32(h, (uint32_t)n); memcpy(h + 4, type, 4);
  fwrite(h, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t c = crc(0, h + 4, 4); c = crc(c, data, n);
  unsigned char t[4]; be32(t, c); fwrite(t, 1, 4, f);
}

int shot_png(const char *path, const unsigned char *rgba, int w, int h) { // rows top to bottom, straight alpha
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  unsigned char ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
  ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0; // 8-bit RGBA
  chunk(f, "IHDR", ih, 13);
  size_t raw = (size_t)h * ((size_t)w * 4 + 1), blocks = raw / 65535 + 1;
  unsigned char *z = malloc(2 + raw + blocks * 5 + 4), *o = z;
  *o++ = 0x78; *o++ = 0x01;
  uint32_t a = 1, b = 0; // adler32 of the raw scanlines
  size_t left = raw, row = 0, col = 0;
  while (left) {
    size_t n = left > 65535 ? 65535 : left;
    *o++ = left == n; *o++ = (unsigned char)n; *o++ = (unsigned char)(n >> 8); *o++ = (unsigned char)~n; *o++ = (unsigned char)(~n >> 8);
    for (size_t i = 0; i < n; i++) {
      unsigned char v = col == 0 ? 0 : rgba[row * (size_t)w * 4 + col - 1]; // filter byte 0, then the row
      if (++col == (size_t)w * 4 + 1) { col = 0; row++; }
      *o++ = v; a = (a + v) % 65521; b = (b + a) % 65521;
    }
    left -= n;
  }
  be32(o, (b << 16) | a); o += 4;
  chunk(f, "IDAT", z, (size_t)(o - z));
  chunk(f, "IEND", NULL, 0);
  free(z);
  fclose(f);
  return 1;
}

/* reads the bound framebuffer (premultiplied, bottom-up) and saves it once, when it's time */
void shot_maybe(const char *name, int w, int h, double age) {
  const char *dir = getenv("JELLY_SHOT_DIR");
  if (!dir || !*dir) return;
  double at = getenv("JELLY_SHOT_AT") ? atof(getenv("JELLY_SHOT_AT")) : 5;
  if (age < at) return;
  static char done[16][32]; static int ndone;
  for (int i = 0; i < ndone; i++) if (!strcmp(done[i], name)) return;
  if (ndone < 16) snprintf(done[ndone++], sizeof done[0], "%s", name);
  unsigned char *px = malloc((size_t)w * h * 4), *out = malloc((size_t)w * h * 4);
  // a multisampled offscreen target (the jelly on Windows) can't be read directly: resolve it into a plain one
  GLint bound = 0, samples = 0;
  glGetIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &bound);
  glGetIntegerv(0x80A9 /* GL_SAMPLES */, &samples);
  GLuint tmpFb = 0, tmpRb = 0;
  if (bound && samples > 0) {
    glGenFramebuffers(1, &tmpFb); glGenRenderbuffers(1, &tmpRb);
    glBindRenderbuffer(GL_RENDERBUFFER, tmpRb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmpFb);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, tmpRb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)bound);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, tmpFb);
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  if (tmpFb) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
    glDeleteFramebuffers(1, &tmpFb); glDeleteRenderbuffers(1, &tmpRb);
  }
  for (int y = 0; y < h; y++) // flip, and undo the premultiplied alpha
    for (int x = 0; x < w; x++) {
      const unsigned char *s = px + ((size_t)(h - 1 - y) * w + x) * 4;
      unsigned char *d = out + ((size_t)y * w + x) * 4;
      int al = s[3];
      for (int c = 0; c < 3; c++) d[c] = (unsigned char)(al ? (s[c] * 255 + al / 2) / al > 255 ? 255 : (s[c] * 255 + al / 2) / al : 0);
      d[3] = (unsigned char)al;
    }
  char path[1024]; snprintf(path, sizeof path, "%s/%s.png", dir, name);
  shot_png(path, out, w, h);
  free(px); free(out);
}
