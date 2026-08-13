#pragma once
#include "IEditorPanel.h"
#include "AssetHandle.h"
#include "MaterialData.h"
#include "ShaderData.h"
#include "TextEditor.h"
#include "Nut/Shader.h" 
#include "RuntimeAsset/RuntimeWGSLMaterial.h"
#include "imgui.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>
class EditorContext;
class ShaderEditorPanel : public IEditorPanel
{
public:
    ShaderEditorPanel();
    ~ShaderEditorPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "着色器编辑器"; }
    void OpenShader(const AssetHandle& shaderHandle);
    void SaveShader();
    bool HasUnsavedChanges() const { return m_hasUnsavedChanges; }
    void OpenMaterial(const AssetHandle& materialHandle);
    void SaveMaterial();
private:
    // 单条编译诊断，editorLine 为映射回编辑器的 0-based 行号，-1 表示无法定位
    struct CompileMessage
    {
        int editorLine = -1;
        int column = 0;
        bool isError = true;
        std::string text;
    };
    void RenderToolbar();
    void RenderCodeEditor();
    void RenderBindingsPanel();
    void RenderCompileOutput();
    void RenderSettingsPanel();
    void RenderUniformEditor();
    void CompileShader();
    void CollectCompileDiagnostics(Nut::ShaderModule& module,
                                   const std::shared_ptr<Nut::NutContext>& nutCtx,
                                   const std::string& expandedCode,
                                   const std::string& exportedModuleName);
    std::vector<int> BuildExpandedLineMap(const std::string& expandedCode,
                                          const std::string& exportedModuleName) const;
    void RenderPreviewPanel();
    void RenderPreviewImage(float width, float height);
    bool EnsurePreviewResources(const std::shared_ptr<Nut::NutContext>& nutCtx);
    void RebuildPreviewMaterialIfNeeded();
    void HandleAutoComplete();
    void RenderAutoCompletePopup();
    std::string GetWordUnderCursor() const;
    std::vector<std::string> ExtractLocalVariables() const;
    void UpdateTextEditorLanguage();
    void HandleFontZoom();
    void LoadFontSize();
    void SaveFontSize();
    void LoadColorSettings();
    void SaveColorSettings();
    void LoadCustomKeywords();
    void SaveCustomKeywords();
    void ApplyColorSettings();
    void ApplyCustomKeywords();
    void AddUniform();
    void RemoveUniform(size_t index);
    EditorContext* m_context = nullptr;
    AssetHandle m_currentShaderHandle;
    Data::ShaderData m_shaderData;
    AssetHandle m_currentMaterialHandle; 
    Data::MaterialDefinition m_materialData; 
    bool m_isOpen = false;
    bool m_isVisible = true;
    bool m_hasUnsavedChanges = false;
    bool m_showSettingsPanel = false;
    bool m_compileSuccess = false;
    std::string m_compileOutput;
    std::vector<CompileMessage> m_compileMessages;
    std::string m_lastExpandedCode;
    TextEditor m_textEditor;
    std::string m_shaderCodeBuffer;
    bool m_codeChanged = false;
    enum class CandidateType
    {
        Keyword,    
        Function,   
        Module,     
        Type,       
        Variable    
    };
    struct AutoCompleteCandidate
    {
        std::string text;
        CandidateType type;
    };
    bool m_isAutoCompleteOpen = false;
    std::vector<AutoCompleteCandidate> m_autoCompleteCandidates;
    int m_autoCompleteSelectedIndex = 0;
    std::string m_currentWordPrefix;
    ImVec2 m_popupPos;
    std::vector<Nut::ShaderBindingInfo> m_shaderBindings;
    bool m_bindingsDirty = true;
    float m_fontSize = 16.0f;
    const float m_fontSizeMin = 8.0f;
    const float m_fontSizeMax = 48.0f;
    TextEditor::Palette m_customPalette;
    bool m_useCustomColors = false;
    std::vector<std::string> m_customKeywords;
    std::string m_newKeywordBuffer;
    int m_selectedUniformIndex = -1;
    bool m_addingUniform = false;
    Data::MaterialUniform m_newUniform;
    // ---- 实时预览 ----
    bool m_previewEnabled = false;
    bool m_previewResourcesReady = false;
    bool m_previewMaterialDirty = true;
    std::string m_previewStatus;
    std::string m_previewShaderCode;
    std::unique_ptr<RuntimeWGSLMaterial> m_previewMaterial;
    Nut::TextureAPtr m_previewWhiteTexture;
    Nut::Buffer m_previewQuadVBO{std::nullopt};
    Nut::Buffer m_previewQuadIBO{std::nullopt};
    Nut::Sampler m_previewSampler;
    float m_previewTime = 0.0f;
    float m_previewDeltaTime = 0.0f;
};
