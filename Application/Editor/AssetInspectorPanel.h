#pragma once
#include "IEditorPanel.h" 
#include "Resources/AssetMetadata.h" 
#include "Resources/RuntimeAsset/RuntimeTexture.h"
#include "EventBus.h"
#include <filesystem>
#include <vector>
#include <set>
#include <any> 
#include <yaml-cpp/yaml.h>
class EditorContext;
class AssetInspectorPanel : public IEditorPanel
{
public:
    AssetInspectorPanel() = default;
    ~AssetInspectorPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "资产检视器"; }
private:
    void resetStateFromSelection();
    void drawInspectorUI();
    void drawTileColliderEditor(void* dataPtr);
    void applyChanges();
    void saveMetadataToFile(const AssetMetadata& updatedMetadata, const YAML::Node& newSettings, bool writeAssetFile);
    void openTextureSlicer();
    void openShaderEditor();
private:
    std::vector<std::filesystem::path> m_currentEditingPaths;
    AssetType m_editingAssetType = AssetType::Unknown;
    std::any m_deserializedSettings;
    bool m_isDeserialized = false;
    std::set<std::string> m_mixedValueProperties;
    std::set<std::string> m_dirtyProperties;
    std::string m_addressName;
    std::string m_groupNamesInput;
    bool m_addressMixed = false;
    bool m_groupMixed = false;
    bool m_addressDirty = false;
    bool m_groupDirty = false;

    int m_tileVertexDragIndex = -1; ///< 瓦片碰撞多边形预览中正被拖拽的顶点下标，-1 表示无。
    sk_sp<RuntimeTexture> m_tilePreviewTexture; ///< 瓦片物理形状预览的纹理缓存。
    Guid m_tilePreviewTextureGuid; ///< 预览纹理对应的资产 GUID，用于失效判断。
};
