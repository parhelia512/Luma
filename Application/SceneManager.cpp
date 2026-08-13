#include "SceneManager.h"
#include "AnimationSystem.h"
#include "ApplicationBase.h"
#include "AudioSystem.h"
#include "ButtonSystem.h"
#include "CommonUIControlSystem.h"
#include "HydrateResources.h"
#include "InputTextSystem.h"
#include "InteractionSystem.h"
#include "LightingSystem.h"
#include "ShadowRenderer.h"
#include "IndirectLightingSystem.h"
#include "AmbientZoneSystem.h"
#include "AreaLightSystem.h"
#include "PhysicsSystem.h"
#if !defined(LUMA_DISABLE_SCRIPTING)
#include "ScriptingSystem.h"
#endif
#include "TransformSystem.h"
#include "UILayoutSystem.h"
#include "../Systems/ParticleSystem.h"
#include "ProjectSettings.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include "../Resources/AssetManager.h"
#include "../Resources/Managers/RuntimeSceneManager.h"
#include "../Data/SceneData.h"
#include "../Resources/Importers/SceneImporter.h"
#include "../Resources/Loaders/SceneLoader.h"
SceneManager::SceneManager()
{
}
SceneManager::~SceneManager()
{
}
void SceneManager::LoadSceneAsync(const Guid& guid, SceneLoadCallback callback)
{
    std::future<sk_sp<RuntimeScene>> future = std::async(std::launch::async, [this, guid]()
    {
        return loadSceneFromDisk(guid);
    });
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_completedLoads.push({guid, std::move(callback), std::move(future)});
}
void SceneManager::Initialize(EngineContext* context)
{
    m_context = context;
}
sk_sp<RuntimeScene> SceneManager::LoadScene(const Guid& guid)
{
    sk_sp<RuntimeScene> newScene = loadSceneFromDisk(guid);
    if (!newScene)
    {
        LogError("加载场景失败，GUID: {}", guid.ToString());
        return nullptr;
    }
    if (ApplicationBase::CURRENT_MODE == ApplicationMode::Runtime)
    {
        setupRuntimeSystems(newScene, m_context);
    }
    activateScene(newScene, guid, m_context);
    return newScene;
}
void SceneManager::Update(EngineContext& engineCtx)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_completedLoads.empty())
    {
        return;
    }
    auto& request = m_completedLoads.front();
    if (request.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        sk_sp<RuntimeScene> loadedScene = request.future.get();
        if (loadedScene)
        {
            if (ApplicationBase::CURRENT_MODE == ApplicationMode::Runtime)
            {
                setupRuntimeSystems(loadedScene, &engineCtx);
            }
            if (request.callback)
            {
                request.callback(loadedScene);
            }
            activateScene(loadedScene, request.guid, &engineCtx);
        }
        else
        {
            LogError("异步加载场景失败，GUID: {}", request.guid.ToString());
            if (request.callback)
            {
                request.callback(nullptr);
            }
        }
        m_completedLoads.pop();
    }
}
void SceneManager::SetCurrentScene(sk_sp<RuntimeScene> scene)
{
    std::unique_lock<std::shared_mutex> lock(m_currentSceneMutex);
    m_currentScene = std::move(scene);
}
sk_sp<RuntimeScene> SceneManager::GetCurrentScene() const
{
    std::shared_lock<std::shared_mutex> lock(m_currentSceneMutex);
    return m_currentScene;
}
Guid SceneManager::GetCurrentSceneGuid() const
{
    std::shared_lock<std::shared_mutex> lock(m_currentSceneMutex);
    return m_currentScene ? m_currentScene->GetGuid() : Guid::Invalid();
}
bool SceneManager::SaveScene(sk_sp<RuntimeScene> scene)
{
    m_markedAsDirty = false;
    if (!scene) return false;
    const AssetMetadata* meta = AssetManager::GetInstance().GetMetadata(scene->GetGuid());
    const bool isFirstSave = (meta == nullptr);
    std::filesystem::path sceneName;
    if (isFirstSave)
    {
        std::string baseName = scene->GetName().empty() ? std::string("NewScene") : scene->GetName();
        sceneName = baseName + ".scene";
        int counter = 1;
        while (std::filesystem::exists(AssetManager::GetInstance().GetAssetsRootPath() / sceneName))
        {
            sceneName = baseName + "_" + std::to_string(counter++) + ".scene";
        }
    }
    else
    {
        sceneName = meta->assetPath;
    }
    Data::SceneData sceneData = scene->SerializeToData();
    YAML::Node sceneNode = YAML::convert<Data::SceneData>::encode(sceneData);
    std::filesystem::path targetPath = AssetManager::GetInstance().GetAssetsRootPath() / sceneName;
    std::ofstream fout(targetPath.generic_string());
    if (!fout.is_open())
    {
        LogError("保存场景失败，无法写入文件: {}", targetPath.generic_string());
        return false;
    }
    fout << sceneNode;
    fout.close();
    if (isFirstSave)
    {
        // 首次保存：立即导入为资产并把 GUID 回填给场景，
        // 否则场景一直保持无效 GUID，每次保存都会生成一个新的 NewScene_N.scene。
        Guid importedGuid = AssetManager::GetInstance().LoadAsset(targetPath);
        if (importedGuid.Valid())
        {
            scene->SetGuid(importedGuid);
            scene->SetName(sceneName.stem().string());
            LogInfo("场景首次保存为 {}，GUID: {}", sceneName.generic_string(), importedGuid.ToString());
        }
        else
        {
            LogWarn("场景已写出到 {}，但导入资产库失败，下次保存可能生成新文件。", targetPath.generic_string());
        }
    }
    return true;
}
bool SceneManager::AutoSaveCurrentScene()
{
    sk_sp<RuntimeScene> scene;
    {
        std::shared_lock<std::shared_mutex> lock(m_currentSceneMutex);
        scene = m_currentScene;
    }
    if (!scene) return false;
    std::error_code ec;
    const std::filesystem::path autosaveDir =
        ProjectSettings::GetInstance().GetProjectRoot() / "Library" / "Autosave";
    std::filesystem::create_directories(autosaveDir, ec);
    if (ec)
    {
        LogWarn("自动保存目录创建失败: {}", ec.message());
        return false;
    }
    const std::string baseName = scene->GetName().empty() ? std::string("Untitled") : scene->GetName();
    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::filesystem::path target = autosaveDir / (baseName + "_" + std::to_string(timestamp) + ".scene");

    Data::SceneData sceneData = scene->SerializeToData();
    YAML::Node sceneNode = YAML::convert<Data::SceneData>::encode(sceneData);
    std::ofstream fout(target.generic_string());
    if (!fout.is_open()) return false;
    fout << sceneNode;
    fout.close();

    // 只保留该场景最近 5 份自动保存
    std::vector<std::filesystem::path> backups;
    for (const auto& entry : std::filesystem::directory_iterator(autosaveDir, ec))
    {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".scene" &&
            entry.path().filename().string().rfind(baseName + "_", 0) == 0)
        {
            backups.push_back(entry.path());
        }
    }
    std::sort(backups.begin(), backups.end());
    constexpr size_t kMaxBackups = 5;
    while (backups.size() > kMaxBackups)
    {
        std::filesystem::remove(backups.front(), ec);
        backups.erase(backups.begin());
    }
    LogInfo("场景已自动保存: {}", target.generic_string());
    return true;
}
bool SceneManager::SaveCurrentScene()
{
    sk_sp<RuntimeScene> scene;
    {
        std::shared_lock<std::shared_mutex> lock(m_currentSceneMutex);
        scene = m_currentScene;
    }
    return SaveScene(scene);
}
void SceneManager::PushUndoState(sk_sp<RuntimeScene> scene)
{
    MarkCurrentSceneDirty();
    if (!scene) return;
    m_redoStack.clear();
    Data::SceneData newState = scene->SerializeToData();
    if (m_undoStack.empty())
    {
        // 栈底基准恒为全量 checkpoint，之后的条目才允许存增量。
        UndoEntry entry;
        entry.isCheckpoint = true;
        entry.state = newState;
        m_undoStack.push_back(std::move(entry));
    }
    else
    {
        // 距最近 checkpoint 未达间隔时存相对栈顶状态的增量；到达间隔，或
        // 场景数据无法安全按 GUID 索引（ComputeSceneDiff 返回空）时，回退
        // 为全量 checkpoint。
        std::optional<Data::SceneDiff> diff;
        if (diffsSinceLastCheckpoint() < CHECKPOINT_INTERVAL)
        {
            diff = Data::ComputeSceneDiff(m_undoTopState, newState);
        }
        UndoEntry entry;
        if (diff.has_value())
        {
            entry.diff = std::move(*diff);
        }
        else
        {
            entry.isCheckpoint = true;
            entry.state = newState;
        }
        m_undoStack.push_back(std::move(entry));
    }
    m_undoTopState = std::move(newState);
    while (m_undoStack.size() > MAX_UNDO_STEPS)
    {
        // 淘汰最老条目前，若第二个条目是增量则先物化为全量 checkpoint，
        // 维持"栈底恒为 checkpoint"的不变量，保证剩余条目始终可重放。
        if (m_undoStack.size() >= 2 && !m_undoStack[1].isCheckpoint)
        {
            m_undoStack[1].state = Data::ApplySceneDiff(m_undoStack.front().state, m_undoStack[1].diff);
            m_undoStack[1].isCheckpoint = true;
            m_undoStack[1].diff = Data::SceneDiff{};
        }
        m_undoStack.pop_front();
    }
}
void SceneManager::Undo()
{
    if (m_undoStack.size() <= 1) return;
    // 重建弹出后新栈顶对应的完整状态（最近 checkpoint + 正向重放增量），
    // 恢复仍沿用 LoadFromData 全场景重建，正确性与全量快照方案等同。
    Data::SceneData prevState = reconstructUndoState(m_undoStack.size() - 2);
    // 栈顶条目原样移入重做栈：增量条目本身就是「新栈顶状态 -> 当前状态」
    // 的正向增量，checkpoint 条目则直接携带完整状态，Redo 均可直接使用。
    m_redoStack.push_back(std::move(m_undoStack.back()));
    m_undoStack.pop_back();
    m_currentScene->LoadFromData(prevState);
    m_undoTopState = std::move(prevState);
}
void SceneManager::Redo()
{
    if (m_redoStack.empty()) return;
    UndoEntry entry = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    // checkpoint 条目直接携带目标状态；增量条目以当前状态（Undo 后的栈顶
    // 状态缓存）为基准正向重放，精确回到 Undo 前的状态。
    Data::SceneData nextState = entry.isCheckpoint
                                    ? entry.state
                                    : Data::ApplySceneDiff(m_undoTopState, entry.diff);
    // 条目原样压回撤销栈，checkpoint 的分布保持 Undo 前的结构不变。
    m_undoStack.push_back(std::move(entry));
    m_currentScene->LoadFromData(nextState);
    m_undoTopState = std::move(nextState);
}
bool SceneManager::CanUndo() const
{
    return m_undoStack.size() > 1;
}
bool SceneManager::CanRedo() const
{
    return !m_redoStack.empty();
}
Data::SceneData SceneManager::reconstructUndoState(size_t index) const
{
    assert(index < m_undoStack.size() && "reconstructUndoState: 条目下标越界");
    size_t checkpointIndex = index;
    while (checkpointIndex > 0 && !m_undoStack[checkpointIndex].isCheckpoint)
    {
        --checkpointIndex;
    }
    // 栈底恒为 checkpoint（PushUndoState 与淘汰逻辑共同维护该不变量），
    // 因此回溯必然停在一个 checkpoint 条目上。
    assert(m_undoStack[checkpointIndex].isCheckpoint && "撤销栈底必须是 checkpoint 条目");
    Data::SceneData state = m_undoStack[checkpointIndex].state;
    for (size_t i = checkpointIndex + 1; i <= index; ++i)
    {
        state = Data::ApplySceneDiff(state, m_undoStack[i].diff);
    }
    return state;
}
size_t SceneManager::diffsSinceLastCheckpoint() const
{
    size_t count = 0;
    for (auto it = m_undoStack.rbegin(); it != m_undoStack.rend(); ++it)
    {
        if (it->isCheckpoint)
        {
            break;
        }
        ++count;
    }
    return count;
}
void SceneManager::Shutdown()
{
    std::unique_lock<std::shared_mutex> lock(m_currentSceneMutex);
    if (m_currentScene)
    {
        LogInfo("关闭场景管理器，停用场景: {}", m_currentScene->GetName());
        m_currentScene->Deactivate();
    }
    m_currentScene.reset();
    m_undoStack.clear();
    m_redoStack.clear();
    m_undoTopState = Data::SceneData{};
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    while (!m_completedLoads.empty())
    {
        m_completedLoads.pop();
    }
}
sk_sp<RuntimeScene> SceneManager::loadSceneFromDisk(const Guid& guid)
{
    SceneLoader loader;
    sk_sp<RuntimeScene> newScene = loader.LoadAsset(guid);
    if (!newScene)
    {
        LogError("从磁盘加载场景失败，GUID: {}", guid.ToString());
        return nullptr;
    }
    return newScene;
}
void SceneManager::ConfigureGameplaySystems(const sk_sp<RuntimeScene>& scene)
{
    if (!scene) return;
    scene->AddEssentialSystem<Systems::HydrateResources>();
    scene->AddEssentialSystem<Systems::TransformSystem>();
    scene->AddSystem<Systems::PhysicsSystem>();
    scene->AddSystem<Systems::InteractionSystem>();
    scene->AddSystem<Systems::AudioSystem>();
    scene->AddSystem<Systems::ButtonSystem>();
    scene->AddSystemToMainThread<Systems::InputTextSystem>();
    scene->AddSystem<Systems::CommonUIControlSystem>();
    scene->AddSystem<Systems::UILayoutSystem>();
#if !defined(LUMA_DISABLE_SCRIPTING)
    scene->AddSystem<Systems::ScriptingSystem>();
#endif
    scene->AddSystem<Systems::AnimationSystem>();
    scene->AddSystem<Systems::ParticleSystem>();
    scene->AddSystemToMainThread<Systems::AmbientZoneSystem>();
    scene->AddSystemToMainThread<Systems::AreaLightSystem>();
    scene->AddSystemToMainThread<Systems::LightingSystem>();
    scene->AddSystemToMainThread<Systems::ShadowRenderer>();
    scene->AddSystemToMainThread<Systems::IndirectLightingSystem>();
    LogInfo("游戏玩法系统已配置完成，场景: {}", scene->GetName());
}
void SceneManager::ConfigureEditorPreviewSystems(const sk_sp<RuntimeScene>& scene)
{
    if (!scene) return;
    scene->AddEssentialSystem<Systems::HydrateResources>();
    scene->AddEssentialSystem<Systems::TransformSystem>();
    scene->AddSystemToMainThread<Systems::AmbientZoneSystem>();
    scene->AddSystemToMainThread<Systems::AreaLightSystem>();
    scene->AddSystemToMainThread<Systems::LightingSystem>();
    scene->AddSystemToMainThread<Systems::ShadowRenderer>();
    scene->AddSystemToMainThread<Systems::IndirectLightingSystem>();
    LogInfo("编辑器预览系统已配置完成，场景: {}", scene->GetName());
}
void SceneManager::setupRuntimeSystems(sk_sp<RuntimeScene> scene, EngineContext* context)
{
    if (!scene)
    {
        LogWarn("尝试为空场景配置运行时系统");
        return;
    }
    auto setupSystems = [scene]()
    {
        ConfigureGameplaySystems(scene);
    };
    if (context)
    {
        context->commandsForSim.Push(setupSystems);
    }
    else
    {
        setupSystems();
    }
}
void SceneManager::activateScene(sk_sp<RuntimeScene> scene, const Guid& guid, EngineContext* context)
{
    if (!scene)
    {
        LogError("尝试激活空场景");
        return;
    }
    auto activateFunc = [this, scene, guid, context]()
    {
        sk_sp<RuntimeScene> oldScene = GetCurrentScene();
        if (oldScene && oldScene != scene)
        {
            LogInfo("停用旧场景: {}", oldScene->GetName());
            oldScene->Deactivate();
        }
        auto& runtimeSceneManager = RuntimeSceneManager::GetInstance();
        runtimeSceneManager.TryAddOrUpdateAsset(guid, scene);
        SetCurrentScene(scene);
        if (context)
        {
            scene->Activate(*context);
        }
        m_markedAsDirty = false;
        LogInfo("场景已激活: {} (GUID: {})", scene->GetName(), guid.ToString());
    };
    if (context)
    {
        context->commandsForSim.Push(activateFunc);
    }
    else
    {
        activateFunc();
    }
}
