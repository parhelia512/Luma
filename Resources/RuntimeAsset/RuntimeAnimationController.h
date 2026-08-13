#ifndef RUNTIMEANIMATIONCONTROLLER_H
#define RUNTIMEANIMATIONCONTROLLER_H
#include <mutex>

#include "AnimationControllerData.h"
#include "AnimationTrackSampler.h"
#include "IRuntimeAsset.h"
#include "RuntimeAnimationClip.h"
#include "Event/LumaEvent.h"

/**
 * @brief 运行时动画控制器类。
 *
 * 管理和播放动画剪辑，处理动画状态、变量和过渡。
 */
class RuntimeAnimationController : public IRuntimeAsset
{
private:
    AnimationControllerData m_animationControllerData; ///< 动画控制器数据。
    std::unordered_map<std::string, std::variant<float, bool, int>> m_variables; ///< 动画控制器中的变量集合。
    std::unordered_map<std::string, VariableType> m_variableTypes; ///< 动画控制器中变量的类型。
    bool EntryPlayed = false; ///< 标记入口动画是否已播放。
    std::unordered_map<std::string, bool> m_animationPlayingStates; ///< 存储动画的播放状态。
    std::unordered_map<std::string, sk_sp<RuntimeAnimationClip>> m_animationClips; ///< 存储所有运行时动画剪辑。
    std::string m_currentAnimationName; ///< 当前正在播放的动画名称。
    Guid m_currentAnimationGuid; ///< 当前正在播放的动画的全局唯一标识符。

    /**
     * @brief 内部播放动画的实现。
     * @param clip 要播放的动画剪辑。
     * @param speed 动画播放速度。
     * @param transitionDuration 动画过渡持续时间。
     * @param stateGuidOverride 有效时以该状态GUID作为当前状态（混合树状态GUID与剪辑GUID不同）。
     * @return 是否真正开始播放。
     */
    bool playInternal(const sk_sp<RuntimeAnimationClip>& clip, float speed = 1.0f, float transitionDuration = 0.0f,
                      const Guid& stateGuidOverride = Guid());
    float m_currentTime = 0.0f; ///< 当前动画播放时间。
    float m_frameRate = 60.f; ///< 动画帧率。
    int m_currentFrameIndex = 0; ///< 当前动画帧索引。
    int m_totalFrames = 0; ///< 当前动画的总帧数。
    float m_animationSpeed = 1.0f; ///< 当前动画播放速度。

    bool m_isTransitioning = false; ///< 标记是否正在进行动画过渡。
    float m_transitionTime = 0.0f; ///< 当前过渡已进行的时间。
    float m_transitionDuration = 0.0f; ///< 过渡的总持续时间。
    sk_sp<RuntimeAnimationClip> m_fromClip; ///< 过渡的起始动画剪辑。
    sk_sp<RuntimeAnimationClip> m_toClip; ///< 过渡的目标动画剪辑。
    int m_fromFrameIndex = 0; ///< 过渡起始动画的帧索引。

    sk_sp<RuntimeAnimationClip> m_currentClip; ///< 当前帧来源剪辑；混合树状态下为主导剪辑。
    bool m_isBlendTreeState = false; ///< 当前状态是否为混合树。
    int m_blendDominantChildIndex = -1; ///< 混合树主导子项索引（子项按阈值升序）。
    int m_blendSecondaryChildIndex = -1; ///< 参与混合的相邻子项索引，无混合时为 -1。
    float m_blendDominantWeight = 1.0f; ///< 主导子项权重（0.5~1.0）。
    std::unordered_map<Guid, sk_sp<RuntimeAnimationClip>> m_blendClipCache; ///< 混合树子项剪辑缓存，按剪辑GUID索引。

    /**
     * @brief 一维混合树求值结果：相邻两个子项与主导权重。
     */
    struct BlendTreeSelection
    {
        int dominantIndex = -1; ///< 主导子项索引（权重较大者），求值失败时为 -1。
        int secondaryIndex = -1; ///< 参与混合的相邻子项索引，参数落在阈值区间外时为 -1。
        float dominantWeight = 1.0f; ///< 主导子项权重。
        float parameterValue = 0.0f; ///< 求值时读到的参数值。
    };

