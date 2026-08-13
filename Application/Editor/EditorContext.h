#ifndef EDITORCONTEXT_H
#define EDITORCONTEXT_H
#include <atomic>
#include <cstdint>
#include <memory>
#include <chrono>
#include <optional>
#include <vector>
#include <map>
#include <filesystem>
#include <imgui.h>
#include <webgpu/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <include/core/SkRefCnt.h>
#include <yaml-cpp/yaml.h>
#include "../Data/EngineContext.h"
#include "../Resources/AssetMetadata.h"
#include "../Data/PrefabData.h"
#include "../Utils/Guid.h"
#include "include/core/SkImage.h"
struct RenderPacket;
class RuntimeTexture;
class Editor;
class ImGuiRenderer;
class SceneRenderer;
class RuntimeScene;
class RuntimeGameObject;
class GraphicsBackend;
struct ComponentRegistration;
struct DirectoryNode;
struct UIDrawData;
enum class EditorState
{
    Editing, 
    Playing, 
    Paused 
};
enum class EditingMode
{
    Scene, 
    Prefab 
};
enum class SelectionType
{
    NA, 
    GameObject, 
    SceneCamera 
};
enum class AssetBrowserViewMode
{
    List, 
    Grid 
};
/**
 * @brief 图案笔刷的单元格：相对锚点（图案左上角）的格偏移 + 瓦片资产与朝向。
 * 与 ECS::TileInstance 字段等价，独立定义以免 EditorContext 依赖 TilemapComponent 的重头文件。
 */
struct TileBrushPatternCell
{
    ECS::Vector2i offset; 
    AssetHandle handle; 
    uint8_t rotation = 0; ///< 0-3，每步顺时针 90°
    bool flipX = false; 
    bool flipY = false; 
};
struct EditorContext
{
    EngineContext* engineContext = nullptr; 
    ImGuiRenderer* imguiRenderer = nullptr; 
    SceneRenderer* sceneRenderer = nullptr; 
    GraphicsBackend* graphicsBackend = nullptr; 
    Editor* editor = nullptr; 
    sk_sp<RuntimeScene> activeScene = nullptr; 
    const std::vector<RenderPacket>* renderQueue = nullptr; ///< 指向 RenderableManager 双缓冲的当前帧渲染包（同帧内有效，避免整帧深拷贝）。
    sk_sp<RuntimeScene> editingScene = nullptr; 
    sk_sp<RuntimeScene> sceneBeforePrefabEdit = nullptr; 
    std::string currentSceneName; 
    EditorState editorState = EditorState::Editing; 
    EditingMode editingMode = EditingMode::Scene; 
    Guid editingPrefabGuid; 
    SelectionType selectionType = SelectionType::NA; 
    std::vector<Guid> selectionList; 
    Guid selectionAnchor; 
    std::optional<std::vector<Data::PrefabNode>> gameObjectClipboard; 
    std::vector<Guid> gameObjectsToDelete; 
    std::vector<std::filesystem::path> selectedAssets; 
    std::filesystem::path assetBrowserSelectionAnchor; 
    Guid currentEditingAnimationClipGuid; 
    Guid currentEditingAnimationControllerGuid; 
    int animationEditorUndoCaptureFrame = -1000; ///< 动画编辑器最近声明接管撤销/重做快捷键的 ImGui 帧号（工具栏全局撤销据此让路；面板绘制顺序晚于工具栏，需跨帧标记）
    int tileEditorUndoCaptureFrame = -1000; ///< 瓦片编辑模式最近声明接管撤销/重做快捷键的 ImGui 帧号（瓦片笔画走面板级增量撤销栈，机制同上）
    std::unique_ptr<DirectoryNode> assetTreeRoot; 
    DirectoryNode* currentAssetDirectory = nullptr; 
    AssetBrowserViewMode assetBrowserViewMode = AssetBrowserViewMode::Grid; 
    bool assetBrowserSortAscending = true; 
    std::filesystem::path itemToRename; 
    char renameBuffer[256] = {0}; 
    std::string componentClipboard_Type; 
    YAML::Node componentClipboard_Data; 
    std::vector<std::filesystem::path> assetClipboard; 
    Guid objectToFocusInHierarchy; 
    Guid assetToFocusInBrowser; 
    bool sceneViewFocusRequest = false; ///< 请求场景视图聚焦到当前选中对象（层级面板双击置位，SceneViewPanel 消费，等价按 F）
    std::vector<std::string> droppedFilesQueue; 
    std::string conflictSourcePath; 
    std::string conflictDestPath; 
    std::chrono::steady_clock::time_point lastFrameTime; 
    std::chrono::steady_clock::time_point lastFpsUpdateTime; 
    int frameCount = 0; 
    float lastFps = 0.0f; 
    float renderLatency = 0.0f; 
    std::chrono::steady_clock::time_point lastUpsUpdateTime; 
    int updateCount = 0; 
    float lastUps = 0.0f; 
    float updateLatency = 0.0f; 
    float assetBrowserRefreshTimer = 0.0f; 
    const float assetBrowserRefreshInterval = 2.0f; 
    UIDrawData* uiCallbacks = nullptr; 
    bool wasSceneDirty; 
    Guid currentEditingTilesetGuid; 
    Guid currentEditingRuleTileGuid; 
    AssetHandle activeTileBrush; 
    std::vector<TileBrushPatternCell> activeTileBrushPattern; ///< 多瓦片图案笔刷，非空时优先于 activeTileBrush 盖章；瓦片编辑模式仍由 activeTileBrush 有效性开启
    sk_sp<RuntimeTexture> activeBrushPreviewImage; 
    SkRect activeBrushPreviewSourceRect; 
    Guid currentEditingBlueprintGuid; 
    std::atomic<bool> stepOneFrame{false}; ///< 暂停态单步：模拟线程消费一次后自动清位。
};
#endif
