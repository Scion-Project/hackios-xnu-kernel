/*
 * vk_test: Vulkan end to end through Scion's gfxstream Vulkan driver (libhakios.vk.dylib,
 * tools/gfxstream/VULKAN.md), with nothing but the Vulkan API. The host's GPU does the work:
 *
 *   1. an instance, the physical device (the host's GPU as gfxstream shows it), a device, a queue
 *   2. vkCmdFillBuffer into device memory, vkCmdCopyBuffer into host-visible memory, read back
 *   3. a pattern written here, flushed, copied through device memory and back (the flush's bytes
 *      must reach the host: the driver carries them over the stream)
 *   4. a compute shader (SPIR-V below) writing data[i] = i * mul + add, read back
 *   5. host-coherent memory with no flush and no invalidate, and written by both sides during one
 *      submit -- only with HAKIOS_VK_COHERENCE=emulate: by default the driver shows the host's
 *      host-visible types as they are, not coherent (the application flushes and invalidates)
 *   6. what that emulation costs: an empty submit and wait with 64 MiB of coherent memory mapped
 *   7. threads, as a runtime has them: a waiter that starts waiting before the work it waits for
 *      is submitted, then several threads at once creating pipeline caches, query pools and compute
 *      pipelines and submitting, while the main thread submits too. Each thread is a stream (a
 *      host context and render thread) of its own; a deadlock between them is reported by a
 *      watchdog after 120 s
 *
 * On iOS the stream is /dev/vgpu3d; on the Mac, the Android emulator's gfxstream in this
 * process (tools/gfxstream/mk-vk.sh mac). "VKTEST ..." lines report; the exit status is the
 * number of checks that failed.
 *
 *   vk_test [words [threads]]   the buffers' size in 32-bit words (default 262144: 1 MiB) and the
 *                               threads of 7 (default 4; 0 leaves 7 out)
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

/*
 * The compute shader, glslangValidator -V -g0 --target-env vulkan1.0 of:
 *   #version 450
 *   layout(local_size_x = 64) in;
 *   layout(std430, binding = 0) buffer Out { uint data[]; };
 *   layout(push_constant) uniform P { uint count; uint mul; uint add; } p;
 *   void main()
 *   {
 *   	uint i = gl_GlobalInvocationID.x;
 *   	if (i < p.count)
 *   		data[i] = i * p.mul + p.add;
 *   }
 */