    /**
     * @brief 按当前参数值对混合树求值，取相邻阈值的两个子项与线性权重。
     * @param tree 混合树数据（子项需按阈值升序）。
     * @return 求值结果。
     */
    BlendTreeSelection evaluateBlendTree(const BlendTreeData& tree) const;
    /**
     * @brief 获取混合树子项剪辑（带缓存，未命中时按GUID加载）。
     * @param clipGuid 子项剪辑的全局唯一标识符。
     * @return 运行时剪辑，加载失败时为 nullptr。
     */
    sk_sp<RuntimeAnimationClip> getBlendChildClip(const Guid& clipGuid);
    /**
     * @brief 进入混合树状态：求主导剪辑并作为帧来源开始播放。
     * @param stateGuid 混合树状态的全局唯一标识符。
     * @param speed 动画播放速度。
     * @param transitionDuration 动画过渡持续时间。
     */
    void playBlendTreeState(const Guid& stateGuid, float speed, float transitionDuration);
    /**
     * @brief 每帧重求混合树参数，主导剪辑变化时按归一化相位切换帧来源。
     */
    void refreshBlendTreeSelection();
    /**
     * @brief 混合树状态的帧推进与采样，独立于 UpdateFrameBasedAnimation。
     *
     * 第一期混合语义为加权选择：快照帧与属性轨道均取主导剪辑（权重大者），
     * 帧率/循环也随主导剪辑；预留升级为两个剪辑各自采样结果按权重插值。
     * @param deltaTime 帧之间的时间差。
     */
    void UpdateBlendTreeAnimation(float deltaTime);

    LumaEvent<float, int> m_onAnimationUpdateEvent; ///< 动画更新事件。
    ListenerHandle m_onAnimationUpdateListener; ///< 动画更新事件的监听器句柄。
    bool ForceStop = false; ///< 标记是否强制停止动画。
    bool m_isPlaying = false; ///< 标记动画是否正在播放。
    bool m_justTransitioned = false; ///< 标记是否刚刚完成过渡。

    /**
     * @brief 评估一组条件是否满足。
     * @param conditions 要评估的条件列表。
     * @return 如果所有条件都满足则返回 true，否则返回 false。
     */
    bool EvaluateCondition(const std::vector<Condition>& conditions);
    /**
     * @brief 查询状态数据中配置的播放速度倍率。
     * @param stateGuid 状态的全局唯一标识符。
     * @return 该状态的速度倍率，未配置或非法时返回 1.0。
     */
    float getStateSpeed(const Guid& stateGuid) const;

    mutable std::mutex m_playbackStatusMutex; ///< 保护播放状态快照：模拟线程写、UI线程读。
    /**
     * @brief 更新基于帧的动画。
     * @param deltaTime 帧之间的时间差。
     */
    void UpdateFrameBasedAnimation(float deltaTime);
    /**
     * @brief 更新动画过渡状态。
     * @param deltaTime 帧之间的时间差。
     */
    void UpdateTransition(float deltaTime);
    /**
     * @brief 应用动画剪辑的某一帧到模型。
     * @param clip 要应用的动画剪辑。
     * @param frameIndex 要应用的帧索引。
     * @param blendWeight 混合权重。
     */
    void ApplyAnimationFrame(const sk_sp<RuntimeAnimationClip>& clip, int frameIndex, float blendWeight = 1.0f);
    /**
     * @brief 对剪辑的属性轨道按连续帧时间求值并写入目标实体。
     * @param clip 要采样的动画剪辑。
     * @param frameTime 以帧为单位的连续时间（可为小数），保证帧间插值平滑。
     */
    void ApplyPropertyTracks(const sk_sp<RuntimeAnimationClip>& clip, float frameTime);

