/*
 * Scion's platform for Mesa's gfxstream Vulkan driver (src/gfxstream/guest/platform): what the
 * driver asks of a virtio-gpu device, for a device that is only a byte pipe (/dev/vgpu3d, or the
 * Mac harness of scion_vk_transport.c). It has no blobs, maps no host memory, runs no execbuffers
 * and makes no sync fds; the stream itself is ScionStream (GFXSTREAM_TRANSPORT_SCION).
 *
 * The caps are the host's gfxstream-vulkan capset when the transport can read it (the Mac),
 * otherwise the one gfxstream's host (VirtioGpuFrontend::fillCaps) would give. Either way the
 * guest is told what this device cannot do: no deferred mapping, no blobs, no external sync, and
 * kParamHostVisible 0, which ResourceTracker takes as "no direct mapping": host-visible memory is
 * a guest copy that vkFlushMappedMemoryRanges and vkInvalidateMappedMemoryRanges carry over the
 * stream (the host must agree: renderer feature VirtioGpuNext disabled).
 */
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "Sync.h"
#include "VirtGpu.h"
#include "scion_vk_transport.h"
#include "util/log.h"

namespace {

class ScionVirtGpuDevice : public VirtGpuDevice {
   public:
    explicit ScionVirtGpuDevice(enum VirtGpuCapset capset) : VirtGpuDevice(capset) {
        memset(&mCaps, 0, sizeof(mCaps));
        struct vulkanCapset* vk = &mCaps.vulkanCapset;
        if (scion_vk_transport_capset(kCapsetGfxStreamVulkan, vk, sizeof(*vk)) != 0) {
            vk->protocolVersion = 1;
            vk->ringSize = 12288;
            vk->bufferSize = 1048576;
            vk->colorBufferMemoryIndex = 0xFFFFFFFF;   /* asked of the host when needed */
            vk->vulkanBatchedDescriptorSetUpdate = 1;  /* the host's default */
            vk->noRenderControlEnc = 1;
        }
        const char* batched = getenv("SCION_VK_BATCHED_DESCRIPTORS");
        if (batched) {
            vk->vulkanBatchedDescriptorSetUpdate = atoi(batched) != 0;
        }
        /* what a byte pipe cannot do */
        vk->deferredMapping = 0;
        vk->alwaysBlob = 0;
        vk->externalSync = 0;
        vk->noRenderControlEnc = 1;
        if (!vk->blobAlignment) {
            vk->blobAlignment = 4096;
        }
        mCaps.params[kParam3D] = 1;
        mCaps.params[kParamCapsetFix] = 1;
        mCaps.params[kParamContextInit] = 1;
        mCaps.params[kParamSupportedCapsetIds] = 1u << kCapsetGfxStreamVulkan;
        /* 0: kParamResourceBlob, kParamHostVisible, kParamCrossDevice, kParamFencePassing,
         * kParamCreateGuestHandle */
    }

    int64_t getDeviceHandle(void) override { return -1; }
    struct VirtGpuCaps getCaps(void) override { return mCaps; }

    VirtGpuResourcePtr createBlob(const struct VirtGpuCreateBlob&) override {
        mesa_loge("scion-vk: no blobs on this device");
        return nullptr;
    }
    VirtGpuResourcePtr createResource(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                      uint32_t) override {
        mesa_loge("scion-vk: no resources on this device");
        return nullptr;
    }
    VirtGpuResourcePtr importBlob(const struct VirtGpuExternalHandle&) override { return nullptr; }
    int execBuffer(struct VirtGpuExecBuffer&, const VirtGpuResource*) override {
        mesa_loge("scion-vk: no execbuffer on this device");
        return -ENOTSUP;
    }

   private:
    struct VirtGpuCaps mCaps;
};

class ScionSyncHelper : public gfxstream::SyncHelper {
   public:
    int wait(int, int) override { return -1; }
    void debugPrint(int) override {}
    int dup(int) override { return -1; }
    int close(int) override { return 0; }
};

}  // namespace

VirtGpuDevice* osCreateVirtGpuDevice(enum VirtGpuCapset capset, int32_t) {
    return new ScionVirtGpuDevice(capset);
}

namespace gfxstream {

SyncHelper* osCreateSyncHelper() { return new ScionSyncHelper(); }

}  // namespace gfxstream
