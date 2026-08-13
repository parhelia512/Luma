#include "AudioStreamDecoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

namespace
{
    constexpr int kAvioBufferSize = 4096; ///< 自定义 AVIO 的内部缓冲大小（字节）。
}

AudioStreamDecoder::~AudioStreamDecoder()
{
    Close();
}

int AudioStreamDecoder::ReadThunk(void* opaque, uint8_t* buf, int bufSize)
{
    auto* cur = static_cast<MemoryCursor*>(opaque);
    if (!cur || bufSize <= 0)
    {
        return AVERROR(EINVAL);
    }
    const size_t remaining = cur->size - cur->pos;
    const size_t toCopy = std::min(remaining, static_cast<size_t>(bufSize));
    if (toCopy == 0)
    {
        return AVERROR_EOF;
    }
    std::memcpy(buf, cur->data + cur->pos, toCopy);
    cur->pos += toCopy;
    return static_cast<int>(toCopy);
}

int64_t AudioStreamDecoder::SeekThunk(void* opaque, int64_t offset, int whence)
{
    auto* cur = static_cast<MemoryCursor*>(opaque);
    if (!cur)
    {
        return AVERROR(EINVAL);
    }
    if ((whence & AVSEEK_SIZE) != 0)
    {
        return static_cast<int64_t>(cur->size);
    }
    int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE)
    {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<int64_t>(cur->pos); break;
    case SEEK_END: base = static_cast<int64_t>(cur->size); break;
    default: return AVERROR(EINVAL);
    }
    const int64_t target = base + offset;
    if (target < 0 || target > static_cast<int64_t>(cur->size))
    {
        return AVERROR(EINVAL);
    }
    cur->pos = static_cast<size_t>(target);
    return target;
}

bool AudioStreamDecoder::Open(const uint8_t* data, size_t size, int targetSampleRate, int targetChannels)
{
    Close();
    if (!data || size == 0 || targetSampleRate <= 0)
    {
        return false;
    }

    srcData = data;
    srcSize = size;
    targetRate = targetSampleRate;
    targetCh = (targetChannels == 1) ? 1 : 2;
    cursor.data = data;
    cursor.size = size;
    cursor.pos = 0;

    fmtCtx = avformat_alloc_context();
    if (!fmtCtx)
    {
        return false;
    }
    unsigned char* ioBuffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (!ioBuffer)
    {
        avformat_free_context(fmtCtx);
        fmtCtx = nullptr;
        return false;
    }
    ioCtx = avio_alloc_context(ioBuffer, kAvioBufferSize, 0, &cursor,
                               &AudioStreamDecoder::ReadThunk, nullptr,
                               &AudioStreamDecoder::SeekThunk);
    if (!ioCtx)
    {
        av_free(ioBuffer);
        avformat_free_context(fmtCtx);
        fmtCtx = nullptr;
        return false;
    }
    fmtCtx->pb = ioCtx;

    if (avformat_open_input(&fmtCtx, nullptr, nullptr, nullptr) < 0)
    {
        // 打开失败时 avformat_open_input 已释放 fmtCtx，自定义 IO 仍由本类清理
        fmtCtx = nullptr;
        av_freep(&ioCtx->buffer);
        avio_context_free(&ioCtx);
        ioCtx = nullptr;
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
    {
        Close();
        return false;
    }

    streamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0)
    {
        Close();
        return false;
    }

    AVStream* stream = fmtCtx->streams[streamIndex];
    AVCodecParameters* par = stream->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
    {
        Close();
        return false;
    }
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        Close();
        return false;
    }
    if (avcodec_parameters_to_context(codecCtx, par) < 0 ||
        avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        Close();
        return false;
    }

    AVChannelLayout outLayout;
    if (targetCh == 1)
    {
        outLayout = AV_CHANNEL_LAYOUT_MONO;
    }
    else
    {
        outLayout = AV_CHANNEL_LAYOUT_STEREO;
    }
    const int ret = swr_alloc_set_opts2(
        &swrCtx,
        &outLayout,
        AV_SAMPLE_FMT_FLT,
        targetRate,
        &codecCtx->ch_layout,
        codecCtx->sample_fmt,
        codecCtx->sample_rate,
        0, nullptr);
    if (ret < 0 || !swrCtx || swr_init(swrCtx) < 0)
    {
        Close();
        return false;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame)
    {
        Close();
        return false;
    }

    pending.clear();
    pendingOffset = 0;
    opened = true;
    draining = false;
    streamEnded = false;
    errorFlag = false;
    return true;
}

void AudioStreamDecoder::Close()
{
    if (swrCtx)
    {
        swr_free(&swrCtx);
    }
    if (codecCtx)
    {
        avcodec_free_context(&codecCtx);
    }
    if (fmtCtx)
    {
        avformat_close_input(&fmtCtx);
    }
    if (ioCtx)
    {
        av_freep(&ioCtx->buffer);
        avio_context_free(&ioCtx);
        ioCtx = nullptr;
    }
    if (packet)
    {
        av_packet_free(&packet);
    }
    if (frame)
    {
        av_frame_free(&frame);
    }
    streamIndex = -1;
    pending.clear();
    pendingOffset = 0;
    opened = false;
    draining = false;
    streamEnded = false;
    errorFlag = false;
}

