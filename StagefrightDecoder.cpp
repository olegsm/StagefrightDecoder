/*****************************************************************************
 * StagefrightDecoder.cpp: Wrapper for AOSP libstagefright.
 *****************************************************************************
 * Copyright (c) 2017 Oleg Smirnov
 *
 * Authors: Oleg Smirnov
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *****************************************************************************/

#define HAVE_PTHREADS 1
#define HAVE_ANDROID_OS 1

#include <binder/ProcessState.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>

#include <ui/GraphicBuffer.h>
#include <utils/List.h>
#include <utils/UniquePtr.h>

#include <media/IOMX.h>
#include <binder/MemoryDealer.h>
#include <OMX_Component.h>

#include <android/log.h>
#include "codec_utils.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#ifdef LOGI
#undef LOGI
#endif
#ifdef LOGV
#undef LOGV
#endif
#ifdef LOGW
#undef LOGW
#endif
#ifdef LOGE
#undef LOGE
#endif

#ifndef ATTRIBUTE_PUBLIC
#define ATTRIBUTE_PUBLIC __attribute__ ((visibility ("default")))
#endif

#define LOG_TAG "[OMX Stagefright]"

#if !defined(NDEBUG)
#define DEBUG_CODEC
#endif

#if defined(DEBUG_CODEC)
#define LOG_DEBUG __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "this=%p, %s:%d, pid=%d, tid=%d", this, __FUNCTION__, __LINE__, getpid(), gettid())
#define LOGV(args...)  __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, ## args)
#define LOGI(args...)  __android_log_print(ANDROID_LOG_INFO, LOG_TAG, ## args)
#define LOGW(args...)  __android_log_print(ANDROID_LOG_WARN, LOG_TAG, ## args)
#define LOGE(args...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, ## args)
#define LOGR(err,args...)  __android_log_print(err == 0?ANDROID_LOG_INFO:ANDROID_LOG_ERROR, LOG_TAG, ## args)
#else
#define LOG_DEBUG
#define LOGI(args...)
#define LOGV(args...)
#define LOGW(args...)
#define LOGE(args...)
#define LOGR(err,args...)
#endif

#define INFO_OUTPUT_END_OF_STREAM   -4
#define INFO_OUTPUT_BUFFERS_CHANGED -3
#define INFO_OUTPUT_FORMAT_CHANGED  -2
#define INFO_TRY_AGAIN_LATER        -1
#define INFO_OK                      0

#define MAX_HOLDED_FRAMES            3

#define IN_BUFFER_COUNT 4
#define OUT_BUFFER_COUNT 10
//#define DECODER_PRIORITY ANDROID_PRIORITY_AUDIO
#define DECODER_PRIORITY ANDROID_PRIORITY_NORMAL

// DEBUG
static int g_frameCount = 0;

using namespace android;

typedef Vector<MediaBuffer*> MediaBufferQueue;

const int OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka = 0x7FA30C01;
const int QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka = 0x7fa30c03; // Sony
const int OMX_QCOM_COLOR_FormatYVU420SemiPlanar = 0x7FA30C00;
const int OMX_TI_COLOR_FormatYUV420PackedSemiPlanar = 0x7F000100;
const int COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced = 0x7f000001;
const int OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB = 0x7fa00000;
const int OMX_DIRECT_RENDERING = 0x100;

#ifdef __cplusplus
extern "C" {
#endif
void __exidx_start() {}
void __exidx_end()   {}
#ifdef __cplusplus
}
#endif

#if !defined(ANDROID_ICS)
/* STE: Added Support of YUV42XMBN, required for Copybit CC acceleration */
const int HAL_PIXEL_FORMAT_YCBCR42XMBN = 0xE;
#endif

// default fps=25
static int s_frameDisplayTimeMsec = 40;

typedef struct {
    int pixel_format;
    int stride;
    int slice_height;
    int crop_top;
    int crop_bottom;
    int crop_left;
    int crop_right;
    int width;
    int height;
} source_video_format_t;

typedef struct {
    int channel_count;
    int sample_rate;
} source_audio_format_t;

static inline int64_t getTimestampMs()
{
    struct timespec time;
    time.tv_sec = time.tv_nsec = 0;
    clock_gettime(CLOCK_REALTIME, &time);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static inline int32_t getPeriodMs(int64_t start)
{
    return getTimestampMs() - start;
}

static inline void releaseMediaBuffer(MediaBuffer*& buffer)
{
    if (buffer) {
        buffer->release();
        buffer = 0;
    }
}

static inline void releaseMediaBufferQueue(MediaBufferQueue& mediaQueue)
{
    if (mediaQueue.empty())
        return;

    for (MediaBufferQueue::iterator it = mediaQueue.begin(); it != mediaQueue.end(); ++it) {
        if (*it) {
            releaseMediaBuffer(*it);
        }
    }
    mediaQueue.clear();
}

// 8192 = 2^13, 13bit AAC frame size (in bytes)
#define AAC_MAX_FRAME_SIZE 8192

static sp<MetaData> MakeAACCodecSpecificData(unsigned profile,
        unsigned sampling_freq_index, unsigned channel_configuration)
{
    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);

    static const int32_t kSamplingFreq[] = { 96000, 88200, 64000, 48000, 44100,
            32000, 24000, 22050, 16000, 12000, 11025, 8000 };

    meta->setInt32(kKeySampleRate, kSamplingFreq[sampling_freq_index]);
    meta->setInt32(kKeyChannelCount, channel_configuration);

    uint8_t kStaticESDS[] = { 0x03, 22, 0x00,
            0x00, // ES_ID
            0x00, // streamDependenceFlag, URL_Flag, OCRstreamFlag
            0x04,
            17,
            0x40, // Audio ISO/IEC 14496-3
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00,
            0x05, 2
            // AudioSpecificInfo follows

            // oooo offf fccc c000
            // o - audioObjectType
            // f - samplingFreqIndex
            // c - channelConfig
            , 0, 0
    };

    int size = sizeof(kStaticESDS);
    kStaticESDS[size - 2] = ((profile + 1) << 3) | (sampling_freq_index >> 1);

    kStaticESDS[size - 1] = ((sampling_freq_index << 7) & 0x80)
                                | (channel_configuration << 3);

    meta->setData(kKeyESDS, 0, kStaticESDS, size);
    return meta;
}

static sp<MetaData> MakeAACCodecSpecificData(const uint8_t *config, size_t config_size)
{
    if (config_size != 2) {
        LOGV("Not correct config size for aac codec");
        return NULL;
    }

    unsigned profile = (config[0] >> 3) - 1;

    unsigned sf_index = ((config[0] & 7) << 1) | (config[1] >> 7);

    unsigned channel = (config[1] >> 3) & 0x0f;

    LOGV("MakeAACCodecSpecificData %d %d %d", int(profile), int(sf_index), int(channel));

    return MakeAACCodecSpecificData(profile, sf_index, channel);
}

static size_t getFrameSize(int32_t colorFormat, int32_t width, int32_t height)
{
    switch (colorFormat) {
    case OMX_COLOR_FormatYCbYCr:
    case OMX_COLOR_FormatCbYCrY:
        return width * height * 2;
    case OMX_COLOR_FormatYUV420Planar:
    case OMX_COLOR_FormatYUV420SemiPlanar:
    case OMX_QCOM_COLOR_FormatYVU420SemiPlanar:
    case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
    case COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced:
    case OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB:
    case OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka:
    case QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka:
        return (width * height * 3) / 2;
    default:
        LOGE("Should not be here. Unsupported color format.");
        break;
    }
    return width * height * 4;
}

OMX_U32 getColorFormatForHWCodec(const sp<IOMX>& omx, const char* szMimeType)
{
    Vector<CodecCapabilities> results;

    CHECK_EQ(QueryCodecs(omx, szMimeType, true, true, &results), (status_t) OK);

    if (results.size() == 0)
        return OMX_COLOR_FormatYUV420SemiPlanar;

    for (size_t c = 0; c < results.size(); ++c) {
        for (size_t f = 0; f < results[c].mColorFormats.size(); ++f) {
            const OMX_U32 colorFormat = results[c].mColorFormats[f];
            if (colorFormat == OMX_COLOR_FormatYUV420Planar ||
                    colorFormat == OMX_COLOR_FormatYUV420SemiPlanar ||
                    colorFormat == OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB ||
                    colorFormat == OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka ||
                    colorFormat == QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka ||
                    colorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanar)
                return colorFormat;
        }
    }

    return results[0].mColorFormats.size() == 0 ? OMX_COLOR_FormatYUV420SemiPlanar :
            results[0].mColorFormats[0];
}

static void dumpCodecColorFormat(int32_t colorFormat)
{
    if (colorFormat == OMX_COLOR_FormatCbYCrY)
        LOGV("Decoder use OMX_COLOR_FormatCbYCrY (0x%x)", colorFormat);
    else if (colorFormat == OMX_COLOR_FormatYUV420Planar)
        LOGV("Decoder use OMX_COLOR_FormatYUV420Planar (0x%x)", colorFormat);
    else if (colorFormat == OMX_COLOR_FormatYUV420SemiPlanar)
        LOGV("Decoder use OMX_COLOR_FormatYUV420SemiPlanar (0x%x)", colorFormat);
    else if (colorFormat == OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka)
        LOGV("Decoder use OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka (0x%x)", colorFormat);
    else if (colorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanar)
        LOGV("Decoder use OMX_QCOM_COLOR_FormatYVU420SemiPlanar (0x%x)", colorFormat);
    else if (colorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar)
        LOGV("Decoder use OMX_TI_COLOR_FormatYUV420PackedSemiPlanar (0x%x)", colorFormat);
    else if (colorFormat == OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB)
        LOGV("Decoder use OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB (0x%x)", colorFormat);
    else if (colorFormat == QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka)
        LOGV("Decoder use QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka (0x%x)", colorFormat);
    else if (colorFormat == COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced)
        LOGV("Decoder use COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced (0x%x)", colorFormat);
    else if (colorFormat == OMX_DIRECT_RENDERING)
        LOGV("Decoder use OMX_DIRECT_RENDERING (0x%x)", colorFormat);
    else
        LOGE("Decoder unknown color format! (0x%x)", colorFormat);
}

