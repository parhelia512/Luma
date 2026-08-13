#ifndef LUMAENGINE_ANIMATIONCLIP_H
#define LUMAENGINE_ANIMATIONCLIP_H
#include <algorithm>
#include "Guid.h"
#include "IComponent.h"
#include "IData.h"

/**
 * @brief 表示动画中的一个关键帧数据。
 */
struct AnimFrame
{
    /// 存储动画组件的数据，键为组件名称，值为YAML节点。
    std::unordered_map<std::string, YAML::Node> animationData;
    /// 存储与此帧相关的可序列化事件目标。
    std::vector<ECS::SerializableEventTarget> eventTargets;
};

/**
 * @brief 属性轨道关键帧之间的插值模式。
 */
enum class AnimInterpMode
{
    Linear = 0, ///< 线性插值。
    Step = 1, ///< 阶梯：保持左侧关键帧的值直到下一关键帧。
    CubicBezier = 2 ///< 三次曲线（Hermite 切线形式）。
};

/**
 * @brief 属性轨道上的单个关键帧（float 标量）。
 */
struct PropertyKey
{
    int frame = 0; ///< 所在帧号。
    float value = 0.0f; ///< 关键帧取值。
    /// 从本关键帧到下一关键帧区间使用的插值模式。
    AnimInterpMode interp = AnimInterpMode::Linear;
    float inTangent = 0.0f; ///< 入切线斜率（值/帧），仅 CubicBezier 模式参与求值。
    float outTangent = 0.0f; ///< 出切线斜率（值/帧），仅 CubicBezier 模式参与求值。
};

/**
 * @brief 逐属性动画轨道：驱动某组件上的单个 float 标量属性。
 *
 * 多分量属性（Vector2/颜色等）拆分为多条标量轨道，
 * propertyPath 形如 "position.x"、"color.r"，无分量后缀时表示属性本身为标量。
 */
struct PropertyTrack
{
    std::string targetComponent; ///< 目标组件在 ComponentRegistry 中的注册名。
    std::string propertyPath; ///< 属性路径：属性名或 属性名.分量。
    std::vector<PropertyKey> keyframes; ///< 关键帧列表，始终按帧号升序维护。
};

/**
 * @brief 表示一个动画剪辑，包含名称、目标实体GUID和一系列动画帧。
 */
struct AnimationClip : Data::IData<AnimationClip>

{
    /// 动画剪辑的名称。
    std::string Name;
    /// 动画剪辑所针对的目标实体的全局唯一标识符。
    Guid TargetEntityGuid;
    /// 播放帧率（帧/秒）。旧资产缺失该字段时按 60 处理。
    float FrameRate = 60.0f;
    /// 是否循环播放。旧资产缺失该字段时按循环处理。
    bool IsLooping = true;
    /// 存储动画的所有帧，键为帧索引，值为对应的动画帧数据。
    std::unordered_map<int, AnimFrame> Frames;
    /// 逐属性曲线轨道，与帧快照（Frames）共存：快照负责 flipbook 硬切，轨道负责数值插值。
    std::vector<PropertyTrack> PropertyTracks;
};


namespace YAML
{
    /**
     * @brief 为 AnimationClip 类型提供 YAML 序列化和反序列化转换。
     */
    template <>
    struct convert<AnimationClip>
    {
        /**
         * @brief 将 AnimationClip 对象编码为 YAML 节点。
         * @param clip 要编码的 AnimationClip 对象。
         * @return 表示 AnimationClip 的 YAML 节点。
         */
        static Node encode(const AnimationClip& clip)
        {
            Node node;
            node["Name"] = clip.Name;
            node["TargetEntityGuid"] = clip.TargetEntityGuid.ToString();
            node["FrameRate"] = clip.FrameRate;
            node["IsLooping"] = clip.IsLooping;


            Node framesNode(YAML::NodeType::Map);
            for (const auto& [index, frame] : clip.Frames)
            {
                Node frameNode;
                for (const auto& [compName, compData] : frame.animationData)
                {
                    frameNode[compName] = compData;
                }
                frameNode["EventTargets"] = frame.eventTargets;
                framesNode[index] = frameNode;
            }
            node["Frames"] = framesNode;

            // 属性轨道为空时不写字段，保证无轨道资产与旧格式完全一致
            if (!clip.PropertyTracks.empty())
            {
                Node tracksNode(YAML::NodeType::Sequence);
                for (const auto& track : clip.PropertyTracks)
                {
                    Node trackNode;
                    trackNode["Component"] = track.targetComponent;
                    trackNode["Property"] = track.propertyPath;
                    Node keysNode(YAML::NodeType::Sequence);
                    for (const auto& key : track.keyframes)
                    {
                        Node keyNode;
                        keyNode["Frame"] = key.frame;
                        keyNode["Value"] = key.value;
                        const char* interpName = "Linear";
                        if (key.interp == AnimInterpMode::Step)
                        {
                            interpName = "Step";
                        }
                        else if (key.interp == AnimInterpMode::CubicBezier)
                        {
                            interpName = "CubicBezier";
                        }
                        keyNode["Interp"] = interpName;
                        keyNode["InTangent"] = key.inTangent;
                        keyNode["OutTangent"] = key.outTangent;
                        keysNode.push_back(keyNode);
                    }
                    trackNode["Keys"] = keysNode;
                    tracksNode.push_back(trackNode);
                }
                node["PropertyTracks"] = tracksNode;
            }

            return node;
        }

