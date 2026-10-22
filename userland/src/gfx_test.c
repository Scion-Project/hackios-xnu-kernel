/*
 * gfx_test: the gfx library (src/gfx) end to end, the way CoreAnimation's renderer uses OpenGL
 * ES: a texture uploaded from client memory, a framebuffer object with a texture as its colour,
 * shaders, client-side vertex arrays, glDrawElements with client indices, blending, scissor,
 * glReadPixels. The host GPU draws; the guest checks the pixels and times frames.
 *
 * On iOS the stream is /dev/vgpu3d (gfx_dev.c); on the Mac, tools/gfxstream/host_test builds
 * this file against the Android emulator's gfxstream (host.c). "GFXTEST ..." lines report.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx/gfx.h"

int gfx_test_transport(struct gfx_transport *t);       /* per platform */

extern const GLubyte *glGetString(GLenum);
extern void glViewport(GLint, GLint, GLsizei, GLsizei);
extern void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void glClear(GLbitfield);
extern void glEnable(GLenum);
extern void glDisable(GLenum);
extern void glScissor(GLint, GLint, GLsizei, GLsizei);
extern void glBlendFunc(GLenum, GLenum);
extern GLuint glCreateShader(GLenum);
extern void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
extern void glCompileShader(GLuint);
extern void glGetShaderiv(GLuint, GLenum, GLint *);
extern void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
extern GLuint glCreateProgram(void);
extern void glAttachShader(GLuint, GLuint);
extern void glBindAttribLocation(GLuint, GLuint, const GLchar *);
extern void glLinkProgram(GLuint);
extern void glGetProgramiv(GLuint, GLenum, GLint *);
extern void glUseProgram(GLuint);
extern GLint glGetUniformLocation(GLuint, const GLchar *);
extern void glUniform1i(GLint, GLint);
extern void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
extern void glGenTextures(GLsizei, GLuint *);
extern void glBindTexture(GLenum, GLuint);
extern void glActiveTexture(GLenum);
extern void glTexParameteri(GLenum, GLenum, GLint);
extern void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
extern void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
extern void glPixelStorei(GLenum, GLint);
extern void glGenFramebuffers(GLsizei, GLuint *);
extern void glBindFramebuffer(GLenum, GLuint);
extern void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum glCheckFramebufferStatus(GLenum);
extern void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
extern void glEnableVertexAttribArray(GLuint);
extern void glDrawArrays(GLenum, GLint, GLsizei);
extern void glDrawElements(GLenum, GLsizei, GLenum, const void *);
extern void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
extern GLenum glGetError(void);
extern void glFinish(void);

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_RGBA 0x1908
#define GL_BGRA_EXT 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SCISSOR_TEST 0x0C11
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

#define W 256
#define H 256

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("GFXTEST shader error: %s\n", log);
	}
	return s;
}

/* a vertex: position (x, y) and texture coordinates (u, v), interleaved as CoreAnimation does */
struct vtx { float x, y, u, v; };

static void
draw(GLuint prog, GLint color_loc, float alpha)
{
	static const struct vtx quad[] = {
		{ -0.5f, -0.5f, 0, 0 }, { 0.5f, -0.5f, 1, 0 }, { -0.5f, 0.5f, 0, 1 }, { 0.5f, 0.5f, 1, 1 },
	};
	static const uint16_t idx[] = { 0, 1, 2, 2, 1, 3 };
	glUseProgram(prog);
	glUniform4f(color_loc, 1, 1, 1, alpha);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(struct vtx), &quad[0].x);
	glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(struct vtx), &quad[0].u);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
}

