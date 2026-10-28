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
	/*
	 * Optional: 1 when the host shows a frame in the memory named `key` (an IOSurface ID) itself,
	 * after rcFBPost of its colour buffer, so that it need not be copied back (SCGPU: the Scion app
	 * runs gfxstream and presents in its own window; /dev/vgpu3d says which surfaces are the display's).
	 */
	int (*host_shows)(void *arg, uint32_t key);
	/*
	 * Optional: until the host has taken in everything sent before `*mark` (a mark this gave before;
	 * 0: none), then a mark of everything sent so far in `*mark`. Taken in, not drawn: SCGPU's app runs
	 * the stream in order, and a frame's rcFBPost waits there until the frame is on the screen. 0, or
	 * an error when the transport cannot (gfx_pace then does not either).
	 */
	int (*ring_mark)(void *arg, uint64_t *mark);
	/*
	 * The render thread's first word (the client flags) has been sent already: /dev/vgpu3d sends it
	 * when it connects the pipe. A second one would be read as the start of a packet, and the
	 * stream would never line up again (the first reply never comes).
	 */
	int flags_sent;
};

/* A texture that is client memory (gfx_share_texture): what is drawn into it is copied back. */
struct gfx_shared {
	uint64_t key;                   /* the caller's name for the memory (an IOSurface ID) */
	GLuint texture;
	uint32_t res, width, height, stride;
	void *base;
	int dirty;                      /* drawn into since the last copy back */
	int gpu_owned;                  /* the memory holds what the colour buffer holds (copied back, not written since) */
	void *user;                     /* the caller's handle (an IOSurfaceRef), for seed_of */
	uint32_t seed;                  /* the caller's change count of the memory when it was last uploaded */
	int host_shown;                 /* the host shows it (transport host_shows): rcFBPost, no copy back */
};
#define GFX_MAX_SHARED  1024
#define GFX_MAX_FBOS    1024            /* by name modulo this; one that another pushes out is asked of the host */
#define GFX_NAMES       32              /* texture and framebuffer names asked of the host at a time */
#define GFX_UNIFORMS    256             /* uniform locations and values known here, by a hash; ditto */

/* A uniform's location by its name, as the host gave it (gfx_gl.c glGetUniformLocation). */
struct gfx_uniform_name {
	GLuint program;                 /* 0: free */
	GLint location;
	char name[44];
};

/* A float uniform's value as the client set it (glUniform4f[v], glUniformMatrix4fv): n floats. */
struct gfx_uniform_value {
	GLuint program;                 /* 0: free */
	GLint location;
	int n;
	GLfloat v[16];
};
#define GFX_MAX_TEX     1024

/* A framebuffer's colour attachment 0 as the host has it: GL_NONE, GL_TEXTURE or GL_RENDERBUFFER, and its name. */
struct gfx_fbo {
	GLuint fbo, texture;
	GLenum kind;
};

