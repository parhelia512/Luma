#include "RuntimeAnimationController.h"
#include "AnimationTrackSampler.h"
#include "Logger.h"
#include "SceneManager.h"
#include "Loaders/AnimationClipLoader.h"
#include <algorithm>

int FindLastKeyframeIndex(const AnimationClip& clipData, int currentFrame)
{
    int lastKeyframe = -1;
    for (const auto& [frameIndex, frame] : clipData.Frames)
    {
        if (frameIndex <= currentFrame)
        {
            if (frameIndex > lastKeyframe)
            {
                lastKeyframe = frameIndex;
            }
        }
    }
    return lastKeyframe;
}

bool RuntimeAnimationController::playInternal(const sk_sp<RuntimeAnimationClip>& clip, float speed,
                                              float transitionDuration, const Guid& stateGuidOverride)
{
    if (clip == nullptr)
    {
        LogWarn("尝试播放空动画剪辑");
        return false;
    }

    auto& animData = clip->getAnimationClip();
    auto currentScene = SceneManager::GetInstance().GetCurrentScene();
    if (!currentScene) return false;

    auto go = currentScene->FindGameObjectByGuid(animData.TargetEntityGuid);
    if (!go.IsValid())
    {
        LogWarn("无法找到目标实体: {}", animData.TargetEntityGuid.ToString());
        return false;
    }

    // 记录切换前的状态，供播放状态快照区分过渡的起点与终点
    const std::string previousAnimationName = m_currentAnimationName;
    const Guid previousAnimationGuid = m_currentAnimationGuid;

    if (transitionDuration > 0.0f && m_isPlaying)
    {
        m_isTransitioning = true;
        m_transitionTime = 0.0f;
        m_transitionDuration = transitionDuration;
        // 直接取当前帧来源剪辑，混合树状态名不在按名索引的剪辑表中
        m_fromClip = m_currentClip;
        m_fromFrameIndex = m_currentFrameIndex;
        m_toClip = clip;
        LogInfo("开始过渡动画，过渡时长: {}秒", transitionDuration);
    }
    else
    {
        m_isTransitioning = false;
    }

    m_currentAnimationGuid = clip->GetSourceGuid();
    m_currentAnimationName = clip->GetName();
    if (stateGuidOverride.Valid())
    {
        // 混合树等状态GUID与剪辑GUID不同的场合：状态机身份（过渡查找/速度倍率/高亮）按状态记
        m_currentAnimationGuid = stateGuidOverride;
        auto stateIt = m_animationControllerData.States.find(stateGuidOverride);
        if (stateIt != m_animationControllerData.States.end() && !stateIt->second.stateName.empty())
        {
            m_currentAnimationName = stateIt->second.stateName;
        }
    }
    m_currentClip = clip;
    // 单剪辑路径默认重置混合标记，进入混合树时由 playBlendTreeState 在其后补写
    m_isBlendTreeState = false;
    m_blendDominantChildIndex = -1;
    m_blendSecondaryChildIndex = -1;
    m_blendDominantWeight = 1.0f;
    m_currentTime = 0.0f;
    m_currentFrameIndex = 0;
    m_totalFrames = 0;

    // 播放帧率以剪辑资产为准（替代硬编码默认 60）
    if (animData.FrameRate > 0.0f)
    {
        m_frameRate = animData.FrameRate;
    }

    if (!animData.Frames.empty())
    {
        for (const auto& [frameIndex, frame] : animData.Frames)
        {
            m_totalFrames = std::max(m_totalFrames, frameIndex + 1);
        }
    }

    // 属性轨道关键帧同样决定时长：纯轨道剪辑（无快照帧）也要能确定总帧数并播放
    for (const auto& track : animData.PropertyTracks)
    {
        for (const auto& key : track.keyframes)
        {
            m_totalFrames = std::max(m_totalFrames, key.frame + 1);
        }
    }

    // 状态配置的速度倍率在此统一乘算，覆盖状态机自动过渡与外部 PlayAnimation 两种入口
    m_animationSpeed = speed * getStateSpeed(m_currentAnimationGuid);
    m_isPlaying = true;


    m_justTransitioned = true;

    {
        std::lock_guard<std::mutex> lock(m_playbackStatusMutex);
        if (m_isTransitioning)
        {
            m_playbackStatus.currentStateName = previousAnimationName;
            m_playbackStatus.currentStateGuid = previousAnimationGuid;
            m_playbackStatus.isTransitioning = true;
            m_playbackStatus.targetStateName = m_currentAnimationName;
            m_playbackStatus.targetStateGuid = m_currentAnimationGuid;
        }
        else
        {
            m_playbackStatus.currentStateName = m_currentAnimationName;
            m_playbackStatus.currentStateGuid = m_currentAnimationGuid;
            m_playbackStatus.isTransitioning = false;
            m_playbackStatus.targetStateName.clear();
            m_playbackStatus.targetStateGuid = Guid();
        }
        m_playbackStatus.transitionProgress = 0.0f;
        m_playbackStatus.isBlendTreeState = false;
        m_playbackStatus.activeChildIndex = -1;
        m_playbackStatus.secondaryChildIndex = -1;
        m_playbackStatus.blendWeight = 1.0f;
        m_playbackStatus.blendParameterValue = 0.0f;
    }
    return true;
}