int
main(int argc, char **argv)
{
	struct gfx_transport t = { 0 };
	int frames = argc > 1 ? atoi(argv[1]) : 100;
	if (gfx_test_transport(&t) != 0) {
		printf("GFXTEST no transport\nGFXTEST-DONE fail\n");
		return 1;
	}
	double t0 = now_ms();
	struct gfx *c = gfx_create(&t, 2);
	if (c == NULL || !gfx_make_current(c)) {
		printf("GFXTEST no context\nGFXTEST-DONE fail\n");
		return 1;
	}
	printf("GFXTEST context in %.1f ms\n", now_ms() - t0);
	printf("GFXTEST vendor=%s\nGFXTEST renderer=%s\nGFXTEST version=%s\n", glGetString(GL_VENDOR),
	    glGetString(GL_RENDERER), glGetString(GL_VERSION));

	/* a 64x64 texture, uploaded from a wider client buffer (row length 80): BGRA, a checker */
	enum { TW = 64, TROW = 80 };
	uint8_t *tex = calloc(TROW * TW, 4);
	for (int y = 0; y < TW; y++)
		for (int x = 0; x < TW; x++) {
			uint8_t *p = tex + (y * TROW + x) * 4;
			int on = ((x >> 3) ^ (y >> 3)) & 1;
			p[0] = on ? 255 : 0;            /* B */
			p[1] = 0;                       /* G */
			p[2] = on ? 0 : 255;            /* R */
			p[3] = 255;
		}
	GLuint texid, target, fbo;
	glGenTextures(1, &texid);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texid);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, TROW);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, TW, TW, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, tex);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	/* the render target: a texture behind a framebuffer object */
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	GLuint vs = shader(GL_VERTEX_SHADER,
	    "attribute vec2 pos; attribute vec2 uv; varying vec2 t;\n"
	    "void main() { t = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n");
	GLuint fs = shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec2 t; uniform sampler2D s; uniform vec4 color;\n"
	    "void main() { gl_FragColor = texture2D(s, t) * color; }\n");
	GLuint prog = glCreateProgram();
	GLint linked = 0;
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "uv");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "s"), 0);
	GLint color_loc = glGetUniformLocation(prog, "color");
	glBindTexture(GL_TEXTURE_2D, texid);

	/* the frame: grey background, a green scissored band, the checker quad */
	glViewport(0, 0, W, H);
	glClearColor(0.5f, 0.5f, 0.5f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, W, 16);
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	draw(prog, color_loc, 1.0f);
	GLenum err = glGetError();

	uint8_t *px = malloc(W * H * 4);
	t0 = now_ms();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	double rb = now_ms() - t0;
