#ifndef AUDIOSTREAMDECODER_H
#define AUDIOSTREAMDECODER_H

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVIOContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

/**
 * @brief 基于 FFmpeg 的顺序音频解码器，从内存中的编码字节按需产出交织 float PCM。
 *
 * 一个实例对应一路解码流（一个流式 Voice 一个实例），内部持有独立的
 * AVFormatContext / AVCodecContext / SwrContext。输出统一重采样为目标
 * 采样率与声道数（packed float）。
 *
 * 线程约定：实例非线程安全，Open/ReadFrames/Restart/Close 必须在同一线程
 * （通常为后台解码线程或资源加载线程）串行调用；禁止在实时音频回调线程使用，
 * 因为解码与内部缓冲会产生堆分配和大块 CPU 工作。
 */
class AudioStreamDecoder
{
public:
    /**
     * @brief 默认构造函数，不打开任何流。
     */
    AudioStreamDecoder() = default;

    /**
     * @brief 析构函数，释放全部 FFmpeg 资源。
     */
    ~AudioStreamDecoder();

    AudioStreamDecoder(const AudioStreamDecoder&) = delete;            ///< 禁止拷贝。
    AudioStreamDecoder& operator=(const AudioStreamDecoder&) = delete; ///< 禁止拷贝赋值。

    /**
     * @brief 从内存中的编码字节打开解码流。
     *
     * 调用方必须保证 data 指向的内存在解码器关闭前始终有效（本类不拷贝字节）。
     *
     * @param data 编码音频字节（完整文件内容）。
     * @param size 字节数。
     * @param targetSampleRate 输出采样率。
     * @param targetChannels 输出声道数（1 = 单声道，其余按 2 = 立体声处理）。
     * @return 打开成功返回 true。
     */
    bool Open(const uint8_t* data, size_t size, int targetSampleRate, int targetChannels);

    /**
     * @brief 关闭解码流并释放 FFmpeg 资源，可安全重复调用。
     */
    void Close();

    /**
     * @brief 查询解码流是否处于已打开状态。
     * @return 已打开返回 true。
     */
    bool IsOpen() const { return opened; }

    /**
     * @brief 获取容器/流报告的音频时长（秒）。
     * @return 时长（秒）；未知时返回负值。
     */
    double DurationSeconds() const;

    /**
     * @brief 顺序解码下一段 PCM 数据。
     *
     * 输出为交织 float，声道数等于 Open 时的目标声道数。
     *
     * @param dst 输出缓冲，容量至少为 maxFrames * targetChannels 个 float。
     * @param maxFrames 最多写入的帧数。
     * @return 实际写入的帧数；0 表示流已结束（含重采样尾部已排空）；负值表示解码错误。
     */
    int ReadFrames(float* dst, int maxFrames);

    /**
     * @brief 回到流起点重新开始解码（用于循环播放）。
     *
     * 实现为对内存字节整体重开，保证对任意容器格式（含不可 seek 格式）都可靠。
     *
     * @return 成功返回 true；失败时解码器处于关闭状态。
     */
    bool Restart();

private:
    /// @brief 内存读取游标，供 FFmpeg 自定义 AVIO 回调使用。
    struct MemoryCursor
    {
        const uint8_t* data = nullptr; ///< 编码字节起始地址。
        size_t size = 0;               ///< 编码字节总数。
        size_t pos = 0;                ///< 当前读取偏移。
    };

    static int ReadThunk(void* opaque, uint8_t* buf, int bufSize);
    static int64_t SeekThunk(void* opaque, int64_t offset, int whence);

    bool PumpOnce();
    void AppendConverted(AVFrame* sourceFrame);
    void AppendResamplerTail();

private:
    MemoryCursor cursor;               ///< 自定义 AVIO 的内存游标。
    AVFormatContext* fmtCtx = nullptr; ///< 容器解复用上下文。
    AVIOContext* ioCtx = nullptr;      ///< 自定义内存 IO 上下文。
    AVCodecContext* codecCtx = nullptr;///< 解码器上下文。
    SwrContext* swrCtx = nullptr;      ///< 重采样上下文（统一输出 packed float）。
    AVPacket* packet = nullptr;        ///< 复用的包对象。
    AVFrame* frame = nullptr;          ///< 复用的帧对象。
    int streamIndex = -1;              ///< 选中的音频流索引。

    const uint8_t* srcData = nullptr;  ///< 源字节指针（Restart 时重开用）。
    size_t srcSize = 0;                ///< 源字节数。
    int targetRate = 0;                ///< 输出采样率。
    int targetCh = 0;                  ///< 输出声道数（1 或 2）。

    std::vector<float> pending;        ///< 已转换但尚未被取走的样本（交织 float）。
    size_t pendingOffset = 0;          ///< pending 中已消费的样本偏移。
    bool opened = false;               ///< 是否处于已打开状态。
    bool draining = false;             ///< 是否已向解码器发送 flush 包。
    bool streamEnded = false;          ///< 流是否已完全排空（含重采样尾部）。
    bool errorFlag = false;            ///< 是否发生不可恢复的解码错误。
};

#endif
