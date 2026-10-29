/*
 * The byte pipe under Scion's gfxstream Vulkan driver (tools/gfxstream/VULKAN.md): one per
 * stream, each a gfxstream "pipe:opengles" render thread on the host. The Vulkan opcodes go
 * down it exactly as the GL ones of src/gfx do.
 *
 *   iOS:   /dev/vgpu3d, one open a stream (the kernel's virtio-gpu context and pipe; it sends
 *          the service name and the flags word itself)
 *   macOS: the Android emulator's libgfxstream_backend.dylib in this process, driven through
 *          stream_renderer_* the way crosvm and the Scion app drive it (a test harness)
 */
#ifndef SCION_VK_TRANSPORT_H
#define SCION_VK_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct scion_vk_transport {
	void *arg;
	/* 0 or an errno; read delivers exactly n bytes */
	int (*write)(void *arg, const void *buf, size_t n);
	int (*read)(void *arg, void *buf, size_t n);
	void (*close)(void *arg);
	/* the render thread's flags word was sent with the service name (the kernel does) */
	int flags_sent;
};

/* A new stream to a new render thread. 0 or an errno. */
int scion_vk_transport_open(struct scion_vk_transport *t);

/*
 * The host's capset `id` (3: gfxstream-vulkan, struct vulkanCapset) into buf, when this transport
 * can ask for it. 0, or ENOTSUP and the caller does without.
 */
int scion_vk_transport_capset(uint32_t id, void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
