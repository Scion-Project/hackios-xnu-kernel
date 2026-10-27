/*
 * gfx: the gl* calls that need the client's state (see gfx.h). Everything else is a generated
 * weak entry point (obj/gfx_api.c) that encodes the call as it is.
 *
 *   - client-side vertex arrays: kept here, and sent with the draw that uses them
 *     (glVertexAttribPointerData, tightly packed: the host decoder takes stride 0), indices of
 *     glDrawElements as glDrawElementsData, rebased to the lowest index sent;
 *   - pixels: the host packs and unpacks with alignment 1 (gfx_make_current), so uploads are
 *     packed and readbacks unpacked here, by the client's pixel-store state;
 *   - glShaderSource: one string, glShaderString;
 *   - strings: rcGetGLString, cached; syncs: gl*SyncAEMU; glFinish: a round trip;
 *   - state the client set (bindings, the scissor, blending, the clear colour, the viewport, what a
 *     framebuffer draws into): asked for, it is answered here, as each query is a round trip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_enc.h"

#define API __attribute__((visibility("default")))

#define GL_NO_ERROR                     0
#define GL_ARRAY_BUFFER                 0x8892
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GL_UNSIGNED_BYTE                0x1401
#define GL_UNSIGNED_SHORT               0x1403
#define GL_UNSIGNED_INT                 0x1405
#define GL_INT_2_10_10_10_REV           0x8D9F
#define GL_UNSIGNED_INT_2_10_10_10_REV  0x8368
#define GL_UNPACK_ROW_LENGTH            0x0CF2
#define GL_UNPACK_SKIP_ROWS             0x0CF3
#define GL_UNPACK_SKIP_PIXELS           0x0CF4
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_PACK_ROW_LENGTH              0x0D02
#define GL_PACK_SKIP_ROWS               0x0D03
#define GL_PACK_SKIP_PIXELS             0x0D04
#define GL_PACK_ALIGNMENT               0x0D05
#define GL_ARRAY_BUFFER_BINDING         0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_TIMEOUT_EXPIRED              0x911B
#define GL_NONE                         0
#define GL_BLEND                        0x0BE2
#define GL_SCISSOR_TEST                 0x0C11
#define GL_SCISSOR_BOX                  0x0C10
#define GL_VIEWPORT                     0x0BA2
#define GL_COLOR_CLEAR_VALUE            0x0C22
#define GL_TEXTURE_BINDING_2D           0x8069
#define GL_DRAW_FRAMEBUFFER_BINDING     0x8CA6  /* = GL_FRAMEBUFFER_BINDING */
#define GL_READ_FRAMEBUFFER_BINDING     0x8CAA
#define GL_READ_FRAMEBUFFER             0x8CA8
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED  0x8622

/* ---- strings, state queries ------------------------------------------------------------- */

API const GLubyte *
glGetString(GLenum name)
{
	struct gfx *c = gfx_current();
	int slot = name >= 0x1F00 && name <= 0x1F03 ? (int)(name - 0x1F00) : name == 0x8B8C ? 4 : -1;
	if (c == NULL || slot < 0) {
		return NULL;
	}
	if (c->strings[slot] == NULL) {
		EGLint size = 1024;
		for (int tries = 0; tries < 3; tries++) {
			char *s = calloc(1, (size_t)size);
			EGLint n = gfxe_rcGetGLString(c, name, s, size);
			if (n >= 0 && n <= size) {
				s[size - 1] = 0;
				c->strings[slot] = s;
				break;
			}
			free(s);
			size = n < 0 ? -n + 16 : n + 16;
		}
	}
	return (const GLubyte *)c->strings[slot];
}

static GLint *
store_slot(struct gfx *c, GLenum pname)
{
	switch (pname) {
	case GL_UNPACK_ALIGNMENT: return &c->unpack_alignment;
	case GL_UNPACK_ROW_LENGTH: return &c->unpack_row_length;
	case GL_UNPACK_SKIP_ROWS: return &c->unpack_skip_rows;
	case GL_UNPACK_SKIP_PIXELS: return &c->unpack_skip_pixels;
	case GL_PACK_ALIGNMENT: return &c->pack_alignment;
	case GL_PACK_ROW_LENGTH: return &c->pack_row_length;
	case GL_PACK_SKIP_ROWS: return &c->pack_skip_rows;
	case GL_PACK_SKIP_PIXELS: return &c->pack_skip_pixels;
	default: return NULL;
	}
}

