/*
 * vk_test: Vulkan end to end through Scion's gfxstream Vulkan driver (libhakios.vk.dylib,
 * tools/gfxstream/VULKAN.md), with nothing but the Vulkan API. The host's GPU does the work:
 *
 *   1. an instance, the physical device (the host's GPU as gfxstream shows it), a device, a queue
 *   2. vkCmdFillBuffer into device memory, vkCmdCopyBuffer into host-visible memory, read back
 *   3. a pattern written here, flushed, copied through device memory and back (the flush's bytes
 *      must reach the host: the driver carries them over the stream)
 *   4. a compute shader (SPIR-V below) writing data[i] = i * mul + add, read back
 *
 * On iOS the stream is /dev/vgpu3d; on the Mac, the Android emulator's gfxstream in this
 * process (tools/gfxstream/mk-vk.sh mac). "VKTEST ..." lines report; the exit status is the
 * number of checks that failed.
 *
 *   vk_test [words]     the buffers' size in 32-bit words (default 262144: 1 MiB)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void
check(const char *what, int ok, const char *detail)
{
	printf("VKTEST %s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail ? ": " : "", detail ? detail : "");
	failed += !ok;
}

struct buf {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t type;
	VkMemoryPropertyFlags flags;
	uint32_t *map;
};

static VkPhysicalDeviceMemoryProperties memprops;

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

static void
make_buffer(VkDevice dev, VkDeviceSize size, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer,
    struct buf *b)
{
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VK(vkCreateBuffer(dev, &bci, NULL, &b->buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b->buffer, &req);
	b->type = memory_type(req.memoryTypeBits, want, prefer);
	if (b->type == UINT32_MAX) {
		printf("VKTEST FAIL no memory type for 0x%x (bits 0x%x)\n", want, req.memoryTypeBits);
		exit(101);
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
	if (b->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
		VK(vkMapMemory(dev, b->memory, 0, VK_WHOLE_SIZE, 0, (void **)&b->map));
	}
}

static void
free_buffer(VkDevice dev, struct buf *b)
{
	if (b->map) {
		vkUnmapMemory(dev, b->memory);
	}
	vkDestroyBuffer(dev, b->buffer, NULL);
	vkFreeMemory(dev, b->memory, NULL);
}

/* host writes reach the device / device writes reach the host, unless the memory is coherent */
static void
flush(VkDevice dev, struct buf *b)
{
	VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, b->memory, 0, VK_WHOLE_SIZE };
	VK(vkFlushMappedMemoryRanges(dev, 1, &r));
}

static void
invalidate(VkDevice dev, struct buf *b)
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

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkFence fence;

static VkCommandBuffer
begin(void)
{
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
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

/* submit, wait; the milliseconds it took */
static double
run(VkCommandBuffer cb)
{
	VK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};
	double t0 = now_ms();
	VK(vkResetFences(dev, 1, &fence));
	VK(vkQueueSubmit(queue, 1, &si, fence));
	VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
	double t = now_ms() - t0;
	vkFreeCommandBuffers(dev, pool, 1, &cb);
	return t;
}

/* the first word that is not want(i), or -1 */
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

