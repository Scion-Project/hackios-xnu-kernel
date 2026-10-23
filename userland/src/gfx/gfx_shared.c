/*
 * gfx: textures that are client memory (gfx.h, gfx_share_texture), and the bookkeeping that
 * keeps them cheap:
 *
 *   - which texture is bound where and which texture is the colour of which framebuffer, so a
 *     draw, a clear or a blit is known to land in a texture;
 *   - per texture, the part drawn into since it was last copied on (the damage): the scissor box
 *     of each draw, or the whole texture. A blit carries its source's damage over into the
 *     destination (eagl.c's frame goes from CoreAnimation's texture into the shared mirror that
 *     way), and at the frame's end only the damaged rows of a shared target are copied back;
 *   - per shared memory, the caller's change count (an IOSurface's seed): a texture is uploaded
 *     again when it is bound and the memory has changed, unless the GPU drew it (gpu_owned).
 */
#include <stdio.h>
#include <string.h>

#include "gfx_enc.h"

#define API __attribute__((visibility("default")))

#define GL_TEXTURE_2D           0x0DE1
#define GL_TEXTURE0             0x84C0
#define GL_FRAMEBUFFER          0x8D40
#define GL_DRAW_FRAMEBUFFER     0x8CA9
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_SCISSOR_TEST         0x0C11

/* ---- textures and their damage ------------------------------------------------------------ */

