/*
 * gfx: the stream, the sizes the generated encoders need, and contexts (see gfx.h).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_enc.h"

#define GFX_BUF         (256 * 1024)
#include <time.h>

/* Everything sent has been done by the host: one round trip, unless nothing was sent since. */
void
gfx_finish(struct gfx *c)
{
	if (c->bytes_out + c->used == c->finished_at) {
		return;
	}
	gfxe_glFinishRoundTrip(c);
	c->finished_at = c->bytes_out + c->used;
}

uint64_t
gfx_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void
gfx_frame_end(struct gfx *c)
{
	uint64_t now = gfx_now_us();
	c->frames++;
	if (c->report_at == 0) {
		c->report_at = now;
		c->report_frames = c->frames;
		c->r_trips = c->round_trips; c->r_out = c->bytes_out; c->r_in = c->bytes_in;
		c->r_down = c->us_download; c->r_up = c->us_upload; c->r_down_bytes = c->download_bytes;
		c->r_up_bytes = c->upload_bytes;
		c->r_presents = c->presents;
		c->r_answered = c->answered;
		c->r_paced = c->paced;
		c->us_write = c->us_read = c->us_paced = 0;
		return;
	}
	if (now - c->report_at < 5000000) {
		return;
	}
	uint64_t f = c->frames - c->report_frames, span = now - c->report_at;
	if (f) {
		fprintf(stderr, "gfx: %llu frames in %llu ms (%.1f a second); a frame: %llu round trips (%llu queries answered here), %llu KB out, %llu KB in;"
		    " %llu us writing, %llu us waiting for replies, %llu us copying back (%llu KB), %llu us uploading (%llu KB);"
		    " %llu shown by the host, %llu paced (%llu us waiting for the frame before)\n",
		    (unsigned long long)f, (unsigned long long)(span / 1000), f * 1e6 / (double)span,
		    (unsigned long long)((c->round_trips - c->r_trips) / f), (unsigned long long)((c->answered - c->r_answered) / f),
		    (unsigned long long)((c->bytes_out - c->r_out) / f / 1024),
		    (unsigned long long)((c->bytes_in - c->r_in) / f / 1024), (unsigned long long)(c->us_write / f),
		    (unsigned long long)(c->us_read / f), (unsigned long long)((c->us_download - c->r_down) / f),
		    (unsigned long long)((c->download_bytes - c->r_down_bytes) / f / 1024),
		    (unsigned long long)((c->us_upload - c->r_up) / f), (unsigned long long)((c->upload_bytes - c->r_up_bytes) / f / 1024),
		    (unsigned long long)(c->presents - c->r_presents), (unsigned long long)(c->paced - c->r_paced),
		    (unsigned long long)(c->us_paced / f));
	}
	c->report_at = now;
	c->report_frames = c->frames;
	c->r_trips = c->round_trips; c->r_out = c->bytes_out; c->r_in = c->bytes_in;
	c->r_down = c->us_download; c->r_up = c->us_upload; c->r_down_bytes = c->download_bytes;
	c->r_up_bytes = c->upload_bytes;
	c->r_presents = c->presents;
	c->r_answered = c->answered;
	c->r_paced = c->paced;
	c->us_write = c->us_read = c->us_paced = 0;
}

/* ---- the stream ------------------------------------------------------------------------- */

static void
fail(struct gfx *c, int err, const char *what)
{
	if (!c->err) {
		c->err = err ? err : -1;
		fprintf(stderr, "gfx: %s failed (%d); the context is dead\n", what, err);
	}
}

void
gfx_flush(struct gfx *c)
{
	if (c == NULL || c->err || c->used == 0) {
		return;
	}
	uint64_t t0 = gfx_now_us();
	int r = c->t.write(c->t.arg, c->buf, c->used);
	c->us_write += gfx_now_us() - t0;
	c->bytes_out += c->used;
	c->used = 0;
	if (r) {
		fail(c, r, "write");
	}
}

uint8_t *
gfx_begin(struct gfx *c, uint32_t op, uint32_t size)
{
	if (c == NULL || c->err) {
		return NULL;
	}
	if (c->used + size > c->cap) {
		gfx_flush(c);
		if (size > c->cap) {
			uint8_t *b = realloc(c->buf, size);
			if (b == NULL) {
				fail(c, 12, "realloc");
				return NULL;
			}
			c->buf = b;
			c->cap = size;
		}
	}
	uint8_t *p = c->buf + c->used;
	c->used += size;
	memcpy(p, &op, 4);
	memcpy(p + 4, &size, 4);
	return p + 8;
}

void
gfx_read(struct gfx *c, void *buf, size_t n)
{
	if (c == NULL) {
		return;
	}
	gfx_flush(c);
	if (c->err) {
		memset(buf, 0, n);
		return;
	}
	uint64_t t0 = gfx_now_us();
	int r = c->t.read(c->t.arg, buf, n);
	c->us_read += gfx_now_us() - t0;
	c->bytes_in += n;
	c->round_trips++;
	if (r) {
		fail(c, r, "read");
		memset(buf, 0, n);
	}
}