#if defined(DEBUG_CODEC)
static void dumpFrameToFile(const void *data, size_t size)
{
    static bool b_dumped = false;
    if (b_dumped || !data || size <= 0)
        return;

    if (FILE *out = fopen("/mnt/sdcard/YUV.bin", "wb")) {
        fwrite(data, 1, size, out);
        fclose(out);
        b_dumped = true;
    }
}

static void dumpCodecProfiles(const sp<IOMX>& omx, bool queryDecoders)
{
    const char *kMimeTypes[] = {
            MEDIA_MIMETYPE_VIDEO_AVC,
            MEDIA_MIMETYPE_VIDEO_MPEG4,
            MEDIA_MIMETYPE_VIDEO_H263,
            MEDIA_MIMETYPE_AUDIO_AAC
    };

    const char *codecType = queryDecoders ? "decoder" : "encoder";
    LOGI("%s profiles:\n", codecType);

    for (size_t k = 0; k < sizeof(kMimeTypes) / sizeof(kMimeTypes[0]); ++k) {
        LOGI("type '%s':\n", kMimeTypes[k]);

        Vector<CodecCapabilities> results;
        // will retrieve hardware and software codecs
        CHECK_EQ(QueryCodecs(omx, kMimeTypes[k], queryDecoders, &results),
                (status_t) OK);

        for (size_t i = 0; i < results.size(); ++i) {
            LOGI("  %s '%s' supports profile levels:", codecType, results[i].mComponentName.string());
            for (size_t j = 0; j < results[i].mProfileLevels.size(); ++j) {
                const CodecProfileLevel &profileLevel =
                        results[i].mProfileLevels[j];
                LOGI("%s%ld/%ld", j > 0 ? ", " : "",
                        profileLevel.mProfile, profileLevel.mLevel);
            }
            LOGI("ColorFormats : ");
            for (size_t j = 0; j < results[i].mColorFormats.size(); ++j) {
                const OMX_U32 colorFormat = results[i].mColorFormats[j];
                LOGI("%s%d(0x%x)", j > 0 ? ", " : "", (uint32_t)colorFormat, (uint32_t)colorFormat);
            }
        }
    }
}

struct MetaDataPublic : public RefBase {
    virtual ~MetaDataPublic() {}

    struct typed_data {
        uint32_t mType;
        size_t mSize;

        union {
            void *ext_data;
            float reservoir;
        } u;
    };

    KeyedVector<uint32_t, typed_data> mItems;
};

void dumpMetaData(MetaData *md_private)
{
    if (!md_private)
        return;

    MetaDataPublic *md = reinterpret_cast<MetaDataPublic*>(md_private);
    size_t size = md->mItems.size();
    LOGV("[Decoder] dumpMetaData, size=%d", size);
    for (int i = 0; i < size; i++) {
        uint32_t key = ntoh4(md->mItems.keyAt(i));
        MetaDataPublic::typed_data data = md->mItems.valueAt(i);
        uint32_t type = ntoh4(data.mType);
        LOGV("[Decoder]   key %4.4s data: type %4.4s size %d value %d(%#x)", (char* )&key,
                (char* )&type, data.mSize, (int)data.u.ext_data, (int)data.u.ext_data);
    }
}
#else
#define dumpMetaData(md_private)
#define dumpCodecProfiles(A,B)
#endif

class Frame {
public:
    Frame()
        : mStatus(OK)
        , mPts(0)
        , mSize(0)
        , mBuffer(0)
        , mMediaBuffer(0)
        , mFlags(0)
    {
    }

    explicit Frame(status_t status, uint8_t* data, size_t size, int64_t pts, uint32_t flags)
        : mStatus(status)
        , mPts(pts)
        , mSize(size)
        , mBuffer(0)
        , mMediaBuffer(0)
        , mFlags(flags)
    {
        if (size > 0 && data) {
            mBuffer = new uint8_t[size];
            memcpy(mBuffer, data, size);
        }
    }

    explicit Frame(status_t status, MediaBuffer* mediaBuffer, int64_t pts, uint32_t flags)
        : mStatus(status)
        , mPts(pts)
        , mSize(0)
        , mBuffer(0)
        , mMediaBuffer(mediaBuffer)
        , mFlags(flags)
    {
    }

    ~Frame()
    {
        clearBuffers(NULL);
    }

    Frame(const Frame& other)
        : mStatus(OK)
        , mPts(0)
        , mSize(0)
        , mBuffer(0)
        , mMediaBuffer(0)
        , mFlags(0)
    {
        *this = other;
    }

    Frame& operator=(const Frame& other)
    {
        if (this != &other) {
            int32_t oldSize = mSize;

            mStatus = other.mStatus;
            mSize = other.mSize;
            mPts = other.mPts;
            mFlags = other.mFlags;
            mMediaBuffer = other.mMediaBuffer;

            if (oldSize > 0 && mSize <= oldSize && mBuffer) {
                // reuse frame allocated memory
                memcpy(mBuffer, other.mBuffer, mSize);
            } else {
                if (mBuffer) {
                    delete[] mBuffer;
                    mBuffer = NULL;
                }
                if (mSize > 0 && other.mBuffer) {
                    mBuffer = new uint8_t[mSize];
                    memcpy(mBuffer, other.mBuffer, mSize);
                }
            }
        }
        return *this;
    }

    void swap(Frame& other)
    {
        if (this != &other) {
            status_t status = mStatus;
            int64_t pts = mPts;
            int size = mSize;
            uint8_t* buffer = mBuffer;
            uint32_t flags = mFlags;
            MediaBuffer* mediaBuffer = mMediaBuffer;

            mStatus = other.mStatus;
            mPts = other.mPts;
            mSize = other.mSize;
            mBuffer = other.mBuffer;
            mFlags = other.mFlags;
            mMediaBuffer = other.mMediaBuffer;

            other.mStatus = status;
            other.mPts = pts;
            other.mSize = size;
            other.mBuffer = buffer;
            other.mFlags = flags;
            other.mMediaBuffer = mediaBuffer;
        }
    }

    bool empty() const { return (!mBuffer && !mMediaBuffer); }

    void clearBuffers(MediaBufferQueue* mediaQueue)
    {
        if (mBuffer) {
            delete[] mBuffer;
            mBuffer = 0;
            mSize = 0;
        }
        if (mMediaBuffer) {
            if (mediaQueue)
                mediaQueue->push_back(mMediaBuffer);
            else {
                LOGV("[Frame] clearBuffers buffer=%p, refs=%d", mMediaBuffer, mMediaBuffer->refcount());
                mMediaBuffer->release();
            }
            mMediaBuffer = 0;
        }
    }

    status_t mStatus;
    int64_t mPts;
    int mSize;
    uint8_t* mBuffer;
    uint32_t mFlags;
    MediaBuffer* mMediaBuffer;
};

static volatile bool s_windowConnected = false;
class NativeWindowRenderer : public RefBase {
public:
    NativeWindowRenderer(const sp<ANativeWindow>& nativeWindow)
        : mNativeWindow(nativeWindow)
        , mColorFormat(OMX_COLOR_FormatUnused)
        , mWidth(0)
        , mHeight(0)
        , mCropLeft(0)
        , mCropTop(0)
        , mCropRight(0)
        , mCropBottom(0)
        , mCropWidth(0)
        , mCropHeight(0)
        , mFenceFd(-1)
        , mSoftwareRendering(false)
    {
        LOG_DEBUG;
        connectWindow();
    }

    virtual ~NativeWindowRenderer()
    {
        LOG_DEBUG;
        disconnectWindow();
    }