bool RuntimeAnimationController::EvaluateCondition(const std::vector<Condition>& conditions)
{
    if (conditions.empty())
    {
        return true;
    }

    for (const auto& condition : conditions)
    {
        bool result = false;


        std::visit([&](auto&& arg)
        {
            using T = std::decay_t<decltype(arg)>;
            if (!m_variables.contains(arg.VarName))
            {
                LogWarn("动画变量 {} 未定义", arg.VarName);
                result = false;
                return;
            }

            if constexpr (std::is_same_v<T, FloatCondition>)
            {
                auto varValue = std::get<float>(m_variables[arg.VarName]);
                result = (arg.op == FloatCondition::GreaterThan) ? (varValue > arg.Value) : (varValue < arg.Value);
            }
            else if constexpr (std::is_same_v<T, IntCondition>)
            {
                auto varValue = std::get<int>(m_variables[arg.VarName]);
                switch (arg.op)
                {
                case IntCondition::GreaterThan: result = varValue > arg.Value;
                    break;
                case IntCondition::LessThan: result = varValue < arg.Value;
                    break;
                case IntCondition::Equal: result = varValue == arg.Value;
                    break;
                case IntCondition::NotEqual: result = varValue != arg.Value;
                    break;
                }
            }
            else if constexpr (std::is_same_v<T, BoolCondition>)
            {
                auto varValue = std::get<bool>(m_variables[arg.VarName]);
                result = (arg.op == BoolCondition::IsTrue) ? varValue : !varValue;
            }

            else if constexpr (std::is_same_v<T, TriggerCondition>)
            {
                result = std::get<bool>(m_variables[arg.VarName]);
            }
        }, condition);

        if (!result)
        {
            return false;
        }
    }
    return true;
}

void RuntimeAnimationController::SetTrigger(const std::string& name)
{
    if (m_variables.contains(name) && m_variableTypes[name] == VariableType::VariableType_Trigger)
    {
        m_variables[name] = true;
    }
    else
    {
        LogWarn("尝试设置一个无效或不存在的触发器: {}", name);
    }
}

