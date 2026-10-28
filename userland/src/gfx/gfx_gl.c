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
#define GL_BLEND_DST_RGB                0x80C8
#define GL_BLEND_SRC_RGB                0x80C9
#define GL_BLEND_DST_ALPHA              0x80CA
#define GL_BLEND_SRC_ALPHA              0x80CB
#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE_WIDTH                0x1000
#define GL_TEXTURE_HEIGHT               0x1001

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

/* What a failed check turns off: each kind of answer on its own, so a host that is wrong about one leaves the rest. */
#define CHECK_STATE     1u              /* bindings, the scissor, the viewport, blending on, the clear colour, attachments, arrays */
#define CHECK_BLEND     2u              /* the blend factors */
#define CHECK_SIZES     4u              /* a texture's size */
#define CHECK_NAMES     8u              /* uniform locations */
#define CHECK_VALUES    16u             /* uniform values */

static void
differs(struct gfx *c, unsigned kind, const char *what, GLenum pname, long here, long host)
{
	if (!(c->check_failed & kind)) {
		fprintf(stderr, "gfx: %s %#x was answered %ld here, the host has %ld; asking the host for it from now on\n",
		    what, pname, here, host);
	}
	c->check_failed |= kind;
}

API void
glGetIntegerv(GLenum pname, GLint *params)
{
	struct gfx *c = gfx_current();
	GLint *slot;
	if (c == NULL) {
		return;
	}
	/* (the pixel store and the buffer bindings were answered here before the checks: they stay) */
	int state = !(c->check_failed & CHECK_STATE);
	int blend = pname >= GL_BLEND_DST_RGB && pname <= GL_BLEND_SRC_ALPHA;
	if ((slot = store_slot(c, pname)) != NULL) {
		*params = *slot;
	} else if (pname == GL_ARRAY_BUFFER_BINDING) {
		*params = (GLint)c->array_buffer;
	} else if (pname == GL_ELEMENT_ARRAY_BUFFER_BINDING) {
		*params = (GLint)c->element_buffer;
	} else if (state && pname == GL_TEXTURE_BINDING_2D && !c->unit_over) {
		*params = (GLint)c->bound_2d[c->active_unit];
	} else if (state && pname == GL_DRAW_FRAMEBUFFER_BINDING) {
		*params = (GLint)c->draw_fbo;
	} else if (state && pname == GL_READ_FRAMEBUFFER_BINDING) {
		*params = (GLint)c->read_fbo;
	} else if (state && pname == GL_SCISSOR_BOX && c->scissor_known) {
		memcpy(params, c->scissor, sizeof(c->scissor));
	} else if (state && pname == GL_VIEWPORT && c->viewport_known) {
		memcpy(params, c->viewport, sizeof(c->viewport));
	} else if (blend && c->blend_func_known && !(c->check_failed & CHECK_BLEND)) {
		static const int which[4] = { 1, 0, 3, 2 };     /* DST_RGB, SRC_RGB, DST_ALPHA, SRC_ALPHA */
		*params = (GLint)c->blend_func[which[pname - GL_BLEND_DST_RGB]];
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
				differs(c, blend ? CHECK_BLEND : CHECK_STATE, "glGetIntegerv", pname, params[i], host[i]);
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
	if ((cap == GL_SCISSOR_TEST || cap == GL_BLEND) && !(c->check_failed & CHECK_STATE)) {
		GLboolean here = (GLboolean)(cap == GL_BLEND ? c->blend_on : c->scissor_on);
		c->answered++;
		if (spot_check(c)) {
			GLboolean host = gfxe_glIsEnabled(c, cap);
			if (host != here) {
				differs(c, CHECK_STATE, "glIsEnabled", cap, here, host);
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
	if (pname == GL_COLOR_CLEAR_VALUE && !c->clear_unknown && !(c->check_failed & CHECK_STATE)) {
		c->answered++;
		memcpy(params, c->clear_colour, sizeof(c->clear_colour));
		if (spot_check(c)) {
			GLfloat host[4];
			gfxe_glGetFloatv(c, pname, host);
			if (memcmp(host, params, sizeof(host)) != 0) {
				differs(c, CHECK_STATE, "glGetFloatv", pname, (long)(params[0] * 1000), (long)(host[0] * 1000));
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
	if (f != NULL && attachment == GL_COLOR_ATTACHMENT0 && !(c->check_failed & CHECK_STATE) &&
	    (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE || (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME && f->kind != GL_NONE))) {
		c->answered++;
		*params = pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE ? (GLint)f->kind : (GLint)f->texture;
		if (spot_check(c)) {
			GLint host = 0;
			gfxe_glGetFramebufferAttachmentParameteriv(c, target, attachment, pname, &host);
			if (host != *params) {
				differs(c, CHECK_STATE, "glGetFramebufferAttachmentParameteriv", pname, *params, host);
				*params = host;
			}
		}
		return;
	}
	gfxe_glGetFramebufferAttachmentParameteriv(c, target, attachment, pname, params);
}

/* A blend factor glBlendFunc takes (anything else is refused, and changes nothing). */
static int
blend_factor(GLenum f)
{
	return f <= 1 || (f >= 0x0300 && f <= 0x0308) || (f >= 0x8001 && f <= 0x8004);
}

API void
glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	c->blend_func_known = blend_factor(sfactor) && blend_factor(dfactor) && dfactor != 0x0308;
	c->blend_func[0] = c->blend_func[2] = sfactor;
	c->blend_func[1] = c->blend_func[3] = dfactor;
	gfxe_glBlendFunc(c, sfactor, dfactor);
}

API void
glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	c->blend_func_known = blend_factor(srcRGB) && blend_factor(dstRGB) && blend_factor(srcAlpha) &&
	    blend_factor(dstAlpha) && dstRGB != 0x0308 && dstAlpha != 0x0308;
	c->blend_func[0] = srcRGB;
	c->blend_func[1] = dstRGB;
	c->blend_func[2] = srcAlpha;
	c->blend_func[3] = dstAlpha;
	gfxe_glBlendFuncSeparate(c, srcRGB, dstRGB, srcAlpha, dstAlpha);
}

/* (one draw buffer's factors: what glGetIntegerv answers for is then not known here) */
API void
glBlendFunciEXT(GLuint buf, GLenum sfactor, GLenum dfactor)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		c->blend_func_known = 0;
		gfxe_glBlendFunciEXT(c, buf, sfactor, dfactor);
	}
}

API void
glBlendFuncSeparateiEXT(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		c->blend_func_known = 0;
		gfxe_glBlendFuncSeparateiEXT(c, buf, srcRGB, dstRGB, srcAlpha, dstAlpha);
	}
}

/* The width and height of the bound 2D texture's first level, as followed here. */
API void
glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
	struct gfx *c = gfx_current();
	uint32_t width, height;
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D && level == 0 && (pname == GL_TEXTURE_WIDTH || pname == GL_TEXTURE_HEIGHT) &&
	    !c->unit_over && !(c->check_failed & CHECK_SIZES) && gfx_texture_size(c, c->bound_2d[c->active_unit], &width, &height)) {
		*params = (GLint)(pname == GL_TEXTURE_WIDTH ? width : height);
		c->answered++;
		if (spot_check(c)) {
			GLint host = 0;
			gfxe_glGetTexLevelParameteriv(c, target, level, pname, &host);
			if (host != *params) {
				differs(c, CHECK_SIZES, "glGetTexLevelParameteriv", pname, *params, host);
				*params = host;
			}
		}
		return;
	}
	gfxe_glGetTexLevelParameteriv(c, target, level, pname, params);
}