    AnimationTrackSampler::TrackBindingCache m_trackBindingCache; ///< 当前剪辑属性轨道的绑定缓存（注册表条目/类型解析结果）。
    Guid m_trackBindingCacheClipGuid; ///< 绑定缓存对应的剪辑GUID，剪辑切换时重建缓存。
    /**
     * @brief 混合两个动画剪辑的帧。
     * @param fromClip 源动画剪辑。
     * @param fromFrame 源动画的帧索引。
     * @param toClip 目标动画剪辑。
     * @param toFrame 目标动画的帧索引。
     * @param blendFactor 混合因子。
     */
    void BlendAnimationFrames(const sk_sp<RuntimeAnimationClip>& fromClip, int fromFrame,
                              const sk_sp<RuntimeAnimationClip>& toClip, int toFrame, float blendFactor);

public:
    /**
     * @brief 播放状态快照，供编辑器等UI线程只读展示。
     *
     * 运行时数据在模拟线程更新，UI线程通过 GetPlaybackStatus 拷贝快照读取，
     * 避免直接访问内部容器造成数据竞争。
     */
    struct PlaybackStatus
    {
        std::string currentStateName; ///< 当前状态（剪辑）名称。
        Guid currentStateGuid; ///< 当前状态的全局唯一标识符。
        bool isTransitioning = false; ///< 是否正在过渡。
        std::string targetStateName; ///< 过渡目标状态名称，未过渡时为空。
        Guid targetStateGuid; ///< 过渡目标状态的全局唯一标识符。
        float transitionProgress = 0.0f; ///< 过渡进度（0-1）。
        bool isBlendTreeState = false; ///< 活跃状态（过渡中为目标状态）是否为混合树。
        int activeChildIndex = -1; ///< 混合树主导子项索引（子项按阈值升序），非混合树时为 -1。
        int secondaryChildIndex = -1; ///< 参与混合的相邻子项索引，无混合时为 -1。
        float blendWeight = 1.0f; ///< 主导子项权重（0.5~1.0），相邻子项权重为 1-blendWeight。
        float blendParameterValue = 0.0f; ///< 混合参数当前值。
    };

    /**
     * @brief 获取播放状态快照（线程安全，返回拷贝）。
     * @return 当前播放状态快照。
     */
    PlaybackStatus GetPlaybackStatus() const;

private:
    PlaybackStatus m_playbackStatus; ///< 播放状态快照，写入点见 playInternal / UpdateTransition。

public:
    /**
     * @brief 获取动画控制器数据。
     * @return 动画控制器数据。
     */
    AnimationControllerData GetAnimationControllerData() const { return m_animationControllerData; }
    /**
     * @brief 构造函数，使用动画控制器数据初始化。
     * @param data 动画控制器数据。
     */
    RuntimeAnimationController(AnimationControllerData data);
    /**
     * @brief 播放指定名称的动画。
     * @param animationName 要播放的动画名称。
     * @param speed 动画播放速度。
     * @param transitionDuration 动画过渡持续时间。
     */
    void PlayAnimation(const std::string& animationName, float speed = 1.0f, float transitionDuration = 0.0f);
    /**
     * @brief 播放指定GUID的动画。
     * @param guid 要播放的动画的GUID。
     * @param speed 动画播放速度。
     * @param transitionDuration 动画过渡持续时间。
     */
    void PlayAnimation(const Guid& guid, float speed = 1.0f, float transitionDuration = 0.0f);
    /**
     * @brief 停止当前正在播放的动画。
     */
    void StopAnimation();
    /**
     * @brief 获取当前正在播放的动画名称。
     * @return 当前动画的名称。
     */
    std::string GetCurrentAnimationName() const;
    /**
     * @brief 检查指定名称的动画是否正在播放。
     * @param animationName 要检查的动画名称。
     * @return 如果动画正在播放则返回 true，否则返回 false。
     */
    bool IsAnimationPlaying(const std::string& animationName);

    /**
     * @brief 设置动画控制器中的变量值。
     * @param name 变量名称。
     * @param value 变量值（可以是浮点数、布尔值或整数）。
     */
    void SetVariable(const std::string& name, std::variant<float, bool, int> value);
    /**
     * @brief 触发动画控制器中的一个触发器。
     * @param name 触发器名称。
     */
    void SetTrigger(const std::string& name);
    /**
     * @brief 设置动画的帧率。
     * @param frameRate 要设置的帧率。
     */
    void SetFrameRate(float frameRate);
    /**
     * @brief 获取动画的帧率。
     * @return 当前动画的帧率。
     */
    float GetFrameRate() const;
    /**
     * @brief 播放入口动画。
     */
    void PlayEntryAnimation();
    /**
     * @brief 查找最佳的动画过渡。
     * @param normalizedTime 当前动画的归一化播放进度（0-1），用于退出时间判定。
     * @return 指向最佳过渡的指针，如果没有找到则为 nullptr。
     */
    const Transition* FindBestTransition(float normalizedTime);

    /**
     * @brief 更新动画控制器的状态。
     * @param deltaTime 帧之间的时间差。
     */
    void Update(float deltaTime);
};

#endif