void RuntimeAnimationController::UpdateFrameBasedAnimation(float deltaTime)
{
    if (!m_isPlaying || m_totalFrames <= 0 || m_frameRate <= 0) return;

    m_currentTime += deltaTime * m_animationSpeed;
    float frameDuration = 1.0f / m_frameRate;
    float animationDuration = m_totalFrames * frameDuration;

    // 用 find 避免 operator[] 在名称未注册时插入空剪辑
    auto clipIt = m_animationClips.find(m_currentAnimationName);
    sk_sp<RuntimeAnimationClip> currentClip = (clipIt != m_animationClips.end()) ? clipIt->second : nullptr;

    if (m_currentTime >= animationDuration)
    {
        // 循环标志来自剪辑资产：非循环剪辑停留在末尾，保持"已结束"状态供 hasExitTime 过渡判定
        const bool looping = currentClip ? currentClip->getAnimationClip().IsLooping : true;
        if (looping)
        {
            m_currentTime = fmod(m_currentTime, animationDuration);
        }
        else
        {
            m_currentTime = animationDuration;
        }
    }

    m_currentFrameIndex = static_cast<int>(m_currentTime / frameDuration);

    if (currentClip)
    {
        int keyframeToApply = FindLastKeyframeIndex(currentClip->getAnimationClip(), m_currentFrameIndex);
        if (keyframeToApply != -1)
        {
            ApplyAnimationFrame(currentClip, keyframeToApply);
        }
        // 属性轨道用连续帧时间（不取整）采样，帧与帧之间也能得到插值结果
        ApplyPropertyTracks(currentClip, m_currentTime / frameDuration);
    }
}

void RuntimeAnimationController::ApplyPropertyTracks(const sk_sp<RuntimeAnimationClip>& clip, float frameTime)
{
    if (!clip)
    {
        return;
    }

    const AnimationClip& animData = clip->getAnimationClip();
    if (animData.PropertyTracks.empty())
    {
        return;
    }

    auto currentScene = SceneManager::GetInstance().GetCurrentScene();
    if (!currentScene)
    {
        return;
    }

    auto go = currentScene->FindGameObjectByGuid(animData.TargetEntityGuid);
    if (!go.IsValid())
    {
        return;
    }

    // 绑定缓存按剪辑GUID隔离：换剪辑时重建，避免逐帧的注册表名称查找与类型探测
    if (m_trackBindingCacheClipGuid != clip->GetSourceGuid())
    {
        m_trackBindingCache = {};
        m_trackBindingCacheClipGuid = clip->GetSourceGuid();
    }
    AnimationTrackSampler::ApplyTracksCached(animData, currentScene->GetRegistry(), go.GetEntityHandle(), frameTime,
                                             m_trackBindingCache);
}

void RuntimeAnimationController::UpdateTransition(float deltaTime)
{
    if (!m_isTransitioning)
        return;

    m_transitionTime += deltaTime;
    float blendFactor = m_transitionTime / m_transitionDuration;

    if (blendFactor >= 1.0f)
    {
        blendFactor = 1.0f;
        m_isTransitioning = false;
        LogInfo("动画过渡完成");
    }

    {
        std::lock_guard<std::mutex> lock(m_playbackStatusMutex);
        if (m_isTransitioning)
        {
            m_playbackStatus.transitionProgress = blendFactor;
        }
        else
        {
            // 过渡完成，目标状态转正
            m_playbackStatus.currentStateName = m_currentAnimationName;
            m_playbackStatus.currentStateGuid = m_currentAnimationGuid;
            m_playbackStatus.isTransitioning = false;
            m_playbackStatus.targetStateName.clear();
            m_playbackStatus.targetStateGuid = Guid();
            m_playbackStatus.transitionProgress = 0.0f;
        }
    }


    int fromFrame = m_fromFrameIndex;
    int toFrame = m_currentFrameIndex;


    BlendAnimationFrames(m_fromClip, fromFrame, m_toClip, toFrame, blendFactor);
}

