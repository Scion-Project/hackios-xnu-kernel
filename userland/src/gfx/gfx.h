/*
 * gfx: an OpenGL ES 2/3 library that draws nothing itself. Every call is encoded in gfxstream's
 * wire format (renderControl + GLES, see tools/gfxstream/mkgfxenc.py) and goes down a byte
 * stream to a gfxstream render thread on the host, which makes the calls on the host's GPU.
 * On iOS under AVF the stream is /dev/vgpu3d (the kernel's virtio-gpu 3D layer, a gfxstream
 * "pipe:opengles" per open); on the Mac, for testing, it is the Android emulator's
 * libgfxstream_backend (tools/gfxstream/host_test).
 *
 * What is generated: one encoder per call (gfxe_*) and a weak gl* entry point for every call
 * that needs nothing of the client's state. What is written here (gfx_gl.c): the calls that
 * do -- client-side vertex arrays, pixel packing, shader sources, strings, syncs.
 */
#ifndef GFX_H
#define GFX_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei, GLfixed, GLclampx;
typedef unsigned char GLboolean, GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort, GLhalf;
typedef float GLfloat, GLclampf;
typedef char GLchar;
typedef void GLvoid;
typedef long GLintptr, GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef int32_t GLint32;
typedef uint32_t GLuint32;
typedef uint64_t GLsync;                /* on the wire; the API's GLsync is a pointer */
typedef void *GLeglImageOES;
typedef int EGLint;
typedef unsigned int EGLenum;

/* How the bytes get to the host and back. read must deliver exactly n bytes. 0 or an errno. */
struct gfx_transport {
	void *arg;
	int (*write)(void *arg, const void *buf, size_t n);
	int (*read)(void *arg, void *buf, size_t n);
	void (*close)(void *arg);
	/*
	 * Optional: 2D colour-buffer resources (BGRA, rows of width * 4 bytes on the host) whose
	 * handle rcBindTexture takes, and rectangles of client memory (any stride) to and from them.
	 */
	int (*res_create)(void *arg, uint32_t width, uint32_t height, uint32_t *res);
	int (*res_unref)(void *arg, uint32_t res);
	int (*res_upload)(void *arg, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
	    const void *src, uint32_t stride);
	int (*res_download)(void *arg, uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
	    void *dst, uint32_t stride);
};

/* A texture that is client memory (gfx_share_texture): what is drawn into it is copied back. */
struct gfx_shared {
	uint64_t key;                   /* the caller's name for the memory (an IOSurface ID) */
	GLuint texture;
	uint32_t res, width, height, stride;
	void *base;
	int dirty;                      /* drawn into since the last copy back */
};
#define GFX_MAX_SHARED  64
#define GFX_MAX_FBOS    32

#define GFX_MAX_ATTRIBS 16

struct gfx_attrib {
	int enabled;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLsizei stride;
	const void *pointer;            /* client memory, or an offset when buffer != 0 */
	GLuint buffer;
};

struct gfx {
	struct gfx_transport t;
	uint8_t *buf;
	size_t cap, used;
	int err;
	uint64_t bytes_out, bytes_in, round_trips;

	/* renderControl */
	uint32_t config, rc_context, rc_surface, rc_color_buffer;
	int version;
	int host_store_set;              /* host pack/unpack alignment set to 1 */

	/* client state */
	struct gfx_attrib attrib[GFX_MAX_ATTRIBS];
	GLuint array_buffer, element_buffer;
	GLint unpack_alignment, unpack_row_length, unpack_skip_rows, unpack_skip_pixels;
	GLint pack_alignment, pack_row_length, pack_skip_rows, pack_skip_pixels;
	char *strings[8];

	/* textures and framebuffers, for knowing when a shared texture is drawn into */
	GLuint active_unit, bound_2d[16], draw_fbo;
	struct { GLuint fbo, texture; } fbo_color[GFX_MAX_FBOS];
	struct gfx_shared shared[GFX_MAX_SHARED];
	uint64_t downloads, download_bytes, uploads, upload_bytes;
	uint8_t *scratch;
	size_t scratch_cap;
};

/* the stream (gfx_core.c) */
uint8_t *gfx_begin(struct gfx *c, uint32_t op, uint32_t size);  /* space for a packet, header written */
void gfx_flush(struct gfx *c);
void gfx_read(struct gfx *c, void *buf, size_t n);
void *gfx_scratch(struct gfx *c, size_t n);

/* sizes the generated encoders ask for (gfx_core.c) */
uint32_t gfx_pixel_size(struct gfx *c, GLsizei w, GLsizei h, GLenum format, GLenum type, int pack);
uint32_t gfx_pixelDataSize3D(struct gfx *c, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, int pack);
uint32_t gfx_pixel_bits(GLenum format, GLenum type);
uint32_t gfx_sizeof(GLenum type);
uint32_t gfx_param_size(GLenum pname);
GLenum gfx_uniformType(struct gfx *c, GLuint program, GLint location);
uint32_t gfx_clearBufferNumElts(struct gfx *c, GLenum buffer);
uint32_t gfx_glActiveUniformBlockivParamSize(struct gfx *c, GLuint program, GLuint index, GLenum pname);

/* contexts (gfx_core.c) */
struct gfx *gfx_current(void);
void gfx_set_current(struct gfx *c);
/*
 * A context of OpenGL ES `version` (2 or 3) on this transport: the stream's flags word, a config,
 * a host context and a small window surface to make it current with. NULL on failure.
 */
struct gfx *gfx_create(const struct gfx_transport *t, int version);
int gfx_make_current(struct gfx *c);                            /* NULL: release; 0 on failure */
void gfx_destroy(struct gfx *c);

/*
 * The texture bound to GL_TEXTURE_2D becomes `width` x `height` BGRA client memory at `base`
 * (`stride` bytes a row), the same memory every time `key` is given: on the host a colour buffer
 * the texture is bound to (rcBindTexture). `upload`: copy the memory to the host now (it may have
 * changed). What is drawn into the texture is copied back into the memory when the frame ends
 * (glFlush, glFinish, glClientWaitSync). Row r of the memory is row r of the texture. 0: the
 * transport cannot (the caller copies with glTexImage2D/glReadPixels instead).
 */
int gfx_share_texture(struct gfx *c, uint64_t key, void *base, uint32_t width, uint32_t height, uint32_t stride,
    int upload);
/* Copy every shared texture drawn into since the last time back into its memory. */
void gfx_sync_shared(struct gfx *c);
void gfx_drawing(struct gfx *c);         /* (gfx_gl.c: a draw lands in the bound framebuffer) */
void gfx_forget_shared(struct gfx *c, uint64_t key);

#endif