    void connectWindow()
    {
        if (!s_windowConnected) {
            if (mNativeWindow != 0) {
                LOGI("[NativeWindowRenderer] connect window!");
                int err = native_window_api_connect(mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
                LOGR(err,"[Decoder] native_window_api_connect: %d", err);
                err = native_window_set_scaling_mode(mNativeWindow.get(), NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
                LOGR(err,"[Decoder] native_window_set_scaling_mode: %d", err);
            }
            s_windowConnected = true;
        }
    }

    void disconnectWindow()
    {
        if (mNativeWindow != 0 && s_windowConnected) {
            LOGI("[NativeWindowRenderer] disconnect window!");
            native_window_api_disconnect(mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
            s_windowConnected = false;
        }
    }

    void init(const sp<MetaData>& meta)
    {
        LOG_DEBUG;
        mSoftwareRendering = true;

        CHECK(meta->findInt32(kKeyColorFormat, &mColorFormat));
        CHECK(meta->findInt32(kKeyWidth, &mWidth));
        CHECK(meta->findInt32(kKeyHeight, &mHeight));

        if (!meta->findRect(kKeyCropRect, &mCropLeft, &mCropTop, &mCropRight,
                &mCropBottom)) {
            mCropLeft = mCropTop = 0;
            mCropRight = mWidth - 1;
            mCropBottom = mHeight - 1;
        }

        mCropWidth = mCropRight - mCropLeft + 1;
        mCropHeight = mCropBottom - mCropTop + 1;

        int32_t rotationDegrees;
        if (!meta->findInt32(kKeyRotation, &rotationDegrees)) {
            rotationDegrees = 0;
        }

        int halFormat;
        size_t bufWidth, bufHeight;

        switch (mColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar: {
            halFormat = HAL_PIXEL_FORMAT_YV12;
            bufWidth = (mCropWidth + 1) & ~1;
            bufHeight = (mCropHeight + 1) & ~1;
            break;
        }
        case OMX_STE_COLOR_FormatYUV420PackedSemiPlanarMB:
            halFormat = HAL_PIXEL_FORMAT_YCBCR42XMBN;
            break;
        default:
            LOGE("[NativeWindowRenderer] ERROR: color convertor isn't implemented(%x)!", mColorFormat);
            halFormat = mColorFormat;
            break;
        }

        bufWidth = (mCropWidth + 1) & ~1;
        bufHeight = (mCropHeight + 1) & ~1;

        if (halFormat == HAL_PIXEL_FORMAT_YCBCR42XMBN) {
            bufWidth = mWidth;
            bufHeight = mHeight;
        }

        LOGV("[NativeWindowRenderer] INIT: w=%d, h=%d, buf_w=%d, buf_h=%d, CROP: top=%d, w=%d, h=%d",
                mWidth, mHeight, bufWidth, bufHeight, mCropTop, mCropWidth, mCropHeight);

        CHECK(mNativeWindow != NULL);
        CHECK(mCropWidth > 0);
        CHECK(mCropHeight > 0);

        CHECK_EQ(0, native_window_set_usage(mNativeWindow.get(),
                        GRALLOC_USAGE_SW_READ_NEVER
                        | GRALLOC_USAGE_SW_WRITE_OFTEN
                        | GRALLOC_USAGE_HW_TEXTURE
                        | GRALLOC_USAGE_EXTERNAL_DISP));

        // Width must be multiple of 32???
#if defined(ANDROID_LL)
        CHECK_EQ(0, native_window_set_buffers_format(mNativeWindow.get(), halFormat));
        CHECK_EQ(0, native_window_set_buffers_user_dimensions(mNativeWindow.get(), bufWidth, bufHeight));
#else
        CHECK_EQ(0, native_window_set_buffers_geometry(mNativeWindow.get(), bufWidth, bufHeight, halFormat));
#endif
        applyRotation(rotationDegrees);
    }

    void render(const uint8_t* data, size_t size)
    {
        if (!data || size == 0)
            return;

        ANativeWindowBuffer* anb = NULL;
#if !defined(ANDROID_ICS)
        status_t err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &anb, &mFenceFd);
#else
        status_t err = mNativeWindow->dequeueBuffer(mNativeWindow.get(), &anb);
#endif
        if (err != NO_ERROR || !anb) {
            LOGE("[NativeWindowRenderer] ERROR: couldn't get video buffer(%d)!", err);
            return;
        }

        uint8_t* img = NULL;
        sp<GraphicBuffer> buf(new GraphicBuffer(anb, false));

        buf->lock(GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN, (void**) (&img));
        // http://stackoverflow.com/questions/10059738/qomx-color-formatyuv420packedsemiplanar64x32tile2m8ka-color-format
        if (img) {
            switch (mColorFormat) {
            case OMX_COLOR_FormatYUV420Planar: {
                convertYUV420Planar_to_YV12(img, anb, data);
                break;
            }
            case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar: {
                convertYUV420PackedSemiPlanar_to_YV12(img, anb, data);
                break;
            }
            default:
                memcpy(img, data, size);
                break;
            }
        }
        buf->unlock();

#if !defined(ANDROID_ICS)
        mNativeWindow->queueBuffer(mNativeWindow.get(), buf->getNativeBuffer(), mFenceFd);
#else
        mNativeWindow->queueBuffer(mNativeWindow.get(), buf->getNativeBuffer());
#endif
    }

    void render(MediaBuffer* buffer, int64_t timeUs)
    {
        if (!buffer || mNativeWindow == 0) {
            LOGW("[NativeWindowRenderer] render: skip frame buffer=%p", buffer);
            return;
        }

        native_window_set_buffers_timestamp(mNativeWindow.get(), timeUs * 1000);

#if !defined(ANDROID_ICS)
        status_t err = mNativeWindow->queueBuffer(mNativeWindow.get(), buffer->graphicBuffer().get(), mFenceFd);
#else
        status_t err = mNativeWindow->queueBuffer(mNativeWindow.get(), buffer->graphicBuffer().get());
#endif
        if (err != 0) {
            LOGE("[NativeWindowRenderer] queueBuffer failed with error %s (%d)", strerror(-err), -err);
            return;
        }
        sp<MetaData> metaData = buffer->meta_data();
        metaData->setInt32(kKeyRendered, 1);
    }

    ANativeWindow* window() const { return mNativeWindow != 0 ? mNativeWindow.get() : 0; }
    bool isSWRenderng() const { return mSoftwareRendering; }

private:
    NativeWindowRenderer(const NativeWindowRenderer&);
    NativeWindowRenderer &operator=(const NativeWindowRenderer&);

    void convertYUV420Planar_to_YV12(uint8_t* dst, ANativeWindowBuffer* buf,
            const uint8_t *data)
    {
        const uint8_t *src_y = (const uint8_t *) data;
        const uint8_t *src_u = (const uint8_t *) data + mWidth * mHeight;
        const uint8_t *src_v = src_u + (mWidth / 2 * mHeight / 2);

        uint8_t *dst_y = (uint8_t *) dst;
        size_t dst_y_size = buf->stride * buf->height;
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;

        for (int y = 0; y < mCropHeight; ++y) {
            memcpy(dst_y, src_y, mCropWidth);

            src_y += mWidth;
            dst_y += buf->stride;
        }

        for (int y = 0; y < (mCropHeight + 1) / 2; ++y) {
            memcpy(dst_u, src_u, (mCropWidth + 1) / 2);
            memcpy(dst_v, src_v, (mCropWidth + 1) / 2);

            src_u += mWidth / 2;
            src_v += mWidth / 2;
            dst_u += dst_c_stride;
            dst_v += dst_c_stride;
        }
    }

    void convertYUV420PackedSemiPlanar_to_YV12(uint8_t* dst,
            ANativeWindowBuffer* buf, const uint8_t *data)
    {
        const uint8_t *src_y = (const uint8_t *) data;
        const uint8_t *src_uv = (const uint8_t *) data
                + mWidth * (mHeight - mCropTop / 2);
        uint8_t *dst_y = (uint8_t *) dst;

        size_t dst_y_size = buf->stride * buf->height;
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;

        for (int y = 0; y < mCropHeight; ++y) {
            memcpy(dst_y, src_y, mCropWidth);

            src_y += mWidth;
            dst_y += buf->stride;
        }

        for (int y = 0; y < (mCropHeight + 1) / 2; ++y) {
            size_t tmp = (mCropWidth + 1) / 2;
            for (size_t x = 0; x < tmp; ++x) {
                dst_u[x] = src_uv[2 * x];
                dst_v[x] = src_uv[2 * x + 1];
            }

            src_uv += mWidth;
            dst_u += dst_c_stride;
            dst_v += dst_c_stride;
        }
    }

    int ALIGN(int x, int y)
    {
        // y must be a power of 2.
        return (x + y - 1) & ~(y - 1);
    }

    void applyRotation(int32_t rotationDegrees)
    {
        LOG_DEBUG;
        uint32_t transform;
        switch (rotationDegrees) {
        case 0:
            transform = 0;
            break;
        case 90:
            transform = HAL_TRANSFORM_ROT_90;
            break;
        case 180:
            transform = HAL_TRANSFORM_ROT_180;
            break;
        case 270:
            transform = HAL_TRANSFORM_ROT_270;
            break;
        default:
            transform = 0;
            break;
        }

        if (transform) {
            CHECK_EQ(0, native_window_set_buffers_transform(mNativeWindow.get(), transform));
        }
    }

    sp<ANativeWindow> mNativeWindow;

    int32_t mColorFormat;
    int32_t mWidth, mHeight;
    int32_t mCropLeft, mCropTop, mCropRight, mCropBottom;
    int32_t mCropWidth, mCropHeight;
    int32_t mFenceFd;
    bool mSoftwareRendering;
};

class Decoder;
class StagefrightContext;
class MediaStreamSource : public MediaSource {
public:
    MediaStreamSource(Decoder* decoder, sp<MetaData>& meta)
        : mFrameSize(0)
        , mSourceType(SOURCE_UNKNOWN)
        , mDecoder(decoder)
    {
        LOG_DEBUG;
        setFormat(meta);
    }

    virtual ~MediaStreamSource()
    {
        LOG_DEBUG;
    }

    virtual sp<MetaData> getFormat() { return mSourceMeta; }
    virtual status_t start(MetaData* params) {
        dumpMetaData(params);
        return OK;
    }
    virtual status_t stop() { LOG_DEBUG; return OK; }
    virtual status_t read(MediaBuffer** buffer,
            const MediaSource::ReadOptions* options);

    int32_t getFrameSize() const { return mFrameSize; }

    void setColorFormat(OMX_U32 colorFormat) {
        mSourceMeta->setInt32(kKeyColorFormat, colorFormat);
    }

    void setFormat(const sp<MetaData>& meta)
    {
        LOG_DEBUG;
        const char* mime = 0;
        if (meta->findCString(kKeyMIMEType, &mime)) {
            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
                mSourceMeta = meta;
                mSourceType = SOURCE_AAC;
                mBufferGroup.add_buffer(new MediaBuffer(AAC_MAX_FRAME_SIZE));
                return;
            }
        }
        int32_t width, height, colorFormat;
        if (meta->findInt32(kKeyWidth, &width)
                && meta->findInt32(kKeyHeight, &height)
                && meta->findInt32(kKeyColorFormat, &colorFormat)) {

            dumpCodecColorFormat(colorFormat);

            mSourceMeta = meta;
            mFrameSize = ::getFrameSize(colorFormat, width, height);

            MediaBuffer* buffer = NULL;
#if 0
            mBufferGroup.acquire_buffer(&buffer);
            if (buffer) {
                buffer->release();
                buffer = NULL;
            }
#endif
            if (mFrameSize > 0) {
                buffer = new MediaBuffer(mFrameSize);
                mBufferGroup.add_buffer(buffer);
            }
        }
        if (mime) {
            if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
                mSourceType = SOURCE_AVC;
            } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4)) {
                mSourceType = SOURCE_MPEG4;
            } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_H263)) {
                mSourceType = SOURCE_H263;
            }
        }
        LOGV("[MediaStreamSource] frameSize=%d, sourceType=%d", mFrameSize, mSourceType);
    }