static struct gfx_tex *
tex_of(struct gfx *c, GLuint name, int make)
{
	struct gfx_tex *free_slot = NULL;
	if (name == 0) {
		return NULL;
	}
	for (int i = 0; i < GFX_MAX_TEX; i++) {
		if (c->tex[i].name == name) {
			return &c->tex[i];
		}
		if (free_slot == NULL && c->tex[i].name == 0) {
			free_slot = &c->tex[i];
		}
	}
	if (!make || free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->name = name;
	return free_slot;
}

void
gfx_note_texture_size(struct gfx *c, GLuint texture, uint32_t width, uint32_t height)
{
	struct gfx_tex *t = tex_of(c, texture, 1);
	if (t != NULL) {
		t->width = width;
		t->height = height;
		t->x0 = t->y0 = t->x1 = t->y1 = 0;
	}
}

static void
damage(struct gfx_tex *t, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
	if (t->width) {
		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > (int32_t)t->width ? (int32_t)t->width : x1;
		y1 = y1 > (int32_t)t->height ? (int32_t)t->height : y1;
	}
	if (x0 >= x1 || y0 >= y1) {
		return;
	}
	if (t->x0 >= t->x1) {
		t->x0 = x0; t->y0 = y0; t->x1 = x1; t->y1 = y1;
		return;
	}
	t->x0 = x0 < t->x0 ? x0 : t->x0;
	t->y0 = y0 < t->y0 ? y0 : t->y0;
	t->x1 = x1 > t->x1 ? x1 : t->x1;
	t->y1 = y1 > t->y1 ? y1 : t->y1;
}

static void
damage_all(struct gfx_tex *t)
{
	damage(t, 0, 0, t->width ? (int32_t)t->width : 1 << 30, t->height ? (int32_t)t->height : 1 << 30);
}

static GLuint
fbo_texture(struct gfx *c, GLuint fbo)
{
	for (int i = 0; fbo && i < GFX_MAX_FBOS; i++) {
		if (c->fbo_color[i].fbo == fbo) {
			return c->fbo_color[i].texture;
		}
	}
	return 0;
}

static struct gfx_shared *
shared_of_key(struct gfx *c, uint64_t key)
{
	for (int i = 0; i < GFX_MAX_SHARED; i++) {
		if (c->shared[i].res && c->shared[i].key == key) {
			return &c->shared[i];
		}
	}
	return NULL;
}

static struct gfx_shared *
shared_of_texture(struct gfx *c, GLuint texture)
{
	for (int i = 0; texture && i < GFX_MAX_SHARED; i++) {
		if (c->shared[i].res && c->shared[i].texture == texture) {
			return &c->shared[i];
		}
	}
	return NULL;
}

/* A draw or a clear is about to land in the bound framebuffer. */
void
gfx_drawing(struct gfx *c)
{
	GLuint texture = fbo_texture(c, c->draw_fbo);
	struct gfx_tex *t = tex_of(c, texture, 1);
	struct gfx_shared *s = shared_of_texture(c, texture);
	if (t == NULL) {
		return;
	}
	if (c->scissor_on) {
		damage(t, c->scissor[0], c->scissor[1], c->scissor[0] + c->scissor[2], c->scissor[1] + c->scissor[3]);
	} else {
		damage_all(t);
	}
	if (s != NULL) {
		s->dirty = 1;
		s->gpu_owned = 0;
	}
}

/* ---- shared textures ------------------------------------------------------------------------ */

static int
upload(struct gfx *c, struct gfx_shared *s)
{
	gfx_flush(c);
	uint64_t t0 = gfx_now_us();
	if (c->t.res_upload(c->t.arg, s->res, 0, 0, s->width, s->height, s->base, s->stride) != 0) {
		return 0;
	}
	c->us_upload += gfx_now_us() - t0;
	c->uploads++;
	c->upload_bytes += (uint64_t)s->width * s->height * 4;
	if (c->seed_of && s->user) {
		s->seed = c->seed_of(s->user);
	}
	return 1;
}

int
gfx_share_texture(struct gfx *c, uint64_t key, void *base, uint32_t width, uint32_t height, uint32_t stride,
    int want_upload, void *user)
{
	GLuint texture = c->bound_2d[c->active_unit];
	struct gfx_shared *s;

	if (c->t.res_create == NULL || texture == 0 || c->err) {
		return 0;
	}
	s = shared_of_key(c, key);
	if (s != NULL && (s->width != width || s->height != height)) {
		gfx_forget_shared(c, key);
		s = NULL;
	}
	if (s == NULL) {
		for (int i = 0; s == NULL && i < GFX_MAX_SHARED; i++) {
			if (c->shared[i].res == 0) {
				s = &c->shared[i];
			}
		}
		if (s == NULL || c->t.res_create(c->t.arg, width, height, &s->res) != 0) {
			if (s) {
				s->res = 0;
			}
			return 0;
		}
		s->key = key;
		s->width = width;
		s->height = height;
		want_upload = 1;
	} else if (s->user != NULL && c->release) {
		/* one reference is kept: the old one, or the same one handed again */
		c->release(s->user);
	}
	/* a texture is shared with one memory at a time */
	struct gfx_shared *old = shared_of_texture(c, texture);
	if (old != NULL && old != s) {
		old->texture = 0;
	}
	s->texture = texture;
	s->base = base;
	s->stride = stride;
	s->user = user;
	gfx_note_texture_size(c, texture, width, height);
	/* the display's buffers, drawn by the GPU, copied back and attached again: no upload */
	if (want_upload && !s->gpu_owned && !upload(c, s)) {
		return 0;
	}
	gfxe_rcBindTexture(c, s->res);
	return 1;
}

void
gfx_forget_shared(struct gfx *c, uint64_t key)
{
	struct gfx_shared *s = shared_of_key(c, key);
	if (s != NULL) {
		gfx_flush(c);
		c->t.res_unref(c->t.arg, s->res);
		if (s->user && c->release) {
			c->release(s->user);
		}
		memset(s, 0, sizeof(*s));
	}
}

/* Copy back the damaged rows of every shared texture drawn into since the last time. */
void
gfx_sync_shared(struct gfx *c)
{
	int any = 0;
	for (int i = 0; i < GFX_MAX_SHARED; i++) {
		any |= c->shared[i].res && c->shared[i].dirty;
	}
	if (!any || c->err) {
		return;
	}
	/* the host must have drawn everything before the colour buffers are read */
	gfxe_glFinishRoundTrip(c);
	for (int i = 0; i < GFX_MAX_SHARED; i++) {
		struct gfx_shared *s = &c->shared[i];
		if (!s->res || !s->dirty || s->base == NULL) {
			continue;
		}
		s->dirty = 0;
		struct gfx_tex *t = tex_of(c, s->texture, 0);
		int32_t x0 = 0, y0 = 0, x1 = (int32_t)s->width, y1 = (int32_t)s->height;
		if (t != NULL && t->x0 < t->x1) {
			x0 = t->x0; y0 = t->y0; x1 = t->x1; y1 = t->y1;
			t->x0 = t->x1 = 0;
		} else if (t != NULL) {
			continue;                       /* marked, but nothing in it was drawn */
		}
		uint8_t *at = (uint8_t *)s->base + (size_t)y0 * s->stride + (size_t)x0 * 4;
		uint64_t t0 = gfx_now_us();
		int r = c->t.res_download(c->t.arg, s->res, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0),
		    (uint32_t)(y1 - y0), at, s->stride);
		c->us_download += gfx_now_us() - t0;
		if (r == 0) {
			s->gpu_owned = 1;
			c->downloads++;
			c->download_bytes += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0) * 4;
		}
	}
}

/* ---- the calls that tell -------------------------------------------------------------------- */

API void
glActiveTexture(GLenum texture)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 16) {
		c->active_unit = texture - GL_TEXTURE0;
	}
	gfxe_glActiveTexture(c, texture);
}

API void
glBindTexture(GLenum target, GLuint texture)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_TEXTURE_2D) {
		c->bound_2d[c->active_unit] = texture;
	}
	gfxe_glBindTexture(c, target, texture);
	/* a shared memory that changed (its seed moved) since it was uploaded: upload it again */
	if (target == GL_TEXTURE_2D && c->seed_of) {
		struct gfx_shared *s = shared_of_texture(c, texture);
		if (s != NULL && s->user && !s->gpu_owned && !s->dirty && c->seed_of(s->user) != s->seed) {
			upload(c, s);
		}
	}
}