void RuntimeAnimationController::ApplyAnimationFrame(const sk_sp<RuntimeAnimationClip>& clip, int frameIndex,
                                                     float blendWeight)
{
    if (!clip)
        return;

    auto animData = clip->getAnimationClip();
    auto currentScene = SceneManager::GetInstance().GetCurrentScene();
    auto go = currentScene->FindGameObjectByGuid(animData.TargetEntityGuid);

    if (!go.IsValid())
    {
        LogWarn("无法找到目标实体: {}", animData.TargetEntityGuid.ToString());
        return;
    }

    if (!animData.Frames.contains(frameIndex))
    {
        return;
    }

    auto frame = animData.Frames[frameIndex];
    for (auto& comp : frame.animationData)
    {
        auto* reg = ComponentRegistry::GetInstance().Get(comp.first);
        if (reg == nullptr)
        {
            LogWarn("组件注册表中未找到组件: {}", comp.first);
            continue;
        }

        if (reg->has(currentScene->GetRegistry(), go.GetEntityHandle()))
        {
            auto component = reg->get_raw_ptr(currentScene->GetRegistry(), go.GetEntityHandle());
            if (component)
            {
                reg->deserialize(currentScene->GetRegistry(), go.GetEntityHandle(), comp.second);
                EventBus::GetInstance().Publish(ComponentUpdatedEvent{
                    currentScene->GetRegistry(), go.GetEntityHandle()
                });
                for (const auto& target : frame.eventTargets)
                {
                    RuntimeGameObject targetGO = currentScene->FindGameObjectByGuid(target.targetEntityGuid);
                    if (targetGO.IsValid() && targetGO.HasComponent<ECS::ScriptsComponent>())
                    {
                        auto& scriptsComp = targetGO.GetComponent<ECS::ScriptsComponent>();
                        for (const auto& script : scriptsComp.scripts)
                        {
                            if (script.metadata && script.metadata->name == target.targetComponentName)
                            {
                                InteractScriptEvent scriptEvent;
                                scriptEvent.type = InteractScriptEvent::CommandType::InvokeMethod;
                                scriptEvent.entityId = static_cast<uint32_t>(targetGO.GetEntityHandle());
                                scriptEvent.methodName = target.targetMethodName;

                                EventBus::GetInstance().Publish(scriptEvent);

                                break;
                            }
                        }
                    }
                }
            }
            else
            {
                LogWarn("组件 {} 在实体 {} 上未找到", comp.first, go.GetGuid().ToString());
            }
        }
    }
}

void RuntimeAnimationController::BlendAnimationFrames(const sk_sp<RuntimeAnimationClip>& fromClip, int fromFrame,
                                                      const sk_sp<RuntimeAnimationClip>& toClip, int toFrame,
                                                      float blendFactor)
{
    if (blendFactor < 0.5f)
    {
        ApplyAnimationFrame(fromClip, fromFrame, 1.0f - blendFactor);
    }
    else
    {
        ApplyAnimationFrame(toClip, toFrame, blendFactor);
    }
}

RuntimeAnimationController::BlendTreeSelection RuntimeAnimationController::evaluateBlendTree(
    const BlendTreeData& tree) const
{
    BlendTreeSelection selection;
    if (tree.children.empty())
    {
        return selection;
    }

    auto varIt = m_variables.find(tree.parameterName);
    if (varIt != m_variables.end() && std::holds_alternative<float>(varIt->second))
    {
        selection.parameterValue = std::get<float>(varIt->second);
    }

    const auto& children = tree.children;
    const float value = selection.parameterValue;
    // 参数落在两端阈值外时钳制到端点子项，与 Unity 1D Blend Tree 一致
    if (children.size() == 1 || value <= children.front().threshold)
    {
        selection.dominantIndex = 0;
        return selection;
    }
    if (value >= children.back().threshold)
    {
        selection.dominantIndex = static_cast<int>(children.size()) - 1;
        return selection;
    }

    for (size_t i = 0; i + 1 < children.size(); ++i)
    {
        if (value > children[i + 1].threshold)
        {
            continue;
        }
        const float range = children[i + 1].threshold - children[i].threshold;
        const float t = range > 0.0f ? (value - children[i].threshold) / range : 0.0f;
        if (t < 0.5f)
        {
            selection.dominantIndex = static_cast<int>(i);
            selection.secondaryIndex = static_cast<int>(i) + 1;
            selection.dominantWeight = 1.0f - t;
        }
        else
        {
            selection.dominantIndex = static_cast<int>(i) + 1;
            selection.secondaryIndex = static_cast<int>(i);
            selection.dominantWeight = t;
        }
        break;
    }
    return selection;
}

