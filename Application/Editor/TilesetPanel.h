#ifndef LUMAENGINE_TILESETPANEL_H
#define LUMAENGINE_TILESETPANEL_H
#pragma once
#include "EditorContext.h"
#include "Resources/RuntimeAsset/IRuntimeAsset.h"
#include "Resources/RuntimeAsset/RuntimeTileset.h"
#include "Renderer/GraphicsBackend.h"
#include <vector>
#include "IEditorPanel.h"
#include "RuntimeAsset/RuntimeRuleTile.h"
#include "RuntimeAsset/RuntimeTile.h"
#include "RuntimeAsset/RuntimeTexture.h"
class TilesetPanel : public IEditorPanel
{
public:
    TilesetPanel() = default;
    ~TilesetPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "瓦片集编辑器"; }
private:
    /**
     * @brief 单个瓦片的缩略图缓存项，texture 为空表示无法生成（如预制体瓦片）。
     */
    struct TileThumbnail
    {
        sk_sp<RuntimeTexture> texture;
        ImVec2 uv0 = ImVec2(0.0f, 0.0f);
        ImVec2 uv1 = ImVec2(1.0f, 1.0f);
    };
    /**
     * @brief 单帧内缩略图按钮的屏幕矩形与其在网格中的序号，供拖选命中测试。
     */
    struct ThumbnailRect
    {
        AssetHandle handle; 
        ImVec2 min; 
        ImVec2 max; 
        int index; 
    };
    void openTileset(Guid tilesetGuid);
    void closeCurrentTileset();
    void saveCurrentTileset();
    void drawTilesetContent();
    void handleDropTarget();
    void createTileAssetFromSource(const AssetHandle& sourceAssetHandle);
    const TileThumbnail& getOrCreateThumbnail(const AssetHandle& handle);
    void buildPatternFromThumbnailSelection(const std::vector<ThumbnailRect>& thumbRects, int columnCount,
                                            const ImVec2& rectMin, const ImVec2& rectMax);
    EditorContext* m_context = nullptr; 
    Guid m_currentTilesetGuid; 
    sk_sp<RuntimeTileset> m_currentTileset = nullptr; 
    std::vector<AssetHandle> m_tileHandles; 
    std::unordered_map<Guid, sk_sp<RuntimeTile>> m_hydratedTiles; 
    std::unordered_map<Guid, sk_sp<RuntimeRuleTile>> m_hydratedRuleTiles; 
    std::unordered_map<Guid, TileThumbnail> m_thumbnailCache; ///< 按资产 GUID 缓存的缩略图，面板打开期间持久。
    AssetHandle m_lastActiveBrushHandle; 
    void updateBrushPreview();
    float m_thumbnailSize = 64.0f; 
    bool m_thumbDragPending = false; ///< 左键已在内容区按下，等待越过位移阈值升级为拖选。
    bool m_thumbDragSelecting = false; ///< 拖选矩形进行中，抑制单击选择。
    ImVec2 m_thumbDragStart; ///< 拖选起点（屏幕坐标）。
};
#endif
