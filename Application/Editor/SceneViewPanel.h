#ifndef SCENEVIEWPANEL_H
#define SCENEVIEWPANEL_H
#include "AmbientZoneComponent.h"
#include "AreaLightComponent.h"
#include "ColliderComponent.h"
#include "DirectionalLightComponent.h"
#include "IEditorPanel.h"
#include "LightProbeComponent.h"
#include "PointLightComponent.h"
#include "SpotLightComponent.h"
#include "Sprite.h"
#include "TextComponent.h"
#include "TilemapComponent.h"
#include "Transform.h"
#include "UIComponents.h"
#include "../Renderer/RenderTarget.h"
#include "Renderer/Camera.h"
#include "../Utils/Guid.h"
#include "Particles/ParticleRenderer.h"
#include "../../Components/ParticleComponent.h"
#include "TouchGestureHandler.h"
#include <deque>
struct AssetHandle;
class RuntimeGameObject;
class SceneViewPanel : public IEditorPanel
{
public:
    SceneViewPanel() = default;
    ~SceneViewPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "场景"; }
private:
    struct DraggedObject
    {
        Guid guid; 
        ECS::Vector2f dragOffset; 
    };
    struct ColliderHandle
    {
        Guid entityGuid; 
        int handleIndex; 
        ImVec2 screenPosition; 
        float radius; 
    };
    struct ActiveColliderHandle
    {
        Guid entityGuid; 
        int handleIndex = -1; 
        ECS::Vector2f fixedPointWorldPos; 
        ECS::Vector2f dragOffset; 
        bool IsValid() const { return handleIndex != -1; }
        void Reset()
        {
            handleIndex = -1;
            entityGuid = Guid();
            fixedPointWorldPos = {0, 0};
            dragOffset = {0, 0};
        }
    };
    struct UIRectHandle
    {
        Guid entityGuid;
        ImVec2 screenPosition;
        float size;
    };
    /**
     * @brief 瓦片编辑工具。工具条持久选择，Alt/Ctrl/Shift/I 修饰键作临时覆盖，G 切换填充。
     */
    enum class TileTool
    {
        Brush,  ///< 笔刷
        Eraser, ///< 橡皮
        Line,   ///< 直线
        Rect,   ///< 矩形
        Fill,   ///< 油漆桶
        Picker, ///< 吸管
        Select  ///< 选择：拖选场景瓦片区域，Ctrl+C/X 拷为图案笔刷
    };
    /**
     * @brief 笔刷/图案的朝向操作（Z/X/H/V 快捷键）。
     */
    enum class TileOrientOp
    {
        RotateCCW, ///< 逆时针 90°
        RotateCW,  ///< 顺时针 90°
        FlipH,     ///< 水平翻转
        FlipV      ///< 垂直翻转
    };
    /**
     * @brief 单元格的完整内容快照（普通瓦片与规则瓦片两张表），句柄无效表示该表无此格。
     */
    struct TileCellState
    {
        ECS::TileInstance normal; 
        ECS::TileInstance rule; 
    };
    /**
     * @brief 一个格子在一次笔画前后的内容变化。
     */
    struct TileDiffEntry
    {
        ECS::Vector2i coord; 
        TileCellState before; 
        TileCellState after; 
    };
    /**
     * @brief 一次笔画的瓦片级增量撤销记录。
     */
    struct TileUndoRecord
    {
        Guid tilemapGuid; 
        std::vector<TileDiffEntry> entries; 
    };
    // Unity 风格变换工具（Q/W/E/R）
    enum class TransformTool
    {
        Select,
        Move,
        Rotate,
        Scale
    };
    // 变换 gizmo 上当前被拖拽的手柄
    enum class GizmoHandle
    {
        None,
        Center,     ///< 中心方块：自由移动 / 等比缩放
        AxisX,      ///< X 轴手柄
        AxisY,      ///< Y 轴手柄
        RotateRing  ///< 旋转圆环
    };
    // gizmo 拖拽开始时选中对象的世界变换快照，拖拽全程基于快照计算，避免增量误差累积
    struct GizmoTarget
    {
        Guid guid;
        ECS::Vector2f startPosition;
        float startRotation = 0.0f;
        ECS::Vector2f startScale = {1.0f, 1.0f};
    };
    entt::entity findEntityByTransform(const ECS::TransformComponent& targetTransform);
    bool isPointInEmptyObject(const ECS::Vector2f& worldPoint, const ECS::TransformComponent& transform);
    void drawSelectionOutlines(const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void drawBoxColliderOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                const ECS::BoxColliderComponent& boxCollider, ImU32 outlineColor, ImU32 fillColor,
                                float thickness);
    void drawCircleColliderOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                   const ECS::CircleColliderComponent& circleCollider, ImU32 outlineColor,
                                   ImU32 fillColor, float thickness);
    void drawPolygonColliderOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                    const ECS::PolygonColliderComponent& polygonCollider, ImU32 outlineColor,
                                    ImU32 fillColor, float thickness);
    void drawEdgeColliderOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                 const ECS::EdgeColliderComponent& edgeCollider, ImU32 outlineColor, float thickness);
    void drawTilemapColliderOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                    const ECS::TilemapColliderComponent& tilemapCollider, ImU32 outlineColor,
                                    float thickness);
    void drawSpriteSelectionOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                    const ECS::SpriteComponent& sprite, ImU32 outlineColor, ImU32 fillColor,
                                    float thickness);
    void drawButtonSelectionOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                    const ECS::ButtonComponent& buttonComp, ImU32 outlineColor, ImU32 fillColor,
                                    float thickness);
    void drawCapsuleColliderOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                    const ECS::CapsuleColliderComponent& capsuleCollider, ImU32 outlineColor,
                                    ImU32 fillColor, float thickness);
    void drawColliderEditHandles(ImDrawList* drawList, RuntimeGameObject& gameObject,
                                 const ECS::TransformComponent& transform);
    void drawDashedLine(ImDrawList* drawList, const ImVec2& start, const ImVec2& end, ImU32 color, float thickness,
                        float dashSize);
    void drawInputTextSelectionOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                       const ECS::InputTextComponent& inputTextComp, ImU32 outlineColor,
                                       ImU32 fillColor, float thickness);
    void drawTextSelectionOutline(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                  const ECS::TextComponent& textComp, ImU32 outlineColor, ImU32 fillColor,
                                  float thickness);
    void drawEmptyObjectSelection(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                  const std::string& objectName,
                                  ImU32 outlineColor, ImU32 labelBgColor, ImU32 labelTextColor);
    void drawObjectNameLabel(ImDrawList* drawList, const ECS::TransformComponent& transform,
                             const std::string& objectName,
                             ImU32 labelBgColor, ImU32 labelTextColor);
    void drawEditorGrid(const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void handleTilePainting(RuntimeGameObject& tilemapGo);
    void handleNavigationAndPick(const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void handleDragDrop();
    void processAssetDrop(const AssetHandle& handle, const ECS::Vector2f& worldPosition);
    void triggerHierarchyUpdate();
    void drawEditorGizmos(const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void drawTilemapGrid(ImDrawList* drawList, const ECS::TransformComponent& tilemapTransform, const ECS::TilemapComponent& tilemap, const
                         ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void drawTileBrushPreview(ImDrawList* drawList, const ECS::TransformComponent& tilemapTransform, const ECS::TilemapComponent& tilemap);
    TileTool effectiveTileTool() const;
    ECS::Vector2i mouseTileCoord(const ECS::TransformComponent& tilemapTransform, const ECS::TilemapComponent& tilemap) const;
    void tileCellScreenRect(const ECS::TransformComponent& tilemapTransform, const ECS::TilemapComponent& tilemap,
                            const ECS::Vector2i& coord, ImVec2& outMin, ImVec2& outMax) const;
    void drawTileCellsPreview(ImDrawList* drawList, const ECS::TransformComponent& tilemapTransform,
                              const ECS::TilemapComponent& tilemap, const std::vector<ECS::Vector2i>& coords,
                              ImU32 color);
    static void collectTileLineCoords(const ECS::Vector2i& from, const ECS::Vector2i& to,
                                      std::vector<ECS::Vector2i>& outCoords);
    static void collectTileRectCoords(const ECS::Vector2i& from, const ECS::Vector2i& to,
                                      std::vector<ECS::Vector2i>& outCoords);
    void computeTileFillRegion(const ECS::TilemapComponent& tilemap, const ECS::Vector2i& anchor,
                               std::vector<ECS::Vector2i>& outCoords) const;
    void drawTileToolbar(ImDrawList* drawList, const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void handleTileEditHotkeys();
    void applyTileBrushOrientationOp(TileOrientOp op);
    void copyTileSelectionToPattern(bool cut);
    void beginTileStroke(RuntimeGameObject& tilemapGo);
    void recordTileCellBefore(const ECS::TilemapComponent& tilemap, const ECS::Vector2i& coord);
    void endTileStroke(const ECS::TilemapComponent& tilemap);
    void undoTileStroke();
    void redoTileStroke();
    bool applyTileUndoRecord(const TileUndoRecord& record, bool useBefore);
    static TileCellState captureTileCellState(const ECS::TilemapComponent& tilemap, const ECS::Vector2i& coord);
    static void applyTileCellState(ECS::TilemapComponent& tilemap, const ECS::Vector2i& coord,
                                   const TileCellState& state);
    /**
     * @brief 场景视图虚影用的瓦片贴图缓存项，texture 为空表示该资产无法提取贴图（预制体瓦片等）。
     */
    struct TileGhostImage
    {
        sk_sp<RuntimeTexture> texture;
        ImVec2 uv0 = ImVec2(0.0f, 0.0f);
        ImVec2 uv1 = ImVec2(1.0f, 1.0f);
    };
    const TileGhostImage& getTileGhostImage(const AssetHandle& handle);
    void drawTileGhostCell(ImDrawList* drawList, const ImVec2& screenMin, const ImVec2& screenMax,
                           const AssetHandle& handle, uint8_t rotation, bool flipX, bool flipY, ImU32 tint);
    ECS::Vector2i patternSnappedAnchor(const ECS::Vector2i& coord) const;
    void pickTileRegionAsPattern(const ECS::TilemapComponent& tilemap, const ECS::Vector2i& from,
                                 const ECS::Vector2i& to);
    entt::entity handleObjectPicking(const ECS::Vector2f& worldMousePos);
    void handleObjectDragging(const ECS::Vector2f& worldMousePos);
    void initiateDragging(const ECS::Vector2f& worldMousePos);
    bool handleColliderHandlePicking(const ECS::Vector2f& worldMousePos);
    void handleColliderHandleDragging(const ECS::Vector2f& worldMousePos);
    void drawUIRectOutline(ImDrawList* drawList, const ECS::TransformComponent& transform, const ECS::RectF& rect,
                           ImU32 outlineColor, ImU32 fillColor, float thickness);
    void drawUIRectEditHandle(ImDrawList* drawList, const ECS::TransformComponent& transform, const ECS::RectF& rect,
                              std::vector<UIRectHandle>& outHandles);
    bool handleUIRectHandlePicking(const ECS::Vector2f& worldMousePos);
    void handleUIRectHandleDragging(const ECS::Vector2f& worldMousePos);
    void handleTransformToolHotkeys();
    void drawTransformToolbar();
    void drawTransformGizmo(ImDrawList* drawList);
    void drawBoxSelection(ImDrawList* drawList);
    bool handleGizmoHandlePicking(const ECS::Vector2f& worldMousePos);
    void handleGizmoDragging(const ECS::Vector2f& worldMousePos);
    void applyWorldTransform(RuntimeGameObject& gameObject, const ECS::Vector2f& worldPosition,
                             float worldRotation, const ECS::Vector2f& worldScale);
    bool computeSelectionCenter(ECS::Vector2f& outCenter);
    bool getEntityWorldBounds(entt::entity entity, ECS::Vector2f& outMin, ECS::Vector2f& outMax);
    void finalizeBoxSelection(const ECS::Vector2f& worldMousePos);
    void selectSingleObject(const Guid& objectGuid);
    void toggleObjectSelection(const Guid& objectGuid);
    void clearSelection();
    ECS::Vector2f screenToWorldWith(const Camera::CamProperties& props, const ImVec2& screenPos) const;
    ImVec2 worldToScreenWith(const Camera::CamProperties& props, const ECS::Vector2f& worldPos) const;
    void drawCameraGizmo(ImDrawList* drawList);
    void drawDesignResolutionFrame(const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void updateParticlePreview(float deltaTime);
    void drawParticlePreview(ImDrawList* drawList, const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void setupTouchGestureCallbacks();
    void handleTouchNavigation(const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    
    // 光源 Gizmo 绘制
    void drawLightGizmos(ImDrawList* drawList, const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void drawPointLightGizmo(ImDrawList* drawList, const ECS::TransformComponent& transform, 
                             const struct ECS::PointLightComponent& light, bool isSelected);
    void drawSpotLightGizmo(ImDrawList* drawList, const ECS::TransformComponent& transform,
                            const struct ECS::SpotLightComponent& light, bool isSelected);
    void drawDirectionalLightGizmo(ImDrawList* drawList, const ECS::TransformComponent& transform,
                                   const struct ECS::DirectionalLightComponent& light, bool isSelected);
    
    // 增强光照组件 Gizmo 绘制 (Requirements: 13.1, 13.2, 13.3)
    void drawAreaLightGizmo(ImDrawList* drawList, const ECS::TransformComponent& transform,
                            const struct ECS::AreaLightComponent& light, bool isSelected);
    void drawAmbientZoneGizmo(ImDrawList* drawList, const ECS::TransformComponent& transform,
                              const struct ECS::AmbientZoneComponent& zone, bool isSelected);
    void drawLightProbeGizmo(ImDrawList* drawList, const ECS::TransformComponent& transform,
                             const struct ECS::LightProbeComponent& probe, bool isSelected);
    
    // 光照调试视图
    void drawLightingDebugOverlay(ImDrawList* drawList, const ImVec2& viewportScreenPos, const ImVec2& viewportSize);
    void drawLightingDebugUI();
private:
    std::vector<ColliderHandle> m_colliderHandles; 
    std::shared_ptr<RenderTarget> m_sceneViewTarget; 
    Camera::CamProperties m_editorCameraProperties; 
    bool m_editorCameraInitialized; 
    bool m_isDragging; 
    bool m_isEditingCollider; 
    bool m_isEditingUIRect = false; 
    ActiveColliderHandle m_activeColliderHandle; 
    std::vector<DraggedObject> m_draggedObjects; 
    std::vector<UIRectHandle> m_uiRectHandles; 
    Guid m_activeUIRectEntity; 
    entt::entity m_potentialDragEntity = entt::null; 
    ImVec2 m_mouseDownScreenPos; 
    std::vector<entt::entity> m_lastPickCandidates; 
    int m_currentPickIndex = -1; 
    ImVec2 m_lastPickScreenPos; 
    bool m_isPainting = false; 
    ECS::Vector2i m_paintStartCoord; 
    std::unordered_set<ECS::Vector2i, ECS::Vector2iHash> m_paintedCoordsThisStroke; 
    TileTool m_activeTileTool = TileTool::Brush; ///< 工具条选中的持久瓦片工具，修饰键仅作临时覆盖。
    bool m_tileToolbarHovered = false; ///< 鼠标悬停在瓦片工具条上时屏蔽绘制输入。
    std::vector<ECS::Vector2i> m_fillPreviewCoords; ///< 油漆桶填充预览的缓存区域。
    ECS::Vector2i m_fillPreviewAnchor; ///< 生成填充预览时的锚点格子。
    bool m_fillPreviewValid = false; ///< 填充预览缓存有效标记，锚点变化或瓦片数据修改后失效。
    const void* m_fillPreviewTilemap = nullptr; ///< 生成填充预览时的 Tilemap 组件地址，区分不同对象。
    uint8_t m_brushRotation = 0; ///< 单瓦片笔刷当前旋转步数（0-3，顺时针 90°），Z/X 调整。
    bool m_brushFlipX = false; ///< 单瓦片笔刷水平翻转（H）。
    bool m_brushFlipY = false; ///< 单瓦片笔刷垂直翻转（V）。
    bool m_tileSelectDragging = false; ///< 选择工具拖框进行中。
    bool m_tileSelectionValid = false; ///< 存在已确认的瓦片选区。
    ECS::Vector2i m_tileSelectStart; ///< 选区起点格。
    ECS::Vector2i m_tileSelectEnd; ///< 选区终点格（拖拽中实时更新）。
    bool m_tilePickerDragging = false; ///< 吸管拖框进行中（拖出范围则拾取为图案笔刷）。
    ECS::Vector2i m_tilePickerStart; ///< 吸管拖框起点格。
    ECS::Vector2i m_tilePickerEnd; ///< 吸管拖框终点格。
    std::unordered_map<Guid, TileGhostImage> m_tileGhostCache; ///< 虚影贴图缓存，键为瓦片资产 Guid。
    std::deque<TileUndoRecord> m_tileUndoStack; ///< 瓦片级增量撤销栈，上限 128 笔画。
    std::vector<TileUndoRecord> m_tileRedoStack; ///< 瓦片级重做栈，新笔画时清空。
    std::unordered_map<ECS::Vector2i, TileCellState, ECS::Vector2iHash> m_strokeBeforeStates; ///< 当前笔画中每个已触碰格子的初始内容。
    bool m_strokeRecording = false; ///< 正在记录一次笔画的增量。
    Guid m_strokeTilemapGuid; ///< 当前笔画所属 Tilemap 对象。
    std::unique_ptr<Particles::ParticleRenderer> m_particleRenderer; 
    std::vector<Guid> m_lastParticleSelection; 
    float m_particlePreviewTime = 0.0f;
    // 触摸手势支持(Android Pad)
    TouchGestureHandler m_touchGesture;
    bool m_touchGestureInitialized = false;
    
    // 光照调试视图
    enum class LightingDebugMode
    {
        None,           ///< 正常渲染
        LightingOnly,   ///< 仅显示光照贡献
        LightLayers,    ///< 显示光照层
        // 缓冲区调试视图 (Requirements: 12.5)
        LightBuffer,    ///< 显示光照缓冲区
        ShadowBuffer,   ///< 显示阴影缓冲区
        EmissionBuffer, ///< 显示自发光缓冲区
        NormalBuffer,   ///< 显示法线缓冲区
        GBuffer         ///< 显示 G-Buffer
    };
    LightingDebugMode m_lightingDebugMode = LightingDebugMode::None;
    uint32_t m_debugLayerMask = 0xFFFFFFFF;  ///< 调试时显示的光照层掩码
    float m_snapGridSize = 16.0f;
    bool m_snapEnabled = false;
    float m_rotationSnapDegrees = 15.0f; ///< 旋转吸附步进（度）
    TransformTool m_activeTool = TransformTool::Move;
    GizmoHandle m_activeGizmoHandle = GizmoHandle::None;
    bool m_isGizmoDragging = false;
    bool m_gizmoChanged = false; ///< 本次 gizmo 拖拽是否实际改动过变换（决定释放时是否压入撤销）
    ECS::Vector2f m_gizmoOrigin; ///< 拖拽开始时的 gizmo 原点（选中集世界坐标平均值）
    ECS::Vector2f m_gizmoDragStartWorld;
    ImVec2 m_gizmoDragStartScreen;
    std::vector<GizmoTarget> m_gizmoTargets;
    float m_gizmoDisplayAngle = 0.0f; ///< 旋转拖拽中鼠标旁显示的角度（度）
    ECS::Vector2f m_gizmoDisplayScale = {1.0f, 1.0f}; ///< 缩放拖拽中显示的倍数
    bool m_isBoxSelecting = false;
    ECS::Vector2f m_boxSelectStartWorld;
    ImVec2 m_boxSelectStartScreen;
};
#endif
