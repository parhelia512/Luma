#include "AudioLoader.h"
#include "AudioStreamDecoder.h"
#include "../AssetManager.h"
#include "../../Utils/Logger.h"

#include <cstdint>
#include <vector>

namespace
{
    /// 解码后 PCM 尺寸不超过该阈值的音频保持全量常驻（48kHz 立体声 float 约合 27 秒）。
    constexpr size_t kMaxResidentPcmBytes = 10ull * 1024ull * 1024ull;

    /// 容器未报告时长时的保守回退：编码字节超过该值即走流式（压缩格式按 20:1 估算仍低于常驻阈值）。
    constexpr size_t kUnknownDurationEncodedLimit = 256ull * 1024ull;

    /// 全量解码时单次向解码器请求的帧数。
    constexpr int kFullDecodeChunkFrames = 4096;
}

sk_sp<RuntimeAudio> AudioLoader::LoadAsset(const AssetMetadata& metadata)
{
    return LoadInternal(metadata);
}

sk_sp<RuntimeAudio> AudioLoader::LoadAsset(const Guid& guid)
{
    const AssetMetadata* meta = AssetManager::GetInstance().GetMetadata(guid);
    if (!meta || meta->type != AssetType::Audio) return nullptr;
    return LoadInternal(*meta);
}

sk_sp<RuntimeAudio> AudioLoader::LoadInternal(const AssetMetadata& meta) const
{
    if (!meta.importerSettings["encodedData"])
    {
        LogError("AudioLoader: No encodedData for asset {}", meta.assetPath.string());
        return nullptr;
    }

    YAML::Binary bin = meta.importerSettings["encodedData"].as<YAML::Binary>();
    if (bin.size() == 0)
    {
        LogError("AudioLoader: Empty encodedData for asset {}", meta.assetPath.string());
        return nullptr;
    }

    const int outChannels = (targetChannels == 1) ? 1 : 2;

    AudioStreamDecoder decoder;
    if (!decoder.Open(bin.data(), bin.size(), targetSampleRate, outChannels))
    {
        LogError("AudioLoader: Failed to open decoder for asset {}", meta.assetPath.string());
        return nullptr;
    }

    // 按容器报告的时长估算解码后 PCM 尺寸，决定全量常驻还是流式
    const double durationSec = decoder.DurationSeconds();
    bool useStreaming;
    if (durationSec > 0.0)
    {
        const double estimatedBytes = durationSec * static_cast<double>(targetSampleRate) *
            static_cast<double>(outChannels) * static_cast<double>(sizeof(float));
        useStreaming = estimatedBytes > static_cast<double>(kMaxResidentPcmBytes);
    }
    else
    {
        useStreaming = bin.size() > kUnknownDurationEncodedLimit;
    }

    auto ra = sk_make_sp<RuntimeAudio>();

    if (useStreaming)
    {
        decoder.Close();
        std::vector<uint8_t> encoded(bin.data(), bin.data() + bin.size());
        const size_t estimatedFrames = durationSec > 0.0
                                           ? static_cast<size_t>(durationSec * static_cast<double>(targetSampleRate))
                                           : 0;
        ra->SetEncodedData(std::move(encoded), targetSampleRate, outChannels, estimatedFrames);
        return ra;
    }

    // 全量解码路径：循环读帧直至流结束
    std::vector<float> outPCM;
    if (durationSec > 0.0)
    {
        outPCM.reserve(static_cast<size_t>(durationSec * static_cast<double>(targetSampleRate)) *
            static_cast<size_t>(outChannels));
    }
    else
    {
        outPCM.reserve(1024 * 1024);
    }

    std::vector<float> chunk(static_cast<size_t>(kFullDecodeChunkFrames) * static_cast<size_t>(outChannels));
    for (;;)
    {
        const int got = decoder.ReadFrames(chunk.data(), kFullDecodeChunkFrames);
        if (got <= 0)
        {
            if (got < 0)
            {
                LogError("AudioLoader: Decode error in asset {}", meta.assetPath.string());
            }
            break;
        }
        outPCM.insert(outPCM.end(), chunk.data(),
                      chunk.data() + static_cast<size_t>(got) * static_cast<size_t>(outChannels));
    }

    if (outPCM.empty()) return nullptr;

    ra->SetPCMData(std::move(outPCM), targetSampleRate, outChannels);
    return ra;
}
