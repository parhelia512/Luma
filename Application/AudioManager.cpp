#include "AudioManager.h"
#include "../Utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <vector>
#include <cmath>

namespace
{
    constexpr double kStreamRingSeconds = 1.0;  ///< 每个流式 voice 预取缓冲的时长（秒）。
    constexpr int kDecodeChunkFrames = 4096;    ///< 解码线程单次向解码器请求的帧数。
    constexpr std::chrono::milliseconds kDecodePollInterval{10}; ///< 解码线程水位轮询周期。

    /**
     * @brief 解码线程侧：为单个流式 voice 检查水位并补充环形缓冲。
     *
     * 仅在解码线程调用。缓冲低于半满水位时解码补充直至填满；
     * 流尾根据 loop 标志决定回到起点继续或声明 endOfStream。
     *
     * @param sv 流式 voice 共享状态。
     * @param scratch 解码线程私有的临时 PCM 缓冲（按需扩容，复用避免反复分配）。
     */
    void ServiceStreamingVoice(AudioStreamingVoice& sv, std::vector<float>& scratch)
    {
        if (sv.stopRequested.load(std::memory_order_relaxed) || sv.decoderFailed)
        {
            return;
        }
        if (!sv.decoderOpened)
        {
            const std::vector<uint8_t>& bytes = sv.audio->GetEncodedData();
            if (!sv.decoder.Open(bytes.data(), bytes.size(),
                                 sv.audio->GetSampleRate(), sv.audio->GetChannels()))
            {
                sv.decoderFailed = true;
                sv.endOfStream.store(true, std::memory_order_release);
                LogError("AudioManager: Failed to open streaming decoder for voice.");
                return;
            }
            sv.decoderOpened = true;
        }
        if (sv.endOfStream.load(std::memory_order_relaxed))
        {
            // 流已结束后 loop 被重新打开：回到起点恢复供给
            if (!sv.loop.load(std::memory_order_relaxed))
            {
                return;
            }
            if (!sv.decoder.Restart())
            {
                sv.decoderFailed = true;
                return;
            }
            sv.endOfStream.store(false, std::memory_order_release);
        }
        // 高于半满水位则无需补充
        if (sv.ring.AvailableFrames() * 2 >= sv.ring.CapacityFrames())
        {
            return;
        }
        const int channels = sv.audio->GetChannels();
        while (!sv.stopRequested.load(std::memory_order_relaxed))
        {
            const size_t freeFrames = sv.ring.FreeFrames();
            if (freeFrames == 0)
            {
                break;
            }
            const int want = static_cast<int>(std::min(freeFrames, static_cast<size_t>(kDecodeChunkFrames)));
            const size_t neededSamples = static_cast<size_t>(want) * static_cast<size_t>(channels);
            if (scratch.size() < neededSamples)
            {
                scratch.resize(neededSamples);
            }
            const int got = sv.decoder.ReadFrames(scratch.data(), want);
            if (got > 0)
            {
                sv.ring.Write(scratch.data(), static_cast<size_t>(got));
                continue;
            }
            if (got == 0)
            {
                // 到达流尾：循环则 seek 回起点继续填充，否则声明不再产出
                if (sv.loop.load(std::memory_order_relaxed))
                {
                    if (sv.decoder.Restart())
                    {
                        continue;
                    }
                    sv.decoderFailed = true;
                }
                sv.endOfStream.store(true, std::memory_order_release);
                break;
            }
            sv.decoderFailed = true;
            sv.endOfStream.store(true, std::memory_order_release);
            break;
        }
    }

    /// @brief 请求流式 voice 停止：解码线程将在下个轮询周期释放其引用。
    inline void RequestStreamStop(const std::shared_ptr<AudioStreamingVoice>& stream)
    {
        if (stream)
        {
            stream->stopRequested.store(true, std::memory_order_relaxed);
        }
    }
}

AudioManager::~AudioManager()
{
    // 兜底：即使 Shutdown 未被调用，也保证解码线程被干净 join
    StopDecodeThread();
}