/* What is known of a texture: its size, and the part of it drawn into since that was copied on. */
struct gfx_tex {
	GLuint name;
	uint32_t width, height;
	int32_t x0, y0, x1, y1;         /* damage, empty when x0 >= x1 */
};

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
	int host_store_set;
	int just_finished;               /* a glFinishRoundTrip has just been answered */
	uint64_t finished_at;            /* bytes sent when the last glFinishRoundTrip was answered */              /* host pack/unpack alignment set to 1 */

	/* client state */
	struct gfx_attrib attrib[GFX_MAX_ATTRIBS];
	GLuint array_buffer, element_buffer;
	GLint unpack_alignment, unpack_row_length, unpack_skip_rows, unpack_skip_pixels;
	GLint pack_alignment, pack_row_length, pack_skip_rows, pack_skip_pixels;
	char *strings[8];

	/* textures and framebuffers, for knowing when a shared texture is drawn into */
	GLuint active_unit, bound_2d[16], draw_fbo;
	struct gfx_fbo fbo_color[GFX_MAX_FBOS];
	struct gfx_shared shared[GFX_MAX_SHARED];
	struct gfx_tex tex[GFX_MAX_TEX];
	GLuint read_fbo;
	int scissor_on;
	GLint scissor[4];
	/*
	 * State the client set, so that asking for it costs no round trip (gfx_gl.c glGet*, glIsEnabled):
	 * eagl.c saves and puts back the scissor and the clear colour around every clear, and asks what a
	 * framebuffer draws into at every bind. The scissor box and the viewport start as the surface's,
	 * so they are asked of the host until set; a value the host would refuse or clamp is not kept.
	 */
	int blend_on, scissor_known, viewport_known, clear_unknown;
	GLint viewport[4];
	GLfloat clear_colour[4];
	GLuint vertex_array;            /* a bound vertex array object has its own attribute state: asked of the host */
	int unit_over;                  /* the active texture unit is past bound_2d's 16: its binding is asked */
	uint64_t answered, r_answered;  /* queries answered here (and at the last report) */
	/*
	 * ... and more of it: the blend factors (eagl.c saves and puts them back around every draw of a
	 * vibrant layer's group), the size of a 2D texture's first level (gfx_tex), uniform locations by
	 * name and float uniforms' values (eagl.c reads CoreAnimation's matrices back at those draws).
	 */
	/* names the host has made, handed out here: glGen* is a round trip, made once for GFX_NAMES of them */
	GLuint texture_names[GFX_NAMES], framebuffer_names[GFX_NAMES];
	int texture_names_left, framebuffer_names_left;
	int errors_from_host;           /* GFX_ERRORS=1: glGetError asks the host every time */
	uint32_t error_queries, errors_said;
	GLenum blend_func[4];           /* source RGB, destination RGB, source alpha, destination alpha */
	int blend_func_known;
	GLuint program;                 /* glUseProgram */
	struct gfx_uniform_name uniform_names[GFX_UNIFORMS];
	struct gfx_uniform_value uniform_values[GFX_UNIFORMS];
	uint32_t check_failed;          /* the kinds of answer a spot check found wrong (gfx_gl.c CHECK_*): asked of the host */
	/* the caller's change count of a shared memory (IOSurfaceGetSeed), and its handle let go */
	uint32_t (*seed_of)(void *user);
	void (*release)(void *user);
	uint64_t downloads, download_bytes, uploads, upload_bytes;
	uint64_t presents;              /* frames the host showed itself (gfx_present_shared) */
	uint64_t paced_mark;            /* gfx_pace: the mark of the frame before */
	int pace_off;                   /* ... which the transport cannot give */
	uint64_t paced, us_paced, r_paced;
	/* time (us) in each part, and frame ends, for the report every few seconds (gfx_report) */
	uint64_t us_write, us_read, us_download, us_upload, frames, report_at, report_frames;
	uint64_t r_trips, r_out, r_in, r_down, r_up, r_down_bytes, r_up_bytes, r_presents;
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
    int upload, void *user);
void gfx_note_texture_size(struct gfx *c, GLuint texture, uint32_t width, uint32_t height);
/* The size of a texture's first level as followed here (gfx_note_texture_size): 1 when it is known. */
int gfx_texture_size(struct gfx *c, GLuint texture, uint32_t *width, uint32_t *height);
/* What is known of framebuffer `fbo`'s colour attachment 0 (gfx_shared.c), or NULL: ask the host. */
struct gfx_fbo *gfx_fbo_of(struct gfx *c, GLuint fbo);
/* Copy every shared texture drawn into since the last time back into its memory. */
void gfx_sync_shared(struct gfx *c);
void gfx_drawing(struct gfx *c);
uint64_t gfx_now_us(void);
void gfx_finish(struct gfx *c);           /* glFinishRoundTrip, skipped when nothing was sent since the last */
void gfx_frame_end(struct gfx *c);      /* counts, and every 5 s one line of where the time went */         /* (gfx_gl.c: a draw lands in the bound framebuffer) */
void gfx_forget_shared(struct gfx *c, uint64_t key);
/*
 * The frame just drawn into the shared texture of `key`, shown by the host itself: rcFBPost of its
 * colour buffer, in the stream after the frame's draws, and no copy back (the host's window has the
 * picture, the memory need not). 1 when it was, 0 when the host does not show that memory (copy it
 * back as before).
 */
int gfx_present_shared(struct gfx *c, uint64_t key);
/*
 * The end of a frame the host shows itself, without waiting for it: until the host has taken in the
 * frame before, so this one is drawn there while the next is made here, never more than a frame
 * behind. 1 when it was, 0 when the transport cannot (wait for the frame as before).
 */
int gfx_pace(struct gfx *c);

#endif