API void
glDeleteTextures(GLsizei n, const GLuint *textures)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	for (GLsizei i = 0; i < n; i++) {
		struct gfx_shared *s = shared_of_texture(c, textures[i]);
		struct gfx_tex *t = tex_of(c, textures[i], 0);
		if (s != NULL) {
			s->texture = 0;         /* the colour buffer stays, for the memory's next texture */
		}
		if (t != NULL) {
			t->name = 0;
		}
		for (int u = 0; u < 16; u++) {
			if (c->bound_2d[u] == textures[i]) {
				c->bound_2d[u] = 0;
			}
		}
	}
	gfxe_glDeleteTextures(c, n, textures);
}

API void
glBindFramebuffer(GLenum target, GLuint framebuffer)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
		c->draw_fbo = framebuffer;
	}
	if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
		c->read_fbo = framebuffer;
	}
	gfxe_glBindFramebuffer(c, target, framebuffer);
}

API void
glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	struct gfx *c = gfx_current();
	GLuint fbo;
	if (c == NULL) {
		return;
	}
	fbo = target == GL_READ_FRAMEBUFFER ? c->read_fbo : c->draw_fbo;
	if (attachment == GL_COLOR_ATTACHMENT0 && fbo) {
		int slot = -1;
		for (int i = 0; i < GFX_MAX_FBOS; i++) {
			if (c->fbo_color[i].fbo == fbo) {
				slot = i;
				break;
			}
			if (slot < 0 && c->fbo_color[i].fbo == 0) {
				slot = i;
			}
		}
		if (slot >= 0) {
			c->fbo_color[slot].fbo = fbo;
			c->fbo_color[slot].texture = texture;
		}
	}
	gfxe_glFramebufferTexture2D(c, target, attachment, textarget, texture, level);
}

API void
glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	for (GLsizei i = 0; i < n; i++) {
		for (int j = 0; j < GFX_MAX_FBOS; j++) {
			if (framebuffers[i] && c->fbo_color[j].fbo == framebuffers[i]) {
				c->fbo_color[j].fbo = c->fbo_color[j].texture = 0;
			}
		}
		if (c->draw_fbo == framebuffers[i]) {
			c->draw_fbo = 0;
		}
		if (c->read_fbo == framebuffers[i]) {
			c->read_fbo = 0;
		}
	}
	gfxe_glDeleteFramebuffers(c, n, framebuffers);
}

API void
glEnable(GLenum cap)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (cap == GL_SCISSOR_TEST) {
		c->scissor_on = 1;
	}
	gfxe_glEnable(c, cap);
}

API void
glDisable(GLenum cap)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (cap == GL_SCISSOR_TEST) {
		c->scissor_on = 0;
	}
	gfxe_glDisable(c, cap);
}

API void
glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	c->scissor[0] = x;
	c->scissor[1] = y;
	c->scissor[2] = width;
	c->scissor[3] = height;
	gfxe_glScissor(c, x, y, width, height);
}

API void
glClear(GLbitfield mask)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	gfx_drawing(c);
	gfxe_glClear(c, mask);
}

/* The destination of a blit gets its source's damage, mapped by the blit (which may flip). */
API void
glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
    GLint dstY1, GLbitfield mask, GLenum filter)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	GLuint dst_name = fbo_texture(c, c->draw_fbo);
	struct gfx_tex *src = tex_of(c, fbo_texture(c, c->read_fbo), 0), *dst = tex_of(c, dst_name, 1);
	struct gfx_shared *s = shared_of_texture(c, dst_name);
	if (dst != NULL) {
		int32_t sw = srcX1 - srcX0, sh = srcY1 - srcY0, dw = dstX1 - dstX0, dh = dstY1 - dstY0;
		if (src != NULL && src->x0 < src->x1 && sw && sh) {
			/* map the source's damage through the blit's transform (either axis may flip) */
			double ax = (double)dw / sw, ay = (double)dh / sh;
			double xa = dstX0 + (src->x0 - srcX0) * ax, xb = dstX0 + (src->x1 - srcX0) * ax;
			double ya = dstY0 + (src->y0 - srcY0) * ay, yb = dstY0 + (src->y1 - srcY0) * ay;
			damage(dst, (int32_t)(xa < xb ? xa : xb), (int32_t)(ya < yb ? ya : yb),
			    (int32_t)((xa < xb ? xb : xa) + 0.999), (int32_t)((ya < yb ? yb : ya) + 0.999));
			src->x0 = src->x1 = 0;          /* (the source's damage has been carried on) */
		} else if (src == NULL || src->width == 0) {
			damage(dst, dstX0 < dstX1 ? dstX0 : dstX1, dstY0 < dstY1 ? dstY0 : dstY1,
			    dstX0 < dstX1 ? dstX1 : dstX0, dstY0 < dstY1 ? dstY1 : dstY0);
		}
		if (s != NULL) {
			s->dirty = 1;
			s->gpu_owned = 0;
		}
	}
	gfxe_glBlitFramebuffer(c, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}
