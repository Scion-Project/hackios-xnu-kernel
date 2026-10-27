/*
 * The OpenGL ES library behind src/eagl.c when that library is gfx (src/gfx): the host's GPU,
 * through gfxstream and /dev/vgpu3d (src/hakios.gl.h; src/gl_osmesa.c is the Mesa one).
 *
 * hakios_gl_surface_texture: the texture bound to GL_TEXTURE_2D becomes the IOSurface. On the
 * host it is a colour buffer that the texture is bound to, filled from the surface's memory now
 * (and every time eagl.c asks again, which it does when the surface has changed). What is drawn
 * into it is copied back into the surface's memory when the frame ends (glFlush, glFinish, the
 * fence wait of eagl.c's frame_end), so the display finds the frame where it looks.
 *
 * Environment GFX_SHARE=0: no shared surfaces (every surface goes through glTexImage2D and
 * glReadPixels instead).
 */
#include <IOSurface/IOSurfaceRef.h>
#include <stdlib.h>
#include <string.h>

#include "../hakios.gl.h"
#include "gfx.h"
#include "gfx_dev.h"

struct hakios_gl {
	struct gfx *gfx;
};

static uint32_t
surface_seed(void *user)
{
	return IOSurfaceGetSeed((IOSurfaceRef)user);
}

static void
surface_release(void *user)
{
	CFRelease((IOSurfaceRef)user);
}

struct hakios_gl *
hakios_gl_create(int version)
{
	struct gfx_transport t = { 0 };
	struct hakios_gl *gl;

	if (gfx_dev_open(&t) != 0) {
		return NULL;
	}
	gl = calloc(1, sizeof(*gl));
	if (gl == NULL || (gl->gfx = gfx_create(&t, version)) == NULL) {
		free(gl);
		return NULL;
	}
	gl->gfx->seed_of = surface_seed;
	gl->gfx->release = surface_release;
	return gl;
}

int
hakios_gl_make_current(struct hakios_gl *gl)
{
	return gfx_make_current(gl ? gl->gfx : NULL);
}

void
hakios_gl_destroy(struct hakios_gl *gl)
{
	gfx_destroy(gl->gfx);
	free(gl);
}

const char *
hakios_gl_library(void)
{
	return "gfx: gfxstream over virtio-gpu (/dev/vgpu3d)";
}

int
hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t id)
{
	static int share = -1;
	IOSurfaceRef surface;
	int ok;

	if (share < 0) {
		const char *e = getenv("GFX_SHARE");
		share = e == NULL || strcmp(e, "0") != 0;
	}
	if (!share || (surface = IOSurfaceLookup(id)) == NULL) {
		return 0;
	}
	if (IOSurfaceGetPlaneCount(surface) > 0 || IOSurfaceGetBytesPerElement(surface) != 4) {
		CFRelease(surface);
		return 0;
	}
	/*
	 * The base address stays valid while the surface lives: eagl.c holds it for as long as the
	 * texture is the surface. The lock is for reading it now.
	 */
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	/* the reference stays with the shared texture: its seed says when the memory has changed */
	ok = gfx_share_texture(gl->gfx, id, IOSurfaceGetBaseAddress(surface), (uint32_t)IOSurfaceGetWidth(surface),
	    (uint32_t)IOSurfaceGetHeight(surface), (uint32_t)IOSurfaceGetBytesPerRow(surface), 1, (void *)surface);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	if (!ok) {
		CFRelease(surface);
	}
	return ok;
}