API void
glPixelStorei(GLenum pname, GLint param)
{
	struct gfx *c = gfx_current();
	GLint *slot;
	if (c != NULL && (slot = store_slot(c, pname)) != NULL) {
		*slot = param;
	}
}

/*
 * One answer given here in 256 is also asked of the host, as a check that the client state follows
 * the host's: a difference is logged once per kind, and that state is asked of the host from then on.
 */
static int
spot_check(struct gfx *c)
{
	return (c->answered & 255) == 0;
}

static void
differs(struct gfx *c, const char *what, GLenum pname, long here, long host)
{
	if (!(c->check_failed & 1u)) {
		fprintf(stderr, "gfx: %s %#x was answered %ld here, the host has %ld; asking the host from now on\n",
		    what, pname, here, host);
	}
	c->check_failed |= 1u;
}

API void
glGetIntegerv(GLenum pname, GLint *params)
{
	struct gfx *c = gfx_current();
	GLint *slot;
	if (c == NULL) {
		return;
	}
	if (c->check_failed) {
		/* (the pixel store and the buffer bindings were answered here before the checks: they stay) */
		if ((slot = store_slot(c, pname)) != NULL) {
			*params = *slot;
		} else {
			gfxe_glGetIntegerv(c, pname, params);
		}
		return;
	}
	if ((slot = store_slot(c, pname)) != NULL) {
		*params = *slot;
	} else if (pname == GL_ARRAY_BUFFER_BINDING) {
		*params = (GLint)c->array_buffer;
	} else if (pname == GL_ELEMENT_ARRAY_BUFFER_BINDING) {
		*params = (GLint)c->element_buffer;
	} else if (pname == GL_TEXTURE_BINDING_2D && !c->unit_over) {
		*params = (GLint)c->bound_2d[c->active_unit];
	} else if (pname == GL_DRAW_FRAMEBUFFER_BINDING) {
		*params = (GLint)c->draw_fbo;
	} else if (pname == GL_READ_FRAMEBUFFER_BINDING) {
		*params = (GLint)c->read_fbo;
	} else if (pname == GL_SCISSOR_BOX && c->scissor_known) {
		memcpy(params, c->scissor, sizeof(c->scissor));
	} else if (pname == GL_VIEWPORT && c->viewport_known) {
		memcpy(params, c->viewport, sizeof(c->viewport));
	} else {
		gfxe_glGetIntegerv(c, pname, params);
		/* the surface's box, until the client sets one: known from now on */
		if (pname == GL_SCISSOR_BOX && !c->err) {
			memcpy(c->scissor, params, sizeof(c->scissor));
			c->scissor_known = 1;
		} else if (pname == GL_VIEWPORT && !c->err) {
			memcpy(c->viewport, params, sizeof(c->viewport));
			c->viewport_known = 1;
		}
		return;
	}
	c->answered++;
	if (spot_check(c) && store_slot(c, pname) == NULL && pname != GL_ARRAY_BUFFER_BINDING &&
	    pname != GL_ELEMENT_ARRAY_BUFFER_BINDING) {
		GLint host[4] = { 0, 0, 0, 0 };
		int n = pname == GL_SCISSOR_BOX || pname == GL_VIEWPORT ? 4 : 1;
		gfxe_glGetIntegerv(c, pname, host);
		for (int i = 0; i < n; i++) {
			if (host[i] != params[i]) {
				differs(c, "glGetIntegerv", pname, params[i], host[i]);
				memcpy(params, host, sizeof(GLint) * (size_t)n);
				break;
			}
		}
	}
}