private:
    bool isIDRFrame(const uint8_t* data, size_t size)
    {
        if (mSourceType == SOURCE_AAC) {
            return false;
        } else if (mSourceType == SOURCE_AVC) {
            for (size_t i = 0; i + 3 < size && i < 60; ++i) {
                if (!memcmp("\x00\x00\x01", &data[i], 3)) {
                    uint8_t nalType = data[i + 3] & 0x1f;
                    if (nalType == 5) {
                        return true;
                    }
                }
            }
        } else if (mSourceType == SOURCE_MPEG4) {
            LOGW("[MediaStreamSource] NOT IMPLEMENTED: sync frame detection not implemented yet for MPEG4");
        } else if (mSourceType == SOURCE_H263) {
            LOGW("[MediaStreamSource] NOT IMPLEMENTED: sync frame detection not implemented yet for H.263");
        }
        return false;
    }

    enum SourceType {
        SOURCE_AVC,
        SOURCE_MPEG4,
        SOURCE_H263,
        SOURCE_AAC,
        SOURCE_UNKNOWN,
    };

    int mFrameSize;
    MediaBufferGroup mBufferGroup;
    sp<MetaData> mSourceMeta;
    SourceType mSourceType;

    Decoder* mDecoder;
};

class BufferQueue {
public:
    explicit BufferQueue(const int32_t capacity = OUT_BUFFER_COUNT)
    {
        mElements.setCapacity(capacity);
        DataElement element;
        for (int32_t i = 0; i < capacity; ++i)
            mElements.push(element);
        mCount = 0;
    }

    virtual ~BufferQueue() { clearAll(); }

    void release()
    {
        mHoldCondition.signal();
    }

    int32_t push(Frame& data, bool wait = false)
    {
        int64_t startTime = getTimestampMs();
        AutoMutex lock(mLock);
        while (isFull())
            mNotFull.wait(mLock);

        int32_t index = 0;
        for (ElementIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->mStatus == FREE) {
                it->mData.swap(data);
                it->mStatus = mCount++;
                index = static_cast<int32_t>(it - mElements.begin());

                if (wait) {
                    mHoldCondition.wait(mLock);
                }
                return index;
            }
        }
        return INFO_TRY_AGAIN_LATER;
    }

    void pull(Frame& data, int32_t index)
    {
        if (index < 0 || index > mElements.size())
            return;

        AutoMutex lock(mLock);
        ElementIter it = mElements.begin() + index;

        if (it->mStatus != HOLDED) {
            LOGW("[BufferQueue] not holded frame %d", index);
            return;
        }
        data.swap(it->mData);
        it->mStatus = FREE;

        mNotFull.signal();
    }

    void get(MediaBuffer*& mediaBuffer, int32_t index)
    {
        if (index < 0 || index > mElements.size())
            return;

        AutoMutex lock(mLock);
        ElementIter it = mElements.begin() + index;

        if (it->mStatus != HOLDED) {
            LOGW("[BufferQueue] not holded frame %d ", index);
            mediaBuffer = NULL;
            return;
        }
        mediaBuffer = it->mData.mMediaBuffer;
    }

    void free(int32_t index)
    {
        if (index < 0 || index > mElements.size())
            return;

        AutoMutex lock(mLock);
        ElementIter it = mElements.begin() + index;
        if (it->mStatus != HOLDED) {
            LOGW("[BufferQueue] not holded frame %d ", index);
            return;
        }

        it->mData.clearBuffers(&mMediaQueue);
        it->mStatus = FREE;

        mNotFull.signal();
    }

    void clearBuffer(int32_t index)
    {
        if (index < 0 || index > mElements.size())
            return;

        MediaBufferQueue mediaQueue;
        { //scoped lock
            AutoMutex lock(mLock);
            (mElements.begin() + index)->mData.clearBuffers(&mMediaQueue);
            mediaQueue.appendVector(mMediaQueue);
            mMediaQueue.clear();
        }

        releaseMediaBufferQueue(mediaQueue);
    }

    int32_t holdNext(Frame& data, uint8_t** buffer = NULL, size_t* size = NULL)
    {
        AutoMutex lock(mLock);
        uint32_t min = FREE;
        ElementIter next = mElements.end();
        for (ElementIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->mStatus < min) {
                min = it->mStatus;
                next = it;
            }
        }

        if (next == mElements.end())
            return INFO_TRY_AGAIN_LATER;

        // copy only metadata, and mark as holded
        data.mStatus = next->mData.mStatus;
        data.mPts = next->mData.mPts;
        if (next->mData.mMediaBuffer) {
            if (buffer)
                *buffer = reinterpret_cast<uint8_t*>(next->mData.mMediaBuffer);
        } else {
            if (buffer)
                *buffer = next->mData.mBuffer;
        }
        if (size)
            *size = next->mData.mSize;
        next->mStatus = HOLDED;
        int32_t index = static_cast<int32_t>(next - mElements.begin());

        mHoldCondition.signal();
        return index;
    }

    size_t size() const
    {
        AutoMutex lock(mLock);
        size_t count = 0;
        for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->getStatus() != FREE)
                count++;
        }
        return count;
    }

    size_t filledCount() const
    {
        AutoMutex lock(mLock);
        size_t count = 0;
        for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (!it->mData.empty())
                count++;
        }
        return count;
    }

    size_t readyCount() const
    {
        size_t count = 0;
        AutoMutex lock(mLock);
        for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->getStatus() != FREE && it->getStatus() != HOLDED)
                count++;
        }
        return count;
    }

    size_t tryGetReadyCount() const
    {
        if (mLock.tryLock() != OK)
            return -1;

        size_t count = 0;
        for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->getStatus() != FREE && it->getStatus() != HOLDED)
                count++;
        }
        mLock.unlock();
        return count;
    }

    size_t capacity() const
    {
        AutoMutex lock(mLock);
        return mElements.size();
    }

    void clearAll()
    {
        MediaBufferQueue mediaQueue;
        { //scoped lock
            AutoMutex lock(mLock);
            for (ElementIter it = mElements.begin(); it != mElements.end(); ++it) {
                it->mData.clearBuffers(&mMediaQueue);
                if (it->mStatus != HOLDED)
                    it->mStatus = FREE;
            }
            mediaQueue.appendVector(mMediaQueue);
            mMediaQueue.clear();
        }
        releaseMediaBufferQueue(mediaQueue);
    }

    void releaseBuffers()
    {
        MediaBufferQueue mediaQueue;
        { // scoped lock
            AutoMutex lock(mLock);
            mediaQueue.appendVector(mMediaQueue);
            mMediaQueue.clear();
        }
        releaseMediaBufferQueue(mediaQueue);
    }

    size_t waitRelease(int32_t waitTime)
    {
        int64_t startTime = getTimestampMs();
        size_t count = 0;
        MediaBufferQueue mediaQueue;
        { // scoped lock
            AutoMutex lock(mLock);
            for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
                if (!it->mData.empty())
                    count++;
            }

            while (count >= MAX_HOLDED_FRAMES) {
                int sleep = waitTime - getPeriodMs(startTime);

                if (sleep <= 0)
                    break;

                count = 0;
                mNotFull.waitRelative(mLock, sleep * 1000000);
                for (ElementIter it = mElements.begin(); it != mElements.end(); ++it) {
                    if (!it->mData.empty())
                        count++;
                }
            }
            mediaQueue.appendVector(mMediaQueue);
            mMediaQueue.clear();
        }
        releaseMediaBufferQueue(mediaQueue);
        return count;
    }

private:
    bool isFull() const
    {
        for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->mStatus == FREE)
                return false;
        }
        return true;
    }

    bool empty() const
    {
        for (ElementConstIter it = mElements.begin(); it != mElements.end(); ++it) {
            if (it->mStatus != FREE)
                return false;
        }
        return true;
    }

    enum { FREE = 0xFFFFFFF0, HOLDED };

    struct DataElement {
        DataElement()
            : mStatus(FREE)
        {}
        Frame mData;
        uint32_t mStatus;

        uint32_t getStatus() const { return mStatus; }
    };

    typedef Vector<DataElement> Elements;
    typedef Elements::iterator ElementIter;
    typedef Elements::const_iterator ElementConstIter;

    MediaBufferQueue mMediaQueue;
    Elements mElements;
    uint32_t mCount;

    mutable Mutex mLock;
    Condition mNotFull;
    Condition mHoldCondition;
};