/* What else gives a 2D texture's first level its size. */
API void
glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D && !c->unit_over) {
		gfx_note_texture_size(c, c->bound_2d[c->active_unit], (uint32_t)width, (uint32_t)height);
	}
	gfxe_glTexStorage2D(c, target, levels, internalformat, width, height);
}

API void
glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height,
    GLint border)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D && level == 0 && !c->unit_over) {
		gfx_note_texture_size(c, c->bound_2d[c->active_unit], (uint32_t)width, (uint32_t)height);
	}
	gfxe_glCopyTexImage2D(c, target, level, internalformat, x, y, width, height, border);
}

API void
glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
    GLsizei imageSize, const void *data)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D && level == 0 && !c->unit_over) {
		gfx_note_texture_size(c, c->bound_2d[c->active_unit], (uint32_t)width, (uint32_t)height);
	}
	gfxe_glCompressedTexImage2D(c, target, level, internalformat, width, height, border, imageSize, data);
}

API void
glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D && !c->unit_over) {
		gfx_note_texture_size(c, c->bound_2d[c->active_unit], 0, 0);     /* (the image's: not known here) */
	}
	gfxe_glEGLImageTargetTexture2DOES(c, target, image);
}

/* ---- uniforms ----------------------------------------------------------------------------- */

static uint32_t
uniform_hash(GLuint program, const char *name, GLint location)
{
	uint32_t h = 2166136261u ^ (program * 2654435761u) ^ ((uint32_t)location * 40503u);
	for (; name != NULL && *name; name++) {
		h = (h ^ (uint8_t)*name) * 16777619u;
	}
	return h % GFX_UNIFORMS;
}

/* Linking gives a program its locations anew, and every uniform 0; a program deleted takes them with it. */
static void
uniforms_forget(struct gfx *c, GLuint program)
{
	for (int i = 0; i < GFX_UNIFORMS; i++) {
		if (c->uniform_names[i].program == program) {
			c->uniform_names[i].program = 0;
		}
		if (c->uniform_values[i].program == program) {
			c->uniform_values[i].program = 0;
		}
	}
}