API GLboolean
glIsEnabled(GLenum cap)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return 0;
	}
	if ((cap == GL_SCISSOR_TEST || cap == GL_BLEND) && !c->check_failed) {
		GLboolean here = (GLboolean)(cap == GL_BLEND ? c->blend_on : c->scissor_on);
		c->answered++;
		if (spot_check(c)) {
			GLboolean host = gfxe_glIsEnabled(c, cap);
			if (host != here) {
				differs(c, "glIsEnabled", cap, here, host);
				return host;
			}
		}
		return here;
	}
	return gfxe_glIsEnabled(c, cap);
}

API void
glGetFloatv(GLenum pname, GLfloat *params)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (pname == GL_COLOR_CLEAR_VALUE && !c->clear_unknown && !c->check_failed) {
		c->answered++;
		memcpy(params, c->clear_colour, sizeof(c->clear_colour));
		if (spot_check(c)) {
			GLfloat host[4];
			gfxe_glGetFloatv(c, pname, host);
			if (memcmp(host, params, sizeof(host)) != 0) {
				differs(c, "glGetFloatv", pname, (long)(params[0] * 1000), (long)(host[0] * 1000));
				memcpy(params, host, sizeof(host));
			}
		}
		return;
	}
	gfxe_glGetFloatv(c, pname, params);
}

API void
glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	GLfloat v[4] = { red, green, blue, alpha };
	c->clear_unknown = 0;
	for (int i = 0; i < 4; i++) {
		if (!(v[i] >= 0.0f && v[i] <= 1.0f)) {
			c->clear_unknown = 1;           /* (the host may clamp it) */
		}
	}
	memcpy(c->clear_colour, v, sizeof(v));
	gfxe_glClearColor(c, red, green, blue, alpha);
}

API void
glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	c->viewport[0] = x;
	c->viewport[1] = y;
	c->viewport[2] = width;
	c->viewport[3] = height;
	/* (a negative size is refused; past GL_MAX_VIEWPORT_DIMS, at least 8192 on the hosts we know, it is clamped) */
	c->viewport_known = width >= 0 && height >= 0 && width <= 8192 && height <= 8192;
	gfxe_glViewport(c, x, y, width, height);
}

/* What a framebuffer draws into, as gfx_shared.c has followed it; anything else is asked. */
API void
glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	struct gfx_fbo *f = gfx_fbo_of(c, target == GL_READ_FRAMEBUFFER ? c->read_fbo : c->draw_fbo);
	if (f != NULL && attachment == GL_COLOR_ATTACHMENT0 && !c->check_failed &&
	    (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE || (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME && f->kind != GL_NONE))) {
		c->answered++;
		*params = pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE ? (GLint)f->kind : (GLint)f->texture;
		if (spot_check(c)) {
			GLint host = 0;
			gfxe_glGetFramebufferAttachmentParameteriv(c, target, attachment, pname, &host);
			if (host != *params) {
				differs(c, "glGetFramebufferAttachmentParameteriv", pname, *params, host);
				*params = host;
			}
		}
		return;
	}
	gfxe_glGetFramebufferAttachmentParameteriv(c, target, attachment, pname, params);
}

/* ---- pixels ----------------------------------------------------------------------------- */

/*
 * The rows of a w x h image in client memory laid out by (row length, skips, alignment): copy
 * them to or from a tight buffer. Returns the tight buffer (the client's own when it already is).
 */
