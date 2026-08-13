#ifndef RUNTIMEAUDIO_H
#define RUNTIMEAUDIO_H

#include "IRuntimeAsset.h"
#include <cstdint>
#include <vector>

/**
 * @brief 表示运行时音频数据的类。
 *
 * 该类继承自 IRuntimeAsset，支持两种存储模式：
 * - 全量模式：整段音频解码为 PCM 常驻内存（低延迟，适合短音效）；
 * - 流式模式：仅保留原始编码字节，播放时由各 Voice 持有的解码器按需解码
 *   （适合 BGM 等长音频，避免解码后的大块 PCM 常驻内存）。
 */
class RuntimeAudio : public IRuntimeAsset
{
public:
    /**
     * @brief 默认构造函数。
     */
    RuntimeAudio() = default;

    /**
     * @brief 设置PCM音频数据、采样率和声道数（全量模式）。
     * @param data PCM数据向量，使用右值引用进行高效移动。
     * @param sampleRate 音频的采样率（每秒样本数）。
     * @param channels 音频的声道数。
     */
    void SetPCMData(std::vector<float>&& data, int sampleRate, int channels)
    {
        pcmData = std::move(data);
        encodedData.clear();
        streaming = false;
        streamFrameCount = 0;
        this->sampleRate = sampleRate;
        this->channels = channels;
    }

    /**
     * @brief 设置原始编码字节（流式模式）。
     *
     * 流式模式下不做全量解码；sampleRate 与 channels 描述的是解码器
     * 将要产出的目标 PCM 格式（由加载器统一指定），而非源文件参数。
     *
     * @param data 原始编码音频字节（完整文件内容），使用右值引用进行高效移动。
     * @param sampleRate 解码输出的目标采样率。
     * @param channels 解码输出的目标声道数。
     * @param estimatedFrames 按容器时长估算的总帧数，未知时传 0。
     */
    void SetEncodedData(std::vector<uint8_t>&& data, int sampleRate, int channels, size_t estimatedFrames)
    {
        encodedData = std::move(data);
        pcmData.clear();
        streaming = true;
        streamFrameCount = estimatedFrames;
        this->sampleRate = sampleRate;
        this->channels = channels;
    }

    /**
     * @brief 查询本资产是否为流式模式。
     * @return 流式模式返回 true，全量 PCM 模式返回 false。
     */
    bool IsStreaming() const { return streaming; }

    /**
     * @brief 获取原始编码字节（仅流式模式下非空）。
     * @return 编码字节向量的常量引用。
     */
    const std::vector<uint8_t>& GetEncodedData() const { return encodedData; }

    /**
     * @brief 获取PCM音频数据（仅全量模式下非空）。
     * @return PCM数据向量的常量引用。
     */
    const std::vector<float>& GetPCMData() const { return pcmData; }

    /**
     * @brief 获取音频的采样率。
     * @return 音频的采样率。
     */
    int GetSampleRate() const { return sampleRate; }

    /**
     * @brief 获取音频的声道数。
     * @return 音频的声道数。
     */
    int GetChannels() const { return channels; }

    /**
     * @brief 获取音频的帧数。
     *
     * 全量模式下为精确值；流式模式下为按容器时长估算的值，未知时为0。
     *
     * @return 音频的帧数。如果声道数为0，则返回0。
     */
    size_t GetFrameCount() const
    {
        if (streaming)
        {
            return streamFrameCount;
        }
        return channels > 0 ? (pcmData.size() / static_cast<size_t>(channels)) : 0;
    }

    /**
     * @brief 获取音频的总时长（秒）。
     * @return 音频的总时长（秒）。如果采样率为0，则返回0.0f。
     */
    float GetDurationSeconds() const
    {
        size_t frames = GetFrameCount();
        return sampleRate > 0 ? (static_cast<float>(frames) / static_cast<float>(sampleRate)) : 0.0f;
    }

private:
    std::vector<float> pcmData;        ///< 存储PCM音频数据的向量（全量模式）。
    std::vector<uint8_t> encodedData;  ///< 原始编码字节（流式模式）。
    size_t streamFrameCount = 0;       ///< 流式模式下按时长估算的总帧数（0 表示未知）。
    bool streaming = false;            ///< 是否为流式模式。
    int sampleRate = 0;                ///< 音频的采样率（流式模式下为解码输出的目标采样率）。
    int channels = 0;                  ///< 音频的声道数（流式模式下为解码输出的目标声道数）。
};

#endif