class Decoder : public Thread {
public:
    Decoder()
        : Thread(false)
        , mTrack(0)
        , mDecoderSource(0)
        , mRenderer(0)
        , mInterrupted(false)
        , mFlushNeeded(false)
        , mNeedSkip(false)
        , mDecoderFlags(OMXCodec::kHardwareCodecsOnly)
        , mVideoWidth(0)
        , mVideoHeight(0)
        , mVideoColorFormat(0)
        , mVideoStride(0)
        , mVideoSliceHeight(0)
        , mVideoCropLeft(0)
        , mVideoCropRight(0)
        , mVideoCropBottom(0)
        , mVideoCropTop(0)
        , mVideoRotation(0)
        , mOutQueue(OUT_BUFFER_COUNT)
        , mSampleRate(0)
        , mChannelCount()
        , mIsVideoDecoder(true)
        , mDelayedOpen(false)
    {
#if defined(ANDROID_ICS)
        LOGI("[Decoder] (%p) Decoder for ICS", this);
#elif defined(ANDROID_JBMR2)
        LOGI("[Decoder] (%p) Decoder for JBMR2", this);
#elif defined(ANDROID_KK)
        LOGI("[Decoder] (%p) Decoder for KK", this);
#elif defined(ANDROID_LL)
        LOGI("[Decoder] (%p) Decoder for LL", this);
#endif
    }

    virtual ~Decoder() { LOGI("[Decoder] (%p) ~Decoder!", this); }

    bool configure(void* nativeWindow, int w, int h, void *p_extra, int i_extra);
    bool createDecoderByType(sp<IOMX>& iomx, const char* type);
    bool createDecoder(sp<IOMX>& iomx, uint8_t* data, size_t size);
    void release();
    void releaseOutputBuffer(uint32_t index, int64_t pts)
    {
        if (pts < 0 || mRenderer == 0) {
            mOutQueue.free(index);
            return;
        }

        if (mRenderer->isSWRenderng()) {
            Frame frame;
            mOutQueue.pull(frame, index);

            mRenderer->render(frame.mBuffer, frame.mSize);
        } else {
            MediaBuffer* mediaBuffer = 0;
            mOutQueue.get(mediaBuffer, index);

            mRenderer->render(mediaBuffer, pts);

            mOutQueue.free(index);
        }
    }

    const char* getName() const
    {
        AutoMutex lock(mLock);
        return mComponentName.string();
    }

    int32_t outputBufferCount() const
    {
        AutoMutex lock(mLock);
        return mOutQueue.readyCount();
    }

    int waitReadOrOutput(size_t& readyCount, int waitMs)
    {
        int res = TIMED_OUT;
        int64_t startTime = getTimestampMs();
        while (true) {
            readyCount = mOutQueue.tryGetReadyCount();
            if (readyCount > 0)
                break;

            int sleep = waitMs - getPeriodMs(startTime);

            if (sleep <= 0)
                break;

            if (sleep > s_frameDisplayTimeMsec / 4)
                sleep = s_frameDisplayTimeMsec / 4;

            res = mReadCondition.waitRelative(mInLock, sleep * 1000000);
            if (res == OK)
                break;
        }
        return res;
    }

    bool queueInputBuffer(int32_t index, uint8_t* data, size_t size,
            int64_t pts, uint32_t flags)
    {
        LOG_DEBUG;

        if (mRenderer != 0) {
            mRenderer->connectWindow();
        }

        int64_t startTime = getTimestampMs();
        bool result = false;
        int sleep = 2 * s_frameDisplayTimeMsec + 5;
        int32_t queueSize;
        size_t readyCount;
        status_t status = OK;
        if ((flags & OMX_BUFFERFLAG_ENDOFFRAME))
            status =  INFO_DISCONTINUITY;

        Frame frame(status, data, size, pts, flags);

        AutoMutex lock(mInLock);
        queueSize = mInQueue.size();

        if (mDecoderSource == 0) {
            if (queueSize < 50) {
                mInQueue.push_back(frame);
                return true;
            } else {
                return false;
            }
        }

        if (queueSize > IN_BUFFER_COUNT) {
            mNeedSkip = true;
            while (queueSize >= IN_BUFFER_COUNT) {
                waitReadOrOutput(readyCount, sleep);
                if (readyCount > 0) {
                    mInQueue.push_back(frame);
                    mInCondition.signal();
                    return true;
                }
                queueSize = mInQueue.size();

                if (queueSize < IN_BUFFER_COUNT) {
                    sleep = 0;
                    break;
                }
            }
        }

        mNeedSkip = false;

        if (queueSize == IN_BUFFER_COUNT) {
            int res = waitReadOrOutput(readyCount, sleep);
            if (res != OK && readyCount > 0) {
                return false;
            }
            queueSize = mInQueue.size();
            sleep = 0;
        }

        if (queueSize < IN_BUFFER_COUNT) {
            mInQueue.push_back(frame);
            if (queueSize + 1 < IN_BUFFER_COUNT)
                sleep = (queueSize + 1) * 25;
            mInCondition.signal();
            result = true;
        }

        waitReadOrOutput(readyCount, sleep);

        return result;
    }

    int32_t dequeueInputBuffer(int64_t timeoutUs)
    {
        LOG_DEBUG;
        if (timeoutUs > 0)
            usleep(timeoutUs);

        AutoMutex lock(mInLock);
        if (mInQueue.size() < IN_BUFFER_COUNT) {
            return 1; //ok?
        }
        return INFO_TRY_AGAIN_LATER;
    }

    int32_t dequeueOutputBuffer(uint8_t** data, size_t* size, int64_t* pts)
    {
        LOG_DEBUG;
        bool directRendering = false;
        { //scopped lock
            AutoMutex lock(mLock);
            directRendering = mRenderer != 0 ? true : false;
        }

        int32_t index = INFO_TRY_AGAIN_LATER;

        Frame frame;
        status_t status = OK;
        if (directRendering) {
            index = mOutQueue.holdNext(frame, data);
            status = frame.mStatus;

            if (INFO_FORMAT_CHANGED == status)
                mOutQueue.pull(frame, index);

            *pts = frame.mPts;
            *size = 0;
        } else {
            //TODO: NOT IMPLEMENTED: copy decoded YUV frame data and size
            index = mOutQueue.holdNext(frame, data, size);
            status = frame.mStatus;
            *pts = frame.mPts;
        }

        if (status == ERROR_END_OF_STREAM) {
            return INFO_OUTPUT_END_OF_STREAM;
        } else if (status == INFO_FORMAT_CHANGED) {
            return INFO_OUTPUT_FORMAT_CHANGED;
        }
        return index;
    }

    void getOutputFormat(void* formatIn)
    {
        if (!formatIn)
            return;

        AutoMutex lock(mLock);
        if (mIsVideoDecoder) {
            source_video_format_t* format = static_cast<source_video_format_t*>(formatIn);
            format->pixel_format = mVideoColorFormat;
            format->stride = mVideoStride;
            format->slice_height = mVideoSliceHeight;
            format->crop_top = mVideoCropTop;
            format->crop_bottom = mVideoCropBottom;
            format->crop_left = mVideoCropLeft;
            format->crop_right = mVideoCropRight;
            format->width = mVideoWidth;
            format->height = mVideoHeight;
        } else {
            source_audio_format_t* format = static_cast<source_audio_format_t*>(formatIn);
            format->sample_rate = mSampleRate;
            format->channel_count = mChannelCount;
        }
    }

    int32_t getOutputBuffers() const { return mOutQueue.capacity(); }

    void flush()
    {
        mFlushNeeded = true;
        signalEOF();
    }

    status_t waitAndPopInputBuffer(Frame& frame)
    {
        LOG_DEBUG;
        AutoMutex lock(mInLock);

        while (mInQueue.empty() && !mInterrupted)
            mInCondition.wait(mInLock);

        if (!mInQueue.empty()) {
            frame.swap(*mInQueue.begin());
            mInQueue.erase(mInQueue.begin());

            mReadCondition.signal();
        }
        return frame.mStatus;
    }

    bool IsDelayedOpen() const { return mDelayedOpen; }

private:
    virtual status_t readyToRun();
    virtual bool threadLoop();

    void decode();

    void signalEOF()
    {
        Frame frame;
        frame.mStatus = ERROR_END_OF_STREAM;

        LOGV("[Decoder] (%p) signalEOF in=%d, out=%d", this, mInQueue.size(), mOutQueue.size());
        AutoMutex lock(mInLock);
        mInQueue.push_back(frame);
        mInCondition.signal();
    }

    void shutdownDecoder()
    {
        LOGI("[Decoder] (%p) shutdown", this);
        if (mDecoderSource != 0)
            mDecoderSource->stop();

        mDecoderSource.clear();
        LOGV("[Decoder] (%p) decoder shutdown completed", this);
    }

    bool createVideoDecoder(sp<IOMX>& iomx, uint8_t* config, size_t size);
    bool createAudioDecoder(sp<IOMX>& iomx, uint8_t* config, size_t size);

    bool openVideoDecoder(const sp<IOMX>& iomx, const sp<MediaStreamSource>& source);
    void openAudioDecoder(const sp<IOMX>& iomx, const sp<MediaSource>& source);
    uint32_t getVideoDecoderFlags() const;
    bool setVideoDecoderFormat();
    bool setAudioDecoderFormat();

    sp<MediaStreamSource> mTrack;
    sp<MediaSource> mDecoderSource;

    sp<NativeWindowRenderer> mRenderer;

    volatile bool mInterrupted;
    volatile bool mFlushNeeded;
    volatile bool mNeedSkip;

    uint32_t mDecoderFlags;

    int32_t mVideoWidth;
    int32_t mVideoHeight;
    int32_t mVideoColorFormat;
    int32_t mVideoStride;
    int32_t mVideoSliceHeight;
    int32_t mVideoCropLeft;
    int32_t mVideoCropRight;
    int32_t mVideoCropTop;
    int32_t mVideoCropBottom;
    int32_t mVideoRotation;

    int32_t mSampleRate;
    int32_t mChannelCount;

    Vector<uint8_t> mCodecConfig;

