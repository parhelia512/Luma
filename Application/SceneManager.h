#ifndef SCENEMANAGER_H
#define SCENEMANAGER_H
#pragma once
#include <deque>
#include <functional>
#include <string>
#include <future>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include "../Utils/LazySingleton.h"
#include "../Utils/Guid.h"
#include "../Data/SceneDiff.h"
#include "../Resources/RuntimeAsset/RuntimeScene.h"
using SceneLoadCallback = std::function<void(sk_sp<RuntimeScene>)>;
class LUMA_API SceneManager : public LazySingleton<SceneManager>
{
public:
    friend class LazySingleton<SceneManager>;
    void LoadSceneAsync(const Guid& guid, SceneLoadCallback callback = nullptr);
    void Initialize(EngineContext* context);
    sk_sp<RuntimeScene> LoadScene(const Guid& guid);
    bool IsCurrentSceneDirty() const { return m_markedAsDirty; }
    void MarkCurrentSceneDirty() { m_markedAsDirty = true; }
    void Update(EngineContext& engineCtx);
    void SetCurrentScene(sk_sp<RuntimeScene> scene);
    sk_sp<RuntimeScene> GetCurrentScene() const;
    Guid GetCurrentSceneGuid() const;
    bool SaveCurrentScene();
    bool SaveScene(sk_sp<RuntimeScene> scene);

    /**
     * @brief 将当前场景自动保存到项目 Library/Autosave 目录（保留最近若干份）。
     * @return 是否成功写出。
     */
    bool AutoSaveCurrentScene();

    /**
     * @brief 为场景配置完整的游戏玩法系统集合。
     *
     * 运行时加载（setupRuntimeSystems）与编辑器 PIE（ToolbarPanel::play）共用这一份注册列表，
     * 避免两处手写清单产生漂移（例如 PIE 曾缺失 UILayoutSystem）。
     * 必须在模拟线程上调用。
     */
    static void ConfigureGameplaySystems(const sk_sp<RuntimeScene>& scene);

    /**
     * @brief 为编辑器预览场景配置系统集合（非播放状态）。
     *
     * 除资源水合与变换外，挂载全部光照相关主线程系统：编辑模式下若无人维护
     * 光照 GPU 数据（光源/阴影/环境光/间接光 buffer），带光照材质的对象会读到
     * 未维护的渲染状态（表现为首次进入场景时闪烁、跑过一次 PIE 借尸还魂才正常），
     * 且编辑器里调整光源参数无法实时预览。
     */
    static void ConfigureEditorPreviewSystems(const sk_sp<RuntimeScene>& scene);

    void PushUndoState(sk_sp<RuntimeScene> scene);
    void Undo();
    void Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    void Shutdown();
private:
    SceneManager();
    ~SceneManager() override;
    struct SceneLoadRequest
    {
        Guid guid; 
        SceneLoadCallback callback; 
        std::future<sk_sp<RuntimeScene>> future; 
    };
    /**
     * @brief 撤销/重做栈中的单个条目。
     *
     * 条目要么持有一份全量快照（checkpoint），要么仅持有相对于前一条目
     * 状态的增量。栈底条目恒为 checkpoint，任意条目对应的完整状态都可
     * 从其之前最近的 checkpoint 出发正向重放增量重建。
     */
    struct UndoEntry
    {
        bool isCheckpoint = false; ///< 为 true 时 state 有效，否则 diff 有效。
        Data::SceneData state;     ///< 全量快照（仅 checkpoint 条目使用）。
        Data::SceneDiff diff;      ///< 相对前一条目状态的正向增量（仅增量条目使用）。
    };
    sk_sp<RuntimeScene> loadSceneFromDisk(const Guid& guid);
    void setupRuntimeSystems(sk_sp<RuntimeScene> scene, EngineContext* context);
    void activateScene(sk_sp<RuntimeScene> scene, const Guid& guid, EngineContext* context);

    /**
     * @brief 重建撤销栈中指定条目对应的完整场景状态。
     *
     * 从该条目之前最近的 checkpoint 出发正向重放增量，重放链长度受
     * CHECKPOINT_INTERVAL 约束。
     * @param index 撤销栈内的条目下标。
     * @return 重建出的完整场景数据。
     */
    Data::SceneData reconstructUndoState(size_t index) const;

    /**
     * @brief 统计撤销栈尾部自最近一个 checkpoint 以来的增量条目数。
     * @return 尾部连续增量条目的数量。
     */
    size_t diffsSinceLastCheckpoint() const;
    sk_sp<RuntimeScene> m_currentScene; 
    mutable std::shared_mutex m_currentSceneMutex; 
    std::queue<SceneLoadRequest> m_completedLoads; 
    std::mutex m_queueMutex; 
    std::deque<UndoEntry> m_undoStack;     ///< 撤销栈：checkpoint 全量快照与增量条目的混合序列。
    std::deque<UndoEntry> m_redoStack;     ///< 重做栈：Undo 时自撤销栈顶原样移入的条目。
    Data::SceneData m_undoTopState;        ///< 撤销栈顶条目对应的完整状态缓存，供增量计算与重做重放使用。
    const size_t MAX_UNDO_STEPS = 100;     ///< 撤销栈最大条目数（增量存储后内存大幅下降，故放宽步数）。
    const size_t CHECKPOINT_INTERVAL = 16; ///< 相邻 checkpoint 之间允许的最大增量条目数，约束重放链长度。
    EngineContext* m_context = nullptr; 
    bool m_markedAsDirty = false; 
};
#endif
