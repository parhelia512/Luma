#ifndef SCENEMANAGER_H
#define SCENEMANAGER_H
#pragma once
#include <functional>
#include <string>
#include <future>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include "../Utils/LazySingleton.h"
#include "../Utils/Guid.h"
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
    sk_sp<RuntimeScene> loadSceneFromDisk(const Guid& guid);
    void setupRuntimeSystems(sk_sp<RuntimeScene> scene, EngineContext* context);
    void activateScene(sk_sp<RuntimeScene> scene, const Guid& guid, EngineContext* context);
    sk_sp<RuntimeScene> m_currentScene; 
    mutable std::shared_mutex m_currentSceneMutex; 
    std::queue<SceneLoadRequest> m_completedLoads; 
    std::mutex m_queueMutex; 
    std::deque<Data::SceneData> m_undoStack; 
    std::deque<Data::SceneData> m_redoStack; 
    const size_t MAX_UNDO_STEPS = 32; 
    EngineContext* m_context = nullptr; 
    bool m_markedAsDirty = false; 
};
#endif
