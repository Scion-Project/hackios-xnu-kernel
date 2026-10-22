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
 *   - strings: rcGetGLString, cached; syncs: gl*SyncAEMU; glFinish: a round trip.
 */
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

API void
glGetIntegerv(GLenum pname, GLint *params)
{
	struct gfx *c = gfx_current();
	GLint *slot;
	if (c == NULL) {
		return;
	}
	if ((slot = store_slot(c, pname)) != NULL) {
		*params = *slot;
	} else if (pname == GL_ARRAY_BUFFER_BINDING) {
		*params = (GLint)c->array_buffer;
	} else if (pname == GL_ELEMENT_ARRAY_BUFFER_BINDING) {
		*params = (GLint)c->element_buffer;
	} else {
		gfxe_glGetIntegerv(c, pname, params);
	}
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
		gfx_flush(c);
	}
}

API void
glFinish(void)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		gfxe_glFinishRoundTrip(c);
	}
}

API void *
glFenceSync(GLenum condition, GLbitfield flags)
{
	struct gfx *c = gfx_current();
	return c ? (void *)(uintptr_t)gfxe_glFenceSyncAEMU(c, condition, flags) : NULL;
}

API GLenum
glClientWaitSync(void *sync, GLbitfield flags, GLuint64 timeout)
{
	struct gfx *c = gfx_current();
	return c ? gfxe_glClientWaitSyncAEMU(c, (uint64_t)(uintptr_t)sync, flags, timeout) : GL_TIMEOUT_EXPIRED;
}

API void
glWaitSync(void *sync, GLbitfield flags, GLuint64 timeout)
{
	struct gfx *c = gfx_current();
	if (c) {
		gfxe_glWaitSyncAEMU(c, (uint64_t)(uintptr_t)sync, flags, timeout);
	}
}

API void
glDeleteSync(void *sync)
{
	struct gfx *c = gfx_current();
	if (c && sync) {
		gfxe_glDeleteSyncAEMU(c, (uint64_t)(uintptr_t)sync);
	}
}

/* ---- markers: nothing to send ----------------------------------------------------------- */

API void glInsertEventMarkerEXT(GLsizei length, const GLchar *marker) { (void)length; (void)marker; }
API void glPushGroupMarkerEXT(GLsizei length, const GLchar *marker) { (void)length; (void)marker; }
API void glPopGroupMarkerEXT(void) { }
