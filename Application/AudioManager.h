#ifndef AUDIOMANAGER_H
#define AUDIOMANAGER_H
#include <SDL3/SDL.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../Utils/LazySingleton.h"
#include "../Utils/Guid.h"
#include "../Resources/RuntimeAsset/RuntimeAudio.h"
#include "AudioStreaming.h"
class AudioManager : public LazySingleton<AudioManager>
{
public:
    friend class LazySingleton<AudioManager>;
    struct PlayDesc
    {
        sk_sp<RuntimeAudio> audio; 
        bool loop = false;         
        float volume = 1.0f;       
        bool spatial = false;      
        float sourceX = 0.0f;      
        float sourceY = 0.0f;      
        float sourceZ = 0.0f;      
        float minDistance = 1.0f;  
        float maxDistance = 30.0f; 
        float rolloffFactor = 1.0f;
        int rolloffMode = 0; // 0=Linear, 1=Logarithmic
    };
public:
    bool Initialize(int sampleRate = 48000, int channels = 2);
    void Shutdown();
    uint32_t Play(const PlayDesc& desc);
    void Stop(uint32_t voiceId);
    void StopAll();
    bool IsFinished(uint32_t voiceId) const;
    void SetVolume(uint32_t voiceId, float volume);
    void SetLoop(uint32_t voiceId, bool loop);
    int GetSampleRate() const { return m_sampleRate; }
    int GetChannels() const { return m_channels; }
    void SetVoicePosition(uint32_t voiceId, float x, float y, float z);
    void SetVoiceSpatial(uint32_t voiceId, bool spatial, float minD, float maxD);
    void Mix(float* out, int frames);

    /**
     * @brief 由游戏/模拟线程周期性更新监听者位姿。
     *
     * 音频回调线程只读取这里缓存的原子值，不再跨线程访问 CameraManager。
     */
    void UpdateListener(float x, float y, float rotation)
    {
        m_listenerX.store(x, std::memory_order_relaxed);
        m_listenerY.store(y, std::memory_order_relaxed);
        m_listenerRot.store(rotation, std::memory_order_relaxed);
    }
private:
    AudioManager() = default;
    ~AudioManager() override;
    struct Voice
    {
        uint32_t id = 0;             
        sk_sp<RuntimeAudio> audio;   
        size_t cursorFrames = 0;     
        bool loop = false;           
        float volume = 1.0f;         
        bool spatial = false;        
        float x = 0.0f, y = 0.0f, z = 0.0f; 
        float minDistance = 1.0f;    
        float maxDistance = 30.0f;   
        float rolloffFactor = 1.0f;
        int rolloffMode = 0;
        bool finished = false;       
        std::shared_ptr<AudioStreamingVoice> stream; ///< 流式播放状态（全量 PCM voice 为空）。
    };
    static void SDLAudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
    void GetListener(float& lx, float& ly, float& lz, float& rx, float& ry, float& rz) const;
    void DecodeThreadMain();
    void StopDecodeThread();
private:
    SDL_AudioDeviceID deviceId = 0; 
    int m_sampleRate = 48000;       
    int m_channels = 2;             
    mutable std::mutex mutex;      
    std::unordered_map<uint32_t, Voice> voices; 
    uint32_t nextVoiceId = 1;       
    float masterVolume = 1.0f;      
    SDL_AudioStream* m_audioStream = nullptr; 
    std::vector<float> m_mixBuffer; ///< 回调线程复用的混音缓冲，避免实时线程内反复堆分配。
    std::vector<float> m_streamReadBuffer;    ///< 回调线程复用：流式 voice 从环形缓冲读出的暂存区。
    std::vector<uint32_t> m_finishedScratch;  ///< 回调线程复用：单次 Mix 中待移除 voice 的 id 列表。
    std::atomic<float> m_listenerX{0.0f};   ///< 监听者位置 X（由模拟线程更新）。
    std::atomic<float> m_listenerY{0.0f};   ///< 监听者位置 Y（由模拟线程更新）。
    std::atomic<float> m_listenerRot{0.0f}; ///< 监听者朝向（弧度，由模拟线程更新）。
    std::thread m_decodeThread;               ///< 后台流式解码线程（AudioManager 独占持有）。
    std::atomic<bool> m_decodeRunning{false}; ///< 解码线程运行标志（false 后线程尽快退出）。
    std::mutex m_streamMutex;                 ///< 保护 m_streamingVoices 注册表（仅短暂持有）。
    std::condition_variable m_streamCv;       ///< 唤醒解码线程（新 voice 加入或请求退出）。
    std::vector<std::shared_ptr<AudioStreamingVoice>> m_streamingVoices; ///< 解码线程服务的流式 voice 注册表。
};
#endif