static const uint32_t index_spv[] = {
	0x07230203, 0x00010000, 0x0008000a, 0x00000030, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
	0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
	0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
	0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x0000000b, 0x0000000b, 0x0000001c,
	0x00050048, 0x00000011, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000011, 0x00000001,
	0x00000023, 0x00000004, 0x00050048, 0x00000011, 0x00000002, 0x00000023, 0x00000008, 0x00030047,
	0x00000011, 0x00000002, 0x00040047, 0x0000001d, 0x00000006, 0x00000004, 0x00050048, 0x0000001e,
	0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x0000001e, 0x00000003, 0x00040047, 0x00000020,
	0x00000022, 0x00000000, 0x00040047, 0x00000020, 0x00000021, 0x00000000, 0x00040047, 0x0000002f,
	0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015,
	0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000007, 0x00000007, 0x00000006, 0x00040017,
	0x00000009, 0x00000006, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000009, 0x0004003b,
	0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c, 0x00000000, 0x00040020,
	0x0000000d, 0x00000001, 0x00000006, 0x0005001e, 0x00000011, 0x00000006, 0x00000006, 0x00000006,
	0x00040020, 0x00000012, 0x00000009, 0x00000011, 0x0004003b, 0x00000012, 0x00000013, 0x00000009,
	0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x0004002b, 0x00000014, 0x00000015, 0x00000000,
	0x00040020, 0x00000016, 0x00000009, 0x00000006, 0x00020014, 0x00000019, 0x0003001d, 0x0000001d,
	0x00000006, 0x0003001e, 0x0000001e, 0x0000001d, 0x00040020, 0x0000001f, 0x00000002, 0x0000001e,
	0x0004003b, 0x0000001f, 0x00000020, 0x00000002, 0x0004002b, 0x00000014, 0x00000023, 0x00000001,
	0x0004002b, 0x00000014, 0x00000027, 0x00000002, 0x00040020, 0x0000002b, 0x00000002, 0x00000006,
	0x0004002b, 0x00000006, 0x0000002d, 0x00000040, 0x0004002b, 0x00000006, 0x0000002e, 0x00000001,
	0x0006002c, 0x00000009, 0x0000002f, 0x0000002d, 0x0000002e, 0x0000002e, 0x00050036, 0x00000002,
	0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007, 0x00000008,
	0x00000007, 0x00050041, 0x0000000d, 0x0000000e, 0x0000000b, 0x0000000c, 0x0004003d, 0x00000006,
	0x0000000f, 0x0000000e, 0x0003003e, 0x00000008, 0x0000000f, 0x0004003d, 0x00000006, 0x00000010,
	0x00000008, 0x00050041, 0x00000016, 0x00000017, 0x00000013, 0x00000015, 0x0004003d, 0x00000006,
	0x00000018, 0x00000017, 0x000500b0, 0x00000019, 0x0000001a, 0x00000010, 0x00000018, 0x000300f7,
	0x0000001c, 0x00000000, 0x000400fa, 0x0000001a, 0x0000001b, 0x0000001c, 0x000200f8, 0x0000001b,
	0x0004003d, 0x00000006, 0x00000021, 0x00000008, 0x0004003d, 0x00000006, 0x00000022, 0x00000008,
	0x00050041, 0x00000016, 0x00000024, 0x00000013, 0x00000023, 0x0004003d, 0x00000006, 0x00000025,
	0x00000024, 0x00050084, 0x00000006, 0x00000026, 0x00000022, 0x00000025, 0x00050041, 0x00000016,
	0x00000028, 0x00000013, 0x00000027, 0x0004003d, 0x00000006, 0x00000029, 0x00000028, 0x00050080,
	0x00000006, 0x0000002a, 0x00000026, 0x00000029, 0x00060041, 0x0000002b, 0x0000002c, 0x00000020,
	0x00000015, 0x00000021, 0x0003003e, 0x0000002c, 0x0000002a, 0x000200f9, 0x0000001c, 0x000200f8,
	0x0000001c, 0x000100fd, 0x00010038,
};

static int failed;

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define VK(call) do { \
	VkResult r_ = (call); \
	if (r_ != VK_SUCCESS) { \
		printf("VKTEST FAIL %s: %d (line %d)\n", #call, r_, __LINE__); \
		exit(100); \
	} \
} while (0)

static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;

static void
check(const char *what, int ok, const char *detail)
{
	pthread_mutex_lock(&check_lock);
	printf("VKTEST %s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail ? ": " : "", detail ? detail : "");
	failed += !ok;
	pthread_mutex_unlock(&check_lock);
}

struct buf {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t type;
	VkMemoryPropertyFlags flags;
	uint32_t *map;
};

static VkPhysicalDeviceMemoryProperties memprops;
static VkDevice dev;
static VkQueue queue;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;   /* vkQueueSubmit wants it held */
static uint32_t family;
static VkCommandPool pool;
static VkFence fence;

#define HOST_VISIBLE    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
#define HOST_COHERENT   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT

/* the first memory type the buffer may live in that has `want` (and, preferred, `prefer`) */
static uint32_t
memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
			VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
			if ((bits & (1u << i)) && (f & want) == want && (pass || (f & prefer) == prefer)) {
				return i;
			}
		}
	}
	return UINT32_MAX;
}

/* 0, or no memory type has `want` (it says so) */
static int
make_buffer_usage(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
    VkMemoryPropertyFlags prefer, struct buf *b)
{
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VK(vkCreateBuffer(dev, &bci, NULL, &b->buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b->buffer, &req);
	b->type = memory_type(req.memoryTypeBits, want, prefer);
	if (b->type == UINT32_MAX) {
		vkDestroyBuffer(dev, b->buffer, NULL);
		return -1;
	}
	b->flags = memprops.memoryTypes[b->type].propertyFlags;
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = b->type,
	};
	VK(vkAllocateMemory(dev, &mai, NULL, &b->memory));
	VK(vkBindBufferMemory(dev, b->buffer, b->memory, 0));
	b->map = NULL;
	if (b->flags & HOST_VISIBLE) {
		VK(vkMapMemory(dev, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->map));
	}
	return 0;
}