    bool mIsVideoDecoder;
    bool mDelayedOpen;

    String8 mMimeType;
    String8 mComponentName;

    List<Frame> mInQueue;
    BufferQueue mOutQueue;

    mutable Mutex mLock;

    Mutex mInLock;
    Condition mInCondition;
    Condition mReadCondition;
};

status_t MediaStreamSource::read(MediaBuffer** buffer,
        const MediaSource::ReadOptions* options)
{
    if (mDecoder == 0)
        return ERROR_END_OF_STREAM;

    MediaSource::ReadOptions::SeekMode mode;
    int64_t seekTime = -1;
    if (options && options->getSeekTo(&seekTime, &mode)) {
        LOGV("[MediaStreamSource] need seekTo:%llu ?", seekTime);
    }

    Frame frame;
    status_t status = mDecoder->waitAndPopInputBuffer(frame);

    if (status == ERROR_END_OF_STREAM || frame.mSize <= 0) {
        LOGI("[MediaStreamSource] have EOF signal!");
        return ERROR_END_OF_STREAM;
    }

    if (status == OK) {
        status = mBufferGroup.acquire_buffer(buffer);

        if (status == OK && buffer) {
            memcpy((*buffer)->data(), frame.mBuffer, frame.mSize);
            (*buffer)->set_range(0, frame.mSize);
            (*buffer)->meta_data()->clear();

            if (frame.mFlags & OMX_BUFFERFLAG_CODECCONFIG) {
                (*buffer)->meta_data()->setInt32(kKeyIsCodecConfig, 1);
            } else {
//              bool syncFrame = isIDRFrame(frame.mBuffer, frame.mSize);
                (*buffer)->meta_data()->setInt32(kKeyIsSyncFrame,
                        frame.mFlags & OMX_BUFFERFLAG_SYNCFRAME ? 1 : 0);
            }

            (*buffer)->meta_data()->setInt64(kKeyTime, frame.mPts);
#if 0
            LOGV("[MediaStreamSource] NEW INPUT FRAME: \
flags=%d, frameSize=%d, time=%lld, range_offset=%d, \
range_length=%d, refs=%d, ret=%d, buffer=%p, FRAME=%d",
                    frame.mFlags, frame.mSize, frame.mPts,
                    (*buffer)->range_offset(), (*buffer)->range_length(),
                    (*buffer)->refcount(), status, *buffer, g_frameCount++);
#endif
        }
    }
    return status;
}

bool Decoder::configure(void* nativeWindow, int w, int h, void *p_extra, int i_extra)
{
    if (!mIsVideoDecoder)
        return true;

    if (nativeWindow) {
        sp<ANativeWindow> window = static_cast<ANativeWindow*>(nativeWindow);
        mRenderer = new NativeWindowRenderer(window);
    }

    mVideoWidth = w;
    mVideoHeight = h;
    mCodecConfig.appendArray((uint8_t*)p_extra, i_extra);

    LOGI("[Decoder] (%p) configure, init resolution=%dx%d, extra size=%d", this, mVideoWidth, mVideoHeight, i_extra);
    return (!mVideoWidth || !mVideoHeight) ? false : true;
}

bool Decoder::createDecoderByType(sp<IOMX>& iomx, const char* mimeType)
{
    LOG_DEBUG;
    mMimeType = strdup(mimeType);

    if (!strncasecmp(mimeType, "audio/", 6)) {
        mSampleRate = mChannelCount = 0;
        mIsVideoDecoder = false;
        mDelayedOpen = true;
        return true;
    }

    mIsVideoDecoder = true;
    return createDecoder(iomx, 0, 0);
}

bool Decoder::createDecoder(sp<IOMX>& iomx, uint8_t* config, size_t configSize)
{
    LOG_DEBUG;
    mDelayedOpen = false;
    return mIsVideoDecoder ? createVideoDecoder(iomx, config, configSize)
            : createAudioDecoder(iomx, config, configSize);
}

bool Decoder::createAudioDecoder(sp<IOMX>& iomx, uint8_t* config, size_t configSize)
{
    LOG_DEBUG;
    sp<MetaData> meta = NULL;

    meta = MakeAACCodecSpecificData(config, configSize);
//    meta->setInt32('adts', true); // kKeyIsADTS

    mTrack = new MediaStreamSource(this, meta);
    if (mTrack == 0)
        return false;

//    meta->dumpToLOGV();

    openAudioDecoder(iomx, mTrack);

    if (mDecoderSource != 0 && mDecoderSource->start() == OK) {
        if (!setAudioDecoderFormat()) {
            LOGW("[Decoder] (%p) Cannot setAudioDecoderFormat for decoder", this);
            return false;
        }

        Frame frame;
        frame.mStatus = INFO_FORMAT_CHANGED;
        mOutQueue.push(frame);
        return true;

    }
    LOGE("[Decoder] (%p) Failed to openAudioDecoder!", this);
    return false;
}

bool Decoder::createVideoDecoder(sp<IOMX>& iomx, uint8_t* config,
        size_t config_size)
{
    LOG_DEBUG;
    sp<MetaData> meta = new MetaData;
    if (meta == 0) {
        return false;
    }
    int32_t width = mVideoWidth;
    int32_t height = mVideoHeight;

    // TODO: need to align resolution ???
    //width = (width + 15) & ~0xF;
    //height = (height + 15) & ~0xF;

    int32_t colorFormat = getColorFormatForHWCodec(iomx, mMimeType.string());
    dumpCodecColorFormat(colorFormat);

    meta->setCString(kKeyMIMEType, mMimeType.string());
    meta->setInt32(kKeyWidth, width);
    meta->setInt32(kKeyHeight, height);
    meta->setInt32(kKeyStride, width);
    meta->setInt32(kKeySliceHeight, height);
    meta->setInt32(kKeyColorFormat, colorFormat);

    if (!mCodecConfig.isEmpty()) {
        if (mCodecConfig[0] == 1) {
            LOGI("[Decoder] set codec config");
            meta->setData(kKeyAVCC, kTypeAVCC, mCodecConfig.array(), mCodecConfig.size());
        } else {
            const uint32_t* ptr32 = (const uint32_t*) mCodecConfig.array();
            UniquePtr<uint8_t[]> AVCConfig(new uint8_t[mCodecConfig.size() + 10]);
            uint8_t hdr[] = { 0x1, 0x42, 0xe0, 0x1e, 0xff, 0x1 };
            size_t nAVCCSize = sizeof(hdr);
            memcpy(AVCConfig.get(), hdr, nAVCCSize);

            int nalLen;
            uint16_t nalLen16;

            const uint8_t* nal = getNALFromFrame(NAL_SPS, mCodecConfig.editArray(), mCodecConfig.size(), &nalLen);
            if (nal) {
                nalLen -= 4;
                nalLen16 = ntoh2(nalLen);
                memcpy(AVCConfig.get() + nAVCCSize, &nalLen16, sizeof(nalLen16));
                nAVCCSize += sizeof(nalLen16);
                memcpy(AVCConfig.get() + nAVCCSize, nal + 4, nalLen);
                nAVCCSize += nalLen;

                nal = getNALFromFrame(NAL_PPS, mCodecConfig.editArray(), mCodecConfig.size(), &nalLen);
                if (nal) {
                    AVCConfig.get()[nAVCCSize] = 1;
                    nAVCCSize += 1;

                    nalLen -= 4;
                    nalLen16 = ntoh2(nalLen);
                    memcpy(AVCConfig.get() + nAVCCSize, &nalLen16, sizeof(nalLen16));
                    nAVCCSize += sizeof(nalLen16);
                    memcpy(AVCConfig.get() + nAVCCSize, nal + 4, nalLen);
                    nAVCCSize += nalLen;
                } else {
                    AVCConfig.get()[nAVCCSize] = 0;
                    nAVCCSize += 1;
                }

                for (int i = 0; i < nAVCCSize; i++) {
                    LOGI("[Decoder] AVCC %x", uint32_t(AVCConfig.get()[i]));
                }

                meta->setData(kKeyAVCC, kTypeAVCC, AVCConfig.get(), nAVCCSize);
            }
        }
    }

    android::ProcessState::self()->startThreadPool();
    //DataSource::RegisterDefaultSniffers();

    AutoMutex lock(mLock);
    mTrack = new MediaStreamSource(this, meta);
    if (mTrack == 0)
        return false;

    bool hasHWRendering = openVideoDecoder(iomx, mTrack);
    LOGI("[Decoder] has hw rendering=%d", hasHWRendering?1:0);

    if (mDecoderSource != 0 && mDecoderSource->start() == OK) {

        if (!hasHWRendering && mRenderer != 0) {
            mRenderer->init(mDecoderSource->getFormat());
        }

        if (!setVideoDecoderFormat()) {
            LOGW("[Decoder] (%p) Can't setVideoDecoderFormat for decoder", this);
            return false;
        }

        Frame frame;
        frame.mStatus = INFO_FORMAT_CHANGED;
        mOutQueue.push(frame);
    }
    return true;
}

void Decoder::release()
{
    mFlushNeeded = false;

    { // scopped lock
        AutoMutex lock(mInLock);
        mInQueue.clear();
    }

    if (!mInterrupted) {
        mInterrupted = true;
        signalEOF();
    }

    mOutQueue.release();

    LOGV("[Decoder] (%p) joining...", this);
    join();

    shutdownDecoder();

    if (mTrack != 0) {
        mTrack->stop();
        mTrack.clear();
    }

    mRenderer.clear();
    LOGI("[Decoder] (%p) release end!", this);
}

status_t Decoder::readyToRun()
{
    mInterrupted = false;
    return OK;
}