sk_sp<RuntimeAnimationClip> RuntimeAnimationController::getBlendChildClip(const Guid& clipGuid)
{
    if (!clipGuid.Valid())
    {
        return nullptr;
    }
    auto it = m_blendClipCache.find(clipGuid);
    if (it != m_blendClipCache.end())
    {
        return it->second;
    }
    auto loader = AnimationClipLoader();
    sk_sp<RuntimeAnimationClip> clip = loader.LoadAsset(clipGuid);
    if (clip)
    {
        m_blendClipCache[clipGuid] = clip;
    }
    else
    {
        LogWarn("加载混合树子项剪辑 {} 失败", clipGuid.ToString());
    }
    return clip;
}

void RuntimeAnimationController::playBlendTreeState(const Guid& stateGuid, float speed, float transitionDuration)
{
    auto stateIt = m_animationControllerData.States.find(stateGuid);
    if (stateIt == m_animationControllerData.States.end())
    {
        LogWarn("尝试播放不存在的混合树状态: {}", stateGuid.ToString());
        return;
    }

    const BlendTreeData& tree = stateIt->second.blendTree;
    BlendTreeSelection selection = evaluateBlendTree(tree);
    if (selection.dominantIndex < 0)
    {
        LogWarn("混合树状态 {} 没有可用子项", stateIt->second.stateName);
        return;
    }

    sk_sp<RuntimeAnimationClip> dominantClip = getBlendChildClip(tree.children[selection.dominantIndex].clipGuid);
    if (!dominantClip)
    {
        return;
    }

    if (!playInternal(dominantClip, speed, transitionDuration, stateGuid))
    {
        return;
    }

    m_isBlendTreeState = true;
    m_blendDominantChildIndex = selection.dominantIndex;
    m_blendSecondaryChildIndex = selection.secondaryIndex;
    m_blendDominantWeight = selection.dominantWeight;

    {
        std::lock_guard<std::mutex> lock(m_playbackStatusMutex);
        m_playbackStatus.isBlendTreeState = true;
        m_playbackStatus.activeChildIndex = selection.dominantIndex;
        m_playbackStatus.secondaryChildIndex = selection.secondaryIndex;
        m_playbackStatus.blendWeight = selection.dominantWeight;
        m_playbackStatus.blendParameterValue = selection.parameterValue;
    }
}

void RuntimeAnimationController::refreshBlendTreeSelection()
{
    auto stateIt = m_animationControllerData.States.find(m_currentAnimationGuid);
    if (stateIt == m_animationControllerData.States.end() ||
        stateIt->second.stateType != AnimationStateType::BlendTree)
    {
        return;
    }

    const BlendTreeData& tree = stateIt->second.blendTree;
    BlendTreeSelection selection = evaluateBlendTree(tree);
    if (selection.dominantIndex < 0)
    {
        return;
    }

    if (selection.dominantIndex != m_blendDominantChildIndex)
    {
        sk_sp<RuntimeAnimationClip> newClip = getBlendChildClip(tree.children[selection.dominantIndex].clipGuid);
        if (!newClip)
        {
            // 新主导剪辑缺失时维持旧剪辑继续播放，不更新权重快照
            return;
        }

        // 主导剪辑切换：按归一化相位换算播放时间，帧率/总帧数/循环改随新主导剪辑
        const float oldDuration = (m_totalFrames > 0 && m_frameRate > 0.0f)
                                      ? static_cast<float>(m_totalFrames) / m_frameRate
                                      : 0.0f;
        const float normalized = oldDuration > 0.0f ? std::clamp(m_currentTime / oldDuration, 0.0f, 1.0f) : 0.0f;

        // 过渡进入本混合树期间主导剪辑变化：过渡终点跟随新主导剪辑，帧索引才能对上
        if (m_isTransitioning && m_toClip == m_currentClip)
        {
            m_toClip = newClip;
        }
        m_currentClip = newClip;
        auto& animData = newClip->getAnimationClip();
        if (animData.FrameRate > 0.0f)
        {
            m_frameRate = animData.FrameRate;
        }
        m_totalFrames = 0;
        for (const auto& [frameIndex, frame] : animData.Frames)
        {
            m_totalFrames = std::max(m_totalFrames, frameIndex + 1);
        }
        // 与单剪辑路径一致：属性轨道关键帧参与时长计算
        for (const auto& track : animData.PropertyTracks)
        {
            for (const auto& key : track.keyframes)
            {
                m_totalFrames = std::max(m_totalFrames, key.frame + 1);
            }
        }
        const float newDuration = (m_totalFrames > 0 && m_frameRate > 0.0f)
                                      ? static_cast<float>(m_totalFrames) / m_frameRate
                                      : 0.0f;
        m_currentTime = normalized * newDuration;
        m_currentFrameIndex = m_frameRate > 0.0f ? static_cast<int>(m_currentTime * m_frameRate) : 0;
    }

    m_blendDominantChildIndex = selection.dominantIndex;
    m_blendSecondaryChildIndex = selection.secondaryIndex;
    m_blendDominantWeight = selection.dominantWeight;

    {
        std::lock_guard<std::mutex> lock(m_playbackStatusMutex);
        m_playbackStatus.isBlendTreeState = true;
        m_playbackStatus.activeChildIndex = selection.dominantIndex;
        m_playbackStatus.secondaryChildIndex = selection.secondaryIndex;
        m_playbackStatus.blendWeight = selection.dominantWeight;
        m_playbackStatus.blendParameterValue = selection.parameterValue;
    }
}

