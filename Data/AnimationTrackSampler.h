#ifndef LUMAENGINE_ANIMATIONTRACKSAMPLER_H
#define LUMAENGINE_ANIMATIONTRACKSAMPLER_H
#include <algorithm>
#include <any>
#include <cmath>
#include <string>
#include <vector>
#include <entt/entt.hpp>

#include "AnimationClip.h"
#include "ComponentRegistry.h"
#include "Core.h"
#include "Event/EventBus.h"
#include "Event/Events.h"

/**
 * @brief 属性轨道的求值与反射写回工具。
 *
 * 运行时播放（RuntimeAnimationController）与编辑器预览（AnimationEditorPanel）
 * 共用同一份逻辑，保证两边采样结果一致。
 */
namespace AnimationTrackSampler
{
    /// 将属性路径拆分为基础属性名与分量后缀（"position.x" → "position" + "x"；无后缀时分量为空）。
    inline void SplitPropertyPath(const std::string& path, std::string& baseName, std::string& channel)
    {
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos)
        {
            baseName = path;
            channel.clear();
        }
        else
        {
            baseName = path.substr(0, dot);
            channel = path.substr(dot + 1);
        }
    }

    /// 保证关键帧按帧号升序（求值二分查找的前提）。
    inline void SortKeys(PropertyTrack& track)
    {
        std::sort(track.keyframes.begin(), track.keyframes.end(),
                  [](const PropertyKey& a, const PropertyKey& b) { return a.frame < b.frame; });
    }

    /**
     * @brief 求轨道在指定帧时刻的值。
     * @param track 轨道，关键帧需按帧号升序。
     * @param frameTime 以帧为单位的连续时间（可为小数）。
     * @return 曲线值；首尾之外的时刻按端点值钳制。
     */
    inline float Evaluate(const PropertyTrack& track, float frameTime)
    {
        const auto& keys = track.keyframes;
        if (keys.empty())
        {
            return 0.0f;
        }
        if (frameTime <= static_cast<float>(keys.front().frame))
        {
            return keys.front().value;
        }
        if (frameTime >= static_cast<float>(keys.back().frame))
        {
            return keys.back().value;
        }
        // 二分定位区间：right 为第一个帧号大于 frameTime 的关键帧
        const auto right = std::upper_bound(keys.begin(), keys.end(), frameTime,
                                            [](float t, const PropertyKey& k)
                                            {
                                                return t < static_cast<float>(k.frame);
                                            });
        const PropertyKey& k1 = *right;
        const PropertyKey& k0 = *(right - 1);
        const float span = static_cast<float>(k1.frame - k0.frame);
        if (span <= 0.0f)
        {
            return k1.value;
        }
        const float t = (frameTime - static_cast<float>(k0.frame)) / span;
        switch (k0.interp)
        {
        case AnimInterpMode::Step:
            return k0.value;
        case AnimInterpMode::CubicBezier:
            {
                // Hermite 基函数；切线单位为 值/帧，乘区间长度换算到归一化参数域
                const float t2 = t * t;
                const float t3 = t2 * t;
                const float m0 = k0.outTangent * span;
                const float m1 = k1.inTangent * span;
                return (2.0f * t3 - 3.0f * t2 + 1.0f) * k0.value
                    + (t3 - 2.0f * t2 + t) * m0
                    + (-2.0f * t3 + 3.0f * t2) * k1.value
                    + (t3 - t2) * m1;
            }
        case AnimInterpMode::Linear:
        default:
            return k0.value + (k1.value - k0.value) * t;
        }
    }

    /// 在组件注册信息中按名称查找属性。
    inline const PropertyRegistration* FindProperty(const ComponentRegistration* compInfo, const std::string& name)
    {
        if (!compInfo)
        {
            return nullptr;
        }
        for (const auto& prop : compInfo->properties)
        {
            if (prop.name == name)
            {
                return &prop;
            }
        }
        return nullptr;
    }

    /**
     * @brief 通过属性反射读取 float 标量的当前值。
     *
     * 支持 float、int 以及 Vector2f（.x/.y）、Color（.r/.g/.b/.a）的分量。
     * @return 属性存在且类型受支持时返回 true。
     */
    inline bool GetValue(entt::registry& registry, entt::entity entity,
                         const std::string& componentName, const std::string& propertyPath, float& outValue)
    {
        const auto* compInfo = ComponentRegistry::GetInstance().Get(componentName);
        if (!compInfo || !compInfo->has(registry, entity))
        {
            return false;
        }
        std::string baseName;
        std::string channel;
        SplitPropertyPath(propertyPath, baseName, channel);
        const PropertyRegistration* prop = FindProperty(compInfo, baseName);
        if (!prop || !prop->get)
        {
            return false;
        }
        const std::any value = prop->get(registry, entity);
        if (const auto* asFloat = std::any_cast<float>(&value); asFloat && channel.empty())
        {
            outValue = *asFloat;
            return true;
        }
        if (const auto* asInt = std::any_cast<int>(&value); asInt && channel.empty())
        {
            outValue = static_cast<float>(*asInt);
            return true;
        }
        if (const auto* asBool = std::any_cast<bool>(&value); asBool && channel.empty())
        {
            outValue = *asBool ? 1.0f : 0.0f;
            return true;
        }
        if (const auto* asVec = std::any_cast<ECS::Vector2f>(&value))
        {
            if (channel == "x")
            {
                outValue = asVec->x;
                return true;
            }
            if (channel == "y")
            {
                outValue = asVec->y;
                return true;
            }
            return false;
        }
        if (const auto* asColor = std::any_cast<ECS::Color>(&value))
        {
            if (channel == "r")
            {
                outValue = asColor->r;
                return true;
            }
            if (channel == "g")
            {
                outValue = asColor->g;
                return true;
            }
            if (channel == "b")
            {
                outValue = asColor->b;
                return true;
            }
            if (channel == "a")
            {
                outValue = asColor->a;
                return true;
            }
            return false;
        }
        return false;
    }

    /**
     * @brief 判断轨道目标属性是否为 bool 类型（bool 轨道的关键帧应默认 Step 插值）。
     * @return 属性存在且为 bool 时返回 true。
     */
    inline bool IsBoolProperty(entt::registry& registry, entt::entity entity,
                               const std::string& componentName, const std::string& propertyPath)
    {
        const auto* compInfo = ComponentRegistry::GetInstance().Get(componentName);
        if (!compInfo || !compInfo->has(registry, entity))
        {
            return false;
        }
        std::string baseName;
        std::string channel;
        SplitPropertyPath(propertyPath, baseName, channel);
        if (!channel.empty())
        {
            return false;
        }
        const PropertyRegistration* prop = FindProperty(compInfo, baseName);
        if (!prop || !prop->get)
        {
            return false;
        }
        const std::any value = prop->get(registry, entity);
        return std::any_cast<bool>(&value) != nullptr;
    }

    /**
     * @brief 通过属性反射把 float 标量写入组件属性。
     *
     * 多分量属性先读出整值、改写单分量再整体 set，避免破坏其他分量。
     * @return 写入成功返回 true。
     */
    inline bool ApplyValue(entt::registry& registry, entt::entity entity,
                           const std::string& componentName, const std::string& propertyPath, float value)
    {
        const auto* compInfo = ComponentRegistry::GetInstance().Get(componentName);
        if (!compInfo || !compInfo->has(registry, entity))
        {
            return false;
        }
        std::string baseName;
        std::string channel;
        SplitPropertyPath(propertyPath, baseName, channel);
        const PropertyRegistration* prop = FindProperty(compInfo, baseName);
        if (!prop || !prop->get || !prop->set)
        {
            return false;
        }
        std::any current = prop->get(registry, entity);
        if (std::any_cast<float>(&current) && channel.empty())
        {
            prop->set(registry, entity, std::any(value));
            return true;
        }
        if (std::any_cast<int>(&current) && channel.empty())
        {
            prop->set(registry, entity, std::any(static_cast<int>(std::lround(value))));
            return true;
        }
        if (std::any_cast<bool>(&current) && channel.empty())
        {
            // bool 属性按 0.5 阈值离散化；配合 Step 关键帧即为硬切换
            prop->set(registry, entity, std::any(value >= 0.5f));
            return true;
        }
        if (auto* asVec = std::any_cast<ECS::Vector2f>(&current))
        {
            if (channel == "x")
            {
                asVec->x = value;
            }
            else if (channel == "y")
            {
                asVec->y = value;
            }
            else
            {
                return false;
            }
            prop->set(registry, entity, current);
            return true;
        }
        if (auto* asColor = std::any_cast<ECS::Color>(&current))
        {
            if (channel == "r")
            {
                asColor->r = value;
            }
            else if (channel == "g")
            {
                asColor->g = value;
            }
            else if (channel == "b")
            {
                asColor->b = value;
            }
            else if (channel == "a")
            {
                asColor->a = value;
            }
            else
            {
                return false;
            }
            prop->set(registry, entity, current);
            return true;
        }
        return false;
    }

    /**
     * @brief 对剪辑的全部属性轨道求值并写入实体。
     * @param frameTime 以帧为单位的连续时间（可为小数）。
     * @return 是否有任意轨道成功写入。
     */
    inline bool ApplyTracks(const AnimationClip& clip, entt::registry& registry, entt::entity entity, float frameTime)
    {
        bool anyApplied = false;
        for (const auto& track : clip.PropertyTracks)
        {
            if (track.keyframes.empty())
            {
                continue;
            }
            const float value = Evaluate(track, frameTime);
            anyApplied |= ApplyValue(registry, entity, track.targetComponent, track.propertyPath, value);
        }
        if (anyApplied)
        {
            EventBus::GetInstance().Publish(ComponentUpdatedEvent{registry, entity});
        }
        return anyApplied;
    }

    /// 轨道绑定的值类别（决定写回时的类型转换与分量处理）。
    enum class BoundValueKind
    {
        Invalid, ///< 尚未解析成功或类型不受支持。
        Float,
        Int,
        Bool,
        Vector2,
        Color
    };

    /**
     * @brief 单条轨道的预解析绑定：注册表条目 + 值类别 + 分量下标。
     *
     * 运行时热路径使用，避免逐帧的注册表名称查找、路径拆分与 any 类型探测。
     * 注册表条目为静态生命周期，指针可长期持有。
     */
    struct ResolvedTrackBinding
    {
        const ComponentRegistration* component = nullptr; ///< 组件注册表条目。
        const PropertyRegistration* property = nullptr; ///< 属性注册表条目。
        BoundValueKind kind = BoundValueKind::Invalid; ///< 值类别。
        int channel = -1; ///< 多分量类型的分量下标（x/r=0，y/g=1，b=2，a=3），标量为 -1。
        std::string channelName; ///< 分量后缀原文，供惰性补探测使用。
        bool probeAttempted = false; ///< 是否已尝试类型探测（探测需要组件实例存在）。
    };

    /// 每个剪辑一份的绑定缓存，bindings 与 clip.PropertyTracks 按下标一一对应。
    struct TrackBindingCache
    {
        std::vector<ResolvedTrackBinding> bindings;
        bool built = false;
    };

    /// 用属性当前值探测绑定的值类别与分量下标（要求目标组件实例存在）。
    inline void ProbeBindingKind(entt::registry& registry, entt::entity entity, ResolvedTrackBinding& binding)
    {
        binding.probeAttempted = true;
        if (!binding.property || !binding.property->get)
        {
            return;
        }
        const std::any value = binding.property->get(registry, entity);
        if (binding.channelName.empty())
        {
            if (std::any_cast<float>(&value))
            {
                binding.kind = BoundValueKind::Float;
            }
            else if (std::any_cast<int>(&value))
            {
                binding.kind = BoundValueKind::Int;
            }
            else if (std::any_cast<bool>(&value))
            {
                binding.kind = BoundValueKind::Bool;
            }
            return;
        }
        if (std::any_cast<ECS::Vector2f>(&value))
        {
            if (binding.channelName == "x")
            {
                binding.channel = 0;
            }
            else if (binding.channelName == "y")
            {
                binding.channel = 1;
            }
            binding.kind = binding.channel >= 0 ? BoundValueKind::Vector2 : BoundValueKind::Invalid;
            return;
        }
        if (std::any_cast<ECS::Color>(&value))
        {
            if (binding.channelName == "r")
            {
                binding.channel = 0;
            }
            else if (binding.channelName == "g")
            {
                binding.channel = 1;
            }
            else if (binding.channelName == "b")
            {
                binding.channel = 2;
            }
            else if (binding.channelName == "a")
            {
                binding.channel = 3;
            }
            binding.kind = binding.channel >= 0 ? BoundValueKind::Color : BoundValueKind::Invalid;
        }
    }

    /**
     * @brief 构建剪辑全部轨道的绑定缓存。
     * 目标组件实例暂缺的轨道保留未探测状态，由 ApplyTracksCached 在实例出现时惰性补探测。
     */
    inline void BuildBindingCache(const AnimationClip& clip, entt::registry& registry, entt::entity entity,
                                  TrackBindingCache& cache)
    {
        cache.bindings.assign(clip.PropertyTracks.size(), ResolvedTrackBinding{});
        for (size_t i = 0; i < clip.PropertyTracks.size(); ++i)
        {
            const PropertyTrack& track = clip.PropertyTracks[i];
            ResolvedTrackBinding& binding = cache.bindings[i];
            binding.component = ComponentRegistry::GetInstance().Get(track.targetComponent);
            std::string baseName;
            SplitPropertyPath(track.propertyPath, baseName, binding.channelName);
            binding.property = FindProperty(binding.component, baseName);
            if (binding.component && binding.property && binding.component->has(registry, entity))
            {
                ProbeBindingKind(registry, entity, binding);
            }
        }
        cache.built = true;
    }

    /// 用预解析绑定写回单条轨道的采样值。
    inline bool ApplyValueBound(entt::registry& registry, entt::entity entity,
                                const ResolvedTrackBinding& binding, float value)
    {
        const PropertyRegistration* prop = binding.property;
        switch (binding.kind)
        {
        case BoundValueKind::Float:
            prop->set(registry, entity, std::any(value));
            return true;
        case BoundValueKind::Int:
            prop->set(registry, entity, std::any(static_cast<int>(std::lround(value))));
            return true;
        case BoundValueKind::Bool:
            prop->set(registry, entity, std::any(value >= 0.5f));
            return true;
        case BoundValueKind::Vector2:
            {
                // 多分量属性整读改写单分量再整写，避免破坏其他分量
                std::any current = prop->get(registry, entity);
                auto* vec = std::any_cast<ECS::Vector2f>(&current);
                if (!vec)
                {
                    return false;
                }
                (*vec)[binding.channel] = value;
                prop->set(registry, entity, current);
                return true;
            }
        case BoundValueKind::Color:
            {
                std::any current = prop->get(registry, entity);
                auto* color = std::any_cast<ECS::Color>(&current);
                if (!color)
                {
                    return false;
                }
                (*color)[binding.channel] = value;
                prop->set(registry, entity, current);
                return true;
            }
        default:
            return false;
        }
    }

    /**
     * @brief 带绑定缓存的整剪辑轨道求值写回（运行时热路径）。
     *
     * 首次调用或轨道数变化时自动重建缓存；同一份缓存只应服务同一个剪辑，
     * 剪辑切换时由调用方重置 cache。
     * @return 是否有任意轨道成功写入。
     */
    inline bool ApplyTracksCached(const AnimationClip& clip, entt::registry& registry, entt::entity entity,
                                  float frameTime, TrackBindingCache& cache)
    {
        if (!cache.built || cache.bindings.size() != clip.PropertyTracks.size())
        {
            BuildBindingCache(clip, registry, entity, cache);
        }
        bool anyApplied = false;
        for (size_t i = 0; i < clip.PropertyTracks.size(); ++i)
        {
            const PropertyTrack& track = clip.PropertyTracks[i];
            if (track.keyframes.empty())
            {
                continue;
            }
            ResolvedTrackBinding& binding = cache.bindings[i];
            if (!binding.component || !binding.property || !binding.component->has(registry, entity))
            {
                continue;
            }
            if (binding.kind == BoundValueKind::Invalid)
            {
                if (binding.probeAttempted)
                {
                    continue;
                }
                // 组件实例此前缺失：实例出现后做一次性补探测
                ProbeBindingKind(registry, entity, binding);
                if (binding.kind == BoundValueKind::Invalid)
                {
                    continue;
                }
            }
            anyApplied |= ApplyValueBound(registry, entity, binding, Evaluate(track, frameTime));
        }
        if (anyApplied)
        {
            EventBus::GetInstance().Publish(ComponentUpdatedEvent{registry, entity});
        }
        return anyApplied;
    }
}
#endif