API void
glLinkProgram(GLuint program)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		uniforms_forget(c, program);
		gfxe_glLinkProgram(c, program);
	}
}

API void
glDeleteProgram(GLuint program)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		uniforms_forget(c, program);
		gfxe_glDeleteProgram(c, program);
	}
}

API void
glUseProgram(GLuint program)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		c->program = program;
		gfxe_glUseProgram(c, program);
	}
}

API GLint
glGetUniformLocation(GLuint program, const GLchar *name)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return -1;
	}
	if (program == 0 || name == NULL || strlen(name) >= sizeof(c->uniform_names[0].name) ||
	    (c->check_failed & CHECK_NAMES)) {
		return gfxe_glGetUniformLocation(c, program, name);
	}
	struct gfx_uniform_name *u = &c->uniform_names[uniform_hash(program, name, 0)];
	if (u->program == program && strcmp(u->name, name) == 0) {
		c->answered++;
		if (spot_check(c)) {
			GLint host = gfxe_glGetUniformLocation(c, program, name);
			if (host != u->location) {
				differs(c, CHECK_NAMES, "glGetUniformLocation", 0, u->location, host);
				return host;
			}
		}
		return u->location;
	}
	GLint location = gfxe_glGetUniformLocation(c, program, name);
	if (!c->err) {
		u->program = program;
		u->location = location;
		strcpy(u->name, name);
	}
	return location;
}

/* A float uniform of the program in use set to n floats (count of them from `location` on). */
static void
uniform_set(struct gfx *c, GLint location, GLsizei count, int n, const GLfloat *v)
{
	if (c->program == 0 || location < 0 || count < 1) {
		return;
	}
	struct gfx_uniform_value *u = &c->uniform_values[uniform_hash(c->program, NULL, location)];
	u->program = c->program;
	u->location = location;
	u->n = n;
	memcpy(u->v, v, sizeof(GLfloat) * (size_t)n);
	/* (an array's other elements: their locations are not known to follow on; forget whatever is there) */
	for (GLsizei i = 1; i < count && i < 64; i++) {
		struct gfx_uniform_value *o = &c->uniform_values[uniform_hash(c->program, NULL, location + i)];
		if (o->program == c->program && o->location == location + i) {
			o->program = 0;
		}
	}
}

API void
glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		const GLfloat v[4] = { x, y, z, w };
		uniform_set(c, location, 1, 4, v);
		gfxe_glUniform4f(c, location, x, y, z, w);
	}
}

API void
glUniform4fv(GLint location, GLsizei count, const GLfloat *v)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		if (v != NULL) {
			uniform_set(c, location, count, 4, v);
		}
		gfxe_glUniform4fv(c, location, count, v);
	}
}

API void
glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
	struct gfx *c = gfx_current();
	if (c != NULL) {
		if (value != NULL && !transpose) {
			uniform_set(c, location, count, 16, value);
		}
		gfxe_glUniformMatrix4fv(c, location, count, transpose, value);
	}
}

/*
 * A uniform's value: what the client set, when it is known here. The host's answer is as big as the
 * biggest uniform (gfx_uniformType: a mat4) and is read into room for that; only as much as the
 * uniform has is handed on when that is known, so a vec4 needs room for 4 floats, not 16.
 */
API void
glGetUniformfv(GLuint program, GLint location, GLfloat *params)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	struct gfx_uniform_value *u = &c->uniform_values[uniform_hash(program, NULL, location)];
	int known = u->program == program && u->location == location && program != 0;
	if (known && !(c->check_failed & CHECK_VALUES)) {
		memcpy(params, u->v, sizeof(GLfloat) * (size_t)u->n);
		c->answered++;
		if (!spot_check(c)) {
			return;
		}
	}
	GLfloat host[16] = { 0 };
	gfxe_glGetUniformfv(c, program, location, host);
	if (known && !(c->check_failed & CHECK_VALUES) && memcmp(host, u->v, sizeof(GLfloat) * (size_t)u->n) != 0) {
		differs(c, CHECK_VALUES, "glGetUniformfv", (GLenum)location, (long)(u->v[0] * 1000), (long)(host[0] * 1000));
	}
	memcpy(params, host, sizeof(GLfloat) * (size_t)(known ? u->n : 16));
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
	if (target == 0x0DE1 /* GL_TEXTURE_2D */ && level == 0 && !c->unit_over) {
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
	if (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED && index < GFX_MAX_ATTRIBS && c->vertex_array == 0 && !(c->check_failed & CHECK_STATE)) {
		c->answered++;
		*params = c->attrib[index].enabled;
		if (spot_check(c)) {
			GLint host = 0;
			gfxe_glGetVertexAttribiv(c, index, pname, &host);
			if (host != *params) {
				differs(c, CHECK_STATE, "glGetVertexAttribiv", pname, *params, host);
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