void *
gfx_scratch(struct gfx *c, size_t n)
{
	if (n > c->scratch_cap) {
		void *p = realloc(c->scratch, n);
		if (p == NULL) {
			return NULL;
		}
		c->scratch = p;
		c->scratch_cap = n;
	}
	return c->scratch;
}

/* ---- sizes -------------------------------------------------------------------------------- */

static uint32_t
components(GLenum format)
{
	switch (format) {
	case 0x1906: /* GL_ALPHA */
	case 0x1909: /* GL_LUMINANCE */
	case 0x1903: /* GL_RED */
	case 0x8D94: /* GL_RED_INTEGER */
	case 0x1902: /* GL_DEPTH_COMPONENT */
	case 0x1901: /* GL_STENCIL_INDEX */
		return 1;
	case 0x190A: /* GL_LUMINANCE_ALPHA */
	case 0x8227: /* GL_RG */
	case 0x8228: /* GL_RG_INTEGER */
	case 0x84F9: /* GL_DEPTH_STENCIL */
		return 2;
	case 0x1907: /* GL_RGB */
	case 0x8D98: /* GL_RGB_INTEGER */
		return 3;
	default:     /* GL_RGBA, GL_BGRA_EXT, GL_RGBA_INTEGER */
		return 4;
	}
}

uint32_t
gfx_sizeof(GLenum type)
{
	switch (type) {
	case 0x1400: case 0x1401: return 1;                           /* (UNSIGNED_)BYTE */
	case 0x1402: case 0x1403: case 0x140B: case 0x8D61: return 2; /* (UNSIGNED_)SHORT, HALF_FLOAT(_OES) */
	case 0x8B50: case 0x8B53: case 0x8DC6: return 8;              /* vec2, ivec2, uvec2 */
	case 0x8B51: case 0x8B54: case 0x8DC7: return 12;
	case 0x8B52: case 0x8B55: case 0x8DC8: case 0x8B5A: return 16; /* vec4, ivec4, uvec4, mat2 */
	case 0x8B5B: return 36;                                         /* mat3 */
	case 0x8B5C: return 64;                                         /* mat4 */
	default: return 4;
	}
}

uint32_t
gfx_pixel_bits(GLenum format, GLenum type)
{
	switch (type) {
	case 0x8363: case 0x8033: case 0x8034:          /* 5_6_5, 4_4_4_4, 5_5_5_1 */
		return 16;
	case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E: /* 2_10_10_10_REV, 24_8, 10F_11F_11F_REV, 5_9_9_9_REV */
		return 32;
	case 0x8DAD:                                    /* FLOAT_32_UNSIGNED_INT_24_8_REV */
		return 64;
	default:
		return components(format) * gfx_sizeof(type) * 8;
	}
}

/*
 * Pixels are always sent and read back tightly packed: this library sets the host's pack and
 * unpack alignment to 1 when it makes a context, never passes the other pixel-store state on,
 * and packs and unpacks rows itself (gfx_gl.c).
 */
uint32_t
gfx_pixel_size(struct gfx *c, GLsizei w, GLsizei h, GLenum format, GLenum type, int pack)
{
	(void)c;
	(void)pack;
	return (uint32_t)(((uint64_t)gfx_pixel_bits(format, type) * (uint32_t)w + 7) / 8 * (uint32_t)h);
}

uint32_t
gfx_pixelDataSize3D(struct gfx *c, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, int pack)
{
	return gfx_pixel_size(c, w, h, format, type, pack) * (uint32_t)d;
}

/* how many values a glGet* of this name writes (the host writes back what is asked for) */
uint32_t
gfx_param_size(GLenum pname)
{
	switch (pname) {
	case 0x846D: /* ALIASED_POINT_SIZE_RANGE */
	case 0x846E: /* ALIASED_LINE_WIDTH_RANGE */
	case 0x0D3A: /* MAX_VIEWPORT_DIMS */
	case 0x0B70: /* DEPTH_RANGE */
		return 2;
	case 0x0BA2: /* VIEWPORT */
	case 0x0C10: /* SCISSOR_BOX */
	case 0x0C22: /* COLOR_CLEAR_VALUE */
	case 0x0C23: /* COLOR_WRITEMASK */
	case 0x8005: /* BLEND_COLOR */
		return 4;
	case 0x86A3: /* COMPRESSED_TEXTURE_FORMATS */
	case 0x8DF8: /* SHADER_BINARY_FORMATS */
	case 0x87FF: /* PROGRAM_BINARY_FORMATS */
		return 64;
	default:
		return 1;
	}
}

GLenum
gfx_uniformType(struct gfx *c, GLuint program, GLint location)
{
	(void)c; (void)program; (void)location;
	return 0x8B5C;      /* the largest: a mat4; callers here never read uniforms back */
}

uint32_t
gfx_clearBufferNumElts(struct gfx *c, GLenum buffer)
{
	(void)c;
	return buffer == 0x1800 /* GL_COLOR */ ? 4 : 1;
}