double AudioStreamDecoder::DurationSeconds() const
{
    if (!opened || !fmtCtx || streamIndex < 0)
    {
        return -1.0;
    }
    const AVStream* stream = fmtCtx->streams[streamIndex];
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
    {
        return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }
    if (fmtCtx->duration != AV_NOPTS_VALUE && fmtCtx->duration > 0)
    {
        return static_cast<double>(fmtCtx->duration) / static_cast<double>(AV_TIME_BASE);
    }
    return -1.0;
}

void AudioStreamDecoder::AppendConverted(AVFrame* sourceFrame)
{
    const int maxOut = swr_get_out_samples(swrCtx, sourceFrame->nb_samples);
    if (maxOut <= 0)
    {
        return;
    }
    pending.resize(static_cast<size_t>(maxOut) * static_cast<size_t>(targetCh));
    uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(pending.data())};
    const int converted = swr_convert(swrCtx, outPlanes, maxOut,
                                      (const uint8_t**)sourceFrame->extended_data,
                                      sourceFrame->nb_samples);
    if (converted > 0)
    {
        pending.resize(static_cast<size_t>(converted) * static_cast<size_t>(targetCh));
    }
    else
    {
        pending.clear();
    }
}

void AudioStreamDecoder::AppendResamplerTail()
{
    const int maxOut = swr_get_out_samples(swrCtx, 0);
    if (maxOut <= 0)
    {
        return;
    }
    pending.resize(static_cast<size_t>(maxOut) * static_cast<size_t>(targetCh));
    uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(pending.data())};
    const int converted = swr_convert(swrCtx, outPlanes, maxOut, nullptr, 0);
    if (converted > 0)
    {
        pending.resize(static_cast<size_t>(converted) * static_cast<size_t>(targetCh));
    }
    else
    {
        pending.clear();
    }
}

bool AudioStreamDecoder::PumpOnce()
{
    pending.clear();
    pendingOffset = 0;
    while (true)
    {
        const int ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == 0)
        {
            AppendConverted(frame);
            av_frame_unref(frame);
            if (!pending.empty())
            {
                return true;
            }
            continue;
        }
        if (ret == AVERROR_EOF)
        {
            // 解码器已排空：冲刷重采样器内部残留样本后宣告流结束
            AppendResamplerTail();
            streamEnded = true;
            return !pending.empty();
        }
        if (ret != AVERROR(EAGAIN))
        {
            errorFlag = true;
            return false;
        }
        if (draining)
        {
            // 已发送 flush 包却仍返回 EAGAIN，属异常状态，防御性终止
            errorFlag = true;
            return false;
        }
        // 解码器需要新数据：读取并送入下一个属于音频流的包
        while (true)
        {
            const int rr = av_read_frame(fmtCtx, packet);
            if (rr == AVERROR_EOF)
            {
                avcodec_send_packet(codecCtx, nullptr);
                draining = true;
                break;
            }
            if (rr < 0)
            {
                errorFlag = true;
                return false;
            }
            if (packet->stream_index != streamIndex)
            {
                av_packet_unref(packet);
                continue;
            }
            const int sr = avcodec_send_packet(codecCtx, packet);
            av_packet_unref(packet);
            if (sr == 0 || sr == AVERROR(EAGAIN))
            {
                // EAGAIN 理论上不出现（收帧已排空）；出现时丢弃该包并转回收帧
                break;
            }
            // 其余错误视为坏包，跳过继续读下一个包
        }
    }
}

int AudioStreamDecoder::ReadFrames(float* dst, int maxFrames)
{
    if (!opened)
    {
        return -1;
    }
    if (!dst || maxFrames <= 0)
    {
        return 0;
    }
    const size_t channelCount = static_cast<size_t>(targetCh);
    int written = 0;
    while (written < maxFrames)
    {
        const size_t pendingSamples = pending.size() - pendingOffset;
        if (pendingSamples >= channelCount)
        {
            const size_t pendingFrames = pendingSamples / channelCount;
            const size_t take = std::min(pendingFrames, static_cast<size_t>(maxFrames - written));
            std::memcpy(dst + static_cast<size_t>(written) * channelCount,
                        pending.data() + pendingOffset,
                        take * channelCount * sizeof(float));
            pendingOffset += take * channelCount;
            written += static_cast<int>(take);
            if (pendingOffset >= pending.size())
            {
                pending.clear();
                pendingOffset = 0;
            }
            continue;
        }
        if (streamEnded || errorFlag)
        {
            break;
        }
        if (!PumpOnce())
        {
            break;
        }
    }
    if (written == 0 && errorFlag)
    {
        return -1;
    }
    return written;
}

bool AudioStreamDecoder::Restart()
{
    const uint8_t* data = srcData;
    const size_t size = srcSize;
    const int rate = targetRate;
    const int channels = targetCh;
    if (!data || size == 0)
    {
        return false;
    }
    return Open(data, size, rate, channels);
}