#define ANY_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)

static void
make_buffer(VkDeviceSize size, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer, struct buf *b)
{
	if (make_buffer_usage(size, ANY_USAGE, want, prefer, b)) {
		printf("VKTEST FAIL no memory type for 0x%x\n", want);
		exit(101);
	}
}

static void
free_buffer(struct buf *b)
{
	if (b->map) {
		vkUnmapMemory(dev, b->memory);
	}
	vkDestroyBuffer(dev, b->buffer, NULL);
	vkFreeMemory(dev, b->memory, NULL);
}

/* host writes reach the device / device writes reach the host (needed unless the memory is coherent) */
static void
flush(struct buf *b)
{
	VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, b->memory, 0, VK_WHOLE_SIZE };
	VK(vkFlushMappedMemoryRanges(dev, 1, &r));
}

static void
invalidate(struct buf *b)
{
	VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, b->memory, 0, VK_WHOLE_SIZE };
	VK(vkInvalidateMappedMemoryRanges(dev, 1, &r));
}

static void
barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags src_access, VkPipelineStageFlags dst,
    VkAccessFlags dst_access)
{
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, src_access, dst_access };
	vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

static VkCommandBuffer
begin_in(VkCommandPool p)
{
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = p,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb;
	VK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

static VkCommandBuffer begin(void) { return begin_in(pool); }

static void
submit(VkCommandBuffer cb, VkFence f)
{
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = cb ? 1 : 0,
		.pCommandBuffers = &cb,
	};
	VK(vkResetFences(dev, 1, &f));
	pthread_mutex_lock(&queue_lock);
	VK(vkQueueSubmit(queue, 1, &si, f));
	pthread_mutex_unlock(&queue_lock);
}

static void
wait_for(VkFence f)
{
	VK(vkWaitForFences(dev, 1, &f, VK_TRUE, 30ull * 1000 * 1000 * 1000));
}

/* submit, wait; the milliseconds it took */
static double
run_in(VkCommandPool p, VkFence f, VkCommandBuffer cb)
{
	VK(vkEndCommandBuffer(cb));
	double t0 = now_ms();
	submit(cb, f);
	wait_for(f);
	double t = now_ms() - t0;
	vkFreeCommandBuffers(dev, p, 1, &cb);
	return t;
}

static double run(VkCommandBuffer cb) { return run_in(pool, fence, cb); }

/* the first word that is not i * mul + add, or -1 */
static long
first_wrong(const uint32_t *p, uint32_t n, uint32_t mul, uint32_t add)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] != i * mul + add) {
			return i;
		}
	}
	return -1;
}

/* ---- the compute shader's pipeline */

struct compute {
	VkShaderModule sm;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dp;
	VkDescriptorSet ds;
};

static void
make_compute(VkPipelineCache cache, VkBuffer target, struct compute *c)
{
	VkShaderModuleCreateInfo smci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(index_spv),
		.pCode = index_spv,
	};
	VK(vkCreateShaderModule(dev, &smci, NULL, &c->sm));
	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dslci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &c->dsl));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &c->dsl,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr,
	};
	VK(vkCreatePipelineLayout(dev, &plci, NULL, &c->pl));
	VkComputePipelineCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = c->sm,
			.pName = "main",
		},
		.layout = c->pl,
	};
	VK(vkCreateComputePipelines(dev, cache, 1, &cpci, NULL, &c->pipe));
	VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &dps,
	};
	VK(vkCreateDescriptorPool(dev, &dpci, NULL, &c->dp));
	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = c->dp,
		.descriptorSetCount = 1,
		.pSetLayouts = &c->dsl,
	};
	VK(vkAllocateDescriptorSets(dev, &dsai, &c->ds));
	VkDescriptorBufferInfo dbi = { target, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wds = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = c->ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
}