uint32_t
gfx_glActiveUniformBlockivParamSize(struct gfx *c, GLuint program, GLuint index, GLenum pname)
{
	(void)c; (void)program; (void)index;
	return pname == 0x8A43 /* UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES */ ? 64 : 1;
}

/* ---- contexts ------------------------------------------------------------------------------ */

static pthread_key_t current_key;
static pthread_once_t current_once = PTHREAD_ONCE_INIT;

static void
current_init(void)
{
	pthread_key_create(&current_key, NULL);
}

struct gfx *
gfx_current(void)
{
	pthread_once(&current_once, current_init);
	return pthread_getspecific(current_key);
}

void
gfx_set_current(struct gfx *c)
{
	pthread_once(&current_once, current_init);
	pthread_setspecific(current_key, c);
}

#define EGL_ALPHA_SIZE          0x3021
#define EGL_BLUE_SIZE           0x3022
#define EGL_GREEN_SIZE          0x3023
#define EGL_RED_SIZE            0x3024
#define EGL_DEPTH_SIZE          0x3025
#define EGL_STENCIL_SIZE        0x3026
#define EGL_SURFACE_TYPE        0x3033
#define EGL_NONE                0x3038
#define EGL_RENDERABLE_TYPE     0x3040
#define EGL_PBUFFER_BIT         0x0001
#define EGL_WINDOW_BIT          0x0004
#define EGL_OPENGL_ES2_BIT      0x0004
#define GL_RGBA                 0x1908
#define GL_PACK_ALIGNMENT       0x0D05
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define WINDOW                  16

struct gfx *
gfx_create(const struct gfx_transport *t, int version)
{
	struct gfx *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}
	c->t = *t;
	c->cap = GFX_BUF;
	c->buf = malloc(c->cap);
	c->version = version;
	c->unpack_alignment = c->pack_alignment = 4;

	/* the render thread reads a flags word first (unused) */
	uint32_t flags = 0;
	if (c->buf == NULL || (!c->t.flags_sent && c->t.write(c->t.arg, &flags, 4) != 0)) {
		gfx_destroy(c);
		return NULL;
	}
	const EGLint attribs[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
	};
	uint32_t configs[4] = { 0 };
	EGLint n = gfxe_rcChooseConfig(c, (EGLint *)attribs, sizeof(attribs), configs, 4);
	if (n <= 0) {
		/* without depth and stencil, then */
		const EGLint plain[] = {
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
		};
		n = gfxe_rcChooseConfig(c, (EGLint *)plain, sizeof(plain), configs, 4);
	}
	c->config = configs[0];
	/* gfxstream's GLESApi: 2 = ES 2, 3 = ES 3.0 */
	c->rc_context = n > 0 ? gfxe_rcCreateContext(c, c->config, 0, version >= 3 ? 3 : 2) : 0;
	c->rc_surface = c->rc_context ? gfxe_rcCreateWindowSurface(c, c->config, WINDOW, WINDOW) : 0;
	c->rc_color_buffer = c->rc_surface ? gfxe_rcCreateColorBuffer(c, WINDOW, WINDOW, GL_RGBA) : 0;
	if (c->err || !c->rc_color_buffer) {
		fprintf(stderr, "gfx: no context (configs %d, context %u, surface %u, colour buffer %u)\n",
		    n, c->rc_context, c->rc_surface, c->rc_color_buffer);
		gfx_destroy(c);
		return NULL;
	}
	gfxe_rcSetWindowColorBuffer(c, c->rc_surface, c->rc_color_buffer);
	return c;
}

int
gfx_make_current(struct gfx *c)
{
	struct gfx *old = gfx_current();
	if (old != NULL && old != c) {
		gfx_flush(old);
	}
	if (c == NULL) {
		if (old != NULL) {
			gfxe_rcMakeCurrent(old, 0, 0, 0);
		}
		gfx_set_current(NULL);
		return 1;
	}
	if (gfxe_rcMakeCurrent(c, c->rc_context, c->rc_surface, c->rc_surface) != 1) {
		return 0;
	}
	gfx_set_current(c);
	if (!c->host_store_set) {
		c->host_store_set = 1;
		/* once: the host packs and unpacks tightly; the client's pixel-store state stays here */
		gfxe_glPixelStorei(c, GL_PACK_ALIGNMENT, 1);
		gfxe_glPixelStorei(c, GL_UNPACK_ALIGNMENT, 1);
	}
	return 1;
}

void
gfx_destroy(struct gfx *c)
{
	if (c == NULL) {
		return;
	}
	if (gfx_current() == c) {
		gfx_set_current(NULL);
	}
	if (!c->err && c->rc_context) {
		gfxe_rcDestroyWindowSurface(c, c->rc_surface);
		gfxe_rcCloseColorBuffer(c, c->rc_color_buffer);
		gfxe_rcDestroyContext(c, c->rc_context);
		gfx_flush(c);
	}
	if (c->t.close) {
		c->t.close(c->t.arg);
	}
	for (int i = 0; i < 8; i++) {
		free(c->strings[i]);
	}
	free(c->scratch);
	free(c->buf);
	free(c);
}
