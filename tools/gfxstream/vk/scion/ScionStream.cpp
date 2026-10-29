/*
 * ScionStream.h. A failed write or read is fatal to the stream, as it is to gfxstream's own
 * streams: the host's render thread and this side no longer agree on where a packet starts.
 */
#include "ScionStream.h"

#include <cstdlib>
#include <cstring>

#include "util/log.h"

ScionStream::ScionStream(size_t bufSize) : IOStream(bufSize), mBufSize(bufSize) {}

ScionStream::~ScionStream() {
    if (mOpen && mTransport.close) {
        mTransport.close(mTransport.arg);
    }
    free(mBuf);
}

int ScionStream::open() {
    int err = scion_vk_transport_open(&mTransport);
    if (err) {
        mesa_loge("scion-vk: no stream to the host (%d)", err);
        return err;
    }
    mOpen = true;
    return 0;
}

void* ScionStream::allocBuffer(size_t minSize) {
    size_t size = mBufSize < minSize ? minSize : mBufSize;
    if (mBuf == nullptr || size > mBufSize) {
        unsigned char* p = (unsigned char*)realloc(mBuf, size);
        if (p == nullptr) {
            mesa_loge("scion-vk: no memory for a %zu-byte stream buffer", size);
            return nullptr;
        }
        mBuf = p;
        mBufSize = size;
    }
    return mBuf;
}

int ScionStream::commitBuffer(size_t size) { return writeFully(mBuf, size); }

int ScionStream::writeFully(const void* buf, size_t len) {
    if (mDead || !mOpen) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    int err = mTransport.write(mTransport.arg, buf, len);
    if (err) {
        mesa_loge("scion-vk: write of %zu bytes failed (%d); the stream is dead", len, err);
        mDead = true;
        abort();
    }
    return 0;
}

const unsigned char* ScionStream::readFully(void* buf, size_t len) {
    flush();
    if (mDead || !mOpen) {
        return nullptr;
    }
    if (len == 0) {
        return (const unsigned char*)buf;
    }
    int err = mTransport.read(mTransport.arg, buf, len);
    if (err) {
        mesa_loge("scion-vk: read of %zu bytes failed (%d); the stream is dead", len, err);
        mDead = true;
        abort();
    }
    return (const unsigned char*)buf;
}

const unsigned char* ScionStream::commitBufferAndReadFully(size_t size, void* buf, size_t len) {
    return commitBuffer(size) ? nullptr : readFully(buf, len);
}

const unsigned char* ScionStream::read(void* buf, size_t* inout_len) {
    return readFully(buf, *inout_len);
}

gfxstream::guest::IOStream* createScionStream(size_t bufSize, bool* flagsSent) {
    ScionStream* s = new ScionStream(bufSize);
    if (s->open()) {
        delete s;
        return nullptr;
    }
    *flagsSent = s->flagsSent();
    return s;
}