static void
record_compute(VkCommandBuffer cb, struct compute *c, uint32_t words, uint32_t mul, uint32_t add)
{
	uint32_t pc[3] = { words, mul, add };
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &c->ds, 0, NULL);
	vkCmdPushConstants(cb, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vkCmdDispatch(cb, (words + 63) / 64, 1, 1);
}

static void
free_compute(struct compute *c)
{
	vkDestroyPipeline(dev, c->pipe, NULL);
	vkDestroyDescriptorPool(dev, c->dp, NULL);
	vkDestroyPipelineLayout(dev, c->pl, NULL);
	vkDestroyDescriptorSetLayout(dev, c->dsl, NULL);
	vkDestroyShaderModule(dev, c->sm, NULL);
}

/* ---- 2-4: buffers, explicit flush and invalidate, compute */

static void
test_buffers(uint32_t words)
{
	VkDeviceSize size = (VkDeviceSize)words * 4;
	char line[256];
	struct buf gpu, host;
	double t;

	make_buffer(size, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gpu);
	make_buffer(size, HOST_VISIBLE, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &host);
	printf("VKTEST buffers of %u words: device in memory type %u, host-visible in type %u%s\n", words, gpu.type,
	    host.type, host.flags & HOST_COHERENT ? " (coherent)" : " (not coherent)");

	/* fill on the device, copy into host-visible memory, read back */
	memset(host.map, 0x5a, size);
	flush(&host);
	VkCommandBuffer cb = begin();
	vkCmdFillBuffer(cb, gpu.buffer, 0, size, 0xC0FFEE11u);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT);
	VkBufferCopy whole = { 0, 0, size };
	vkCmdCopyBuffer(cb, gpu.buffer, host.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	t = run(cb);
	double ti = now_ms();
	invalidate(&host);
	ti = now_ms() - ti;
	long bad = first_wrong(host.map, words, 0, 0xC0FFEE11u);
	if (bad < 0) {
		snprintf(line, sizeof(line), "%u words of 0xc0ffee11 (submit and wait %.2f ms; invalidate %.2f ms: %.0f MB/s)",
		    words, t, ti, size / 1e3 / ti);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0xc0ffee11", bad, host.map[bad]);
	}
	check("vkCmdFillBuffer + vkCmdCopyBuffer, read back", bad < 0, line);

	/* a pattern from here, flushed, through device memory and back */
	for (uint32_t i = 0; i < words; i++) {
		host.map[i] = i * 2654435761u + 12345u;
	}
	double tf = now_ms();
	flush(&host);
	tf = now_ms() - tf;
	cb = begin();
	vkCmdCopyBuffer(cb, host.buffer, gpu.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	vkCmdFillBuffer(cb, host.buffer, 0, size, 0);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	vkCmdCopyBuffer(cb, gpu.buffer, host.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	t = run(cb);
	if (!(host.flags & HOST_COHERENT)) {
		memset(host.map, 0, size);      /* what is read next came from the host */
	}
	invalidate(&host);
	bad = first_wrong(host.map, words, 2654435761u, 12345u);
	if (bad < 0) {
		snprintf(line, sizeof(line), "%u words (flush %.2f ms: %.0f MB/s; submit and wait %.2f ms)", words, tf,
		    size / 1e3 / tf, t);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0x%08x", bad, host.map[bad],
		    (uint32_t)bad * 2654435761u + 12345u);
	}
	check("flushed pattern through device memory and back", bad < 0, line);

	/* a compute shader writes data[i] = i * 3 + 7 */
	struct compute c;
	t = now_ms();
	make_compute(VK_NULL_HANDLE, gpu.buffer, &c);
	double tp = now_ms() - t;
	cb = begin();
	record_compute(cb, &c, words, 3, 7);
	barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT);
	vkCmdCopyBuffer(cb, gpu.buffer, host.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	t = run(cb);
	invalidate(&host);
	bad = first_wrong(host.map, words, 3, 7);
	if (bad < 0) {
		snprintf(line, sizeof(line), "%u words of i * 3 + 7 (pipeline %.1f ms; dispatch, copy and wait %.2f ms)", words,
		    tp, t);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0x%08x", bad, host.map[bad], (uint32_t)bad * 3 + 7);
	}
	check("compute shader", bad < 0, line);
	free_compute(&c);
	free_buffer(&gpu);
	free_buffer(&host);
}

/* ---- 5: host-coherent memory, with no flush and no invalidate */

static void
test_coherent(uint32_t words)
{
	VkDeviceSize size = (VkDeviceSize)words * 4;
	uint32_t half = words / 2;
	char line[256];
	struct buf gpu, coh, host;
	long bad;

	if (make_buffer_usage(size, ANY_USAGE, HOST_VISIBLE | HOST_COHERENT, 0, &coh)) {
		/* (the driver's default: the host's types as they are; HAKIOS_VK_COHERENCE=emulate) */
		printf("VKTEST SKIP coherent memory: no host-visible type is coherent\n");
		return;
	}
	make_buffer(size, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gpu);
	make_buffer(size, HOST_VISIBLE, 0, &host);
	VkBufferCopy whole = { 0, 0, size };

	/* written here, read by the device; written by the device, read here: neither flushed nor invalidated */
	for (uint32_t i = 0; i < words; i++) {
		coh.map[i] = i * 7 + 3;
	}
	VkCommandBuffer cb = begin();
	vkCmdCopyBuffer(cb, coh.buffer, gpu.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	vkCmdFillBuffer(cb, coh.buffer, 0, size, 0);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
	vkCmdCopyBuffer(cb, gpu.buffer, coh.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	double t = run(cb);
	bad = first_wrong(coh.map, words, 7, 3);
	if (bad < 0) {
		snprintf(line, sizeof(line), "type %u, %u words there and back (submit and wait %.2f ms)", coh.type, words, t);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0x%08x", bad, coh.map[bad], (uint32_t)bad * 7 + 3);
	}
	check("coherent memory, no flush or invalidate", bad < 0, line);

	/*
	 * Both sides at once: while the device fills the second half, this side writes the first. After
	 * the wait each half must be the writer's -- here, and (after another submit) on the device.
	 */
	cb = begin();
	VkBufferCopy second = { (VkDeviceSize)half * 4, (VkDeviceSize)half * 4, size - (VkDeviceSize)half * 4 };
	vkCmdFillBuffer(cb, coh.buffer, second.dstOffset, second.size, 0xAAAAAAAAu);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	VK(vkEndCommandBuffer(cb));
	submit(cb, fence);
	for (uint32_t i = 0; i < half; i++) {
		coh.map[i] = 0x55555555u;
	}
	wait_for(fence);
	vkFreeCommandBuffers(dev, pool, 1, &cb);
	long here = -1, there = -1;
	for (uint32_t i = 0; i < words && here < 0; i++) {
		if (coh.map[i] != (i < half ? 0x55555555u : 0xAAAAAAAAu)) {
			here = i;
		}
	}
	cb = begin();
	vkCmdCopyBuffer(cb, coh.buffer, host.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	run(cb);
	invalidate(&host);
	for (uint32_t i = 0; i < words && there < 0; i++) {
		if (host.map[i] != (i < half ? 0x55555555u : 0xAAAAAAAAu)) {
			there = i;
		}
	}
	if (here < 0 && there < 0) {
		snprintf(line, sizeof(line), "the first half from here, the second from the device, on both sides");
	} else if (here >= 0) {
		snprintf(line, sizeof(line), "here, word %ld is 0x%08x", here, coh.map[here]);
	} else {
		snprintf(line, sizeof(line), "on the device, word %ld is 0x%08x", there, host.map[there]);
	}
	check("coherent memory written by both sides during one submit", here < 0 && there < 0, line);
	free_buffer(&gpu);
	free_buffer(&host);
	free_buffer(&coh);
}

/*
 * ---- 6: what coherence costs: an empty submit and its wait with a big block of coherent memory
 * mapped (as metal2vulkan keeps 64 MiB blocks), clean or with some of it written here
 */
static void
coherence_cost(uint32_t mib)
{
	VkDeviceSize size = (VkDeviceSize)mib << 20;
	static const struct { VkBufferUsageFlags usage; const char *what; } kinds[] = {
		{ VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "bound read-only" },
		{ ANY_USAGE, "bound writable" },
	};
	for (unsigned k = 0; k < 2; k++) {
		struct buf b;
		if (make_buffer_usage(size, kinds[k].usage, HOST_VISIBLE | HOST_COHERENT, 0, &b)) {
			return;
		}
		static const uint32_t dirty_kib[] = { 0, 4, 1024 };
		for (unsigned d = 0; d < 3; d++) {
			double ts = 0, tw = 0;
			const int n = 10;
			for (int i = 0; i < n + 1; i++) {
				for (uint32_t off = 0; off < dirty_kib[d] * 256; off += 1) {
					b.map[off]++;
				}
				double t0 = now_ms();
				submit(VK_NULL_HANDLE, fence);
				double t1 = now_ms();
				wait_for(fence);
				double t2 = now_ms();
				if (i) {                /* (the first one settles what the allocation did) */
					ts += t1 - t0;
					tw += t2 - t1;
				}
			}
			printf("VKTEST coherence: %u MiB mapped, %s, %u KiB written here: submit %.3f ms, wait %.3f ms\n", mib,
			    kinds[k].what, dirty_kib[d], ts / n, tw / n);
		}
		free_buffer(&b);
	}
}

/* ---- 7: many threads at once, as a real runtime does (each thread is a stream of its own) */

struct worker {
	pthread_t thread;
	int index, rounds;
	int bad;
	VkPipelineCache shared_cache;
};

static void *
worker_main(void *arg)
{
	struct worker *w = arg;
	const uint32_t words = 4096;
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family,
	};
	VkCommandPool p;
	VkFence f;
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	struct buf gpu, host;
	VK(vkCreateCommandPool(dev, &pci, NULL, &p));
	VK(vkCreateFence(dev, &fci, NULL, &f));
	make_buffer(words * 4, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gpu);
	make_buffer(words * 4, HOST_VISIBLE, 0, &host);
	for (int r = 0; r < w->rounds && !w->bad; r++) {
		VkPipelineCacheCreateInfo pcci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
		VkPipelineCache cache;
		VK(vkCreatePipelineCache(dev, &pcci, NULL, &cache));
		VkQueryPoolCreateInfo qpci = {
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_OCCLUSION,
			.queryCount = 8,
		};
		VkQueryPool qp;
		VK(vkCreateQueryPool(dev, &qpci, NULL, &qp));
		struct compute c;
		make_compute(r & 1 ? cache : w->shared_cache, gpu.buffer, &c);
		uint32_t mul = (uint32_t)w->index + 2, add = (uint32_t)(r * 1000 + w->index);
		VkCommandBuffer cb = begin_in(p);
		record_compute(cb, &c, words, mul, add);
		barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferCopy whole = { 0, 0, words * 4 };
		vkCmdCopyBuffer(cb, gpu.buffer, host.buffer, 1, &whole);
		barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		    VK_ACCESS_HOST_READ_BIT);
		run_in(p, f, cb);
		invalidate(&host);
		long bad = first_wrong(host.map, words, mul, add);
		if (bad >= 0) {
			char line[160];
			snprintf(line, sizeof(line), "thread %d round %d: word %ld is 0x%08x, not 0x%08x", w->index, r, bad,
			    host.map[bad], (uint32_t)bad * mul + add);
			check("threads", 0, line);
			w->bad = 1;
		}
		free_compute(&c);
		vkDestroyQueryPool(dev, qp, NULL);
		vkDestroyPipelineCache(dev, cache, NULL);
	}
	free_buffer(&gpu);
	free_buffer(&host);
	vkDestroyFence(dev, f, NULL);
	vkDestroyCommandPool(dev, p, NULL);
	return NULL;
}

/*
 * A waiter that starts waiting before the work it waits for is submitted (metal2vulkan has one a
 * queue): on a fence and, where there are timeline semaphores, on a value. Meanwhile the main thread
 * makes things (calls that wait for the host's reply) and only then submits. A wait that blocked on
 * the host would hold the phone's one mailbox, and the main thread's replies with it, for ever.
 */
static int timeline_ok;

struct waiter {
	VkFence fence;
	VkSemaphore timeline;
	VkResult fence_result, timeline_result;
	double waited_ms;
};

static void *
waiter_main(void *arg)
{
	struct waiter *w = arg;
	double t0 = now_ms();
	w->fence_result = vkWaitForFences(dev, 1, &w->fence, VK_TRUE, 30ull * 1000 * 1000 * 1000);
	if (w->timeline) {
		uint64_t value = 1;
		VkSemaphoreWaitInfo wi = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &w->timeline,
			.pValues = &value,
		};
		w->timeline_result = vkWaitSemaphores(dev, &wi, 30ull * 1000 * 1000 * 1000);
	}
	w->waited_ms = now_ms() - t0;
	return NULL;
}

static void
test_waiter(void)
{
	struct waiter w = { 0 };
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VK(vkCreateFence(dev, &fci, NULL, &w.fence));
	if (timeline_ok) {
		VkSemaphoreTypeCreateInfo tci = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		};
		VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci };
		VK(vkCreateSemaphore(dev, &sci, NULL, &w.timeline));
	}
	pthread_t t;
	pthread_create(&t, NULL, waiter_main, &w);
	usleep(50 * 1000);                      /* (the waiter is waiting by now) */
	struct buf b;
	make_buffer(65536, HOST_VISIBLE, 0, &b);   /* calls that want replies */
	free_buffer(&b);
	uint64_t one = 1;
	VkTimelineSemaphoreSubmitInfo tsi = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &one,
	};
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = w.timeline ? &tsi : NULL,
		.signalSemaphoreCount = w.timeline ? 1 : 0,
		.pSignalSemaphores = &w.timeline,
	};
	pthread_mutex_lock(&queue_lock);
	VK(vkQueueSubmit(queue, 1, &si, w.fence));
	pthread_mutex_unlock(&queue_lock);
	pthread_join(t, NULL);
	char line[200];
	snprintf(line, sizeof(line), "the fence %s, the timeline value %s; %.0f ms waiting (50 of them before the submit)",
	    w.fence_result == VK_SUCCESS ? "came" : "did not come",
	    !w.timeline ? "not tried (no timeline semaphores)" : w.timeline_result == VK_SUCCESS ? "came" : "did not come",
	    w.waited_ms);
	check("a waiter on work not yet submitted, while this thread makes things", w.fence_result == VK_SUCCESS &&
	    (!w.timeline || w.timeline_result == VK_SUCCESS), line);
	if (w.timeline) {
		vkDestroySemaphore(dev, w.timeline, NULL);
	}
	vkDestroyFence(dev, w.fence, NULL);
}