bool AudioManager::Initialize(int sampleRate, int channels)
{
    if (m_audioStream)
    {
        return true;
    }
    SDL_AudioSpec spec{};
    spec.freq = sampleRate;
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<Uint8>(channels);
    m_audioStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        &AudioManager::SDLAudioCallback,
        this
    );
    if (m_audioStream == nullptr)
    {
        LogError("AudioManager: Failed to open audio device stream: {}", SDL_GetError());
        return false;
    }
    m_sampleRate = spec.freq;
    m_channels = spec.channels;
    // 预分配混音缓冲（按 1 秒封顶的常见回调块大小预留），避免首次回调内分配
    m_mixBuffer.resize(static_cast<size_t>(m_sampleRate / 10) * m_channels);
    m_streamReadBuffer.resize(m_mixBuffer.size());
    m_finishedScratch.reserve(64);
    // 启动后台流式解码线程（专职维护各流式 voice 的预取缓冲水位）
    m_decodeRunning.store(true, std::memory_order_release);
    m_decodeThread = std::thread(&AudioManager::DecodeThreadMain, this);
    SDL_ResumeAudioStreamDevice(m_audioStream);
    LogInfo("AudioManager: Initialized. {} Hz, channels {}", m_sampleRate, m_channels);
    return true;
}
void AudioManager::Shutdown()
{
    // 先停设备回调，再停解码线程，最后清理 voice 状态
    if (m_audioStream)
    {
        SDL_DestroyAudioStream(m_audioStream);
        m_audioStream = nullptr;
    }
    StopDecodeThread();
    {
        std::scoped_lock lk(mutex);
        for (auto& kv : voices)
        {
            RequestStreamStop(kv.second.stream);
        }
        voices.clear();
        nextVoiceId = 1;
    }
    std::scoped_lock slk(m_streamMutex);
    m_streamingVoices.clear();
}
void AudioManager::StopDecodeThread()
{
    m_decodeRunning.store(false, std::memory_order_release);
    m_streamCv.notify_all();
    if (m_decodeThread.joinable())
    {
        m_decodeThread.join();
    }
}
void AudioManager::DecodeThreadMain()
{
    // 解码线程私有缓存：shared_ptr 快照与解码暂存区（本线程允许堆分配）
    std::vector<std::shared_ptr<AudioStreamingVoice>> snapshot;
    std::vector<float> scratch(static_cast<size_t>(kDecodeChunkFrames) * 2, 0.0f);
    while (m_decodeRunning.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lk(m_streamMutex);
            m_streamCv.wait_for(lk, kDecodePollInterval);
            if (!m_decodeRunning.load(std::memory_order_acquire))
            {
                break;
            }
            // 清理已停止的 voice（释放解码器与环形缓冲），再快照存活列表
            std::erase_if(m_streamingVoices,
                          [](const std::shared_ptr<AudioStreamingVoice>& s)
                          {
                              return s->stopRequested.load(std::memory_order_relaxed);
                          });
            snapshot.assign(m_streamingVoices.begin(), m_streamingVoices.end());
        }
        // 锁外解码：不阻塞 Play/Stop，也绝不让音频回调等待本线程
        for (const std::shared_ptr<AudioStreamingVoice>& sv : snapshot)
        {
            ServiceStreamingVoice(*sv, scratch);
        }
        snapshot.clear();
    }
}
uint32_t AudioManager::Play(const PlayDesc& desc)
{
    if (!m_audioStream || !desc.audio)
    {
        return 0;
    }
    // 流式资产：先在调用方线程完成状态与缓冲的预分配（实时线程零分配）
    std::shared_ptr<AudioStreamingVoice> streamState;
    if (desc.audio->IsStreaming())
    {
        const int srcRate = desc.audio->GetSampleRate();
        const int srcChannels = desc.audio->GetChannels();
        if (srcRate <= 0 || srcChannels <= 0 || desc.audio->GetEncodedData().empty())
        {
            LogError("AudioManager: Invalid streaming audio asset.");
            return 0;
        }
        streamState = std::make_shared<AudioStreamingVoice>();
        streamState->audio = desc.audio;
        streamState->loop.store(desc.loop, std::memory_order_relaxed);
        const size_t capacityFrames =
            static_cast<size_t>(static_cast<double>(srcRate) * kStreamRingSeconds);
        streamState->ring.Reset(capacityFrames, srcChannels);
    }
    uint32_t id = 0;
    {
        std::scoped_lock lk(mutex);
        Voice v;
        v.id = nextVoiceId++;
        v.audio = desc.audio;
        v.cursorFrames = 0;
        v.loop = desc.loop;
        v.volume = std::clamp(desc.volume, 0.0f, 1.0f);
        v.spatial = desc.spatial;
        v.x = desc.sourceX;
        v.y = desc.sourceY;
        v.z = desc.sourceZ;
        v.minDistance = std::max(0.001f, desc.minDistance);
        v.maxDistance = std::max(v.minDistance, desc.maxDistance);
        v.rolloffFactor = std::max(0.0f, desc.rolloffFactor);
        v.rolloffMode = desc.rolloffMode;
        v.finished = false;
        v.stream = streamState;
        id = v.id;
        voices[v.id] = std::move(v);
    }
    if (streamState)
    {
        // 注册给解码线程并立即唤醒，尽快完成首次预取
        {
            std::scoped_lock slk(m_streamMutex);
            m_streamingVoices.push_back(std::move(streamState));
        }
        m_streamCv.notify_one();
    }
    return id;
}
void AudioManager::Stop(uint32_t voiceId)
{
    std::scoped_lock lk(mutex);
    auto it = voices.find(voiceId);
    if (it != voices.end())
    {
        RequestStreamStop(it->second.stream);
        voices.erase(it);
    }
}
void AudioManager::StopAll()
{
    std::scoped_lock lk(mutex);
    for (auto& kv : voices)
    {
        RequestStreamStop(kv.second.stream);
    }
    voices.clear();
}
bool AudioManager::IsFinished(uint32_t voiceId) const
{
    std::scoped_lock lk(mutex);
    auto it = voices.find(voiceId);
    if (it == voices.end()) return true;
    return it->second.finished;
}
void AudioManager::SetVolume(uint32_t voiceId, float volume)
{
    std::scoped_lock lk(mutex);
    auto it = voices.find(voiceId);
    if (it != voices.end()) it->second.volume = std::clamp(volume, 0.0f, 1.0f);
}
void AudioManager::SetLoop(uint32_t voiceId, bool loop)
{
    std::scoped_lock lk(mutex);
    auto it = voices.find(voiceId);
    if (it != voices.end())
    {
        it->second.loop = loop;
        // 流式 voice 的循环由解码线程执行（流尾 seek 回起点），需同步原子标志
        if (it->second.stream)
        {
            it->second.stream->loop.store(loop, std::memory_order_relaxed);
        }
    }
}
void AudioManager::SetVoicePosition(uint32_t voiceId, float x, float y, float z)
{
    std::scoped_lock lk(mutex);
    auto it = voices.find(voiceId);
    if (it != voices.end())
    {
        it->second.x = x;
        it->second.y = y;
        it->second.z = z;
    }
}
void AudioManager::SetVoiceSpatial(uint32_t voiceId, bool spatial, float minD, float maxD)
{
    std::scoped_lock lk(mutex);
    auto it = voices.find(voiceId);
    if (it != voices.end())
    {
        it->second.spatial = spatial;
        it->second.minDistance = std::max(0.001f, minD);
        it->second.maxDistance = std::max(it->second.minDistance, maxD);
    }
}
void AudioManager::SDLAudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    auto* self = static_cast<AudioManager*>(userdata);
    const int frames_needed = total_amount / (sizeof(float) * self->m_channels);
    if (frames_needed <= 0)
    {
        return;
    }
    // 复用成员缓冲：仅在需求增长时扩容一次，稳态下回调内零堆分配
    const size_t samplesNeeded = static_cast<size_t>(frames_needed) * self->m_channels;
    if (self->m_mixBuffer.size() < samplesNeeded)
    {
        self->m_mixBuffer.resize(samplesNeeded);
    }
    self->Mix(self->m_mixBuffer.data(), frames_needed);
    SDL_PutAudioStreamData(stream, self->m_mixBuffer.data(), total_amount);
}
void AudioManager::GetListener(float& lx, float& ly, float& lz, float& rx, float& ry, float& rz) const
{
    // 只读原子缓存，不跨线程访问 CameraManager（相机状态由模拟线程通过 UpdateListener 喂入）
    lx = m_listenerX.load(std::memory_order_relaxed);
    ly = m_listenerY.load(std::memory_order_relaxed);
    lz = 0.0f;
    const float theta = m_listenerRot.load(std::memory_order_relaxed);
    rx = std::cos(theta);
    ry = std::sin(theta);
    rz = 0.0f;
}
void AudioManager::Mix(float* out, int frames)
{
    const int ch = m_channels;
    std::fill_n(out, static_cast<size_t>(frames) * ch, 0.0f);
    float lx = 0, ly = 0, lz = 0, rx = 1, ry = 0, rz = 0;
    GetListener(lx, ly, lz, rx, ry, rz);
    std::scoped_lock lk(mutex);
    m_finishedScratch.clear();
    for (auto& kv : voices)
    {
        Voice& v = kv.second;
        if (!v.audio)
        {
            m_finishedScratch.push_back(v.id);
            continue;
        }
        const int srcCh = v.audio->GetChannels();
        if (srcCh <= 0)
        {
            m_finishedScratch.push_back(v.id);
            continue;
        }
        float gainL = v.volume * masterVolume;
        float gainR = v.volume * masterVolume;
        if (v.spatial)
        {
            float dx = v.x - lx, dy = v.y - ly, dz = v.z - lz;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float att;
            if (v.rolloffMode == 1)
            {
                float d = std::max(dist, v.minDistance);
                att = v.minDistance / (v.minDistance + v.rolloffFactor * (d - v.minDistance));
                if (dist > v.maxDistance) att = 0.0f;
            }
            else
            {
                dist = std::max(v.minDistance, std::min(dist, v.maxDistance));
                float range = v.maxDistance - v.minDistance;
                att = 1.0f - v.rolloffFactor * (dist - v.minDistance) / std::max(0.001f, range);
            }
            att = std::clamp(att, 0.0f, 1.0f);
            float dotR = (dx * rx + dy * ry + dz * rz);
            float pan = std::clamp(dist > 0.0f ? (dotR / dist) : 0.0f, -1.0f, 1.0f);
            float panL = (pan <= 0.0f) ? 1.0f : 1.0f - pan;
            float panR = (pan >= 0.0f) ? 1.0f : 1.0f + pan;
            gainL *= att * panL;
            gainR *= att * panR;
        }
        if (v.stream)
        {
            // 流式路径：只从预取环形缓冲消费，不做 IO/解码/加锁等待。
            // 缓冲不足时剩余部分保持静音（输出已预置零），绝不阻塞。
            AudioStreamingVoice& sv = *v.stream;
            const size_t samplesNeeded = static_cast<size_t>(frames) * static_cast<size_t>(srcCh);
            if (m_streamReadBuffer.size() < samplesNeeded)
            {
                m_streamReadBuffer.resize(samplesNeeded);
            }
            const size_t gotFrames = sv.ring.Read(m_streamReadBuffer.data(), static_cast<size_t>(frames));
            for (size_t f = 0; f < gotFrames; ++f)
            {
                const size_t base = f * static_cast<size_t>(srcCh);
                float sL = 0.0f, sR = 0.0f;
                if (srcCh == 1)
                {
                    sL = sR = m_streamReadBuffer[base];
                }
                else
                {
                    sL = m_streamReadBuffer[base + 0];
                    sR = m_streamReadBuffer[base + 1];
                }
                const size_t outBase = f * static_cast<size_t>(ch);
                out[outBase + 0] += sL * gainL;
                if (ch > 1) out[outBase + 1] += sR * gainR;
            }
            v.cursorFrames += gotFrames;
            // 仅当生产端已声明流结束（先读原子标志再确认缓冲已排空）才判定播放完成
            if (gotFrames < static_cast<size_t>(frames) &&
                sv.endOfStream.load(std::memory_order_acquire) &&
                sv.ring.AvailableFrames() == 0)
            {
                v.finished = true;
                m_finishedScratch.push_back(v.id);
            }
            continue;
        }
        const auto& pcm = v.audio->GetPCMData();
        const size_t totalFrames = v.audio->GetFrameCount();
        if (totalFrames == 0)
        {
            m_finishedScratch.push_back(v.id);
            continue;
        }
        for (int f = 0; f < frames; ++f)
        {
            if (v.cursorFrames >= totalFrames)
            {
                if (v.loop)
                {
                    v.cursorFrames = 0;
                }
                else
                {
                    v.finished = true;
                    m_finishedScratch.push_back(v.id);
                    break;
                }
            }
            if (v.cursorFrames >= totalFrames) break;
            const size_t base = v.cursorFrames * static_cast<size_t>(srcCh);
            float sL = 0.0f, sR = 0.0f;
            if (srcCh == 1)
            {
                float s = pcm[base];
                sL = sR = s;
            }
            else
            {
                sL = pcm[base + 0];
                sR = pcm[base + 1];
            }
            size_t outBase = static_cast<size_t>(f) * ch;
            out[outBase + 0] += sL * gainL;
            if (ch > 1) out[outBase + 1] += sR * gainR;
            v.cursorFrames++;
        }
    }
    for (uint32_t id : m_finishedScratch)
    {
        auto it = voices.find(id);
        if (it == voices.end()) continue;
        // 流式 voice：通知解码线程释放解码器与缓冲（析构发生在解码线程，不在实时线程）
        RequestStreamStop(it->second.stream);
        voices.erase(it);
    }
}