void RuntimeAnimationController::UpdateBlendTreeAnimation(float deltaTime)
{
    if (!m_isPlaying || !m_isBlendTreeState)
    {
        return;
    }

    // 参数每帧直接求值，无惯性；主导剪辑变化时保持归一化相位切换
    refreshBlendTreeSelection();

    if (!m_currentClip || m_totalFrames <= 0 || m_frameRate <= 0.0f)
    {
        return;
    }

    m_currentTime += deltaTime * m_animationSpeed;
    const float frameDuration = 1.0f / m_frameRate;
    const float animationDuration = m_totalFrames * frameDuration;

    if (m_currentTime >= animationDuration)
    {
        // 循环语义与单剪辑路径一致：非循环剪辑停在末尾供 hasExitTime 过渡判定
        if (m_currentClip->getAnimationClip().IsLooping)
        {
            m_currentTime = fmod(m_currentTime, animationDuration);
        }
        else
        {
            m_currentTime = animationDuration;
        }
    }

    m_currentFrameIndex = static_cast<int>(m_currentTime / frameDuration);

    int keyframeToApply = FindLastKeyframeIndex(m_currentClip->getAnimationClip(), m_currentFrameIndex);
    if (keyframeToApply != -1)
    {
        // 第一期混合语义：加权选择，快照帧仅应用主导剪辑的
        ApplyAnimationFrame(m_currentClip, keyframeToApply);
    }
    // 属性轨道同样以主导剪辑采样（加权选择）。后续升级点：相邻子项剪辑各自
    // Evaluate 后按 m_blendDominantWeight 插值再写回，需等属性轨道采样接口稳定
    ApplyPropertyTracks(m_currentClip, m_currentTime / frameDuration);
}

RuntimeAnimationController::RuntimeAnimationController(AnimationControllerData data)
    : m_animationControllerData(std::move(data))
{
    auto loader = AnimationClipLoader();
    for (const auto& clip : m_animationControllerData.Clips)
    {
        if (sk_sp<RuntimeAnimationClip> runtimeClip = loader.LoadAsset(clip.second))
        {
            runtimeClip->SetName(clip.first);
            m_animationClips[clip.first] = runtimeClip;
        }
        else
        {
            LogWarn("加载动画剪辑 {} 失败", clip.first);
        }
    }


    for (auto& var : m_animationControllerData.Variables)
    {
        m_variables[var.Name] = var.Value;
        m_variableTypes[var.Name] = var.Type;

        if (var.Type == VariableType::VariableType_Trigger)
        {
            m_variables[var.Name] = false;
        }
    }

    for (auto& [stateGuid, state] : m_animationControllerData.States)
    {
        if (state.stateType != AnimationStateType::BlendTree)
        {
            continue;
        }
        // 子项按阈值升序是求值前提，防御手改文件或旧编辑器保存的乱序数据
        std::stable_sort(state.blendTree.children.begin(), state.blendTree.children.end(),
                         [](const BlendTreeData::Child& a, const BlendTreeData::Child& b)
                         {
                             return a.threshold < b.threshold;
                         });
        for (const auto& child : state.blendTree.children)
        {
            getBlendChildClip(child.clipGuid);
        }
    }
}