static void *
watchdog(void *arg)
{
	int seconds = *(int *)arg;
	sleep((unsigned)seconds);
	printf("VKTEST FAIL threads: still running after %d s (a deadlock between the threads' streams?)\n", seconds);
	fflush(stdout);
	_exit(99);
}

static void
test_threads(int n, int rounds)
{
	struct worker w[64];
	n = n > 64 ? 64 : n;
	VkPipelineCacheCreateInfo pcci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	VkPipelineCache shared;
	VK(vkCreatePipelineCache(dev, &pcci, NULL, &shared));
	static int limit = 120;
	pthread_t dog;
	pthread_create(&dog, NULL, watchdog, &limit);
	pthread_detach(dog);
	test_waiter();
	double t0 = now_ms();
	for (int i = 0; i < n; i++) {
		w[i] = (struct worker){ .index = i, .rounds = rounds, .shared_cache = shared };
		pthread_create(&w[i].thread, NULL, worker_main, &w[i]);
	}
	/* and this thread too: query pools and empty submits, meanwhile */
	VkFence f;
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VK(vkCreateFence(dev, &fci, NULL, &f));
	for (int r = 0; r < rounds * 4; r++) {
		VkQueryPoolCreateInfo qpci = {
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_OCCLUSION,
			.queryCount = 4,
		};
		VkQueryPool qp;
		VK(vkCreateQueryPool(dev, &qpci, NULL, &qp));
		submit(VK_NULL_HANDLE, f);
		wait_for(f);
		vkDestroyQueryPool(dev, qp, NULL);
	}
	vkDestroyFence(dev, f, NULL);
	int bad = 0;
	for (int i = 0; i < n; i++) {
		pthread_join(w[i].thread, NULL);
		bad |= w[i].bad;
	}
	vkDestroyPipelineCache(dev, shared, NULL);
	char line[160];
	snprintf(line, sizeof(line), "%d threads x %d rounds of pipeline cache, query pool, compute pipeline, dispatch, "
	    "submit and check, with this thread submitting too: %.0f ms", n, rounds, now_ms() - t0);
	check("threads", !bad, line);
}

