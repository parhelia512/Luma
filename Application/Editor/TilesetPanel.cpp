#include "TilesetPanel.h"
#include "imgui.h"
#include "AssetManager.h"
#include "ImGuiRenderer.h"
#include "Path.h"
#include "Loaders/TileLoader.h"
#include "Loaders/RuleTileLoader.h"
#include "Utils/Logger.h"
#include <fstream>
#include <limits>
#include <Resources/Loaders/RuleTileLoader.h>
#include <Resources/Loaders/TilesetLoader.h>
#include "Profiler.h"
#include "../Resources/RuntimeAsset/RuntimeScene.h"
#include "Loaders/TextureLoader.h"
void TilesetPanel::Initialize(EditorContext* context)
{
    m_context = context;
}
void TilesetPanel::Update(float deltaTime)
{
    PROFILE_FUNCTION();
    if (!m_isVisible) return;
    if (m_context->currentEditingTilesetGuid.Valid() && m_context->currentEditingTilesetGuid != m_currentTilesetGuid)
    {
        openTileset(m_context->currentEditingTilesetGuid);
    }
    else if (!m_context->currentEditingTilesetGuid.Valid() && m_currentTileset)
    {
        closeCurrentTileset();
    }
    if (m_context->activeTileBrush.assetGuid != m_lastActiveBrushHandle.assetGuid)
    {
        updateBrushPreview();
        m_lastActiveBrushHandle = m_context->activeTileBrush;
    }
}
void TilesetPanel::Draw()
{
    PROFILE_FUNCTION();
    if (!m_isVisible) return;
    std::string windowTitle = std::string(GetPanelName());
    if (m_currentTileset)
    {
        const auto* meta = AssetManager::GetInstance().GetMetadata(m_currentTilesetGuid);
        if (meta)
        {
            windowTitle += " - " + meta->assetPath.filename().string();
        }
    }
    if (ImGui::Begin(windowTitle.c_str(), &m_isVisible))
    {
        m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (!m_currentTileset)
        {
            ImVec2 center = ImGui::GetContentRegionAvail();
            center.x *= 0.5f;
            center.y *= 0.5f;
            ImGui::SetCursorPos(center);
            ImGui::Text("请从资源浏览器双击一个 Tileset 资产以开始编辑");
        }
        else
        {
            if (ImGui::Button("保存")) { saveCurrentTileset(); }
            ImGui::SameLine();
            if (ImGui::Button("关闭"))
            {
                m_context->currentEditingTilesetGuid = Guid();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("缩放", &m_thumbnailSize, 32.0f, 128.0f, "%.0f");
            ImGui::Separator();
            if (ImGui::BeginChild("TilesetContent"))
            {
                drawTilesetContent();
                handleDropTarget();
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}
void TilesetPanel::Shutdown()
{
    closeCurrentTileset();
}
void TilesetPanel::openTileset(Guid tilesetGuid)
{
    closeCurrentTileset();
    TilesetLoader loader;
    m_currentTileset = loader.LoadAsset(tilesetGuid);
    if (!m_currentTileset)
    {
        LogError("无法加载Tileset资产: {}", tilesetGuid.ToString());
        m_context->currentEditingTilesetGuid = Guid();
        return;
    }
    m_currentTilesetGuid = tilesetGuid;
    m_tileHandles = m_currentTileset->GetData().tiles;
    TileLoader tileLoader;
    RuleTileLoader ruleTileLoader;
    for (const auto& handle : m_tileHandles)
    {
        if (handle.assetType == AssetType::Tile && !m_hydratedTiles.contains(handle.assetGuid))
        {
            m_hydratedTiles[handle.assetGuid] = tileLoader.LoadAsset(handle.assetGuid);
        }
        else if (handle.assetType == AssetType::RuleTile && !m_hydratedRuleTiles.contains(handle.assetGuid))
        {
            m_hydratedRuleTiles[handle.assetGuid] = ruleTileLoader.LoadAsset(handle.assetGuid);
        }
    }
}
void TilesetPanel::closeCurrentTileset()
{
    m_currentTileset = nullptr;
    m_currentTilesetGuid = Guid();
    m_tileHandles.clear();
    m_hydratedTiles.clear();
    m_hydratedRuleTiles.clear();
    m_thumbnailCache.clear();
}
void TilesetPanel::saveCurrentTileset()
{
    if (!m_currentTileset) return;
    const auto* meta = AssetManager::GetInstance().GetMetadata(m_currentTilesetGuid);
    if (!meta)
    {
        LogError("找不到Tileset元数据，保存失败");
        return;
    }
    TilesetData data;
    data.tiles = m_tileHandles;
    YAML::Node node = YAML::convert<TilesetData>::encode(data);
    std::ofstream fout(AssetManager::GetInstance().GetAssetsRootPath() / meta->assetPath);
    fout << node;
    fout.close();
    LogInfo("Tileset资产已保存: {}", meta->assetPath.string());
}
void TilesetPanel::drawTilesetContent()
{
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = std::max(1, static_cast<int>(panelWidth / (m_thumbnailSize + 20.0f)));
    // 本帧所有缩略图按钮的屏幕矩形，供拖选矩形做命中测试
    std::vector<ThumbnailRect> thumbRects;
    thumbRects.reserve(m_tileHandles.size());
    int pendingRemoveIndex = -1;
    if (ImGui::BeginTable("TilesetGrid", columnCount))
    {
        for (int index = 0; index < static_cast<int>(m_tileHandles.size()); ++index)
        {
            const AssetHandle handle = m_tileHandles[index];
            ImGui::TableNextColumn();
            ImGui::PushID(handle.assetGuid.ToString().c_str());
            const auto* meta = AssetManager::GetInstance().GetMetadata(handle.assetGuid);
            std::string name = meta ? meta->assetPath.stem().string() : "无效资产";
            bool isSelected = m_context->activeTileBrush.assetGuid == handle.assetGuid;
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            }
            const TileThumbnail& thumb = getOrCreateThumbnail(handle);
            wgpu::Texture gpuTexture = (thumb.texture && thumb.texture->getNutTexture())
                                           ? thumb.texture->getNutTexture()->GetTexture()
                                           : wgpu::Texture();
            bool clicked = false;
            if (gpuTexture)
            {
                ImTextureID texId = m_context->imguiRenderer->GetOrCreateTextureIdFor(gpuTexture);
                clicked = ImGui::ImageButton("##thumb", texId, ImVec2(m_thumbnailSize, m_thumbnailSize),
                                             thumb.uv0, thumb.uv1);
            }
            else
            {
                // 无法提取纹理（预制体瓦片、缺失资产等）时退回文字按钮
                clicked = ImGui::Button(name.c_str(), ImVec2(m_thumbnailSize, m_thumbnailSize));
            }
            thumbRects.push_back({handle, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), index});
            if (clicked && !m_thumbDragSelecting)
            {
                // 单击回到单瓦片笔刷，退出图案模式
                m_context->activeTileBrush = handle;
                m_context->activeTileBrushPattern.clear();
            }
            if (isSelected)
            {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                    IM_COL32(255, 200, 60, 255), 3.0f, 0, 2.0f);
            }
            if (ImGui::BeginPopupContextItem("TileItemContext"))
            {
                if (ImGui::MenuItem("删除"))
                {
                    // 延迟到循环外删除，避免遍历中修改容器
                    pendingRemoveIndex = index;
                }
                ImGui::EndPopup();
            }
            if (isSelected)
            {
                ImGui::PopStyleColor();
            }
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("DRAG_DROP_ASSET_HANDLE", &m_tileHandles[index], sizeof(AssetHandle));
                ImGui::Text("%s", name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::TextWrapped("%s", name.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (pendingRemoveIndex >= 0 && pendingRemoveIndex < static_cast<int>(m_tileHandles.size()))
    {
        const AssetHandle removed = m_tileHandles[pendingRemoveIndex];
        if (m_context->activeTileBrush.assetGuid == removed.assetGuid) { m_context->activeTileBrush = {}; }
        m_hydratedTiles.erase(removed.assetGuid);
        m_hydratedRuleTiles.erase(removed.assetGuid);
        m_thumbnailCache.erase(removed.assetGuid);
        m_tileHandles.erase(m_tileHandles.begin() + pendingRemoveIndex);
    }
    // 缩略图网格拖选：划出矩形把命中的瓦片按网格相对行列组成多瓦片图案笔刷
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_thumbDragStart = io.MousePos;
        m_thumbDragPending = true;
    }
    if (m_thumbDragPending && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f))
    {
        m_thumbDragSelecting = true;
    }
    if (m_thumbDragSelecting)
    {
        const ImVec2 rectMin = {std::min(m_thumbDragStart.x, io.MousePos.x), std::min(m_thumbDragStart.y, io.MousePos.y)};
        const ImVec2 rectMax = {std::max(m_thumbDragStart.x, io.MousePos.x), std::max(m_thumbDragStart.y, io.MousePos.y)};
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(rectMin, rectMax, IM_COL32(80, 160, 255, 40));
        drawList->AddRect(rectMin, rectMax, IM_COL32(80, 160, 255, 220));
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            buildPatternFromThumbnailSelection(thumbRects, columnCount, rectMin, rectMax);
            m_thumbDragSelecting = false;
            m_thumbDragPending = false;
        }
    }
    else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_thumbDragPending = false;
    }
}
void TilesetPanel::buildPatternFromThumbnailSelection(const std::vector<ThumbnailRect>& thumbRects, int columnCount,
                                                      const ImVec2& rectMin, const ImVec2& rectMax)
{
    // 调色板按添加顺序网格排列，相对坐标取网格行列差
    struct Hit
    {
        AssetHandle handle;
        int row;
        int col;
    };
    std::vector<Hit> hits;
    for (const auto& thumb : thumbRects)
    {
        const bool overlap = thumb.max.x >= rectMin.x && thumb.min.x <= rectMax.x &&
            thumb.max.y >= rectMin.y && thumb.min.y <= rectMax.y;
        if (overlap)
        {
            hits.push_back({thumb.handle, thumb.index / columnCount, thumb.index % columnCount});
        }
    }
    if (hits.empty()) return;
    if (hits.size() == 1)
    {
        // 只框到一个缩略图等价于单击选择
        m_context->activeTileBrush = hits[0].handle;
        m_context->activeTileBrushPattern.clear();
        return;
    }
    int minRow = std::numeric_limits<int>::max();
    int minCol = std::numeric_limits<int>::max();
    for (const auto& hit : hits)
    {
        minRow = std::min(minRow, hit.row);
        minCol = std::min(minCol, hit.col);
    }
    std::vector<TileBrushPatternCell> pattern;
    pattern.reserve(hits.size());
    for (const auto& hit : hits)
    {
        pattern.push_back({{hit.col - minCol, hit.row - minRow}, hit.handle, 0, false, false});
    }
    m_context->activeTileBrushPattern = std::move(pattern);
    // 瓦片编辑模式由 activeTileBrush 有效性开启，用图案首格句柄保底
    m_context->activeTileBrush = hits[0].handle;
}
void TilesetPanel::handleDropTarget()
{
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DRAG_DROP_ASSET_HANDLE"))
        {
            AssetHandle droppedHandle = *static_cast<const AssetHandle*>(payload->Data);
            if (droppedHandle.assetType == AssetType::Texture || droppedHandle.assetType == AssetType::Prefab)
            {
                createTileAssetFromSource(droppedHandle);
            }
            else if (droppedHandle.assetType == AssetType::Tile || droppedHandle.assetType == AssetType::RuleTile)
            {
                bool exists = false;
                for (const auto& h : m_tileHandles)
                {
                    if (h.assetGuid == droppedHandle.assetGuid)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                {
                    m_tileHandles.push_back(droppedHandle);
                    LogInfo("已将资产 {} 添加到Tileset", droppedHandle.assetGuid.ToString());
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}
void TilesetPanel::createTileAssetFromSource(const AssetHandle& sourceAssetHandle)
{
    auto& assetManager = AssetManager::GetInstance();
    const auto* sourceMeta = assetManager.GetMetadata(sourceAssetHandle.assetGuid);
    if (!sourceMeta)
    {
        LogError("找不到源资产的元数据");
        return;
    }
    std::filesystem::path sourcePath = sourceMeta->assetPath;
    std::filesystem::path tilesDir = sourcePath.parent_path() / "Tiles";
    std::filesystem::create_directories(assetManager.GetAssetsRootPath() / tilesDir);
    std::string newAssetName = sourcePath.stem().string() + ".tile";
    std::filesystem::path newAssetRelativePath = tilesDir / newAssetName;
    std::filesystem::path newAssetFullPath = assetManager.GetAssetsRootPath() / newAssetRelativePath;
    TileAssetData tileData;
    if (sourceAssetHandle.assetType == AssetType::Texture)
    {
        SpriteTileData spriteData;
        spriteData.textureHandle = sourceAssetHandle;
        tileData = spriteData;
    }
    else if (sourceAssetHandle.assetType == AssetType::Prefab)
    {
        PrefabTileData prefabData;
        prefabData.prefabHandle = sourceAssetHandle;
        tileData = prefabData;
    }
    else
    {
        return;
    }
    YAML::Node node = YAML::convert<TileAssetData>::encode(tileData);
    std::ofstream fout(newAssetFullPath);
    fout << node;
    fout.close();
    LogInfo("已自动创建Tile资产: {}. 请从资源浏览器中将其拖入本面板。", newAssetRelativePath.string());
}
const TilesetPanel::TileThumbnail& TilesetPanel::getOrCreateThumbnail(const AssetHandle& handle)
{
    auto cached = m_thumbnailCache.find(handle.assetGuid);
    if (cached != m_thumbnailCache.end()) return cached->second;
    TileThumbnail thumb;
    // 与 updateBrushPreview 相同的提取逻辑：RuleTile 取默认瓦片，精灵瓦片取纹理与源矩形
    AssetHandle finalTileHandle;
    if (handle.assetType == AssetType::Tile)
    {
        finalTileHandle = handle;
    }
    else if (handle.assetType == AssetType::RuleTile)
    {
        RuleTileLoader ruleTileLoader;
        sk_sp<RuntimeRuleTile> ruleTile = ruleTileLoader.LoadAsset(handle.assetGuid);
        if (ruleTile)
        {
            finalTileHandle = ruleTile->GetData().defaultTileHandle;
        }
    }
    if (finalTileHandle.Valid())
    {
        TileLoader tileLoader;
        sk_sp<RuntimeTile> tileAsset = tileLoader.LoadAsset(finalTileHandle.assetGuid);
        if (tileAsset && std::holds_alternative<SpriteTileData>(tileAsset->GetData()))
        {
            const auto& spriteData = std::get<SpriteTileData>(tileAsset->GetData());
            if (spriteData.textureHandle.Valid())
            {
                TextureLoader textureLoader(*m_context->graphicsBackend);
                thumb.texture = textureLoader.LoadAsset(spriteData.textureHandle.assetGuid);
                if (thumb.texture && thumb.texture->getImage() &&
                    spriteData.sourceRect.Width() > 0 && spriteData.sourceRect.Height() > 0)
                {
                    const float texW = static_cast<float>(thumb.texture->getImage()->width());
                    const float texH = static_cast<float>(thumb.texture->getImage()->height());
                    if (texW > 0.0f && texH > 0.0f)
                    {
                        thumb.uv0 = ImVec2(spriteData.sourceRect.x / texW, spriteData.sourceRect.y / texH);
                        thumb.uv1 = ImVec2((spriteData.sourceRect.x + spriteData.sourceRect.Width()) / texW,
                                           (spriteData.sourceRect.y + spriteData.sourceRect.Height()) / texH);
                    }
                }
            }
        }
    }
    return m_thumbnailCache.emplace(handle.assetGuid, std::move(thumb)).first->second;
}
void TilesetPanel::updateBrushPreview()
{
    m_context->activeBrushPreviewImage = nullptr;
    m_context->activeBrushPreviewSourceRect = SkRect::MakeEmpty();
    const AssetHandle& brushHandle = m_context->activeTileBrush;
    if (!brushHandle.Valid()) return;
    TileLoader tileLoader;
    RuleTileLoader ruleTileLoader;
    TextureLoader textureLoader(*m_context->graphicsBackend);
    AssetHandle finalTileHandle;
    if (brushHandle.assetType == AssetType::Tile)
    {
        finalTileHandle = brushHandle;
    }
    else if (brushHandle.assetType == AssetType::RuleTile)
    {
        sk_sp<RuntimeRuleTile> ruleTile = ruleTileLoader.LoadAsset(brushHandle.assetGuid);
        if (ruleTile)
        {
            finalTileHandle = ruleTile->GetData().defaultTileHandle;
        }
    }
    if (!finalTileHandle.Valid()) return;
    sk_sp<RuntimeTile> tileAsset = tileLoader.LoadAsset(finalTileHandle.assetGuid);
    if (tileAsset && std::holds_alternative<SpriteTileData>(tileAsset->GetData()))
    {
        const auto& spriteData = std::get<SpriteTileData>(tileAsset->GetData());
        if (spriteData.textureHandle.Valid())
        {
            m_context->activeBrushPreviewImage = textureLoader.LoadAsset(spriteData.textureHandle.assetGuid);
            if (m_context->activeBrushPreviewImage)
            {
                if (spriteData.sourceRect.Width() <= 0 || spriteData.sourceRect.Height() <= 0)
                {
                    m_context->activeBrushPreviewSourceRect = SkRect::MakeWH(
                        m_context->activeBrushPreviewImage->getImage()->width(),
                        m_context->activeBrushPreviewImage->getImage()->height());
                }
                else
                {
                    m_context->activeBrushPreviewSourceRect = SkRect::MakeXYWH(
                        spriteData.sourceRect.x, spriteData.sourceRect.y, spriteData.sourceRect.Width(),
                        spriteData.sourceRect.Height());
                }
            }
        }
    }
}