#define PX(x, y) (px + ((y) * W + (x)) * 4)
	int checks = 0, passed = 0;
	const uint8_t *p;
	checks++; p = PX(4, 100); passed += p[0] > 120 && p[0] < 135 && p[1] > 120 && p[1] < 135;   /* grey */
	checks++; p = PX(4, 4); passed += p[0] == 0 && p[1] == 255 && p[2] == 0;                     /* green band */
	/* quad covers 64..191; texel (0,0) of the checker is red (off), texel (8,0) blue (on) */
	checks++; p = PX(66, 66); passed += p[0] == 255 && p[1] == 0 && p[2] == 0;
	checks++; p = PX(66 + 17, 66); passed += p[0] == 0 && p[1] == 0 && p[2] == 255;
	checks++; passed += status == GL_FRAMEBUFFER_COMPLETE;
	checks++; passed += linked == 1;
	checks++; passed += err == 0;
	printf("GFXTEST fbo=0x%x linked=%d gl_error=0x%x readback %dx%d in %.2f ms\n", status, linked, err, W, H, rb);
	for (int y = H - 16; y >= 0; y -= 32) {
		char line[33];
		for (int x = 0; x < 32; x++) {
			const uint8_t *q = PX(x * 8 + 4, y);
			line[x] = q[1] > 200 ? 'G' : q[0] > 200 && q[2] < 50 ? 'R' : q[2] > 200 && q[0] < 50 ? 'B' : '.';
		}
		line[32] = 0;
		printf("GFXTEST |%s|\n", line);
	}

	/* frames: clear + 10 blended quads + a texture update, waited for */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	t0 = now_ms();
	for (int f = 0; f < frames; f++) {
		glClearColor(0.1f, 0.1f, 0.1f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, 8, GL_BGRA_EXT, GL_UNSIGNED_BYTE, tex);
		for (int q = 0; q < 10; q++) {
			draw(prog, color_loc, 0.3f);
		}
		glFinish();
	}
	double per = (now_ms() - t0) / frames;
	printf("GFXTEST frames=%d frame_ms=%.3f (%.0f fps) bytes_out=%llu bytes_in=%llu round_trips=%llu\n", frames, per,
	    1000.0 / per, (unsigned long long)c->bytes_out, (unsigned long long)c->bytes_in,
	    (unsigned long long)c->round_trips);
	/* textures that are client memory (gfx_share_texture), when the transport has resources */
	if (t.res_create != NULL) {
		enum { SW = 64, SSTRIDE = 72 * 4 };
		uint8_t *s1 = calloc(SW, SSTRIDE), *s2 = calloc(SW, SSTRIDE);
		for (int y = 0; y < SW; y++)
			for (int x = 0; x < SW; x++) {
				uint8_t *q = s1 + y * SSTRIDE + x * 4;      /* BGRA: memory rows 0-31 green, 32-63 blue */
				q[0] = y < 32 ? 0 : 255; q[1] = y < 32 ? 255 : 0; q[2] = 0; q[3] = 255;
			}
		GLuint st[2];
		glGenTextures(2, st);
		glBindTexture(GL_TEXTURE_2D, st[0]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		int ok1 = gfx_share_texture(c, 1, s1, SW, SW, SSTRIDE, 1);
		/* sample it: the quad's bottom (texture row 0 side) should be memory row 0's colour */
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glDisable(GL_BLEND);
		glViewport(0, 0, W, H);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		draw(prog, color_loc, 1.0f);
		uint8_t lo[4], hi[4];
		glReadPixels(W / 2, 64 + 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
		glReadPixels(W / 2, 192 - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
		checks++; passed += ok1 && lo[1] == 255 && hi[2] == 255;
		printf("GFXTEST shared sample: ok=%d bottom rgb=%u,%u,%u top rgb=%u,%u,%u (memory row 0 = texture row 0: %s)\n",
		    ok1, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], lo[1] == 255 ? "yes" : "no");

		/* draw into one: red, then GL rows 0-7 blue; memory rows 0-7 should come back blue */
		GLuint fbo2;
		glBindTexture(GL_TEXTURE_2D, st[1]);
		int ok2 = gfx_share_texture(c, 2, s2, SW, SW, SSTRIDE, 1);
		glGenFramebuffers(1, &fbo2);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st[1], 0);
		glViewport(0, 0, SW, SW);
		glClearColor(1, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, SW, 8);
		glClearColor(0, 0, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glFinish();
		const uint8_t *r0 = s2, *r60 = s2 + 60 * SSTRIDE;
		checks++; passed += ok2 && r0[0] == 255 && r0[2] == 0 && r60[2] == 255 && r60[0] == 0;
		printf("GFXTEST shared target: ok=%d memory row 0 bgra=%u,%u,%u row 60 bgra=%u,%u,%u downloads=%llu\n", ok2,
		    r0[0], r0[1], r0[2], r60[0], r60[1], r60[2], (unsigned long long)c->downloads);

		/* the display's size: a frame drawn into a shared 1125x2436 target and copied back */
		enum { DW = 1125, DH = 2436, DSTRIDE = 1152 * 4 };
		uint8_t *disp = calloc(DH, DSTRIDE);
		GLuint dt, dfbo;
		glGenTextures(1, &dt);
		glBindTexture(GL_TEXTURE_2D, dt);
		gfx_share_texture(c, 3, disp, DW, DH, DSTRIDE, 1);
		glGenFramebuffers(1, &dfbo);
		glBindFramebuffer(GL_FRAMEBUFFER, dfbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dt, 0);
		glViewport(0, 0, DW, DH);
		glBindTexture(GL_TEXTURE_2D, texid);
		glEnable(GL_BLEND);
		double d0 = now_ms();
		for (int f = 0; f < 10; f++) {
			glClearColor(0.2f, 0.2f, 0.2f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			for (int q = 0; q < 10; q++) {
				draw(prog, color_loc, 0.3f);
			}
			glFinish();
		}
		printf("GFXTEST display-size frame (10 quads, copied back into a %dx%d surface): %.2f ms\n", DW, DH,
		    (now_ms() - d0) / 10);
	}
	printf("GFXTEST checks %d/%d\nGFXTEST-DONE %s\n", passed, checks, passed == checks ? "pass" : "fail");
	gfx_destroy(c);
	return passed == checks ? 0 : 1;
}