int
main(int argc, char **argv)
{
	uint32_t words = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 262144;
	int threads = argc > 2 ? atoi(argv[2]) : 4;
	double t0 = now_ms(), t;

	setvbuf(stdout, NULL, _IOLBF, 0);
	/* ---- 1. instance, physical device, device, queue */
	uint32_t api = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion(&api);
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vk_test",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	VK(vkCreateInstance(&ici, NULL, &instance));
	printf("VKTEST instance: Vulkan %u.%u.%u offered, %.1f ms (the stream to the host included)\n",
	    VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api), now_ms() - t0);

	uint32_t n = 0;
	VK(vkEnumeratePhysicalDevices(instance, &n, NULL));
	check("physical device", n > 0, NULL);
	if (n == 0) {
		return failed;
	}
	VkPhysicalDevice pdevs[8];
	n = n > 8 ? 8 : n;
	VK(vkEnumeratePhysicalDevices(instance, &n, pdevs));
	VkPhysicalDevice pd = pdevs[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("VKTEST physical device: \"%s\" (vendor 0x%04x device 0x%04x, Vulkan %u.%u.%u, driver 0x%08x, type %d)\n",
	    props.deviceName, props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
	    VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion,
	    props.deviceType);
	vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
	for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
		printf("VKTEST memory type %u: heap %u%s%s%s%s\n", i, memprops.memoryTypes[i].heapIndex,
		    f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
		    f & HOST_VISIBLE ? " host-visible" : "",
		    f & HOST_COHERENT ? " host-coherent" : "",
		    f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " host-cached" : "");
	}

	uint32_t nq = 0;
	family = UINT32_MAX;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	VkQueueFamilyProperties qf[16];
	nq = nq > 16 ? 16 : nq;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			family = i;
		}
	}
	check("a compute queue family", family != UINT32_MAX, NULL);
	if (family == UINT32_MAX) {
		return failed;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	/* timeline semaphores, where the device has them (for 7's waiter) */
	VkPhysicalDeviceTimelineSemaphoreFeatures tsf = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
	};
	VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &tsf };
	if (props.apiVersion >= VK_API_VERSION_1_2) {
		vkGetPhysicalDeviceFeatures2(pd, &f2);
	}
	timeline_ok = tsf.timelineSemaphore;
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = timeline_ok ? &tsf : NULL,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
	};
	t = now_ms();
	VK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, family, 0, &queue);
	printf("VKTEST device and queue (family %u of %u): %.1f ms\n", family, nq, now_ms() - t);
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family,
	};
	VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VK(vkCreateFence(dev, &fci, NULL, &fence));

	test_buffers(words);
	test_coherent(words);
	coherence_cost(64);

	/* the round trip: an empty submit waited for, a hundred times */
	t = now_ms();
	for (int i = 0; i < 100; i++) {
		submit(VK_NULL_HANDLE, fence);
		wait_for(fence);
	}
	printf("VKTEST an empty submit and wait: %.3f ms\n", (now_ms() - t) / 100);

	if (threads > 0) {
		test_threads(threads, 8);
	}

	vkDestroyFence(dev, fence, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(instance, NULL);
	printf("VKTEST %s: %d failed, %.0f ms in all\n", failed ? "FAILED" : "OK", failed, now_ms() - t0);
	return failed;
}
