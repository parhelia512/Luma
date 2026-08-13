#include "TextureSlicerPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "AssetManager.h"
#include "Path.h"
#include "Logger.h"
#include "Profiler.h"
#include "TextureImporterSettings.h"
#include "GraphicsBackend.h"
#include "ImGuiRenderer.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <webgpu/webgpu_cpp.h>
#include "Utils/stb_image.h"
#include "Utils/stb_image_write.h"
#include "Resources/RuntimeAsset/RuntimeScene.h"
#include "Data/Tile.h"
#include "Event/EventBus.h"
#include "Event/Events.h"
constexpr size_t SLICE_PREVIEW_PERF_THRESHOLD = 500;
void TextureSlicerPanel::Initialize(EditorContext* context)
{
    m_context = context;
    m_isVisible = false;
}
void TextureSlicerPanel::Update(float deltaTime)
{
}
void TextureSlicerPanel::Draw()
{
    if (!m_isOpen)
    {
        if (m_gpuTexture)
        {
            LogInfo("TextureSlicerPanel: 释放已关闭面板的资源...");
            if (m_textureData)
            {
                stbi_image_free(m_textureData);
                m_textureData = nullptr;
            }
            m_gpuTexture.Destroy();
            m_gpuTexture = nullptr;
            m_slices.clear();
            m_currentTextureGuid = Guid::Invalid();
            m_textureID = -1;
        }
        m_isVisible = false;
        return;
    }
    if (!m_isVisible)
        return;
    PROFILE_FUNCTION();
    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("纹理切片编辑器", &m_isOpen, ImGuiWindowFlags_NoCollapse))
    {
        drawToolbar();
        ImGui::Separator();
        ImGui::BeginChild("MainContent", ImVec2(0, -40), false);
        {
            ImGui::BeginChild("TexturePreview", ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, 0), true);
            {
                drawTexturePreview();
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("RightPanel", ImVec2(0, 0), true);
            {
                drawSettingsPanel();
                ImGui::Separator();
                drawSliceList();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("应用切片", ImVec2(120, 30)))
        {
            applySlices();
        }
        ImGui::SameLine();
        // 旧的像素裁剪导出改为显式可选操作，不再是应用切片的默认行为
        if (ImGui::Button("导出为独立图片", ImVec2(140, 30)))
        {
            exportSlicesAsImages();
        }
        ImGui::SameLine();
        if (ImGui::Button("关闭", ImVec2(120, 30)))
        {
            m_isOpen = false;
            m_isVisible = false;
        }
    }
    ImGui::End();
}
void TextureSlicerPanel::Shutdown()
{
    if (m_textureData)
    {
        stbi_image_free(m_textureData);
        m_textureData = nullptr;
    }
    if (m_gpuTexture)
    {
        m_gpuTexture.Destroy();
        m_gpuTexture = nullptr;
    }
    m_slices.clear();
    m_textureID = -1;
}
void TextureSlicerPanel::OpenTexture(const Guid& textureGuid)
{
    if (m_gpuTexture)
    {
        m_gpuTexture.Destroy();
        m_gpuTexture = nullptr;
    }
    if (m_textureData)
    {
        stbi_image_free(m_textureData);
        m_textureData = nullptr;
    }
    m_textureID = -1;
    m_currentTextureGuid = textureGuid;
    m_isOpen = true;
    m_isVisible = true;
    m_slices.clear();
    m_showSlicePreviews = true;
    m_interactionMode = InteractionMode::Create;
    m_selectedSliceIndex = -1;
    m_isMovingSlice = false;
    m_isResizingSlice = false;
    loadTexture();
    if (m_textureData)
    {
        // 优先回载 .meta 中已保存的切片元数据，实现非破坏性重切
        loadSlicesFromMetadata();
        if (m_slices.empty())
        {
            m_sliceMode = SliceMode::Grid;
            m_gridRows = 1;
            m_gridColumns = 1;
            generateGridSlices();
        }
    }
}
void TextureSlicerPanel::Close()
{
    m_isOpen = false;
    m_isVisible = false;
}
void TextureSlicerPanel::drawToolbar()
{
    PROFILE_FUNCTION();
    ImGui::Text("切片模式:");
    ImGui::SameLine();
    if (ImGui::RadioButton("网格切片", m_sliceMode == SliceMode::Grid))
    {
        m_sliceMode = SliceMode::Grid;
        generateGridSlices();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("手动切片", m_sliceMode == SliceMode::Manual))
    {
        m_sliceMode = SliceMode::Manual;
        m_showSlicePreviews = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("自动切片", m_sliceMode == SliceMode::Auto))
    {
        m_sliceMode = SliceMode::Auto;
        m_showSlicePreviews = true;
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();
    ImGui::Text("交互模式:");
    ImGui::SameLine();
    if (ImGui::RadioButton("创建", m_interactionMode == InteractionMode::Create))
    {
        m_interactionMode = InteractionMode::Create;
        m_isMovingSlice = false;
        m_isResizingSlice = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("编辑", m_interactionMode == InteractionMode::Edit))
    {
        m_interactionMode = InteractionMode::Edit;
        m_isDragging = false;
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();
    if (ImGui::Button("清除所有切片", ImVec2(140, 0)))
    {
        m_slices.clear();
        m_selectedSliceIndex = -1;
        m_isMovingSlice = false;
        m_isResizingSlice = false;
        LogInfo("已清除所有切片");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("显示切片预览", &m_showSlicePreviews))
    {
        if (m_showSlicePreviews && m_slices.size() >= SLICE_PREVIEW_PERF_THRESHOLD)
        {
            LogWarn("启用切片预览，但切片数量 ({}) 较多，可能会导致性能下降。", m_slices.size());
        }
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20, 0));
    ImGui::SameLine();
    if (m_textureData)
    {
        ImGui::Text("纹理: %dx%d (%d通道) | 缩放: %.0f%%",
                    m_textureWidth, m_textureHeight, m_textureChannels, m_zoom * 100);
    }
    else
    {
        ImGui::Text("纹理: %s", m_texturePath.c_str());
    }
}
void TextureSlicerPanel::drawTexturePreview()
{
    PROFILE_FUNCTION();
    if (!m_textureData)
    {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "无法加载纹理图像");
        return;
    }
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    float scaleX = (availSize.x - 20) / m_textureWidth;
    float scaleY = (availSize.y - 20) / m_textureHeight;
    float scale = std::min(scaleX, scaleY) * m_zoom;
    ImVec2 displaySize(m_textureWidth * scale, m_textureHeight * scale);
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImVec2 imagePos(
        cursorPos.x + (availSize.x - displaySize.x) * 0.5f + m_panX,
        cursorPos.y + (availSize.y - displaySize.y) * 0.5f + m_panY
    );
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        imagePos,
        ImVec2(imagePos.x + displaySize.x, imagePos.y + displaySize.y),
        IM_COL32(50, 50, 50, 255)
    );
    if (m_textureID != -1)
    {
        drawList->AddImage(
            m_textureID,
            imagePos,
            ImVec2(imagePos.x + displaySize.x, imagePos.y + displaySize.y),
            ImVec2(0, 0),
            ImVec2(1, 1)
        );
    }
    else
    {
        ImVec2 textPos(imagePos.x + displaySize.x * 0.5f - 100, imagePos.y + displaySize.y * 0.5f);
        drawList->AddText(textPos, IM_COL32(255, 200, 0, 255), "GPU纹理加载中...");
    }
    drawList->AddRect(
        imagePos,
        ImVec2(imagePos.x + displaySize.x, imagePos.y + displaySize.y),
        IM_COL32(150, 150, 150, 255), 0.0f, 0, 1.0f
    );
    if (m_showSlicePreviews)
    {
        for (size_t i = 0; i < m_slices.size(); ++i)
        {
            const auto& slice = m_slices[i];
            ImVec2 sliceMin(
                imagePos.x + slice.rect.left() * scale,
                imagePos.y + slice.rect.top() * scale
            );
            ImVec2 sliceMax(
                imagePos.x + slice.rect.right() * scale,
                imagePos.y + slice.rect.bottom() * scale
            );
            ImU32 sliceColor = slice.selected ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 255, 0, 255);
            drawList->AddRect(sliceMin, sliceMax, sliceColor, 0.0f, 0, 2.0f);
            if (m_interactionMode == InteractionMode::Edit && slice.selected)
            {
                const float handleSize = 6.0f;
                const float midX = (sliceMin.x + sliceMax.x) * 0.5f;
                const float midY = (sliceMin.y + sliceMax.y) * 0.5f;
                // 0-3 四角，4-7 四边中点（顺序与 handlePreviewEditing 的命中判定一致）
                ImVec2 handles[8] = {
                    sliceMin,
                    ImVec2(sliceMax.x, sliceMin.y),
                    sliceMax,
                    ImVec2(sliceMin.x, sliceMax.y),
                    ImVec2(midX, sliceMin.y),
                    ImVec2(sliceMax.x, midY),
                    ImVec2(midX, sliceMax.y),
                    ImVec2(sliceMin.x, midY)
                };
                for (int c = 0; c < 8; ++c)
                {
                    ImVec2 hmin(handles[c].x - handleSize, handles[c].y - handleSize);
                    ImVec2 hmax(handles[c].x + handleSize, handles[c].y + handleSize);
                    drawList->AddRectFilled(hmin, hmax, IM_COL32(0, 200, 255, 255));
                    drawList->AddRect(hmin, hmax, IM_COL32(0, 100, 150, 255));
                }
            }
            if ((sliceMax.x - sliceMin.x) > 15.0f)
            {
                char label[32];
                snprintf(label, sizeof(label), "%zu", i);
                drawList->AddText(sliceMin, IM_COL32(255, 255, 255, 255), label);
            }
        }
    }
    else if (m_slices.size() >= SLICE_PREVIEW_PERF_THRESHOLD)
    {
        ImVec2 textPos(imagePos.x + 5, imagePos.y + 5);
        drawList->AddRectFilled(ImVec2(textPos.x - 2, textPos.y - 2), ImVec2(textPos.x + 350, textPos.y + 20),
                                IM_COL32(0, 0, 0, 150));
        drawList->AddText(textPos, IM_COL32(255, 150, 0, 255), "切片预览已关闭 (切片数 > 500)");
    }
    if (m_interactionMode == InteractionMode::Create && m_isDragging)
    {
        ImVec2 dragStartScreen(
            imagePos.x + m_dragStartX * scale,
            imagePos.y + m_dragStartY * scale
        );
        ImVec2 dragEndScreen(
            imagePos.x + m_dragEndX * scale,
            imagePos.y + m_dragEndY * scale
        );
        drawList->AddRect(dragStartScreen, dragEndScreen, IM_COL32(0, 255, 255, 255), 0.0f, 0, 2.0f);
    }
    ImGui::SetCursorScreenPos(imagePos);
    ImGui::InvisibleButton("TextureCanvas", displaySize);
    if (ImGui::IsItemHovered())
    {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0)
        {
            m_zoom = std::clamp(m_zoom + wheel * 0.1f, 0.1f, 5.0f);
        }
        if (m_sliceMode == SliceMode::Manual && m_interactionMode == InteractionMode::Create)
        {
            handleManualSlicing(imagePos, displaySize, scale);
        }
        if (m_interactionMode == InteractionMode::Edit)
        {
            handlePreviewEditing(imagePos, displaySize, scale);
        }
    }
    ImVec2 tipPos(cursorPos.x + 10, cursorPos.y + availSize.y - 50);
    drawList->AddRectFilled(ImVec2(tipPos.x - 5, tipPos.y - 5),
                            ImVec2(tipPos.x + 420, tipPos.y + 40),
                            IM_COL32(0, 0, 0, 150));
    drawList->AddText(tipPos, IM_COL32(200, 200, 200, 255),
                      "滚轮: 缩放 | 创建: 左键拖拽 | 编辑: 拖拽移动，角/边中点手柄缩放");
    ImVec2 tipPos2(tipPos.x, tipPos.y + 20);
    char tipText[128];
    snprintf(tipText, sizeof(tipText), "缩放: %.0f%% | 切片数: %zu", m_zoom * 100, m_slices.size());
    drawList->AddText(tipPos2, IM_COL32(150, 200, 255, 255), tipText);
}
void TextureSlicerPanel::drawSettingsPanel()
{
    PROFILE_FUNCTION();
    ImGui::Text("设置");
    ImGui::Separator();
    ImGui::InputText("切片名称前缀", m_namePrefix, sizeof(m_namePrefix));
    ImGui::Checkbox("应用时生成 .tile 子精灵资产", &m_generateTileAssets);
    ImGui::Spacing();
    if (m_sliceMode == SliceMode::Grid)
    {
        ImGui::Text("网格切片设置");
        ImGui::Spacing();
        bool changed = ImGui::Checkbox("使用像素单元大小", &m_usePixelGrid);
        ImGui::Spacing();
        if (m_usePixelGrid)
        {
            ImGui::Text("每个切片单元的像素大小：");
            changed |= ImGui::InputInt("单元宽度 (px)", &m_cellWidth);
            changed |= ImGui::InputInt("单元高度 (px)", &m_cellHeight);
            m_cellWidth = std::max(1, m_cellWidth);
            m_cellHeight = std::max(1, m_cellHeight);
        }
        else
        {
            ImGui::Text("网格行列数");
            changed |= ImGui::InputInt("行数", &m_gridRows);
            changed |= ImGui::InputInt("列数", &m_gridColumns);
            m_gridRows = std::max(1, m_gridRows);
            m_gridColumns = std::max(1, m_gridColumns);
        }
        changed |= ImGui::InputInt("偏移 X (px)", &m_gridOffsetX);
        changed |= ImGui::InputInt("偏移 Y (px)", &m_gridOffsetY);
        changed |= ImGui::InputInt("间距 X (px)", &m_gridSpacingX);
        changed |= ImGui::InputInt("间距 Y (px)", &m_gridSpacingY);
        m_gridOffsetX = std::max(0, m_gridOffsetX);
        m_gridOffsetY = std::max(0, m_gridOffsetY);
        m_gridSpacingX = std::max(0, m_gridSpacingX);
        m_gridSpacingY = std::max(0, m_gridSpacingY);
        if (m_textureData)
        {
            if (m_usePixelGrid)
            {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                                   "将生成 %d x %d = %d 个切片",
                                   m_gridColumns, m_gridRows, m_gridColumns * m_gridRows);
            }
            else
            {
                // 行列模式下单元大小 = (可用区域 - 间距总和) / 行列数
                int sliceWidth = (m_textureWidth - m_gridOffsetX - (m_gridColumns - 1) * m_gridSpacingX) /
                    m_gridColumns;
                int sliceHeight = (m_textureHeight - m_gridOffsetY - (m_gridRows - 1) * m_gridSpacingY) /
                    m_gridRows;
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                                   "每个切片: %d x %d 像素", sliceWidth, sliceHeight);
            }
        }
        if (changed)
        {
            generateGridSlices();
        }
        if (ImGui::Button("生成网格", ImVec2(-1, 0)))
        {
            generateGridSlices();
        }
        ImGui::Spacing();
    }
    else if (m_sliceMode == SliceMode::Auto)
    {
        ImGui::Text("自动切片设置");
        ImGui::Spacing();
        ImGui::InputInt("最小尺寸阈值 (px)", &m_autoMinSize);
        m_autoMinSize = std::max(1, m_autoMinSize);
        ImGui::TextWrapped("对 alpha > 0 的像素做 4-连通域分析，每个连通域生成一个包围盒切片；包围盒宽或高小于阈值的连通域将被过滤。");
        if (m_textureData &&
            static_cast<uint64_t>(m_textureWidth) * static_cast<uint64_t>(m_textureHeight) > 4096ull * 4096ull)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "纹理超过 4096x4096 像素，自动切片已禁用。");
        }
        if (ImGui::Button("执行自动切片", ImVec2(-1, 0)))
        {
            generateAutoSlices();
        }
        ImGui::Spacing();
    }
    if (m_sliceMode == SliceMode::Manual)
    {
        ImGui::Text("手动切片");
        ImGui::TextWrapped("在左侧图像上按住鼠标左键拖拽以创建切片区域（创建模式）；编辑模式用于移动/缩放已存在切片。");
        if (ImGui::Button("清除所有切片（工具栏已提供）", ImVec2(-1, 0)))
        {
            m_slices.clear();
            m_selectedSliceIndex = -1;
        }
        if (m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
        {
            if (ImGui::Button("删除选中切片", ImVec2(-1, 0)))
            {
                m_slices.erase(m_slices.begin() + m_selectedSliceIndex);
                m_selectedSliceIndex = -1;
            }
        }
    }
    ImGui::Separator();
    ImGui::Text("编辑选中切片（应用前可修改）");
    if (m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
    {
        SliceRect& s = m_slices[m_selectedSliceIndex];
        float left = s.rect.left();
        float top = s.rect.top();
        float width = s.rect.width();
        float height = s.rect.height();
        if (ImGui::InputFloat("Left (px)", &left))
        {
            left = std::clamp(left, 0.0f, static_cast<float>(m_textureWidth - 1));
        }
        if (ImGui::InputFloat("Top (px)", &top))
        {
            top = std::clamp(top, 0.0f, static_cast<float>(m_textureHeight - 1));
        }
        if (ImGui::InputFloat("Width (px)", &width))
        {
            width = std::max(1.0f, width);
            if (left + width > m_textureWidth) width = m_textureWidth - left;
        }
        if (ImGui::InputFloat("Height (px)", &height))
        {
            height = std::max(1.0f, height);
            if (top + height > m_textureHeight) height = m_textureHeight - top;
        }
        s.rect = SimpleRect(left, top, width, height);
        char nameBuf[256];
        strncpy(nameBuf, s.name.c_str(), sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
        {
            s.name = std::string(nameBuf);
        }
        drawPivotEditor(s);
        if (ImGui::Button("将选中切片居中在纹理中", ImVec2(-1, 0)))
        {
            float centerX = (m_textureWidth - width) * 0.5f;
            float centerY = (m_textureHeight - height) * 0.5f;
            s.rect = SimpleRect(centerX, centerY, width, height);
        }
        ImGui::SameLine();
        if (ImGui::Button("对齐到像素网格", ImVec2(-1, 0)))
        {
            float l = std::round(s.rect.left());
            float t = std::round(s.rect.top());
            float w = std::round(s.rect.width());
            float h = std::round(s.rect.height());
            if (l + w > m_textureWidth) w = m_textureWidth - l;
            if (t + h > m_textureHeight) h = m_textureHeight - t;
            s.rect = SimpleRect(l, t, w, h);
        }
    }
    else
    {
        ImGui::TextDisabled("未选择切片");
    }
}
void TextureSlicerPanel::drawPivotEditor(SliceRect& slice)
{
    ImGui::Spacing();
    ImGui::Text("轴心 (Pivot)");
    // 九宫格预设，坐标系与切片矩形一致：左上原点，Y 向下
    static const char* presetLabels[3][3] = {
        {"左上", "上", "右上"},
        {"左", "中心", "右"},
        {"左下", "下", "右下"}
    };
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            if (col > 0)
                ImGui::SameLine();
            ImGui::PushID(row * 3 + col);
            const float presetX = col * 0.5f;
            const float presetY = row * 0.5f;
            const bool active = std::fabs(slice.pivotX - presetX) < 0.0001f &&
                std::fabs(slice.pivotY - presetY) < 0.0001f;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.9f, 1.0f));
            if (ImGui::Button(presetLabels[row][col], ImVec2(52, 0)))
            {
                slice.pivotX = presetX;
                slice.pivotY = presetY;
            }
            if (active)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    float pivot[2] = {slice.pivotX, slice.pivotY};
    if (ImGui::InputFloat2("自定义 (0-1)", pivot))
    {
        slice.pivotX = std::clamp(pivot[0], 0.0f, 1.0f);
        slice.pivotY = std::clamp(pivot[1], 0.0f, 1.0f);
    }
}
void TextureSlicerPanel::drawSliceList()
{
    PROFILE_FUNCTION();
    ImGui::Text("切片列表 (%zu)", m_slices.size());
    ImGui::Separator();
    ImGui::BeginChild("SliceListScroll", ImVec2(0, 0), false);
    const float thumbnailSize = 48.0f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < m_slices.size(); ++i)
    {
        auto& slice = m_slices[i];
        ImGui::PushID(static_cast<int>(i));
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        bool selected = (m_selectedSliceIndex == static_cast<int>(i));
        if (selected)
        {
            ImVec2 selectSize(ImGui::GetContentRegionAvail().x, thumbnailSize + 8);
            drawList->AddRectFilled(cursorPos, ImVec2(cursorPos.x + selectSize.x, cursorPos.y + selectSize.y),
                                    IM_COL32(50, 100, 200, 100));
        }
        ImVec2 thumbStart(cursorPos.x + 4, cursorPos.y + 4);
        ImVec2 thumbEnd(thumbStart.x + thumbnailSize, thumbStart.y + thumbnailSize);
        if (m_textureID != -1 && m_textureWidth > 0 && m_textureHeight > 0)
        {
            const auto& rect = slice.rect;
            ImVec2 uv0(rect.left() / m_textureWidth, rect.top() / m_textureHeight);
            ImVec2 uv1(rect.right() / m_textureWidth, rect.bottom() / m_textureHeight);
            drawList->AddImage(
                m_textureID,
                thumbStart,
                thumbEnd,
                uv0,
                uv1,
                IM_COL32_WHITE
            );
        }
        else
        {
            drawList->AddRectFilled(thumbStart, thumbEnd, IM_COL32(60, 60, 60, 255));
        }
        drawList->AddRect(thumbStart, thumbEnd, IM_COL32(200, 200, 200, 255));
        ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + thumbnailSize + 12, cursorPos.y + 4));
        ImGui::Text("%s", slice.name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + thumbnailSize + 12, cursorPos.y + 24));
        // 直接编辑矩形 (x, y, w, h)：先于整行按钮提交，可优先获得鼠标交互
        float rectVals[4] = {slice.rect.x, slice.rect.y, slice.rect.w, slice.rect.h};
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 12.0f));
        if (ImGui::InputFloat4("##SliceRectEdit", rectVals, "%.0f"))
        {
            rectVals[0] = std::clamp(rectVals[0], 0.0f, static_cast<float>(m_textureWidth - 1));
            rectVals[1] = std::clamp(rectVals[1], 0.0f, static_cast<float>(m_textureHeight - 1));
            rectVals[2] = std::clamp(rectVals[2], 1.0f, static_cast<float>(m_textureWidth) - rectVals[0]);
            rectVals[3] = std::clamp(rectVals[3], 1.0f, static_cast<float>(m_textureHeight) - rectVals[1]);
            slice.rect = SimpleRect(rectVals[0], rectVals[1], rectVals[2], rectVals[3]);
        }
        ImGui::SetCursorScreenPos(cursorPos);
        if (ImGui::InvisibleButton("##SliceItem", ImVec2(ImGui::GetContentRegionAvail().x, thumbnailSize + 8)))
        {
            if (m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
            {
                m_slices[m_selectedSliceIndex].selected = false;
            }
            m_selectedSliceIndex = static_cast<int>(i);
            slice.selected = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup(("slice_context_" + std::to_string(i)).c_str());
        }
        if (ImGui::BeginPopup(("slice_context_" + std::to_string(i)).c_str()))
        {
            if (ImGui::MenuItem("删除切片"))
            {
                if (static_cast<int>(i) == m_selectedSliceIndex)
                {
                    m_selectedSliceIndex = -1;
                }
                m_slices.erase(m_slices.begin() + i);
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            if (ImGui::MenuItem("重命名"))
            {
                if (m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
                    m_slices[m_selectedSliceIndex].selected = false;
                m_selectedSliceIndex = static_cast<int>(i);
                m_slices[m_selectedSliceIndex].selected = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("位置: (%.0f, %.0f)\n大小: %.0f x %.0f\n右键: 操作菜单",
                              slice.rect.left(), slice.rect.top(),
                              slice.rect.width(), slice.rect.height());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}
namespace
{
    // 切片名可能来自用户输入，转成安全的文件名
    std::string SanitizeSliceFileName(const std::string& name, size_t fallbackIndex)
    {
        std::string result = name;
        for (char& c : result)
        {
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|')
            {
                c = '_';
            }
        }
        if (result.empty())
        {
            result = "slice_" + std::to_string(fallbackIndex);
        }
        return result;
    }

    // 把切片矩形收敛到纹理范围内，返回是否有效
    bool ClampSliceRect(const SimpleRect& in, int texWidth, int texHeight, SimpleRect& out)
    {
        float left = std::clamp(in.left(), 0.0f, static_cast<float>(texWidth));
        float top = std::clamp(in.top(), 0.0f, static_cast<float>(texHeight));
        float right = std::clamp(in.right(), 0.0f, static_cast<float>(texWidth));
        float bottom = std::clamp(in.bottom(), 0.0f, static_cast<float>(texHeight));
        if (right - left < 1.0f || bottom - top < 1.0f)
        {
            return false;
        }
        out = SimpleRect(left, top, right - left, bottom - top);
        return true;
    }
}
void TextureSlicerPanel::applySlices()
{
    PROFILE_FUNCTION();
    if (!m_textureData)
    {
        LogError("纹理图像无效");
        return;
    }
    if (m_slices.empty())
    {
        // 允许应用空列表：等价于清除纹理元数据中的全部切片
        LogInfo("切片列表为空，将清空纹理元数据中的切片定义");
    }
    const AssetMetadata* metadata = AssetManager::GetInstance().GetMetadata(m_currentTextureGuid);
    if (!metadata)
    {
        LogError("无法找到纹理元数据，切片未保存");
        return;
    }
    const std::string assetPathStr = metadata->assetPath.string();
    TextureImporterSettings settings;
    if (metadata->importerSettings && metadata->importerSettings.IsMap())
    {
        try
        {
            settings = metadata->importerSettings.as<TextureImporterSettings>();
        }
        catch (const std::exception& e)
        {
            LogWarn("解析纹理导入设置失败，将重建设置: {}", e.what());
        }
    }
    // 切片作为源纹理导入元数据保存（非破坏性），而不是裁剪像素另存文件
    settings.slices.clear();
    settings.slices.reserve(m_slices.size());
    for (size_t i = 0; i < m_slices.size(); ++i)
    {
        const SliceRect& slice = m_slices[i];
        SimpleRect clamped;
        if (!ClampSliceRect(slice.rect, m_textureWidth, m_textureHeight, clamped))
        {
            LogWarn("切片 {} 区域无效，已跳过", slice.name);
            continue;
        }
        TextureSliceEntry entry;
        entry.name = slice.name.empty() ? ("slice_" + std::to_string(i)) : slice.name;
        entry.rect = ECS::RectF(clamped.x, clamped.y, clamped.w, clamped.h);
        entry.pivot = ECS::Vector2f(slice.pivotX, slice.pivotY);
        settings.slices.push_back(std::move(entry));
    }
    AssetMetadata updatedMeta = *metadata;
    updatedMeta.importerSettings = settings;
    // ReImport 会保留 slices 字段、刷新哈希并把 .meta 落盘
    AssetManager::GetInstance().ReImport(updatedMeta);
    if (m_generateTileAssets && !m_slices.empty())
    {
        generateTileAssets();
    }
    EventBus::GetInstance().Publish(AssetUpdatedEvent{AssetType::Texture, m_currentTextureGuid});
    LogInfo("已将 {} 个切片写入纹理元数据: {}", settings.slices.size(), assetPathStr);
}
void TextureSlicerPanel::generateTileAssets()
{
    PROFILE_FUNCTION();
    const AssetMetadata* metadata = AssetManager::GetInstance().GetMetadata(m_currentTextureGuid);
    if (!metadata)
    {
        LogError("无法找到纹理元数据，跳过 .tile 生成");
        return;
    }
    ECS::FilterQuality filterQuality = ECS::FilterQuality::Bilinear;
    ECS::WrapMode wrapMode = ECS::WrapMode::Clamp;
    if (metadata->importerSettings && metadata->importerSettings.IsMap())
    {
        filterQuality = static_cast<ECS::FilterQuality>(metadata->importerSettings["filterQuality"].as<int>(1));
        wrapMode = static_cast<ECS::WrapMode>(metadata->importerSettings["wrapMode"].as<int>(0));
    }
    const std::filesystem::path relativeDir =
        metadata->assetPath.parent_path() / (metadata->assetPath.stem().string() + "_Slices");
    const std::filesystem::path fullDir = AssetManager::GetInstance().GetAssetsRootPath() / relativeDir;
    std::error_code ec;
    std::filesystem::create_directories(fullDir, ec);
    if (ec)
    {
        LogError("无法创建 .tile 输出目录: {}", fullDir.string());
        return;
    }
    size_t written = 0;
    for (size_t i = 0; i < m_slices.size(); ++i)
    {
        const SliceRect& slice = m_slices[i];
        SimpleRect clamped;
        if (!ClampSliceRect(slice.rect, m_textureWidth, m_textureHeight, clamped))
        {
            continue;
        }
        SpriteTileData spriteData;
        spriteData.textureHandle = AssetHandle(m_currentTextureGuid, AssetType::Texture);
        spriteData.sourceRect = ECS::RectF(clamped.x, clamped.y, clamped.w, clamped.h);
        spriteData.filterQuality = filterQuality;
        spriteData.wrapMode = wrapMode;
        TileAssetData tileData = spriteData;
        const std::string fileName = SanitizeSliceFileName(slice.name, i) + ".tile";
        const std::filesystem::path tilePath = fullDir / fileName;
        // 覆盖写内容不会动到旁边的 .meta，已存在的 .tile 保持 GUID 稳定，引用不失效
        std::ofstream fout(tilePath);
        if (!fout.is_open())
        {
            LogError("无法写入 .tile 资产: {}", tilePath.string());
            continue;
        }
        fout << YAML::Dump(YAML::convert<TileAssetData>::encode(tileData));
        ++written;
    }
    LogInfo("已生成/更新 {} 个 .tile 子精灵资产到 {}，资源管理器稍后自动导入", written, relativeDir.string());
}
void TextureSlicerPanel::exportSlicesAsImages()
{
    PROFILE_FUNCTION();
    if (m_slices.empty())
    {
        LogWarn("没有切片可以导出");
        return;
    }
    if (!m_textureData)
    {
        LogError("纹理图像无效");
        return;
    }
    for (size_t i = 0; i < m_slices.size(); ++i)
    {
        saveSlice(m_slices[i], static_cast<int>(i));
    }
    LogInfo("成功导出 {} 个切片为独立图片", m_slices.size());
}
void TextureSlicerPanel::loadSlicesFromMetadata()
{
    PROFILE_FUNCTION();
    m_slices.clear();
    m_selectedSliceIndex = -1;
    const AssetMetadata* metadata = AssetManager::GetInstance().GetMetadata(m_currentTextureGuid);
    if (!metadata || !metadata->importerSettings || !metadata->importerSettings.IsMap())
    {
        return;
    }
    TextureImporterSettings settings;
    try
    {
        settings = metadata->importerSettings.as<TextureImporterSettings>();
    }
    catch (const std::exception& e)
    {
        LogWarn("解析纹理导入设置失败，无法回载切片: {}", e.what());
        return;
    }
    for (const auto& entry : settings.slices)
    {
        SliceRect slice;
        slice.rect = SimpleRect(entry.rect.x, entry.rect.y, entry.rect.z, entry.rect.w);
        slice.name = entry.name;
        slice.pivotX = std::clamp(entry.pivot.x, 0.0f, 1.0f);
        slice.pivotY = std::clamp(entry.pivot.y, 0.0f, 1.0f);
        m_slices.push_back(std::move(slice));
    }
    if (!m_slices.empty())
    {
        // 已有切片时进入手动模式，避免网格设置误触重新生成而丢失编辑
        m_sliceMode = SliceMode::Manual;
        m_showSlicePreviews = (m_slices.size() < SLICE_PREVIEW_PERF_THRESHOLD);
        LogInfo("从纹理元数据回载了 {} 个切片", m_slices.size());
    }
}
void TextureSlicerPanel::generateGridSlices()
{
    PROFILE_FUNCTION();
    if (!m_textureData)
        return;
    m_slices.clear();
    m_selectedSliceIndex = -1;
    const int offsetX = std::max(0, m_gridOffsetX);
    const int offsetY = std::max(0, m_gridOffsetY);
    const int spacingX = std::max(0, m_gridSpacingX);
    const int spacingY = std::max(0, m_gridSpacingY);
    float sliceWidth, sliceHeight;
    if (m_usePixelGrid)
    {
        sliceWidth = static_cast<float>(m_cellWidth);
        sliceHeight = static_cast<float>(m_cellHeight);
        // 单元格从 offset 起、按 单元+间距 步进，只要起点仍在图内就算一列/一行（尾部允许裁剪出部分单元）
        const int availW = m_textureWidth - offsetX;
        const int availH = m_textureHeight - offsetY;
        const int stepX = m_cellWidth + spacingX;
        const int stepY = m_cellHeight + spacingY;
        m_gridColumns = availW > 0 ? (availW + stepX - 1) / stepX : 0;
        m_gridRows = availH > 0 ? (availH + stepY - 1) / stepY : 0;
        if (m_gridColumns <= 0 || m_gridRows <= 0)
        {
            LogWarn("偏移超出纹理范围，未生成任何切片");
            return;
        }
    }
    else
    {
        // 行列模式：可用区域扣除间距后均分
        const float availW = static_cast<float>(m_textureWidth - offsetX - (m_gridColumns - 1) * spacingX);
        const float availH = static_cast<float>(m_textureHeight - offsetY - (m_gridRows - 1) * spacingY);
        sliceWidth = availW / m_gridColumns;
        sliceHeight = availH / m_gridRows;
        if (sliceWidth < 1.0f || sliceHeight < 1.0f)
        {
            LogWarn("偏移/间距过大，单元尺寸不足 1 像素，未生成任何切片");
            return;
        }
    }
    for (int row = 0; row < m_gridRows; ++row)
    {
        for (int col = 0; col < m_gridColumns; ++col)
        {
            SliceRect slice;
            float x = offsetX + col * (sliceWidth + spacingX);
            float y = offsetY + row * (sliceHeight + spacingY);
            float w = sliceWidth;
            float h = sliceHeight;
            if (m_usePixelGrid)
            {
                if (x + w > m_textureWidth)
                    w = m_textureWidth - x;
                if (y + h > m_textureHeight)
                    h = m_textureHeight - y;
                if (w < 1 || h < 1)
                    continue;
            }
            slice.rect = SimpleRect(x, y, w, h);
            char nameBuf[256];
            auto formatted = std::format("{}_{}_{}", m_namePrefix, row, col);
            std::strncpy(nameBuf, formatted.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            slice.name = nameBuf;
            m_slices.push_back(slice);
        }
    }
    if (m_usePixelGrid)
    {
        LogInfo("生成了 {} 个像素级网格切片 (每个 {}x{} 像素)",
                m_slices.size(), m_cellWidth, m_cellHeight);
    }
    else
    {
        LogInfo("生成了 {} 个网格切片 ({}x{})", m_slices.size(), m_gridRows, m_gridColumns);
    }
    m_showSlicePreviews = (m_slices.size() < SLICE_PREVIEW_PERF_THRESHOLD);
}
void TextureSlicerPanel::generatePixelSizeSlices(int sliceW, int sliceH)
{
    PROFILE_FUNCTION();
    if (!m_textureData)
        return;
    m_slices.clear();
    sliceW = std::max(1, sliceW);
    sliceH = std::max(1, sliceH);
    int cols = (m_textureWidth + sliceW - 1) / sliceW;
    int rows = (m_textureHeight + sliceH - 1) / sliceH;
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            float x = c * sliceW;
            float y = r * sliceH;
            float w = static_cast<float>(sliceW);
            float h = static_cast<float>(sliceH);
            if (x + w > m_textureWidth) w = m_textureWidth - x;
            if (y + h > m_textureHeight) h = m_textureHeight - y;
            if (w < 1 || h < 1) continue;
            SliceRect slice;
            slice.rect = SimpleRect(x, y, w, h);
            char nameBuf[256];
            auto formatted = std::format("{}_{}_{}", m_namePrefix, r, c);
            std::strncpy(nameBuf, formatted.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            slice.name = nameBuf;
            m_slices.push_back(slice);
        }
    }
    LogInfo("按像素尺寸生成 {} 个切片 (每个 {}x{} px)", m_slices.size(), sliceW, sliceH);
    m_showSlicePreviews = (m_slices.size() < SLICE_PREVIEW_PERF_THRESHOLD);
}
void TextureSlicerPanel::generateAutoSlices()
{
    PROFILE_FUNCTION();
    if (!m_textureData || m_textureChannels != 4)
    {
        LogError("纹理图像无效，无法自动切片");
        return;
    }
    const int width = m_textureWidth;
    const int height = m_textureHeight;
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    constexpr uint64_t kMaxAutoSlicePixels = 4096ull * 4096ull;
    if (pixelCount > kMaxAutoSlicePixels)
    {
        LogWarn("纹理 {}x{} 超过 4096x4096 像素上限，自动切片已取消", width, height);
        return;
    }
    if (pixelCount > 2048ull * 2048ull)
    {
        LogInfo("纹理较大（{}x{}），正在分析透明度连通域，可能需要数秒...", width, height);
    }
    m_slices.clear();
    m_selectedSliceIndex = -1;
    std::vector<uint8_t> visited(pixelCount, 0);
    std::vector<int> stack;
    const unsigned char* pixels = m_textureData;
    auto isOpaque = [&](int idx) { return pixels[idx * 4 + 3] > 0; };
    int sliceIndex = 0;
    size_t filtered = 0;
    for (int startY = 0; startY < height; ++startY)
    {
        for (int startX = 0; startX < width; ++startX)
        {
            const int startIdx = startY * width + startX;
            if (visited[startIdx] || !isOpaque(startIdx))
                continue;
            // 4-连通洪泛填充，统计连通域包围盒
            int minX = startX, maxX = startX;
            int minY = startY, maxY = startY;
            visited[startIdx] = 1;
            stack.clear();
            stack.push_back(startIdx);
            while (!stack.empty())
            {
                const int idx = stack.back();
                stack.pop_back();
                const int px = idx % width;
                const int py = idx / width;
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
                const int neighbors[4] = {
                    px > 0 ? idx - 1 : -1,
                    px < width - 1 ? idx + 1 : -1,
                    py > 0 ? idx - width : -1,
                    py < height - 1 ? idx + width : -1
                };
                for (int n : neighbors)
                {
                    if (n >= 0 && !visited[n] && isOpaque(n))
                    {
                        visited[n] = 1;
                        stack.push_back(n);
                    }
                }
            }
            const int boxW = maxX - minX + 1;
            const int boxH = maxY - minY + 1;
            if (boxW < m_autoMinSize || boxH < m_autoMinSize)
            {
                ++filtered;
                continue;
            }
            SliceRect slice;
            slice.rect = SimpleRect(static_cast<float>(minX), static_cast<float>(minY),
                                    static_cast<float>(boxW), static_cast<float>(boxH));
            slice.name = std::format("{}_{}", m_namePrefix, sliceIndex++);
            m_slices.push_back(std::move(slice));
        }
    }
    LogInfo("自动切片完成: 生成 {} 个切片，过滤 {} 个小于 {}px 的噪点连通域",
            m_slices.size(), filtered, m_autoMinSize);
    m_showSlicePreviews = (m_slices.size() < SLICE_PREVIEW_PERF_THRESHOLD);
}
void TextureSlicerPanel::handleManualSlicing(ImVec2 imagePos, ImVec2 imageSize, float scale)
{
    if (!m_textureData)
        return;
    ImVec2 mousePos = ImGui::GetMousePos();
    float relX = (mousePos.x - imagePos.x) / imageSize.x;
    float relY = (mousePos.y - imagePos.y) / imageSize.y;
    float texX = relX * m_textureWidth;
    float texY = relY * m_textureHeight;
    texX = std::clamp(texX, 0.0f, static_cast<float>(m_textureWidth));
    texY = std::clamp(texY, 0.0f, static_cast<float>(m_textureHeight));
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_isDragging = true;
        m_dragStartX = texX;
        m_dragStartY = texY;
        m_dragEndX = texX;
        m_dragEndY = texY;
    }
    if (m_isDragging)
    {
        m_dragEndX = texX;
        m_dragEndY = texY;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_isDragging = false;
            SliceRect slice;
            float left = std::min(m_dragStartX, m_dragEndX);
            float top = std::min(m_dragStartY, m_dragEndY);
            float right = std::max(m_dragStartX, m_dragEndX);
            float bottom = std::max(m_dragStartY, m_dragEndY);
            slice.rect = SimpleRect(left, top, right - left, bottom - top);
            if (slice.rect.width() > 5 && slice.rect.height() > 5)
            {
                char nameBuf[256];
                std::string name = std::format("{}_{}", m_namePrefix, m_slices.size());
                auto formatted = std::format("{}_{}", m_namePrefix, m_slices.size());
                std::strncpy(nameBuf, formatted.c_str(), sizeof(nameBuf) - 1);
                nameBuf[sizeof(nameBuf) - 1] = '\0';
                slice.name = nameBuf;
                m_slices.push_back(slice);
                LogInfo("添加手动切片: {}", slice.name);
            }
        }
    }
}
void TextureSlicerPanel::handlePreviewEditing(ImVec2 imagePos, ImVec2 imageSize, float scale)
{
    if (!m_textureData)
        return;
    ImVec2 mousePos = ImGui::GetMousePos();
    float texX = (mousePos.x - imagePos.x) / scale;
    float texY = (mousePos.y - imagePos.y) / scale;
    texX = std::clamp(texX, 0.0f, static_cast<float>(m_textureWidth));
    texY = std::clamp(texY, 0.0f, static_cast<float>(m_textureHeight));
    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (clicked && m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
    {
        SliceRect& s = m_slices[m_selectedSliceIndex];
        const float midX = (s.rect.left() + s.rect.right()) * 0.5f;
        const float midY = (s.rect.top() + s.rect.bottom()) * 0.5f;
        // 0-3 四角，4-7 四边中点，与绘制顺序一致
        float handlesX[8] = {
            s.rect.left(), s.rect.right(), s.rect.right(), s.rect.left(),
            midX, s.rect.right(), midX, s.rect.left()
        };
        float handlesY[8] = {
            s.rect.top(), s.rect.top(), s.rect.bottom(), s.rect.bottom(),
            s.rect.top(), midY, s.rect.bottom(), midY
        };
        const float handleRadiusPx = 6.0f / scale; 
        for (int c = 0; c < 8; ++c)
        {
            float dx = texX - handlesX[c];
            float dy = texY - handlesY[c];
            if (dx * dx + dy * dy <= handleRadiusPx * handleRadiusPx)
            {
                m_isResizingSlice = true;
                m_isMovingSlice = false;
                m_resizeCorner = c;
                m_moveStartMouseX = texX;
                m_moveStartMouseY = texY;
                m_moveStartRect = s.rect;
                return;
            }
        }
    }
    if (m_isResizingSlice)
    {
        if (!down)
        {
            m_isResizingSlice = false;
            m_resizeCorner = -1;
            return;
        }
        SliceRect& s = m_slices[m_selectedSliceIndex];
        SimpleRect r = m_moveStartRect;
        switch (m_resizeCorner)
        {
        case 0: 
            {
                float newLeft = std::clamp(texX, 0.0f, r.right() - 1.0f);
                float newTop = std::clamp(texY, 0.0f, r.bottom() - 1.0f);
                float newW = r.right() - newLeft;
                float newH = r.bottom() - newTop;
                s.rect = SimpleRect(newLeft, newTop, newW, newH);
                break;
            }
        case 1: 
            {
                float newRight = std::clamp(texX, r.left() + 1.0f, static_cast<float>(m_textureWidth));
                float newTop = std::clamp(texY, 0.0f, r.bottom() - 1.0f);
                float newW = newRight - r.left();
                float newH = r.bottom() - newTop;
                s.rect = SimpleRect(r.left(), newTop, newW, newH);
                break;
            }
        case 2: 
            {
                float newRight = std::clamp(texX, r.left() + 1.0f, static_cast<float>(m_textureWidth));
                float newBottom = std::clamp(texY, r.top() + 1.0f, static_cast<float>(m_textureHeight));
                float newW = newRight - r.left();
                float newH = newBottom - r.top();
                s.rect = SimpleRect(r.left(), r.top(), newW, newH);
                break;
            }
        case 3: 
            {
                float newLeft = std::clamp(texX, 0.0f, r.right() - 1.0f);
                float newBottom = std::clamp(texY, r.top() + 1.0f, static_cast<float>(m_textureHeight));
                float newW = r.right() - newLeft;
                float newH = newBottom - r.top();
                s.rect = SimpleRect(newLeft, r.top(), newW, newH);
                break;
            }
        case 4: // 上边中点：只调整顶边
            {
                float newTop = std::clamp(texY, 0.0f, r.bottom() - 1.0f);
                s.rect = SimpleRect(r.left(), newTop, r.width(), r.bottom() - newTop);
                break;
            }
        case 5: // 右边中点：只调整右边
            {
                float newRight = std::clamp(texX, r.left() + 1.0f, static_cast<float>(m_textureWidth));
                s.rect = SimpleRect(r.left(), r.top(), newRight - r.left(), r.height());
                break;
            }
        case 6: // 下边中点：只调整底边
            {
                float newBottom = std::clamp(texY, r.top() + 1.0f, static_cast<float>(m_textureHeight));
                s.rect = SimpleRect(r.left(), r.top(), r.width(), newBottom - r.top());
                break;
            }
        case 7: // 左边中点：只调整左边
            {
                float newLeft = std::clamp(texX, 0.0f, r.right() - 1.0f);
                s.rect = SimpleRect(newLeft, r.top(), r.right() - newLeft, r.height());
                break;
            }
        default:
            break;
        }
        return;
    }
    if (clicked)
    {
        int hitIndex = -1;
        for (int i = static_cast<int>(m_slices.size()) - 1; i >= 0; --i)
        {
            const SliceRect& s = m_slices[i];
            if (texX >= s.rect.left() && texX <= s.rect.right() &&
                texY >= s.rect.top() && texY <= s.rect.bottom())
            {
                hitIndex = i;
                break;
            }
        }
        if (hitIndex >= 0)
        {
            if (m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
                m_slices[m_selectedSliceIndex].selected = false;
            m_selectedSliceIndex = hitIndex;
            m_slices[m_selectedSliceIndex].selected = true;
            m_isMovingSlice = true;
            m_isResizingSlice = false;
            m_moveStartMouseX = texX;
            m_moveStartMouseY = texY;
            m_moveStartRect = m_slices[m_selectedSliceIndex].rect;
        }
        else
        {
            if (m_selectedSliceIndex >= 0 && m_selectedSliceIndex < static_cast<int>(m_slices.size()))
            {
                m_slices[m_selectedSliceIndex].selected = false;
            }
            m_selectedSliceIndex = -1;
        }
    }
    if (m_isMovingSlice)
    {
        if (!down)
        {
            m_isMovingSlice = false;
            return;
        }
        if (m_selectedSliceIndex < 0 || m_selectedSliceIndex >= static_cast<int>(m_slices.size()))
        {
            m_isMovingSlice = false;
            return;
        }
        SliceRect& s = m_slices[m_selectedSliceIndex];
        float dx = texX - m_moveStartMouseX;
        float dy = texY - m_moveStartMouseY;
        float newLeft = m_moveStartRect.left() + dx;
        float newTop = m_moveStartRect.top() + dy;
        if (newLeft < 0) newLeft = 0;
        if (newTop < 0) newTop = 0;
        if (newLeft + m_moveStartRect.width() > m_textureWidth) newLeft = m_textureWidth - m_moveStartRect.width();
        if (newTop + m_moveStartRect.height() > m_textureHeight) newTop = m_textureHeight - m_moveStartRect.height();
        s.rect = SimpleRect(newLeft, newTop, m_moveStartRect.width(), m_moveStartRect.height());
    }
}
void TextureSlicerPanel::saveSlice(const SliceRect& slice, int index)
{
    if (!m_textureData)
        return;
    int left = static_cast<int>(slice.rect.left());
    int top = static_cast<int>(slice.rect.top());
    int width = static_cast<int>(slice.rect.width());
    int height = static_cast<int>(slice.rect.height());
    left = std::clamp(left, 0, m_textureWidth - 1);
    top = std::clamp(top, 0, m_textureHeight - 1);
    width = std::clamp(width, 1, m_textureWidth - left);
    height = std::clamp(height, 1, m_textureHeight - top);
    if (width <= 0 || height <= 0)
    {
        LogWarn("切片 {} 区域无效", slice.name);
        return;
    }
    int channels = m_textureChannels;
    if (channels != 4)
    {
        LogError("saveSlice: 内部状态错误，纹理通道不为 4");
        return;
    }
    unsigned char* sliceData = new unsigned char[width * height * channels];
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int srcX = left + x;
            int srcY = top + y;
            int srcIdx = (srcY * m_textureWidth + srcX) * channels;
            int dstIdx = (y * width + x) * channels;
            sliceData[dstIdx + 0] = m_textureData[srcIdx + 0];
            sliceData[dstIdx + 1] = m_textureData[srcIdx + 1];
            sliceData[dstIdx + 2] = m_textureData[srcIdx + 2];
            sliceData[dstIdx + 3] = m_textureData[srcIdx + 3];
        }
    }
    std::filesystem::path outputPath = std::filesystem::path(m_texturePath).parent_path();
    std::string filename = slice.name + ".png";
    outputPath /= filename;
    int result = stbi_write_png(outputPath.string().c_str(), width, height, channels,
                                sliceData, width * channels);
    delete[] sliceData;
    if (!result)
    {
        LogError("无法保存PNG: {}", outputPath.string());
        return;
    }
    LogInfo("保存切片: {} -> {}", slice.name, outputPath.string());
}
void TextureSlicerPanel::loadTexture()
{
    PROFILE_FUNCTION();
    auto* metadata = AssetManager::GetInstance().GetMetadata(m_currentTextureGuid);
    if (!metadata)
    {
        LogError("无法找到纹理元数据");
        return;
    }
    m_texturePath = (AssetManager::GetInstance().GetAssetsRootPath() / metadata->assetPath).string();
    int originalChannels = 0;
    m_textureData = stbi_load(m_texturePath.c_str(), &m_textureWidth, &m_textureHeight,
                              &originalChannels, 4);
    if (!m_textureData)
    {
        LogError("无法读取纹理文件: {}", m_texturePath);
        return;
    }
    m_textureChannels = 4;
    LogInfo("成功加载纹理数据: {} ({}x{}, {} channels -> 4 channels)", m_texturePath,
            m_textureWidth, m_textureHeight, originalChannels);
    if (m_context && m_context->graphicsBackend)
    {
        try
        {
            if (auto gpuTexture = m_context->graphicsBackend->LoadTextureFromFile(m_texturePath))
            {
                m_gpuTexture = gpuTexture->GetTexture();
                if (m_context->imguiRenderer)
                {
                    m_textureID = m_context->imguiRenderer->GetOrCreateTextureIdFor(m_gpuTexture);
                    LogInfo("成功创建 GPU 纹理预览");
                }
                else
                {
                    LogWarn("ImGuiRenderer 不可用，使用后备预览方式");
                }
            }
            else
            {
                LogWarn("无法创建 GPU 纹理，将使用后备预览方式");
            }
        }
        catch (const std::exception& e)
        {
            LogError("创建 GPU 纹理时发生异常: {}", e.what());
        }
    }
    else
    {
        LogWarn("GraphicsBackend 不可用，将使用后V预览方式");
    }
    if (m_defaultSliceWidthPx <= 0) m_defaultSliceWidthPx = 64;
    if (m_defaultSliceHeightPx <= 0) m_defaultSliceHeightPx = 64;
}