        /**
         * @brief 将 YAML 节点解码为 AnimationClip 对象。
         * @param node 要解码的 YAML 节点。
         * @param clip 接收解码结果的 AnimationClip 对象。
         * @return 如果解码成功则返回 true，否则返回 false。
         */
        static bool decode(const Node& node, AnimationClip& clip)
        {
            if (!node.IsMap() || !node["Name"] || !node["TargetEntityGuid"])
            {
                return false;
            }

            clip.Name = node["Name"].as<std::string>();
            clip.TargetEntityGuid = Guid::FromString(node["TargetEntityGuid"].as<std::string>());
            // 向后兼容：旧资产缺少帧率/循环字段时使用默认值
            clip.FrameRate = node["FrameRate"] ? node["FrameRate"].as<float>(60.0f) : 60.0f;
            if (clip.FrameRate <= 0.0f)
            {
                clip.FrameRate = 60.0f;
            }
            clip.IsLooping = node["IsLooping"] ? node["IsLooping"].as<bool>(true) : true;

            // 向后兼容：旧资产没有 PropertyTracks 字段。
            // 必须在 Frames 缺失的提前 return 之前解析，否则无 Frames 的剪辑会丢轨道。
            clip.PropertyTracks.clear();
            const Node& tracksNode = node["PropertyTracks"];
            if (tracksNode && tracksNode.IsSequence())
            {
                for (const auto& trackNode : tracksNode)
                {
                    if (!trackNode["Component"] || !trackNode["Property"])
                    {
                        continue;
                    }
                    PropertyTrack track;
                    track.targetComponent = trackNode["Component"].as<std::string>();
                    track.propertyPath = trackNode["Property"].as<std::string>();
                    const Node& keysNode = trackNode["Keys"];
                    if (keysNode && keysNode.IsSequence())
                    {
                        for (const auto& keyNode : keysNode)
                        {
                            PropertyKey key;
                            key.frame = keyNode["Frame"] ? keyNode["Frame"].as<int>(0) : 0;
                            key.value = keyNode["Value"] ? keyNode["Value"].as<float>(0.0f) : 0.0f;
                            const std::string interpName = keyNode["Interp"]
                                                               ? keyNode["Interp"].as<std::string>("Linear")
                                                               : "Linear";
                            if (interpName == "Step")
                            {
                                key.interp = AnimInterpMode::Step;
                            }
                            else if (interpName == "CubicBezier")
                            {
                                key.interp = AnimInterpMode::CubicBezier;
                            }
                            else
                            {
                                key.interp = AnimInterpMode::Linear;
                            }
                            key.inTangent = keyNode["InTangent"] ? keyNode["InTangent"].as<float>(0.0f) : 0.0f;
                            key.outTangent = keyNode["OutTangent"] ? keyNode["OutTangent"].as<float>(0.0f) : 0.0f;
                            track.keyframes.push_back(key);
                        }
                    }
                    // 求值使用二分查找，加载后立即保证升序
                    std::sort(track.keyframes.begin(), track.keyframes.end(),
                              [](const PropertyKey& a, const PropertyKey& b) { return a.frame < b.frame; });
                    clip.PropertyTracks.push_back(std::move(track));
                }
            }


            clip.Frames.clear();

            const Node& framesNode = node["Frames"];
            if (!framesNode)
            {
                return true;
            }


            if (framesNode.IsMap())
            {
                for (const auto& it : framesNode)
                {
                    int index = it.first.as<int>();
                    const Node& frameValueNode = it.second;

                    AnimFrame frame;
                    for (const auto& compNode : frameValueNode)
                    {
                        std::string compName = compNode.first.as<std::string>();

                        if (compName == "EventTargets")
                        {
                            continue;
                        }
                        frame.animationData[compName] = compNode.second;
                    }

                    if (frameValueNode["EventTargets"])
                    {
                        frame.eventTargets = frameValueNode["EventTargets"].as<std::vector<
                            ECS::SerializableEventTarget>>();
                    }
                    clip.Frames[index] = std::move(frame);
                }
            }

            else if (framesNode.IsSequence())
            {
                int index = 0;
                for (const auto& frameValueNode : framesNode)
                {
                    AnimFrame frame;
                    for (const auto& compNode : frameValueNode)
                    {
                        std::string compName = compNode.first.as<std::string>();
                        if (compName == "EventTargets")
                        {
                            continue;
                        }
                        frame.animationData[compName] = compNode.second;
                    }

                    if (frameValueNode["EventTargets"])
                    {
                        frame.eventTargets = frameValueNode["EventTargets"].as<std::vector<
                            ECS::SerializableEventTarget>>();
                    }

                    clip.Frames[index++] = std::move(frame);
                }
            }

            return true;
        }
    };
}
#endif