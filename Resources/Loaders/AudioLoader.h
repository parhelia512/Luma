#ifndef AUDIOLOADER_H
#define AUDIOLOADER_H

#include "IAssetLoader.h"
#include "yaml-cpp/yaml.h"
#include "../RuntimeAsset/RuntimeAudio.h"

/**
 * @brief 音频资产加载器。
 *
 * 负责从各种源加载音频资产并将其转换为运行时可用的 RuntimeAudio 对象。
 * 根据估算的解码后 PCM 尺寸自动选择存储模式（对调用方透明）：
 * - 小于阈值：全量解码为 PCM 常驻内存（低延迟音效）；
 * - 大于阈值：仅保留原始编码字节，播放时按需流式解码（BGM 等长音频）。
 */
class AudioLoader : public IAssetLoader<RuntimeAudio>
{
public:
    /**
     * @brief 构造一个 AudioLoader 实例。
     * @param targetSampleRate 目标采样率，默认为 48000 Hz。
     * @param targetChannels 目标通道数，默认为 2 (立体声)。
     */
    explicit AudioLoader(int targetSampleRate = 48000, int targetChannels = 2)
        : targetSampleRate(targetSampleRate), targetChannels(targetChannels)
    {
    }

    /**
     * @brief 根据资产元数据加载音频资产。
     * @param metadata 包含资产信息的元数据。
     * @return 一个指向加载的 RuntimeAudio 对象的智能指针。
     */
    sk_sp<RuntimeAudio> LoadAsset(const AssetMetadata& metadata) override;

    /**
     * @brief 根据全局唯一标识符 (GUID) 加载音频资产。
     * @param guid 资产的全局唯一标识符。
     * @return 一个指向加载的 RuntimeAudio 对象的智能指针。
     */
    sk_sp<RuntimeAudio> LoadAsset(const Guid& guid) override;

private:
    // 私有方法不添加 Doxygen 注释，因为任务要求只为公开函数添加。
    sk_sp<RuntimeAudio> LoadInternal(const AssetMetadata& meta) const;

private:
    int targetSampleRate; ///< 目标采样率。
    int targetChannels;   ///< 目标通道数。
};

#endif