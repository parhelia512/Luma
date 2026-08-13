#include "AssetInspectorPanel.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "AssetImporterRegistry.h"
#include "Resources/AssetManager.h"
#include "Utils/Logger.h"
#include "Profiler.h"
#include <fstream>
#include <any>
#include <cctype>
#include <algorithm>
#include <limits>
#include <variant>
#include <unordered_set>
#include "Resources/RuntimeAsset/RuntimeScene.h"
#include "Editor.h"
#include "TextureSlicerPanel.h"
#include "ShaderEditorPanel.h"
#include "ImGuiRenderer.h"
#include "Data/Tile.h"
#include "Loaders/TextureLoader.h"

namespace
{
    std::string TrimString(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        {
            ++start;
        }

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    std::vector<std::string> ParseGroupNames(const std::string& input)
    {
        std::vector<std::string> groups;
        std::unordered_set<std::string> seen;
        std::string token;

        auto flushToken = [&]()
        {
            std::string trimmed = TrimString(token);
            if (!trimmed.empty() && seen.insert(trimmed).second)
            {
                groups.push_back(trimmed);
            }
            token.clear();
        };

        for (char ch : input)
        {
            if (ch == ',' || ch == ';' || ch == '\n')
            {
                flushToken();
            }
            else
            {
                token.push_back(ch);
            }
        }
        flushToken();

        return groups;
    }

    std::string JoinGroupNames(const std::vector<std::string>& groups)
    {
        std::string result;
        bool first = true;
        for (const auto& entry : groups)
        {
            std::string trimmed = TrimString(entry);
            if (trimmed.empty())
            {
                continue;
            }
            if (!first)
            {
                result += ", ";
            }
            result += trimmed;
            first = false;
        }
        return result;
    }

    std::vector<std::string> NormalizeGroupNames(const std::vector<std::string>& groups)
    {
        std::vector<std::string> output;
        std::unordered_set<std::string> seen;
        for (const auto& entry : groups)
        {
            std::string trimmed = TrimString(entry);
            if (!trimmed.empty() && seen.insert(trimmed).second)
            {
                output.push_back(trimmed);
            }
        }
        std::sort(output.begin(), output.end());
        return output;
    }

    /**
     * @brief 判断多边形是否为凸（按顶点顺序检查相邻边叉积符号是否一致）。
     */
    bool IsConvexPolygon(const std::vector<ECS::Vector2f>& vertices)
    {
        const size_t count = vertices.size();
        if (count < 4) return true;
        bool hasPositive = false;
        bool hasNegative = false;
        for (size_t i = 0; i < count; ++i)
        {
            const ECS::Vector2f& a = vertices[i];
            const ECS::Vector2f& b = vertices[(i + 1) % count];
            const ECS::Vector2f& c = vertices[(i + 2) % count];
            const float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
            if (cross > 1e-6f) hasPositive = true;
            else if (cross < -1e-6f) hasNegative = true;
            if (hasPositive && hasNegative) return false;
        }
        return true;
    }

    float PointSegmentDistSq(const ECS::Vector2f& p, const ECS::Vector2f& a, const ECS::Vector2f& b)
    {
        const float abx = b.x - a.x;
        const float aby = b.y - a.y;
        const float lenSq = abx * abx + aby * aby;
        float t = lenSq > 1e-12f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / lenSq : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float dx = p.x - (a.x + t * abx);
        const float dy = p.y - (a.y + t * aby);
        return dx * dx + dy * dy;
    }
}
void AssetInspectorPanel::Initialize(EditorContext* context)
{
    m_context = context;
}
void AssetInspectorPanel::Update(float deltaTime)
{
}
void AssetInspectorPanel::Draw()
{
    PROFILE_FUNCTION();
    {
        PROFILE_SCOPE("AssetInspectorPanel::ImGui::Begin");
        ImGui::Begin(GetPanelName(), &m_isVisible);
        m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    }
    {
        PROFILE_SCOPE("AssetInspectorPanel::CheckSelectionChange");
        if (m_context->selectedAssets != m_currentEditingPaths)
        {
            resetStateFromSelection();
        }
    }
    {
        PROFILE_SCOPE("AssetInspectorPanel::drawInspectorUI");
        drawInspectorUI();
    }
    {
        PROFILE_SCOPE("AssetInspectorPanel::ImGui::End");
        ImGui::End();
    }
}
void AssetInspectorPanel::Shutdown()
{
    m_currentEditingPaths.clear();
    m_deserializedSettings.reset();
    m_isDeserialized = false;
    m_mixedValueProperties.clear();
    m_dirtyProperties.clear();
    m_addressName.clear();
    m_groupNamesInput.clear();
    m_addressMixed = false;
    m_groupMixed = false;
    m_addressDirty = false;
    m_groupDirty = false;
    m_tileVertexDragIndex = -1;
    m_tilePreviewTexture = nullptr;
    m_tilePreviewTextureGuid = Guid();
}
void AssetInspectorPanel::resetStateFromSelection()
{
    PROFILE_FUNCTION();
    m_currentEditingPaths = m_context->selectedAssets;
    m_deserializedSettings.reset();
    m_isDeserialized = false;
    m_mixedValueProperties.clear();
    m_dirtyProperties.clear();
    m_addressName.clear();
    m_groupNamesInput.clear();
    m_addressMixed = false;
    m_groupMixed = false;
    m_addressDirty = false;
    m_groupDirty = false;
    m_tileVertexDragIndex = -1;
    m_editingAssetType = AssetType::Unknown;
    if (m_currentEditingPaths.empty()) return;
    const AssetMetadata* firstMetadata = nullptr;
    {
        PROFILE_SCOPE("AssetInspectorPanel::GetFirstMetadata");
        firstMetadata = AssetManager::GetInstance().GetMetadata(m_currentEditingPaths[0]);
    }
    if (!firstMetadata)
    {
        m_currentEditingPaths.clear();
        return;
    }
    m_editingAssetType = firstMetadata->type;
    m_addressName = firstMetadata->addressName;
    m_groupNamesInput = JoinGroupNames(firstMetadata->groupNames);
    const auto normalizedFirstGroups = NormalizeGroupNames(firstMetadata->groupNames);
    {
        PROFILE_SCOPE("AssetInspectorPanel::ValidateMultiSelection");
        for (size_t i = 1; i < m_currentEditingPaths.size(); ++i)
        {
            const AssetMetadata* metadata = AssetManager::GetInstance().GetMetadata(m_currentEditingPaths[i]);
            if (!metadata || metadata->type != m_editingAssetType)
            {
                m_editingAssetType = AssetType::Unknown;
                m_deserializedSettings.reset();
                m_isDeserialized = false;
                return;
            }
            if (metadata->addressName != m_addressName)
            {
                m_addressMixed = true;
            }
            if (NormalizeGroupNames(metadata->groupNames) != normalizedFirstGroups)
            {
                m_groupMixed = true;
            }
        }
    }
    const auto* registration = AssetImporterRegistry::GetInstance().Get(m_editingAssetType);
    if (!registration || !registration->Deserialize)
    {
        LogWarn("资产类型 {} (ID: {}) 没有注册反序列化函数。",
                firstMetadata->assetPath.string(), (int)m_editingAssetType);
        m_editingAssetType = AssetType::Unknown;
        return;
    }
    try
    {
        PROFILE_SCOPE("AssetInspectorPanel::DeserializeSettings");
        YAML::Node sourceSettings = firstMetadata->importerSettings;
        m_deserializedSettings = registration->Deserialize(sourceSettings);
        m_isDeserialized = true;
    }
    catch (const std::exception& e)
    {
        LogError("反序列化资产 {} 的设置失败: {}", firstMetadata->assetPath.string(), e.what());
        m_isDeserialized = false;
        m_deserializedSettings.reset();
    }
    {
        PROFILE_SCOPE("AssetInspectorPanel::DetectMixedValues");
        if (m_currentEditingPaths.size() > 1)
        {
            if (!registration) return;
            for (const auto& [propName, propInfo] : registration->properties)
            {
                const YAML::Node& firstValueNode = firstMetadata->importerSettings[propName];
                if (!firstValueNode) continue;
                for (size_t i = 1; i < m_currentEditingPaths.size(); ++i)
                {
                    const AssetMetadata* otherMetadata = AssetManager::GetInstance().GetMetadata(
                        m_currentEditingPaths[i]);
                    const YAML::Node& otherValueNode = otherMetadata->importerSettings[propName];
                    if (!otherValueNode || firstValueNode.Scalar() != otherValueNode.Scalar())
                    {
                        m_mixedValueProperties.insert(propName);
                        break;
                    }
                }
            }
        }
    }
}
void AssetInspectorPanel::drawInspectorUI()
{
    PROFILE_FUNCTION();
    if (m_currentEditingPaths.empty())
    {
        ImGui::Text("请在资源浏览器中选择一个或多个资产以查看其设置。");
        return;
    }
    if (m_editingAssetType == AssetType::Unknown)
    {
        ImGui::Text("选择了多个不同类型的资产。请选择相同类型的资产进行批量编辑。");
        return;
    }
    {
        PROFILE_SCOPE("AssetInspectorPanel::DrawHeader");
        if (m_currentEditingPaths.size() == 1)
        {
            ImGui::Text("资产: %s", m_currentEditingPaths[0].filename().string().c_str());
        }
        else
        {
            ImGui::Text("正在编辑 %zu 个资产", m_currentEditingPaths.size());
        }
        ImGui::Separator();
    }

    {
        PROFILE_SCOPE("AssetInspectorPanel::DrawAddressables");
        ImGui::Text("Addressable");
        ImGui::Separator();

        ImGui::PushItemWidth(-1);
        if (m_addressMixed)
        {
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        }
        ImGui::Text("地址");
        if (ImGui::InputTextWithHint("##AssetAddress", "默认使用资产路径", &m_addressName))
        {
            m_addressDirty = true;
            m_addressMixed = false;
        }
        if (m_addressMixed)
        {
            ImGui::PopItemFlag();
        }

        if (m_groupMixed)
        {
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        }
        ImGui::Text("分组");
        if (ImGui::InputTextWithHint("##AssetGroups", "用逗号分隔多个分组", &m_groupNamesInput))
        {
            m_groupDirty = true;
            m_groupMixed = false;
        }
        if (m_groupMixed)
        {
            ImGui::PopItemFlag();
        }
        ImGui::PopItemWidth();
        ImGui::Spacing();
    }
    const auto* registration = AssetImporterRegistry::GetInstance().Get(m_editingAssetType);
    const bool canEditImporterSettings = registration && m_isDeserialized && registration->GetDataPointer;
    void* dataPtr = nullptr;
    if (!canEditImporterSettings)
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "无法编辑的资产 (未注册或反序列化失败)");
    }
    else
    {
        try
        {
            dataPtr = registration->GetDataPointer(m_deserializedSettings);
        }
        catch (const std::exception& e)
        {
            LogError("获取数据指针失败: {}", e.what());
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "获取数据指针失败 (std::any_cast 异常)");
            dataPtr = nullptr;
        }

        if (!dataPtr)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "内部错误: 数据指针为空。");
        }
    }

    if (dataPtr)
    {
        PROFILE_SCOPE("AssetInspectorPanel::DrawProperties");
        for (const auto& [name, prop] : registration->properties)
        {
            if (name == "rawData")
            {
                continue;
            }
            if (prop.isExposedInEditor)
            {
                PROFILE_SCOPE("AssetInspectorPanel::DrawSingleProperty");
                bool isMixed = m_mixedValueProperties.count(name);
                if (isMixed)
                {
                    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                }
                bool changed = false;
                {
                    std::string scopeName = "AssetInspectorPanel::Property::" + name;
                    PROFILE_SCOPE(scopeName.c_str());
                    changed = prop.draw_ui(name, dataPtr);
                }
                if (changed)
                {
                    m_dirtyProperties.insert(name);
                    if (isMixed)
                    {
                        m_mixedValueProperties.erase(name);
                    }
                }
                if (isMixed)
                {
                    ImGui::PopItemFlag();
                }
            }
        }
    }
    if (m_editingAssetType == AssetType::Tile && m_currentEditingPaths.size() == 1 && dataPtr)
    {
        PROFILE_SCOPE("AssetInspectorPanel::TileColliderEditor");
        drawTileColliderEditor(dataPtr);
    }
    if (m_editingAssetType == AssetType::Texture && m_currentEditingPaths.size() == 1)
    {
        PROFILE_SCOPE("AssetInspectorPanel::TextureSlicerButton");
        ImGui::Separator();
        if (ImGui::Button("打开切片编辑器", ImVec2(-1, 30)))
        {
            openTextureSlicer();
        }
    }
    if (m_editingAssetType == AssetType::Material && m_currentEditingPaths.size() == 1)
    {
        PROFILE_SCOPE("AssetInspectorPanel::ShaderEditorButton");
        ImGui::Separator();
        if (ImGui::Button("打开Shader编辑器", ImVec2(-1, 30)))
        {
            openShaderEditor();
        }
    }
    {
        PROFILE_SCOPE("AssetInspectorPanel::DrawActionButtons");
        if (!m_dirtyProperties.empty() || m_addressDirty || m_groupDirty)
        {
            if (ImGui::Button("应用"))
            {
                applyChanges();
            }
            ImGui::SameLine();
            if (ImGui::Button("撤销"))
            {
                resetStateFromSelection();
            }
        }
    }
}
void AssetInspectorPanel::drawTileColliderEditor(void* dataPtr)
{
    auto* tileData = static_cast<TileAssetData*>(dataPtr);
    auto* spriteData = std::get_if<SpriteTileData>(tileData);
    if (!spriteData)
    {
        return; // 预制体瓦片没有瓦片级物理形状
    }

    ImGui::Separator();
    ImGui::Text("物理形状");

    bool changed = false;
    const char* colliderTypeNames[] = {"无碰撞", "整格碰撞", "自定义多边形"};
    int colliderType = (spriteData->colliderType < 0 || spriteData->colliderType > 2) ? 1 : spriteData->colliderType;
    if (ImGui::Combo("碰撞类型", &colliderType, colliderTypeNames, IM_ARRAYSIZE(colliderTypeNames)))
    {
        spriteData->colliderType = colliderType;
        // 切到自定义且尚无顶点时以整格四角起步
        if (colliderType == 2 && spriteData->colliderVertices.empty())
        {
            spriteData->colliderVertices = {
                {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
            };
        }
        changed = true;
    }

    if (spriteData->colliderType == 2)
    {
        auto& vertices = spriteData->colliderVertices;

        // 预览纹理按 GUID 缓存，选中其它瓦片或换贴图时失效
        const Guid textureGuid = spriteData->textureHandle.assetGuid;
        if (m_tilePreviewTextureGuid != textureGuid)
        {
            m_tilePreviewTexture = nullptr;
            m_tilePreviewTextureGuid = textureGuid;
            if (textureGuid.Valid() && m_context->graphicsBackend)
            {
                TextureLoader textureLoader(*m_context->graphicsBackend);
                m_tilePreviewTexture = textureLoader.LoadAsset(textureGuid);
            }
        }

        const float canvasSize = 128.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        const ImVec2 canvasMax = ImVec2(canvasPos.x + canvasSize, canvasPos.y + canvasSize);
        ImGui::InvisibleButton("##TileColliderCanvas", ImVec2(canvasSize, canvasSize));
        const bool canvasHovered = ImGui::IsItemHovered();

        drawList->AddRectFilled(canvasPos, canvasMax, IM_COL32(30, 30, 30, 255));
        if (m_tilePreviewTexture && m_tilePreviewTexture->getNutTexture() && m_context->imguiRenderer)
        {
            ImVec2 uv0(0.0f, 0.0f);
            ImVec2 uv1(1.0f, 1.0f);
            const ECS::RectF& rect = spriteData->sourceRect;
            if (m_tilePreviewTexture->getImage() && rect.Width() > 0.0f && rect.Height() > 0.0f)
            {
                const float texW = static_cast<float>(m_tilePreviewTexture->getImage()->width());
                const float texH = static_cast<float>(m_tilePreviewTexture->getImage()->height());
                if (texW > 0.0f && texH > 0.0f)
                {
                    uv0 = ImVec2(rect.x / texW, rect.y / texH);
                    uv1 = ImVec2((rect.x + rect.Width()) / texW, (rect.y + rect.Height()) / texH);
                }
            }
            ImTextureID texId = m_context->imguiRenderer->GetOrCreateTextureIdFor(
                m_tilePreviewTexture->getNutTexture()->GetTexture());
            drawList->AddImage(texId, canvasPos, canvasMax, uv0, uv1);
        }
        drawList->AddRect(canvasPos, canvasMax, IM_COL32(120, 120, 120, 255));

        auto toScreen = [&](const ECS::Vector2f& v)
        {
            return ImVec2(canvasPos.x + v.x * canvasSize, canvasPos.y + v.y * canvasSize);
        };
        auto toNormalized = [&](const ImVec2& p)
        {
            return ECS::Vector2f(std::clamp((p.x - canvasPos.x) / canvasSize, 0.0f, 1.0f),
                                 std::clamp((p.y - canvasPos.y) / canvasSize, 0.0f, 1.0f));
        };
        const ImVec2 mousePos = ImGui::GetIO().MousePos;

        // 命中检测：8 像素内最近顶点
        int hoveredVertex = -1;
        float bestDistSq = 8.0f * 8.0f;
        for (int i = 0; i < static_cast<int>(vertices.size()); ++i)
        {
            const ImVec2 sp = toScreen(vertices[i]);
            const float dx = sp.x - mousePos.x;
            const float dy = sp.y - mousePos.y;
            const float distSq = dx * dx + dy * dy;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                hoveredVertex = i;
            }
        }

        // 撤销等外部操作可能收缩顶点数组，防拖拽下标越界
        if (m_tileVertexDragIndex >= static_cast<int>(vertices.size()))
        {
            m_tileVertexDragIndex = -1;
        }

        if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hoveredVertex >= 0)
            {
                m_tileVertexDragIndex = hoveredVertex;
            }
            else if (vertices.size() < kTileColliderMaxVertices)
            {
                const ECS::Vector2f newVertex = toNormalized(mousePos);
                size_t insertIndex = vertices.size();
                if (vertices.size() >= 3)
                {
                    // 插到距点击位置最近的边之后，保持轮廓顺序
                    float bestEdgeDistSq = std::numeric_limits<float>::max();
                    for (size_t i = 0; i < vertices.size(); ++i)
                    {
                        const float distSq = PointSegmentDistSq(newVertex, vertices[i],
                                                                vertices[(i + 1) % vertices.size()]);
                        if (distSq < bestEdgeDistSq)
                        {
                            bestEdgeDistSq = distSq;
                            insertIndex = i + 1;
                        }
                    }
                }
                vertices.insert(vertices.begin() + insertIndex, newVertex);
                m_tileVertexDragIndex = static_cast<int>(insertIndex);
                changed = true;
            }
        }
        if (m_tileVertexDragIndex >= 0)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const ECS::Vector2f dragged = toNormalized(mousePos);
                if (dragged.x != vertices[m_tileVertexDragIndex].x || dragged.y != vertices[m_tileVertexDragIndex].y)
                {
                    vertices[m_tileVertexDragIndex] = dragged;
                    changed = true;
                }
            }
            else
            {
                m_tileVertexDragIndex = -1;
            }
        }
        if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoveredVertex >= 0)
        {
            vertices.erase(vertices.begin() + hoveredVertex);
            m_tileVertexDragIndex = -1;
            hoveredVertex = -1;
            changed = true;
        }

        const ImU32 lineColor = IM_COL32(0, 255, 0, 220);
        if (vertices.size() >= 2)
        {
            const size_t count = vertices.size();
            const size_t edgeCount = count >= 3 ? count : count - 1;
            for (size_t i = 0; i < edgeCount; ++i)
            {
                drawList->AddLine(toScreen(vertices[i]), toScreen(vertices[(i + 1) % count]), lineColor, 1.5f);
            }
        }
        for (int i = 0; i < static_cast<int>(vertices.size()); ++i)
        {
            const bool active = (i == m_tileVertexDragIndex) || (i == hoveredVertex && m_tileVertexDragIndex < 0);
            drawList->AddCircleFilled(toScreen(vertices[i]), active ? 5.0f : 3.5f,
                                      active ? IM_COL32(255, 200, 60, 255) : IM_COL32(0, 255, 0, 255));
        }

        ImGui::Text("顶点 %zu/%zu（左键添加/拖动，右键删除）", vertices.size(), kTileColliderMaxVertices);
        if (vertices.size() < 3)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "至少 3 个顶点才会生成碰撞");
        }
        else if (!IsConvexPolygon(vertices))
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "非凸多边形：物理生成时将取其凸包");
        }
    }

    if (changed)
    {
        // 该资产的 YAML 根只有 Type/Data 两个键，整体标脏后走通用的 应用→保存→重导入 流程
        m_dirtyProperties.insert("Type");
        m_dirtyProperties.insert("Data");
    }
}
void AssetInspectorPanel::applyChanges()
{
    PROFILE_FUNCTION();
    if (m_currentEditingPaths.empty()) return;

    const bool hasImporterChanges = !m_dirtyProperties.empty();
    const bool hasAddressChanges = m_addressDirty || m_groupDirty;
    if (!hasImporterChanges && !hasAddressChanges) return;

    const auto* registration = AssetImporterRegistry::GetInstance().Get(m_editingAssetType);
    if (hasImporterChanges && (!registration || !m_isDeserialized || !registration->Serialize))
    {
        LogError("应用更改失败：找不到注册或序列化函数。");
        return;
    }

    YAML::Node newSettingsBase;
    if (hasImporterChanges)
    {
        try
        {
            PROFILE_SCOPE("AssetInspectorPanel::SerializeSettings");
            newSettingsBase = registration->Serialize(m_deserializedSettings);
        }
        catch (const std::exception& e)
        {
            LogError("序列化资产设置失败: {}", e.what());
            return;
        }
    }

    const std::string resolvedAddress = m_addressDirty ? TrimString(m_addressName) : std::string{};
    const std::vector<std::string> resolvedGroups = m_groupDirty ? ParseGroupNames(m_groupNamesInput)
                                                                  : std::vector<std::string>{};

    LogInfo("正在为 {} 个资产应用 {} 项设置更改...",
            m_currentEditingPaths.size(),
            m_dirtyProperties.size()
            + (m_addressDirty ? 1 : 0)
            + (m_groupDirty ? 1 : 0));
    for (const auto& assetPath : m_currentEditingPaths)
    {
        PROFILE_SCOPE("AssetInspectorPanel::ApplySingleAsset");
        const AssetMetadata* originalMetadata = AssetManager::GetInstance().GetMetadata(assetPath);
        if (!originalMetadata)
        {
            LogWarn("找不到资产 {} 的元数据，跳过保存。", assetPath.string());
            continue;
        }
        AssetMetadata updatedMeta = *originalMetadata;
        if (m_addressDirty)
        {
            updatedMeta.addressName = resolvedAddress;
        }
        if (m_groupDirty)
        {
            updatedMeta.groupNames = resolvedGroups;
        }

        YAML::Node finalSettings = originalMetadata->importerSettings;
        if (hasImporterChanges)
        {
            for (const std::string& propName : m_dirtyProperties)
            {
                if (newSettingsBase[propName])
                {
                    finalSettings[propName] = newSettingsBase[propName];
                }
            }
        }

        saveMetadataToFile(updatedMeta, finalSettings, hasImporterChanges);
        EventBus::GetInstance().Publish(AssetUpdatedEvent{updatedMeta.type, updatedMeta.guid});
    }
    m_dirtyProperties.clear();
    if (m_addressDirty)
    {
        m_addressName = TrimString(m_addressName);
    }
    if (m_groupDirty)
    {
        m_groupNamesInput = JoinGroupNames(ParseGroupNames(m_groupNamesInput));
    }
    m_addressDirty = false;
    m_groupDirty = false;
    m_addressMixed = false;
    m_groupMixed = false;
}
void AssetInspectorPanel::saveMetadataToFile(const AssetMetadata& updatedMetadata, const YAML::Node& newSettings,
                                             bool writeAssetFile)
{
    PROFILE_FUNCTION();
    AssetMetadata updatedMeta = updatedMetadata;
    updatedMeta.importerSettings = newSettings;

    if (writeAssetFile)
    {
        std::fstream file(AssetManager::GetInstance().GetAssetsRootPath() / updatedMeta.assetPath, std::ios::out);
        if (!file.is_open())
        {
            LogError("无法保存资产元数据到文件: {}", updatedMeta.assetPath.string());
            return;
        }
        file << YAML::Dump(newSettings);
        file.close();
    }
    AssetManager::GetInstance().ReImport(updatedMeta);
}
void AssetInspectorPanel::openTextureSlicer()
{
    PROFILE_FUNCTION();
    if (m_currentEditingPaths.empty())
        return;
    const AssetMetadata* metadata = AssetManager::GetInstance().GetMetadata(m_currentEditingPaths[0]);
    if (!metadata || metadata->type != AssetType::Texture)
        return;
    if (!m_context->editor)
    {
        LogError("EditorContext 中的 editor 指针为空！");
        return;
    }
    auto* slicerPanel = dynamic_cast<TextureSlicerPanel*>(m_context->editor->GetPanelByName("纹理切片编辑器"));
    if (slicerPanel)
    {
        slicerPanel->OpenTexture(metadata->guid);
    }
    else
    {
        LogError("无法找到纹理切片编辑器面板");
    }
}
void AssetInspectorPanel::openShaderEditor()
{
    PROFILE_FUNCTION();
    if (m_currentEditingPaths.empty())
        return;
    const AssetMetadata* metadata = AssetManager::GetInstance().GetMetadata(m_currentEditingPaths[0]);
    if (!metadata || metadata->type != AssetType::Material)
        return;
    if (!m_context->editor)
    {
        LogError("EditorContext 中的 editor 指针为空！");
        return;
    }
    Data::MaterialDefinition materialData = metadata->importerSettings.as<Data::MaterialDefinition>();
    if (!materialData.shaderHandle.Valid())
    {
        LogError("材质没有关联的shader资产");
        return;
    }
    auto* shaderEditorPanel = dynamic_cast<ShaderEditorPanel*>(m_context->editor->GetPanelByName("着色器编辑器"));
    if (shaderEditorPanel)
    {
        shaderEditorPanel->OpenShader(materialData.shaderHandle);
    }
    else
    {
        LogError("无法找到Shader编辑器面板");
    }
}
