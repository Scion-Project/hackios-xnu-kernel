/*
 * gfx: textures that are client memory (gfx.h, gfx_share_texture), and the bookkeeping that
 * knows when one is drawn into: which texture is bound where, which texture is the colour of
 * which framebuffer, and so whether a draw, a clear or a blit lands in a shared texture.
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

int
gfx_share_texture(struct gfx *c, uint64_t key, void *base, uint32_t width, uint32_t height, uint32_t stride,
    int upload)
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
		upload = 1;
	}
	/* a texture is shared with one memory at a time */
	struct gfx_shared *old = shared_of_texture(c, texture);
	if (old != NULL && old != s) {
		old->texture = 0;
	}
	s->texture = texture;
	s->base = base;
	s->stride = stride;
	if (upload) {
		gfx_flush(c);
		if (c->t.res_upload(c->t.arg, s->res, 0, 0, width, height, base, stride) != 0) {
			return 0;
		}
		c->uploads++;
		c->upload_bytes += (uint64_t)width * height * 4;
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
		memset(s, 0, sizeof(*s));
	}
}

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
		if (s->res && s->dirty && s->base != NULL) {
			s->dirty = 0;
			if (c->t.res_download(c->t.arg, s->res, 0, 0, s->width, s->height, s->base, s->stride) == 0) {
				c->downloads++;
				c->download_bytes += (uint64_t)s->width * s->height * 4;
			}
		}
	}
}

/* A draw, a clear or a blit is about to land in the bound framebuffer: is its colour shared? */
void
gfx_drawing(struct gfx *c)
{
	if (c->draw_fbo == 0) {
		return;
	}
	for (int i = 0; i < GFX_MAX_FBOS; i++) {
		if (c->fbo_color[i].fbo == c->draw_fbo) {
			struct gfx_shared *s = shared_of_texture(c, c->fbo_color[i].texture);
			if (s != NULL) {
				s->dirty = 1;
			}
			return;
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
		if (s != NULL) {
			s->texture = 0;         /* the colour buffer stays, for the memory's next texture */
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
	gfxe_glBindFramebuffer(c, target, framebuffer);
}

API void
glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	if (attachment == GL_COLOR_ATTACHMENT0 && target != GL_READ_FRAMEBUFFER && c->draw_fbo) {
		int slot = -1;
		for (int i = 0; i < GFX_MAX_FBOS; i++) {
			if (c->fbo_color[i].fbo == c->draw_fbo || (slot < 0 && c->fbo_color[i].fbo == 0)) {
				slot = i;
				if (c->fbo_color[i].fbo == c->draw_fbo) {
					break;
				}
			}
		}
		if (slot >= 0) {
			c->fbo_color[slot].fbo = c->draw_fbo;
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
	}
	gfxe_glDeleteFramebuffers(c, n, framebuffers);
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

API void
glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
    GLint dstY1, GLbitfield mask, GLenum filter)
{
	struct gfx *c = gfx_current();
	if (c == NULL) {
		return;
	}
	gfx_drawing(c);
	gfxe_glBlitFramebuffer(c, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}
