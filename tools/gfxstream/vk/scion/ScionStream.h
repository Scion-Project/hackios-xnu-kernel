/*
 * The gfxstream IOStream of Scion's Vulkan driver: a byte pipe to a "pipe:opengles" render thread
 * (scion_vk_transport.h), the way src/gfx's GL stream is one. Writes are gathered in a buffer and
 * sent when it fills or a reply is wanted; a reply is read exactly.
 */
#ifndef SCION_STREAM_H
#define SCION_STREAM_H

#include <cstddef>

#include "gfxstream/guest/IOStream.h"
#include "scion_vk_transport.h"

class ScionStream : public gfxstream::guest::IOStream {
   public:
    explicit ScionStream(size_t bufSize);
    ~ScionStream();

    /* 0 or an errno */
    int open();
    /* the transport sent the render thread's flags word itself */
    bool flagsSent() const { return mTransport.flags_sent != 0; }

    void* allocBuffer(size_t minSize) override;
    int commitBuffer(size_t size) override;
    const unsigned char* readFully(void* buf, size_t len) override;
    const unsigned char* commitBufferAndReadFully(size_t size, void* buf, size_t len) override;
    const unsigned char* read(void* buf, size_t* inout_len) override;
    int writeFully(const void* buf, size_t len) override;

   private:
    struct scion_vk_transport mTransport = {};
    bool mOpen = false;
    bool mDead = false;
    size_t mBufSize;
    unsigned char* mBuf = nullptr;
};

/* A stream for GFXSTREAM_TRANSPORT_SCION, or nullptr; *flagsSent as flagsSent() above. */
gfxstream::guest::IOStream* createScionStream(size_t bufSize, bool* flagsSent);

#endif