void RuntimeAnimationController::PlayAnimation(const Guid& guid, float speed, float transitionDuration)
{
    // 混合树状态的GUID不是剪辑资产，路由到混合树播放（覆盖入口过渡等按GUID播放的入口）
    auto stateIt = m_animationControllerData.States.find(guid);
    if (stateIt != m_animationControllerData.States.end() &&
        stateIt->second.stateType == AnimationStateType::BlendTree)
    {
        playBlendTreeState(guid, speed, transitionDuration);
        return;
    }

    auto loader = AnimationClipLoader();
    sk_sp<RuntimeAnimationClip> clip = loader.LoadAsset(guid);
    playInternal(clip, speed, transitionDuration);
}

void RuntimeAnimationController::PlayAnimation(const std::string& animationName, float speed, float transitionDuration)
{
    auto it = m_animationClips.find(animationName);
    if (it != m_animationClips.end())
    {
        playInternal(it->second, speed, transitionDuration);
    }
    else
    {
        LogWarn("尝试播放一个不存在的动画: {}", animationName);
    }
}

void RuntimeAnimationController::StopAnimation()
{
    ForceStop = true;
    m_isPlaying = false;
    m_isTransitioning = false;
    m_currentTime = 0.0f;
    m_currentFrameIndex = 0;

    {
        std::lock_guard<std::mutex> lock(m_playbackStatusMutex);
        m_playbackStatus.isTransitioning = false;
        m_playbackStatus.targetStateName.clear();
        m_playbackStatus.targetStateGuid = Guid();
        m_playbackStatus.transitionProgress = 0.0f;
    }
}

std::string RuntimeAnimationController::GetCurrentAnimationName() const
{
    return m_currentAnimationName;
}

RuntimeAnimationController::PlaybackStatus RuntimeAnimationController::GetPlaybackStatus() const
{
    std::lock_guard<std::mutex> lock(m_playbackStatusMutex);
    return m_playbackStatus;
}

float RuntimeAnimationController::getStateSpeed(const Guid& stateGuid) const
{
    auto it = m_animationControllerData.States.find(stateGuid);
    if (it != m_animationControllerData.States.end() && it->second.speed > 0.0f)
    {
        return it->second.speed;
    }
    return 1.0f;
}

bool RuntimeAnimationController::IsAnimationPlaying(const std::string& animationName)
{
    return m_isPlaying && m_currentAnimationName == animationName;
}

void RuntimeAnimationController::SetVariable(const std::string& name, std::variant<float, bool, int> value)
{
    if (m_variables.contains(name))
    {
        m_variables[name] = value;
    }
    else
    {
        LogWarn("尝试设置未定义的动画变量: {}", name);
    }
}

void RuntimeAnimationController::SetFrameRate(float frameRate)
{
    if (frameRate > 0.0f)
    {
        m_frameRate = frameRate;
    }
    else
    {
        LogWarn("无效的帧率设置: {}", frameRate);
    }
}

float RuntimeAnimationController::GetFrameRate() const
{
    return m_frameRate;
}

void RuntimeAnimationController::PlayEntryAnimation()
{
    if (!EntryPlayed)
    {
        EntryPlayed = true;

        if (m_animationControllerData.States.count(SpecialStateGuids::Entry()))
        {
            const auto& entryState = m_animationControllerData.States.at(SpecialStateGuids::Entry());
            if (!entryState.Transitions.empty())
            {
                const auto& entryTransition = entryState.Transitions[0];
                PlayAnimation(entryTransition.ToGuid, 1.0f, 0.0f);
                return;
            }
        }
    }
}