int
main(int argc, char **argv)
{
	uint32_t words = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 262144;
	VkDeviceSize size = (VkDeviceSize)words * 4;
	char line[256];
	double t0 = now_ms(), t;

	setvbuf(stdout, NULL, _IOLBF, 0);
	/* ---- 1. instance, physical device, device, queue */
	uint32_t api = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion(&api);
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vk_test",
		.apiVersion = VK_API_VERSION_1_1,
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
		    f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "",
		    f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " host-coherent" : "",
		    f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " host-cached" : "");
	}

	uint32_t nq = 0, family = UINT32_MAX;
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
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
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

	struct buf gpu, host;
	make_buffer(dev, size, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gpu);
	make_buffer(dev, size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &host);
	printf("VKTEST buffers of %u words: device in memory type %u, host-visible in type %u%s\n", words, gpu.type,
	    host.type, host.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " (coherent)" : " (not coherent)");

	/* ---- 2. fill on the device, copy into host-visible memory, read back */
	memset(host.map, 0x5a, size);
	flush(dev, &host);
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
	invalidate(dev, &host);
	ti = now_ms() - ti;
	long bad = first_wrong(host.map, words, 0, 0xC0FFEE11u);
	if (bad < 0) {
		snprintf(line, sizeof(line), "%u words of 0xc0ffee11 (submit and wait %.2f ms; invalidate %.2f ms: %.0f MB/s)",
		    words, t, ti, size / 1e3 / ti);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0xc0ffee11", bad, host.map[bad]);
	}
	check("vkCmdFillBuffer + vkCmdCopyBuffer, read back", bad < 0, line);

	/* ---- 3. a pattern from here, flushed, through device memory and back */
	for (uint32_t i = 0; i < words; i++) {
		host.map[i] = i * 2654435761u + 12345u;
	}
	double tf = now_ms();
	flush(dev, &host);
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
	memset(host.map, 0, size);              /* what is read next came from the host */
	invalidate(dev, &host);
	bad = first_wrong(host.map, words, 2654435761u, 12345u);
	if (bad < 0) {
		snprintf(line, sizeof(line), "%u words (flush %.2f ms: %.0f MB/s; submit and wait %.2f ms)", words, tf,
		    size / 1e3 / tf, t);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0x%08x", bad, host.map[bad],
		    (uint32_t)bad * 2654435761u + 12345u);
	}
	check("flushed pattern through device memory and back", bad < 0, line);

	/* ---- 4. a compute shader writes data[i] = i * 3 + 7 */
	VkShaderModuleCreateInfo smci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(index_spv),
		.pCode = index_spv,
	};
	VkShaderModule sm;
	VK(vkCreateShaderModule(dev, &smci, NULL, &sm));
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
	VkDescriptorSetLayout dsl;
	VK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &dsl,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr,
	};
	VkPipelineLayout pl;
	VK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkComputePipelineCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = sm,
			.pName = "main",
		},
		.layout = pl,
	};
	VkPipeline pipe;
	t = now_ms();
	VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
	double tp = now_ms() - t;
	VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &dps,
	};
	VkDescriptorPool dp;
	VK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp,
		.descriptorSetCount = 1,
		.pSetLayouts = &dsl,
	};
	VkDescriptorSet ds;
	VK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorBufferInfo dbi = { gpu.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wds = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

	uint32_t pc[3] = { words, 3, 7 };
	cb = begin();
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vkCmdDispatch(cb, (words + 63) / 64, 1, 1);
	barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT);
	vkCmdCopyBuffer(cb, gpu.buffer, host.buffer, 1, &whole);
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	    VK_ACCESS_HOST_READ_BIT);
	t = run(cb);
	invalidate(dev, &host);
	bad = first_wrong(host.map, words, 3, 7);
	if (bad < 0) {
		snprintf(line, sizeof(line), "%u words of i * 3 + 7 (pipeline %.1f ms; dispatch, copy and wait %.2f ms)", words,
		    tp, t);
	} else {
		snprintf(line, sizeof(line), "word %ld is 0x%08x, not 0x%08x", bad, host.map[bad], (uint32_t)bad * 3 + 7);
	}
	check("compute shader", bad < 0, line);

	/* ---- the round trip: an empty submit waited for, a hundred times */
	t = now_ms();
	for (int i = 0; i < 100; i++) {
		VK(vkResetFences(dev, 1, &fence));
		VK(vkQueueSubmit(queue, 0, NULL, fence));
		VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
	}
	printf("VKTEST an empty submit and wait: %.3f ms\n", (now_ms() - t) / 100);

	vkDestroyPipeline(dev, pipe, NULL);
	vkDestroyDescriptorPool(dev, dp, NULL);
	vkDestroyPipelineLayout(dev, pl, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	vkDestroyShaderModule(dev, sm, NULL);
	free_buffer(dev, &gpu);
	free_buffer(dev, &host);
	vkDestroyFence(dev, fence, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(instance, NULL);
	printf("VKTEST %s: %d failed, %.0f ms in all\n", failed ? "FAILED" : "OK", failed, now_ms() - t0);
	return failed;
}