static void *
repack(struct gfx *c, void *client, GLsizei w, GLsizei h, GLenum format, GLenum type, int pack, int to_client,
    void *tight_out)
{
	uint32_t bits = gfx_pixel_bits(format, type);
	GLint align = pack ? c->pack_alignment : c->unpack_alignment;
	GLint rowlen = pack ? c->pack_row_length : c->unpack_row_length;
	GLint skip_rows = pack ? c->pack_skip_rows : c->unpack_skip_rows;
	GLint skip_px = pack ? c->pack_skip_pixels : c->unpack_skip_pixels;
	size_t tight_row = ((size_t)bits * (size_t)w + 7) / 8;
	size_t row = ((size_t)bits * (size_t)(rowlen > 0 ? rowlen : w) + 7) / 8;
	if (align > 1) {
		row = (row + (size_t)align - 1) / (size_t)align * (size_t)align;
	}
	uint8_t *base = (uint8_t *)client + (size_t)skip_rows * row + (size_t)skip_px * bits / 8;
	if (row == tight_row && base == client && !to_client) {
		return client;
	}
	uint8_t *tight = tight_out ? tight_out : gfx_scratch(c, tight_row * (size_t)h);
	if (tight == NULL) {
		return NULL;
	}
	for (GLsizei y = 0; y < h; y++) {
		if (to_client) {
			memcpy(base + (size_t)y * row, tight + (size_t)y * tight_row, tight_row);
		} else {
			memcpy(tight + (size_t)y * tight_row, base + (size_t)y * row, tight_row);
		}
	}
	return tight;
}

API void
glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	void *p = pixels ? repack(c, (void *)pixels, width, height, format, type, 0, 0, NULL) : NULL;
	if (target == 0x0DE1 /* GL_TEXTURE_2D */ && level == 0) {
		gfx_note_texture_size(c, c->bound_2d[c->active_unit], (uint32_t)width, (uint32_t)height);
	}
	gfxe_glTexImage2D(c, target, level, internalformat, width, height, border, format, type, p);
}

API void
glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels)
{
	struct gfx *c = gfx_current();
	if (c == NULL || pixels == NULL) {
		return;
	}
	void *p = repack(c, (void *)pixels, width, height, format, type, 0, 0, NULL);
	gfxe_glTexSubImage2D(c, target, level, xoffset, yoffset, width, height, format, type, p);
}

API void
glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	size_t n = gfx_pixel_size(c, width, height, format, type, 1);
	int direct = c->pack_alignment <= 1 && c->pack_row_length == 0 && !c->pack_skip_rows && !c->pack_skip_pixels;
	if (!direct && c->pack_alignment == 4 && c->pack_row_length == 0 && !c->pack_skip_rows &&
	    !c->pack_skip_pixels && (gfx_pixel_bits(format, type) * (uint32_t)width) % 32 == 0) {
		direct = 1;     /* rows that are whole words: tight is aligned */
	}
	if (direct) {
		gfxe_glReadPixels(c, x, y, width, height, format, type, pixels);
		return;
	}
	void *tight = malloc(n);
	if (tight == NULL) {
		return;
	}
	gfxe_glReadPixels(c, x, y, width, height, format, type, tight);
	repack(c, pixels, width, height, format, type, 1, 1, tight);
	free(tight);
}

/* ---- shaders ---------------------------------------------------------------------------- */

API void
glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	struct gfx *c = gfx_current();
	size_t total = 1;
	if (c == NULL) {
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
	}
	char *s = malloc(total), *p = s;
	if (s == NULL) {
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
		memcpy(p, strings[i], n);
		p += n;
	}
	*p = 0;
	gfxe_glShaderString(c, shader, s, (GLsizei)total);
	free(s);
}

/* ---- buffers and vertex arrays ---------------------------------------------------------- */

API void
glBindBuffer(GLenum target, GLuint buffer)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_ARRAY_BUFFER) {
		c->array_buffer = buffer;
	} else if (target == GL_ELEMENT_ARRAY_BUFFER) {
		c->element_buffer = buffer;
	}
	gfxe_glBindBuffer(c, target, buffer);
}

API void
glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	for (GLsizei i = 0; i < n; i++) {
		if (buffers[i] && buffers[i] == c->array_buffer) {
			c->array_buffer = 0;
		}
		if (buffers[i] && buffers[i] == c->element_buffer) {
			c->element_buffer = 0;
		}
	}
	gfxe_glDeleteBuffers(c, n, buffers);
}

API void
glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
{
	struct gfx *c = gfx_current();
	if (c == NULL || index >= GFX_MAX_ATTRIBS) {
		return;
	}
	struct gfx_attrib *a = &c->attrib[index];
	a->size = size;
	a->type = type;
	a->normalized = normalized;
	a->stride = stride;
	a->pointer = pointer;
	a->buffer = c->array_buffer;
	if (a->buffer) {
		gfxe_glVertexAttribPointerOffset(c, index, size, type, normalized, stride, (GLuint)(uintptr_t)pointer);
	}
}