const Transition* RuntimeAnimationController::FindBestTransition(float normalizedTime)
{
    const Transition* bestCandidate = nullptr;


    auto evaluate = [&](const Transition& t)
    {
        if (EvaluateCondition(t.Conditions))
        {
            if (!bestCandidate || t.priority > bestCandidate->priority)
            {
                bestCandidate = &t;
            }
        }
    };


    if (m_animationControllerData.States.contains(SpecialStateGuids::AnyState()))
    {
        const auto& anyState = m_animationControllerData.States.at(SpecialStateGuids::AnyState());
        for (const auto& transition : anyState.Transitions)
        {
            if (transition.ToGuid != m_currentAnimationGuid)
            {
                evaluate(transition);
            }
        }
    }


    if (m_animationControllerData.States.contains(m_currentAnimationGuid))
    {
        const auto& currentState = m_animationControllerData.States.at(m_currentAnimationGuid);
        for (const auto& transition : currentState.Transitions)
        {
            // 归一化退出时间：播放进度达到 exitTime 才允许过渡；默认 1.0 等价于旧的“播放完毕”语义
            if (!transition.hasExitTime || normalizedTime >= transition.exitTime)
            {
                evaluate(transition);
            }
        }
    }

    return bestCandidate;
}

void RuntimeAnimationController::Update(float deltaTime)
{
    if (!SceneManager::GetInstance().GetCurrentScene() || ForceStop)
    {
        ForceStop = false;
        return;
    }

    if (m_isTransitioning)
    {
        UpdateTransition(deltaTime);
        if (m_isBlendTreeState)
        {
            UpdateBlendTreeAnimation(deltaTime);
        }
        else
        {
            UpdateFrameBasedAnimation(deltaTime);
        }
        return;
    }

    // 归一化播放进度（0-1），供退出时间判定；未播放时为 0 以保持旧行为（不触发带退出时间的过渡）
    float normalizedTime = 0.0f;
    if (m_isPlaying)
    {
        float animationDuration = (m_totalFrames > 0 && m_frameRate > 0) ? (float)m_totalFrames / m_frameRate : 0.0f;
        if (animationDuration > 0)
        {
            normalizedTime = m_currentTime / animationDuration;
        }
    }

    if (m_justTransitioned)
    {
        m_justTransitioned = false;
    }
    else
    {
        const Transition* bestTransition = FindBestTransition(normalizedTime);

        if (bestTransition)
        {
            // 过渡真正触发后消耗触发器，避免同一触发器重复生效
            auto consumeTriggers = [this](const Transition& transition)
            {
                for (const auto& condition : transition.Conditions)
                {
                    if (std::holds_alternative<TriggerCondition>(condition))
                    {
                        const auto& triggerCond = std::get<TriggerCondition>(condition);
                        m_variables[triggerCond.VarName] = false;
                    }
                }
            };

            auto toStateIt = m_animationControllerData.States.find(bestTransition->ToGuid);
            const bool targetIsBlendTree = toStateIt != m_animationControllerData.States.end() &&
                toStateIt->second.stateType == AnimationStateType::BlendTree;

            if (targetIsBlendTree)
            {
                if (bestTransition->ToGuid != m_currentAnimationGuid)
                {
                    LogInfo("过渡触发: 从 {} 切换到混合树状态", m_currentAnimationName);
                    playBlendTreeState(bestTransition->ToGuid, 1.0f, bestTransition->TransitionDuration);
                    consumeTriggers(*bestTransition);
                }
            }
            else
            {
                auto loader = AnimationClipLoader();
                sk_sp<RuntimeAnimationClip> nextClip = loader.LoadAsset(bestTransition->ToGuid);

                if (nextClip && nextClip->GetSourceGuid() != m_currentAnimationGuid)
                {
                    LogInfo("过渡触发: 从 {} 切换到目标状态", m_currentAnimationName);
                    playInternal(nextClip, 1.0f, bestTransition->TransitionDuration);
                    consumeTriggers(*bestTransition);
                }
            }
        }
    }

    if (m_isBlendTreeState)
    {
        UpdateBlendTreeAnimation(deltaTime);
    }
    else
    {
        UpdateFrameBasedAnimation(deltaTime);
    }
}
