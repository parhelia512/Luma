#ifndef AUDIOSTREAMING_H
#define AUDIOSTREAMING_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

#include "../Resources/RuntimeAsset/RuntimeAudio.h"
#include "../Resources/Loaders/AudioStreamDecoder.h"

/**
 * @brief 单生产者/单消费者（SPSC）交织 float PCM 环形缓冲。
 *
 * 生产者为后台解码线程（Write），消费者为实时音频回调线程（Read）。
 * 缓冲在 Reset 时一次性预分配；Read/Write 均为无锁、无堆分配、
 * 无阻塞操作，满足实时线程纪律。读写游标为单调递增的帧计数，
 * 借助 acquire/release 原子序保证数据可见性。
 */
class AudioSpscRing
{
public:
    /**
     * @brief 预分配缓冲并清空读写游标（仅可在缓冲尚未被两线程并发使用时调用）。
     * @param capacityFrames 缓冲容量（帧数）。
     * @param channels 交织声道数。
     */
    void Reset(size_t capacityFrames, int channels)
    {
        m_capacityFrames = std::max<size_t>(1, capacityFrames);
        m_channelCount = static_cast<size_t>(std::max(1, channels));
        m_buffer.assign(m_capacityFrames * m_channelCount, 0.0f);
        m_writeFrames.store(0, std::memory_order_relaxed);
        m_readFrames.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief 获取缓冲容量（帧数）。
     * @return 容量帧数。
     */
    size_t CapacityFrames() const { return m_capacityFrames; }

    /**
     * @brief 获取当前可读帧数（消费者视角，生产者亦可用于水位判断）。
     * @return 可读帧数。
     */
    size_t AvailableFrames() const
    {
        return m_writeFrames.load(std::memory_order_acquire) -
            m_readFrames.load(std::memory_order_acquire);
    }

    /**
     * @brief 获取当前可写帧数（仅生产者调用）。
     * @return 可写帧数。
     */
    size_t FreeFrames() const
    {
        const size_t write = m_writeFrames.load(std::memory_order_relaxed);
        const size_t read = m_readFrames.load(std::memory_order_acquire);
        return m_capacityFrames - (write - read);
    }

    /**
     * @brief 写入交织 PCM 帧（仅生产者调用），空间不足时截断。
     * @param interleaved 源数据，至少 frames * channels 个 float。
     * @param frames 期望写入的帧数。
     * @return 实际写入的帧数。
     */
    size_t Write(const float* interleaved, size_t frames)
    {
        const size_t read = m_readFrames.load(std::memory_order_acquire);
        const size_t write = m_writeFrames.load(std::memory_order_relaxed);
        const size_t freeFrames = m_capacityFrames - (write - read);
        const size_t n = std::min(frames, freeFrames);
        if (n == 0)
        {
            return 0;
        }
        const size_t start = write % m_capacityFrames;
        const size_t firstPart = std::min(n, m_capacityFrames - start);
        std::memcpy(m_buffer.data() + start * m_channelCount,
                    interleaved,
                    firstPart * m_channelCount * sizeof(float));
        if (n > firstPart)
        {
            std::memcpy(m_buffer.data(),
                        interleaved + firstPart * m_channelCount,
                        (n - firstPart) * m_channelCount * sizeof(float));
        }
        m_writeFrames.store(write + n, std::memory_order_release);
        return n;
    }

    /**
     * @brief 读出交织 PCM 帧（仅消费者调用），数据不足时读出可用部分，绝不阻塞。
     * @param interleaved 目标缓冲，至少 frames * channels 个 float。
     * @param frames 期望读出的帧数。
     * @return 实际读出的帧数。
     */
    size_t Read(float* interleaved, size_t frames)
    {
        const size_t write = m_writeFrames.load(std::memory_order_acquire);
        const size_t read = m_readFrames.load(std::memory_order_relaxed);
        const size_t available = write - read;
        const size_t n = std::min(frames, available);
        if (n == 0)
        {
            return 0;
        }
        const size_t start = read % m_capacityFrames;
        const size_t firstPart = std::min(n, m_capacityFrames - start);
        std::memcpy(interleaved,
                    m_buffer.data() + start * m_channelCount,
                    firstPart * m_channelCount * sizeof(float));
        if (n > firstPart)
        {
            std::memcpy(interleaved + firstPart * m_channelCount,
                        m_buffer.data(),
                        (n - firstPart) * m_channelCount * sizeof(float));
        }
        m_readFrames.store(read + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> m_buffer;              ///< 预分配的交织样本存储。
    size_t m_capacityFrames = 1;              ///< 容量（帧数）。
    size_t m_channelCount = 1;                ///< 交织声道数。
    std::atomic<size_t> m_writeFrames{0};     ///< 已写入总帧数（单调递增，生产者更新）。
    std::atomic<size_t> m_readFrames{0};      ///< 已读出总帧数（单调递增，消费者更新）。
};

/**
 * @brief 流式 Voice 的共享状态。
 *
 * 由 AudioManager 在 Play 时创建（shared_ptr），voices 表与后台解码线程
 * 各持有一份引用；任一侧释放后由另一侧安全销毁，保证解码器与环形缓冲
 * 不会在实时线程上被析构时仍被生产者访问。
 *
 * 线程分工：
 * - decoder / decoderOpened / decoderFailed 仅由解码线程访问；
 * - ring 由解码线程写入、音频回调线程读出（SPSC）；
 * - 原子标志用于跨线程通知（loop 由模拟线程写、stopRequested 由
 *   模拟/回调线程写、endOfStream 由解码线程写）。
 */
struct AudioStreamingVoice
{
    sk_sp<RuntimeAudio> audio;              ///< 流式音频资产（保证编码字节存活）。
    AudioStreamDecoder decoder;             ///< 每 Voice 独立的解码器实例（仅解码线程使用）。
    AudioSpscRing ring;                     ///< 预取环形缓冲（解码线程写、回调线程读）。
    std::atomic<bool> loop{false};          ///< 是否循环播放（解码线程在流尾据此 seek 回起点）。
    std::atomic<bool> stopRequested{false}; ///< Voice 已被移除，解码线程应停止服务并释放引用。
    std::atomic<bool> endOfStream{false};   ///< 解码线程声明不再产出新数据（消费端排空后即结束）。
    bool decoderOpened = false;             ///< 解码器是否已完成首次打开（仅解码线程访问）。
    bool decoderFailed = false;             ///< 解码器是否发生不可恢复错误（仅解码线程访问）。
};

#endif