API void
glEnableVertexAttribArray(GLuint index)
{
	struct gfx *c = gfx_current();
	if (c == NULL || index >= GFX_MAX_ATTRIBS) {
		return;
	}
	c->attrib[index].enabled = 1;
	gfxe_glEnableVertexAttribArray(c, index);
}

API void
glDisableVertexAttribArray(GLuint index)
{
	struct gfx *c = gfx_current();
	if (c == NULL || index >= GFX_MAX_ATTRIBS) {
		return;
	}
	c->attrib[index].enabled = 0;
	gfxe_glDisableVertexAttribArray(c, index);
}

API void
glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED && index < GFX_MAX_ATTRIBS && c->vertex_array == 0 && !c->check_failed) {
		c->answered++;
		*params = c->attrib[index].enabled;
		if (spot_check(c)) {
			GLint host = 0;
			gfxe_glGetVertexAttribiv(c, index, pname, &host);
			if (host != *params) {
				differs(c, "glGetVertexAttribiv", pname, *params, host);
				*params = host;
			}
		}
		return;
	}
	gfxe_glGetVertexAttribiv(c, index, pname, params);
}

API void
glBindVertexArray(GLuint array)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		c->vertex_array = array;
		gfxe_glBindVertexArray(c, array);
	}
}

API void
glBindVertexArrayOES(GLuint array)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		c->vertex_array = array;
		gfxe_glBindVertexArrayOES(c, array);
	}
}

static uint32_t
attrib_size(const struct gfx_attrib *a)
{
	if (a->type == GL_INT_2_10_10_10_REV || a->type == GL_UNSIGNED_INT_2_10_10_10_REV) {
		return 4;
	}
	return (uint32_t)a->size * gfx_sizeof(a->type);
}

/* The client arrays' elements first .. first + count - 1, packed, before a draw. */
static void
send_arrays(struct gfx *c, GLint first, GLsizei count)
{
	for (GLuint i = 0; i < GFX_MAX_ATTRIBS; i++) {
		struct gfx_attrib *a = &c->attrib[i];
		if (!a->enabled || a->buffer || a->pointer == NULL || count <= 0) {
			continue;
		}
		uint32_t es = attrib_size(a);
		size_t stride = a->stride ? (size_t)a->stride : es;
		size_t len = (size_t)es * (size_t)count;
		const uint8_t *src = (const uint8_t *)a->pointer + (size_t)first * stride;
		uint8_t *packed;
		if (stride == es) {
			packed = (uint8_t *)src;
		} else {
			packed = malloc(len);
			if (packed == NULL) {
				continue;
			}
			for (GLsizei e = 0; e < count; e++) {
				memcpy(packed + (size_t)e * es, src + (size_t)e * stride, es);
			}
		}
		gfxe_glVertexAttribPointerData(c, i, a->size, a->type, a->normalized, a->stride, packed, (GLuint)len);
		if (packed != src) {
			free(packed);
		}
	}
}

static int
any_client_arrays(struct gfx *c)
{
	for (int i = 0; i < GFX_MAX_ATTRIBS; i++) {
		if (c->attrib[i].enabled && !c->attrib[i].buffer && c->attrib[i].pointer) {
			return 1;
		}
	}
	return 0;
}

API void
glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	struct gfx *c = gfx_current();
	if (c == NULL || count <= 0) {
		return;
	}
	gfx_drawing(c);
	if (any_client_arrays(c)) {
		send_arrays(c, first, count);
		/* the arrays now start at element `first`; buffer-backed ones are rare beside them */
		gfxe_glDrawArrays(c, mode, 0, count);
	} else {
		gfxe_glDrawArrays(c, mode, first, count);
	}
}