bool Decoder::threadLoop()
{
    decode();
    mInterrupted = true;
    LOGI("[Decoder] (%p) ************ EXIT DECODER! **********", this);
    return false;
}

void Decoder::decode()
{
    if (mDecoderSource == 0)
        return;

    bool decodeDone = false;
    status_t status = OK;
//    MediaSource::ReadOptions readopt;
    int64_t startTime = getTimestampMs();
    MediaBuffer* mediaBuffer = 0;
    bool skipEnabled = false;

    do {
        releaseMediaBuffer(mediaBuffer);

        mOutQueue.releaseBuffers();

        startTime = getTimestampMs();

        status = mDecoderSource->read(&mediaBuffer, NULL);//&readopt);

        mOutQueue.releaseBuffers();
//        readopt.clearSeekTo();

        if (mInterrupted)
            break;

        if (status == OK) {
            if (!mediaBuffer)
                continue;

            if (!mediaBuffer->graphicBuffer().get() && !mediaBuffer->range_length()) {
                LOGI("[Decoder] (%p) ERROR: soft buffer with zero length", this);
                releaseMediaBuffer(mediaBuffer);
                continue;
            }

            if (skipEnabled && mNeedSkip) {
                releaseMediaBuffer(mediaBuffer);
                skipEnabled = false;
                continue;
            }

            dumpMetaData(mediaBuffer->meta_data().get());

            int64_t timeUs = 0;
            if (!mediaBuffer->meta_data()->findInt64(kKeyTime, &timeUs)) {
                LOGE("[Decoder] (%p) ERROR: no frame time", this);
                break;
            }

            int filled = mOutQueue.filledCount();

            if (timeUs < 0) {
                LOGW("[Decoder] (%p) frame time %lld must be nonnegative", this, timeUs);
                continue;
            }

            if (!mediaBuffer->graphicBuffer().get()) {
                uint8_t* data = reinterpret_cast<uint8_t*>(mediaBuffer->data())
                            + mediaBuffer->range_offset();
                size_t length = mediaBuffer->range_length();

                Frame frame(status, data, length, timeUs, 0);
                mOutQueue.push(frame);

                releaseMediaBuffer(mediaBuffer);
            } else {
                if (filled >= MAX_HOLDED_FRAMES && !mInterrupted) {
                    filled = mOutQueue.waitRelease(2 * s_frameDisplayTimeMsec);
                    if (filled >= MAX_HOLDED_FRAMES)
                        releaseMediaBuffer(mediaBuffer);
                }
                Frame frame(status, mediaBuffer, timeUs, 0);
                int index = mOutQueue.push(frame, true);
                mediaBuffer = 0;

                skipEnabled = true;
                if (filled + 1 >= MAX_HOLDED_FRAMES && !mInterrupted) {
                    filled = mOutQueue.waitRelease(mFlushNeeded ? (OUT_BUFFER_COUNT * s_frameDisplayTimeMsec) : (2 * s_frameDisplayTimeMsec));
                    if (filled >= MAX_HOLDED_FRAMES) {
                        mOutQueue.clearBuffer(index);
                        skipEnabled = false;
                    }
                }
            }
        } else if (status == INFO_FORMAT_CHANGED) {
            LOGI("[Decoder] (%p) decode ====== INFO_FORMAT_CHANGED ======", this);

            if (mIsVideoDecoder) {
                AutoMutex lock(mLock);
                setVideoDecoderFormat();
            } else {
                setAudioDecoderFormat();
            }

            //TODO: need to change frame size??
//            sp<MetaData> meta = mDecoderSource->getFormat();
//            mTrack->setFormat(meta);

            Frame frame;
            frame.mStatus = status;
            mOutQueue.push(frame);
            continue;

        } else if (status == ERROR_END_OF_STREAM) {
            LOGI("[Decoder] (%p) decode ====== END_OF_STREAM ======", this);

            releaseMediaBuffer(mediaBuffer);
            if (mFlushNeeded) {
                Frame frame;
                frame.mStatus = status;
                mOutQueue.push(frame, true);
            }
            decodeDone = true;

            continue;

        } else if (status == INFO_DISCONTINUITY) {
            LOGI("[Decoder] (%p) decode ====== INFO_DISCONTINUITY ======", this);

            releaseMediaBuffer(mediaBuffer);
        } else {
            LOGE("[Decoder] (%p) decode ERROR %d(%#x)", this, status, status);
            releaseMediaBuffer(mediaBuffer);

            if (status == ETIMEDOUT) { // -110
                // rised by OMXCodec::waitForBufferFilled_l
            }
            if (status == 0xfffffbb1) { // -1103
            }
            if (status == UNKNOWN_ERROR) {
            }

            if (mInterrupted)
                break;

            mOutQueue.clearAll();
            usleep(s_frameDisplayTimeMsec * 1000);

            continue;
        }

    } while (!decodeDone && !mInterrupted);

    releaseMediaBuffer(mediaBuffer);
    mOutQueue.clearAll();
}

bool Decoder::setVideoDecoderFormat()
{
    LOG_DEBUG;
    if (mDecoderSource == 0)
        return false;

    sp<MetaData> format = mDecoderSource->getFormat();
    dumpMetaData(format.get());

#if 1
    int32_t unexpected;
    if (format->findInt32(kKeyStride, &unexpected))
        LOGW("[Decoder] (%p) Expected kKeyWidth, but found kKeyStride %d", this, unexpected);
    if (format->findInt32(kKeySliceHeight, &unexpected))
        LOGW("[Decoder] (%p) Expected kKeyHeight, but found kKeySliceHeight %d", this, unexpected);
#endif

    const char* componentName = NULL;
    if (!format->findInt32(kKeyWidth, &mVideoStride)
        || !format->findInt32(kKeyHeight, &mVideoSliceHeight)
        || !format->findCString(kKeyDecoderComponent, &componentName)
        || !format->findInt32(kKeyColorFormat, &mVideoColorFormat)) {
        return false;
    }

    if (componentName) {
        mComponentName = strdup(componentName);
#if defined(ANDROID_JBMR2) && defined(DEBUG_CODEC)
        uint32_t quirks = 0;
        if (OMXCodec::findCodecQuirks(componentName, &quirks)) {
            LOGV("[Decoder] Codec quirks=%#x", quirks);
        }
#endif
    }

    dumpCodecColorFormat(mVideoColorFormat);

    if (mVideoStride <= 0) {
        LOGW("[Decoder] (%p) stride %d must be positive", this, mVideoStride);
        return false;
    }

    if (mVideoSliceHeight <= 0) {
        LOGW("[Decoder] (%p) slice height %d must be positive", this, mVideoSliceHeight);
        return false;
    }

    if (!format->findRect(kKeyCropRect, &mVideoCropLeft, &mVideoCropTop,
        &mVideoCropRight, &mVideoCropBottom)) {
        mVideoCropLeft = 0;
        mVideoCropTop = 0;
        mVideoCropRight = mVideoStride - 1;
        mVideoCropBottom = mVideoSliceHeight - 1;
        LOGI("[Decoder] crop rect not available, assuming no cropping");
    }

    if (mVideoCropLeft < 0 || mVideoCropLeft >= mVideoCropRight
        || mVideoCropRight >= mVideoStride || mVideoCropTop < 0
        || mVideoCropTop >= mVideoCropBottom
        || mVideoCropBottom >= mVideoSliceHeight) {
        LOGW("[Decoder] (%p) invalid crop rect %d,%d-%d,%d", this,
                mVideoCropLeft, mVideoCropTop, mVideoCropRight, mVideoCropBottom);
        return false;
    }

    mVideoWidth = mVideoCropRight - mVideoCropLeft + 1;
    mVideoHeight = mVideoCropBottom - mVideoCropTop + 1;
    if (mVideoWidth > 0 && mVideoWidth <= mVideoStride) {

    }

    if (mVideoHeight > 0 && mVideoHeight <= mVideoSliceHeight) {

    }

    if (!format->findInt32(kKeyRotation, &mVideoRotation)) {
        mVideoRotation = 0;
        LOGV("[Decoder] (%p) rotation not available, assuming 0", this);
    }

    if (mVideoRotation != 0 && mVideoRotation != 90 && mVideoRotation != 180
            && mVideoRotation != 270) {
        LOGW("[Decoder] (%p) invalid rotation %d, assuming 0", this, mVideoRotation);
    }
#if 0
    // TODO: need to check on tegra device qurks
    if (mVideoSliceHeight <= 0 && mVideoColorFormat == OMX_COLOR_FormatYUV420Planar) {
        // NVidia Tegra 3 on Nexus 7 does not set slice_heights
        if (strstr(componentName, "OMX.Nvidia.") != NULL) {
            mVideoSliceHeight = (((mVideoHeight) + 15) & ~15);
            LOGV("NVidia Tegra 3 quirk, slice_height(%d)", mVideoSliceHeight);
        }
    }
#endif

    format->setInt32(kKeyWidth, mVideoWidth);
    format->setInt32(kKeyHeight, mVideoHeight);
    return true;
}

bool Decoder::setAudioDecoderFormat()
{
    LOG_DEBUG;
    if (mDecoderSource == 0)
        return false;

    sp<MetaData> format = mDecoderSource->getFormat();
    if (!format->findInt32(kKeySampleRate, &mSampleRate)
        || !format->findInt32(kKeyChannelCount, &mChannelCount)) {
        return false;
    }

//    format->dumpToLOGV();
    return true;
}

bool Decoder::openVideoDecoder(const sp<IOMX>& omx, const sp<MediaStreamSource>& track)
{
    LOGV("[Decoder] (%p) openVideoDecoder", this);
    int32_t decoderFlags = getVideoDecoderFlags();
    if (mRenderer != 0) {
        sp<ANativeWindow> window = mRenderer->window();
        if (window != 0) {
            mDecoderSource = OMXCodec::Create(omx, track->getFormat(), false, track, 0, decoderFlags, window);
            if (mDecoderSource != 0) {
                LOGV("[Decoder] (%p) decoder opened!", this);
            }
        }
    }
    if (mDecoderSource == 0) {
        LOGW("[Decoder] (%p) cannot open OMXCodec!", this);
        return false;
    }

    sp<MetaData> format = mDecoderSource->getFormat();
    const char* component = 0;
    if (format->findCString(kKeyDecoderComponent, &component)) {
        if (strncmp(component, "OMX.", 4)
            || !strncmp(component, "OMX.google.", 11)
            || !strncmp(component, "OMX.Nvidia.mpeg2v.decode", 24)) {
            LOGV("[Decoder] (%p), use software renderer for %s decoder", this, component);
            track->setColorFormat(OMX_COLOR_FormatYUV420Planar);
            mDecoderSource.clear();
            decoderFlags |= OMXCodec::kClientNeedsFramebuffer;
            mDecoderSource = OMXCodec::Create(omx, track->getFormat(), false, track, 0, decoderFlags, 0);
            return false;
        }
    }
    return true;
}

void Decoder::openAudioDecoder(const sp<IOMX>& omx, const sp<MediaSource>& track)
{
    LOGV("[Decoder] (%p) openAudioDecoder", this);
    int32_t decoderFlags = OMXCodec::kClientNeedsFramebuffer | OMXCodec::kHardwareCodecsOnly;
    mDecoderSource = OMXCodec::Create(omx, track->getFormat(), false, track, 0, decoderFlags, 0);
    if (mDecoderSource == 0) {
        LOGW("[Decoder] (%p) falling back to software audio decoder", this);
        mDecoderSource.clear();
        decoderFlags = OMXCodec::kClientNeedsFramebuffer | OMXCodec::kSoftwareCodecsOnly;
        mDecoderSource = OMXCodec::Create(omx, track->getFormat(), false, track, NULL, decoderFlags, 0);
    }
}

uint32_t Decoder::getVideoDecoderFlags() const
{
    LOG_DEBUG;
    uint32_t flags = mDecoderFlags;
    if ((flags & OMXCodec::kHardwareCodecsOnly) != 0) {
        LOGI("[Decoder] (%p) try to use HW decoding", this);
    } else if ((flags & OMXCodec::kSoftwareCodecsOnly) != 0) {
        LOGI("[Decoder] (%p) try to use SW decoding", this);
    }
    flags |= OMXCodec::kClientNeedsFramebuffer;
    return flags;
}

class StagefrightContext {
public:
    StagefrightContext()
        : mDecoder(new Decoder())
    {}

    ~StagefrightContext() {}

    bool configure(void* nativewWindow, int w, int h, void *p_extra, int i_extra);
    bool createDecoderByType(const char* mimeType);
    void release();
    void releaseOutputBuffer(int index, int64_t pts);
    const char* getName();
    void getOutputFormat(void*);
    int32_t getOutputBuffers();
    bool queueInputBuffer(int32_t index, uint8_t* data, size_t size,
            int64_t pts, uint32_t flags);
    int32_t dequeueInputBuffer(int64_t timeoutUs);
    int32_t dequeueOutputBuffer(uint8_t** data, size_t* size, int64_t* pts);
    int32_t outputBufferCount();
    void flush() { if (mDecoder != NULL) mDecoder->flush(); }

private:
    OMXClient mClient;
    sp<Decoder> mDecoder;
};

bool StagefrightContext::configure(void* nativeWindow, int w, int h, void *p_extra, int i_extra)
{
    LOG_DEBUG;
    if (mClient.connect() != OK) {
        LOGW("[StagefrightContext] OMXClient failed to connect");
        mDecoder = NULL;
        return false;
    }

    if (!mDecoder->configure(nativeWindow, w, h, p_extra, i_extra)) {
        mClient.disconnect();
        mDecoder = NULL;
        return false;
    }
    return true;
}

bool StagefrightContext::createDecoderByType(const char* mimeType)
{
    LOG_DEBUG;
    if (mDecoder != NULL) {
        sp<IOMX> iomx = mClient.interface();
        dumpCodecProfiles(iomx, true);
        bool result = mDecoder->createDecoderByType(iomx, mimeType);

        if (result) {
            if (mDecoder->IsDelayedOpen())
                return true;
            if (OK == mDecoder->run(0, DECODER_PRIORITY))
                return true;
        }
    }
    return false;
}

void StagefrightContext::release()
{
    sp<Decoder> decoder = mDecoder;
    if (mDecoder != 0) {
        mDecoder.clear();
    }

    if (decoder != 0) {
        decoder->release();
        decoder = 0;
    }
    mClient.disconnect();
}

void StagefrightContext::releaseOutputBuffer(int index, int64_t pts)
{
    if (mDecoder != 0 && index >= 0)
        mDecoder->releaseOutputBuffer(index, pts);
}

const char* StagefrightContext::getName()
{
    if (mDecoder != 0) return mDecoder->getName();
    return NULL;
}

void StagefrightContext::getOutputFormat(void* outFormat)
{
    if (mDecoder != 0) mDecoder->getOutputFormat(outFormat);
}

int32_t StagefrightContext::getOutputBuffers()
{
    if (mDecoder != 0) return mDecoder->getOutputBuffers();
    return 0;
}

bool StagefrightContext::queueInputBuffer(int32_t index, uint8_t* data,
        size_t size, int64_t pts, uint32_t flags)
{
    if (size > 0 && mDecoder != 0) {
        if (mDecoder->IsDelayedOpen()) {
            if (flags & OMX_BUFFERFLAG_CODECCONFIG) {
                sp<IOMX> iomx = mClient.interface();
                bool result = mDecoder->createDecoder(iomx, data, size);

                if (result)
                    mDecoder->run(0, DECODER_PRIORITY);
                return true;
            } else {
                LOGW("[Decoder] First frame must contain config!");
                return false;
            }
        }
        return mDecoder->queueInputBuffer(index, data, size, pts, flags);
    }
    return false;
}

int32_t StagefrightContext::dequeueInputBuffer(int64_t timeoutUs)
{
    if (mDecoder != 0) return mDecoder->dequeueInputBuffer(timeoutUs);
    return INFO_TRY_AGAIN_LATER;
}

int32_t StagefrightContext::dequeueOutputBuffer(uint8_t** data, size_t* size,
        int64_t* pts)
{
    if (mDecoder != 0) return mDecoder->dequeueOutputBuffer(data, size, pts);
    return INFO_TRY_AGAIN_LATER;
}

int32_t StagefrightContext::outputBufferCount()
{
    if (mDecoder != 0) return mDecoder->outputBufferCount();
    return 0;
}

extern "C" {
ATTRIBUTE_PUBLIC void* Stagefright_Configure(void* nativeWindow, int width, int height, void *p_extra, int i_extra)
{
    StagefrightContext* ctx = new StagefrightContext();
    if (!ctx || !ctx->configure(nativeWindow, width, height, p_extra, i_extra)) {
        if (ctx) {
            delete ctx;
            ctx = NULL;
        }
        return NULL;
    }
    return ctx;
}

ATTRIBUTE_PUBLIC bool Stagefright_CreateDecoderByType(StagefrightContext* ctx, const char* mimeType)
{
    if (ctx) return ctx->createDecoderByType(mimeType);
    return false;
}

ATTRIBUTE_PUBLIC void Stagefright_Release(StagefrightContext* ctx)
{
    if (ctx) {
        ctx->release();
        delete ctx;
        ctx = NULL;
    }
}

ATTRIBUTE_PUBLIC const char* Stagefright_GetName(StagefrightContext* ctx)
{
    if (ctx) return ctx->getName();
    return NULL;
}

ATTRIBUTE_PUBLIC void Stagefright_GetOutputFormat(StagefrightContext* ctx, void* outFormat)
{
    if (ctx) return ctx->getOutputFormat(outFormat);
}

ATTRIBUTE_PUBLIC int32_t Stagefright_GetOutputBuffers(StagefrightContext* ctx)
{
    if (ctx) return ctx->getOutputBuffers();
    return 0;
}

ATTRIBUTE_PUBLIC void Stagefright_Flush(StagefrightContext* ctx)
{
    if (ctx) ctx->flush();
}

ATTRIBUTE_PUBLIC bool Stagefright_QueueInputBuffer(StagefrightContext* ctx, int32_t index, uint8_t* data, size_t size,
        int64_t pts, uint32_t flags)
{
    if (ctx) return ctx->queueInputBuffer(index, data, size, pts, flags);
    return false;
}

ATTRIBUTE_PUBLIC int32_t Stagefright_DequeueInputBuffer(StagefrightContext* ctx, int64_t timeoutUs)
{
    if (ctx) return ctx->dequeueInputBuffer(timeoutUs);
    return INFO_TRY_AGAIN_LATER;
}

ATTRIBUTE_PUBLIC void Stagefright_ReleaseOutputBuffer(StagefrightContext* ctx, int32_t index, int64_t pts)
{
    if (ctx) ctx->releaseOutputBuffer(index, pts);
}

ATTRIBUTE_PUBLIC int32_t Stagefright_DequeueOutputBuffer(StagefrightContext* ctx, uint8_t** outData,
        unsigned int* outSize, int64_t* outTs)
{
    if (ctx) return ctx->dequeueOutputBuffer(outData, outSize, outTs);
    return INFO_TRY_AGAIN_LATER;
}

ATTRIBUTE_PUBLIC int32_t Stagefright_OutputBufferCount(StagefrightContext* ctx) {
    if (ctx) return ctx->outputBufferCount();
    return 0;
}

}