API void
glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	struct gfx *c = gfx_current();
	if (c == NULL || count <= 0) {
		return;
	}
	uint32_t isz = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
	gfx_drawing(c);
	if (c->element_buffer) {
		if (any_client_arrays(c)) {
			/* rare: indices in a buffer, vertices in client memory; send every vertex we may need */
			send_arrays(c, 0, 65536);
		}
		gfxe_glDrawElementsOffset(c, mode, count, type, (GLuint)(uintptr_t)indices);
		return;
	}
	uint32_t lo = UINT32_MAX, hi = 0;
	for (GLsizei i = 0; i < count; i++) {
		uint32_t v = isz == 1 ? ((const uint8_t *)indices)[i] : isz == 2 ? ((const uint16_t *)indices)[i]
		    : ((const uint32_t *)indices)[i];
		lo = v < lo ? v : lo;
		hi = v > hi ? v : hi;
	}
	if (!any_client_arrays(c)) {
		gfxe_glDrawElementsData(c, mode, count, type, (void *)indices, (GLuint)(count * (GLsizei)isz));
		return;
	}
	send_arrays(c, (GLint)lo, (GLsizei)(hi - lo + 1));
	void *rebased = (void *)indices;
	if (lo) {
		rebased = malloc((size_t)count * isz);
		if (rebased == NULL) {
			return;
		}
		for (GLsizei i = 0; i < count; i++) {
			if (isz == 1) {
				((uint8_t *)rebased)[i] = (uint8_t)(((const uint8_t *)indices)[i] - lo);
			} else if (isz == 2) {
				((uint16_t *)rebased)[i] = (uint16_t)(((const uint16_t *)indices)[i] - lo);
			} else {
				((uint32_t *)rebased)[i] = ((const uint32_t *)indices)[i] - lo;
			}
		}
	}
	gfxe_glDrawElementsData(c, mode, count, type, rebased, (GLuint)(count * (GLsizei)isz));
	if (rebased != indices) {
		free(rebased);
	}
}

/* ---- synchronization -------------------------------------------------------------------- */

API void
glFlush(void)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		gfxe_glFlush(c);
		gfx_sync_shared(c);
		gfx_flush(c);
		gfx_frame_end(c);
	}
}

API void
glFinish(void)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		gfx_finish(c);
		c->just_finished = 1;
		gfx_sync_shared(c);
		c->just_finished = 0;
		gfx_frame_end(c);
	}
}

/*
 * Fences without the host: a fence is a number, and waiting for one waits for everything
 * (glFinishRoundTrip, one round trip), which is also when the shared targets are copied back.
 * gfxstream's own fences (gl*SyncAEMU) would cost a round trip to make one, another to wait.
 * GL_SYNC_FLUSH_COMMANDS_BIT with timeout 0, eagl.c's frame end, is a wait too: the display
 * reads the surface as soon as the swap tells it to.
 */
#define GL_CONDITION_SATISFIED  0x911C

API void *
glFenceSync(GLenum condition, GLbitfield flags)
{
	static uintptr_t next = 1;
	(void)condition;
	(void)flags;
	return (void *)next++;
}

API GLenum
glClientWaitSync(void *sync, GLbitfield flags, GLuint64 timeout)
{
	struct gfx *c = gfx_current();
	(void)sync;
	(void)flags;
	(void)timeout;
	if (c == NULL) {
		return GL_TIMEOUT_EXPIRED;
	}
	gfx_finish(c);
	c->just_finished = 1;
	gfx_sync_shared(c);
	c->just_finished = 0;
	gfx_frame_end(c);
	return GL_CONDITION_SATISFIED;
}

API void
glWaitSync(void *sync, GLbitfield flags, GLuint64 timeout)
{
	(void)sync; (void)flags; (void)timeout;   /* one stream: the host runs it in order anyway */
}

API void
glDeleteSync(void *sync)
{
	(void)sync;
}

/* ---- markers: nothing to send ----------------------------------------------------------- */

API void glInsertEventMarkerEXT(GLsizei length, const GLchar *marker) { (void)length; (void)marker; }
API void glPushGroupMarkerEXT(GLsizei length, const GLchar *marker) { (void)length; (void)marker; }
API void glPopGroupMarkerEXT(void) { }
