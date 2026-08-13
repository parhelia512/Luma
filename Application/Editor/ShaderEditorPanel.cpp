#include "ShaderEditorPanel.h"
#include "AssetManager.h"
#include "Logger.h"
#include "EditorContext.h"
#include "ImGuiRenderer.h"
#include "RenderTarget.h"
#include "Renderer/Nut/NutContext.h"
#include "Renderer/Nut/ShaderModuleRegistry.h"
#include "Renderer/Nut/RenderPass.h"
#include "Renderer/Nut/ShaderStruct.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include "GraphicsBackend.h"
namespace
{
    // Dawn 的 StringView 不保证以 \0 结尾，按 length 语义安全转换
    std::string WGPUStringViewToString(const wgpu::StringView& view)
    {
        if (view.data == nullptr)
        {
            return {};
        }
        if (view.length == WGPU_STRLEN)
        {
            return std::string(view.data);
        }
        return std::string(view.data, view.length);
    }
}
ShaderEditorPanel::ShaderEditorPanel()
{
    m_textEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::WGSL());
    m_textEditor.SetShowWhitespaces(false);
    m_textEditor.SetImGuiChildIgnored(true);
    m_textEditor.SetTabSize(4);
    m_customPalette = TextEditor::GetDarkPalette();
    LoadFontSize();
    LoadColorSettings();
    LoadCustomKeywords();
    ApplyColorSettings();
    ApplyCustomKeywords();
}
void ShaderEditorPanel::Initialize(EditorContext* context)
{
    m_context = context;
    if (!m_context)
    {
        LogError("ShaderEditorPanel::Initialize - Invalid EditorContext provided.");
    }
}
void ShaderEditorPanel::Update(float deltaTime)
{
    if (m_isOpen && m_previewEnabled)
    {
        m_previewTime += deltaTime;
        m_previewDeltaTime = deltaTime;
    }
}
void ShaderEditorPanel::Draw()
{
    if (!m_isVisible || !m_isOpen) return;
    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
    // ### 之后为固定窗口 ID，前半段标题可动态附加未保存标记 *
    std::string windowTitle = std::string(GetPanelName()) + (m_hasUnsavedChanges ? " *" : "");
    windowTitle += "###";
    windowTitle += GetPanelName();
    if (ImGui::Begin(windowTitle.c_str(), &m_isOpen, ImGuiWindowFlags_MenuBar))
    {
        // 面板（含子窗口）聚焦时的快捷键；IsKeyChordPressed 为边沿触发
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && m_hasUnsavedChanges)
            {
                SaveShader();
            }
            if (ImGui::IsKeyChordPressed(ImGuiKey_F5))
            {
                CompileShader();
            }
        }
        RenderToolbar();
        ImGui::BeginChild("##shader_editor_split", ImVec2(0, -200), false);
        {
            ImGui::BeginChild("##code_editor", ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, 0), true);
            {
                RenderCodeEditor();
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##bindings_panel", ImVec2(0, 0), true);
            {
                if (m_previewEnabled)
                {
                    RenderPreviewPanel();
                    ImGui::Separator();
                }
                RenderBindingsPanel();
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::BeginChild("##compile_output", ImVec2(0, 0), true);
        {
            RenderCompileOutput();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (m_showSettingsPanel)
    {
        RenderSettingsPanel();
    }
    RenderAutoCompletePopup();
}
void ShaderEditorPanel::HandleAutoComplete()
{
    if (!m_textEditor.IsHandleKeyboardInputsEnabled())
    {
        m_textEditor.SetHandleKeyboardInputs(true);
    }
    ImGuiIO& io = ImGui::GetIO();
    bool isCtrl = io.KeyCtrl;
    bool isAlt = io.KeyAlt;
    if (m_isAutoCompleteOpen)
    {
        std::string prefix = GetWordUnderCursor();
        m_popupPos = m_textEditor.GetCursorScreenPosition();
        m_popupPos.y += 20;
        // 方向键优先，Ctrl+N/P 作为备选；避免占用 Ctrl+S（保存）/Ctrl+W（关闭）
        bool selectNext = ImGui::IsKeyPressed(ImGuiKey_DownArrow) || (isCtrl && ImGui::IsKeyPressed(ImGuiKey_N));
        bool selectPrev = ImGui::IsKeyPressed(ImGuiKey_UpArrow) || (isCtrl && ImGui::IsKeyPressed(ImGuiKey_P));
        if (selectNext)
        {
            m_autoCompleteSelectedIndex++;
            if (m_autoCompleteSelectedIndex >= static_cast<int>(m_autoCompleteCandidates.size()))
                m_autoCompleteSelectedIndex = 0;
            m_textEditor.SetHandleKeyboardInputs(false);
            return;
        }
        else if (selectPrev)
        {
            m_autoCompleteSelectedIndex--;
            if (m_autoCompleteSelectedIndex < 0)
                m_autoCompleteSelectedIndex = static_cast<int>(m_autoCompleteCandidates.size()) - 1;
            m_textEditor.SetHandleKeyboardInputs(false);
            return;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            if (!m_autoCompleteCandidates.empty() &&
                m_autoCompleteSelectedIndex >= 0 &&
                m_autoCompleteSelectedIndex < static_cast<int>(m_autoCompleteCandidates.size()))
            {
                std::string toInsert = m_autoCompleteCandidates[m_autoCompleteSelectedIndex].text;
                if (toInsert.length() > prefix.length())
                {
                    m_textEditor.InsertText(toInsert.substr(prefix.length()));
                }
            }
            m_isAutoCompleteOpen = false;
            m_autoCompleteCandidates.clear();
            m_currentWordPrefix.clear();
            m_textEditor.SetHandleKeyboardInputs(false);
            return;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_isAutoCompleteOpen = false;
            m_currentWordPrefix.clear();
            return;
        }
        if (!prefix.empty())
        {
            if (prefix != m_currentWordPrefix)
            {
                m_currentWordPrefix = prefix;
                m_autoCompleteCandidates.clear();
                const auto& langDef = m_textEditor.GetLanguageDefinition();
                std::string lowerPrefix = prefix;
                std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(), ::tolower);
                auto AddCandidate = [&](const std::string& candidate, CandidateType type)
                {
                    std::string lowerCandidate = candidate;
                    std::transform(lowerCandidate.begin(), lowerCandidate.end(), lowerCandidate.begin(), ::tolower);
                    if (lowerCandidate.find(lowerPrefix) == 0 && candidate != prefix)
                    {
                        m_autoCompleteCandidates.push_back({candidate, type});
                    }
                };
                for (const auto& kw : langDef.mKeywords)
                {
                    if (kw.find("vec") == 0 || kw.find("mat") == 0 || kw == "f32" || kw == "i32" || kw == "u32" ||
                        kw == "bool" || kw == "f16" || kw.find("texture") == 0 || kw.find("sampler") == 0)
                    {
                        AddCandidate(kw, CandidateType::Type);
                    }
                    else
                    {
                        AddCandidate(kw, CandidateType::Keyword);
                    }
                }
                for (const auto& ident : langDef.mIdentifiers)
                {
                    AddCandidate(ident.first, CandidateType::Function);
                }
                for (const auto& kw : m_customKeywords)
                {
                    AddCandidate(kw, CandidateType::Keyword);
                }
                auto& registry = Nut::ShaderModuleRegistry::GetInstance();
                auto allModules = registry.GetAllModuleNames();
                for (const auto& moduleName : allModules)
                {
                    AddCandidate(moduleName, CandidateType::Module);
                }
                auto localVars = ExtractLocalVariables();
                for (const auto& varName : localVars)
                {
                    AddCandidate(varName, CandidateType::Variable);
                }
                std::sort(m_autoCompleteCandidates.begin(), m_autoCompleteCandidates.end(),
                          [](const AutoCompleteCandidate& a, const AutoCompleteCandidate& b)
                          {
                              if (a.type != b.type) return static_cast<int>(a.type) < static_cast<int>(b.type);
                              return a.text < b.text;
                          });
                if (m_autoCompleteCandidates.empty())
                {
                    m_isAutoCompleteOpen = false;
                    m_currentWordPrefix.clear();
                }
                else
                {
                    m_autoCompleteSelectedIndex = 0;
                }
            }
        }
        else
        {
            m_isAutoCompleteOpen = false;
            m_currentWordPrefix.clear();
        }
        return;
    }
    std::string prefix = GetWordUnderCursor();
    if (prefix.empty())
    {
        if (m_currentWordPrefix != "")
        {
            m_currentWordPrefix.clear();
        }
        return;
    }
    bool prefixChanged = (prefix != m_currentWordPrefix);
    bool shouldUpdate = !isCtrl && !isAlt && prefixChanged;
    if (shouldUpdate)
    {
        if (prefix.length() >= 1)
        {
            m_currentWordPrefix = prefix;
            m_autoCompleteCandidates.clear();
            const auto& langDef = m_textEditor.GetLanguageDefinition();
            std::string lowerPrefix = prefix;
            std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(), ::tolower);
            auto AddCandidate = [&](const std::string& candidate, CandidateType type)
            {
                std::string lowerCandidate = candidate;
                std::transform(lowerCandidate.begin(), lowerCandidate.end(), lowerCandidate.begin(), ::tolower);
                if (lowerCandidate.find(lowerPrefix) == 0 && candidate != prefix)
                {
                    m_autoCompleteCandidates.push_back({candidate, type});
                }
            };
            for (const auto& kw : langDef.mKeywords)
            {
                if (kw.find("vec") == 0 || kw.find("mat") == 0 || kw == "f32" || kw == "i32" || kw == "u32" ||
                    kw == "bool" || kw == "f16" || kw.find("texture") == 0 || kw.find("sampler") == 0)
                {
                    AddCandidate(kw, CandidateType::Type);
                }
                else
                {
                    AddCandidate(kw, CandidateType::Keyword);
                }
            }
            for (const auto& ident : langDef.mIdentifiers)
            {
                AddCandidate(ident.first, CandidateType::Function);
            }
            for (const auto& kw : m_customKeywords)
            {
                AddCandidate(kw, CandidateType::Keyword);
            }
            auto& registry = Nut::ShaderModuleRegistry::GetInstance();
            auto allModules = registry.GetAllModuleNames();
            for (const auto& moduleName : allModules)
            {
                AddCandidate(moduleName, CandidateType::Module);
            }
            auto localVars = ExtractLocalVariables();
            for (const auto& varName : localVars)
            {
                AddCandidate(varName, CandidateType::Variable);
            }
            std::sort(m_autoCompleteCandidates.begin(), m_autoCompleteCandidates.end(),
                      [](const AutoCompleteCandidate& a, const AutoCompleteCandidate& b)
                      {
                          if (a.type != b.type) return static_cast<int>(a.type) < static_cast<int>(b.type);
                          return a.text < b.text;
                      });
            if (!m_autoCompleteCandidates.empty())
            {
                m_isAutoCompleteOpen = true;
                m_autoCompleteSelectedIndex = 0;
                m_popupPos = m_textEditor.GetCursorScreenPosition();
                m_popupPos.y += 20;
            }
            else
            {
                m_isAutoCompleteOpen = false;
                m_currentWordPrefix.clear();
            }
        }
        else
        {
            m_isAutoCompleteOpen = false;
            m_currentWordPrefix.clear();
        }
    }
}
void ShaderEditorPanel::RenderAutoCompletePopup()
{
    if (!m_isAutoCompleteOpen || m_autoCompleteCandidates.empty()) return;
    ImGui::SetNextWindowPos(m_popupPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(220, 0), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_Tooltip;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    bool windowVisible = ImGui::Begin("ShaderAutoComplete", nullptr, flags);
    if (windowVisible)
    {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Auto Complete (%d)", (int)m_autoCompleteCandidates.size());
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "↑/↓ 或 Ctrl+P/N: 选择 | Tab/Enter: 确认 | Esc: 取消");
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(m_autoCompleteCandidates.size()); ++i)
        {
            const auto& candidate = m_autoCompleteCandidates[i];
            bool isSelected = (i == m_autoCompleteSelectedIndex);
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            }
            const char* typeIcon = "";
            ImVec4 typeColor;
            switch (candidate.type)
            {
            case CandidateType::Keyword:
                typeIcon = "K";
                typeColor = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
                break;
            case CandidateType::Function:
                typeIcon = "F";
                typeColor = ImVec4(1.0f, 0.9f, 0.4f, 1.0f);
                break;
            case CandidateType::Module:
                typeIcon = "M";
                typeColor = ImVec4(0.4f, 1.0f, 0.6f, 1.0f);
                break;
            case CandidateType::Type:
                typeIcon = "T";
                typeColor = ImVec4(0.4f, 0.9f, 0.9f, 1.0f);
                break;
            case CandidateType::Variable:
                typeIcon = "V";
                typeColor = ImVec4(1.0f, 0.7f, 0.4f, 1.0f);
                break;
            }
            ImGui::TextColored(typeColor, "[%s]", typeIcon);
            ImGui::SameLine();
            if (ImGui::Selectable(candidate.text.c_str(), isSelected))
            {
                std::string prefix = GetWordUnderCursor();
                std::string toInsert = candidate.text;
                if (toInsert.length() > prefix.length())
                {
                    m_textEditor.InsertText(toInsert.substr(prefix.length()));
                }
                m_isAutoCompleteOpen = false;
                m_currentWordPrefix.clear();
                m_autoCompleteCandidates.clear();
            }
            if (isSelected)
            {
                ImGui::PopStyleColor(2);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}
std::string ShaderEditorPanel::GetWordUnderCursor() const
{
    auto pos = m_textEditor.GetCursorPosition();
    std::string line = m_textEditor.GetCurrentLineText();
    if (pos.mColumn == 0 || pos.mColumn > static_cast<int>(line.length())) return "";
    char leftChar = line[pos.mColumn - 1];
    if (!isalnum(leftChar) && leftChar != '_')
    {
        return "";
    }
    int start = pos.mColumn - 1;
    while (start >= 0)
    {
        char c = line[start];
        if (!isalnum(c) && c != '_') break;
        start--;
    }
    return line.substr(start + 1, pos.mColumn - (start + 1));
}
std::vector<std::string> ShaderEditorPanel::ExtractLocalVariables() const
{
    std::vector<std::string> variables;
    std::set<std::string> uniqueVars;
    std::string text = m_textEditor.GetText();
    std::regex varPattern(R"(\b(?:var|let|const)\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*[:=])");
    std::smatch match;
    auto searchStart = text.cbegin();
    while (std::regex_search(searchStart, text.cend(), match, varPattern))
    {
        std::string varName = match[1].str();
        if (uniqueVars.insert(varName).second)
        {
            variables.push_back(varName);
        }
        searchStart = match.suffix().first;
    }
    return variables;
}
void ShaderEditorPanel::RenderCodeEditor()
{
    ImGui::Text("代码视图:");
    ImGui::SameLine();
    if (m_currentShaderHandle.Valid())
    {
        if (m_shaderData.language == Data::ShaderLanguage::WGSL)
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "(WGSL)");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(SkSL - [Obsolete])");
    }
    ImGui::Separator();
    auto cpos = m_textEditor.GetCursorPosition();
    ImGui::Text("Ln: %d | Col: %d | Lines: %d", cpos.mLine + 1, cpos.mColumn + 1, m_textEditor.GetTotalLines());
    ImGui::BeginChild("##code_editor_content", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar |
                      ImGuiWindowFlags_NoMove);
    {
        HandleFontZoom();
        float scale = m_fontSize / 16.0f;
        ImGui::SetWindowFontScale(scale);
        HandleAutoComplete();
        m_textEditor.Render("##shader_code_editor");
        if (!m_textEditor.IsHandleKeyboardInputsEnabled())
        {
            m_textEditor.SetHandleKeyboardInputs(true);
        }
    }
    ImGui::EndChild();
    if (m_textEditor.IsTextChanged())
    {
        m_shaderCodeBuffer = m_textEditor.GetText();
        m_hasUnsavedChanges = true;
        m_codeChanged = true;
        m_bindingsDirty = true;
    }
}
void ShaderEditorPanel::CompileShader()
{
    m_compileOutput.clear();
    m_compileSuccess = false;
    m_shaderBindings.clear();
    m_compileMessages.clear();
    m_lastExpandedCode.clear();
    m_textEditor.SetErrorMarkers({});
    if (m_shaderCodeBuffer.empty())
    {
        m_compileOutput = "错误: 代码为空。";
        return;
    }
    bool isWGSL = false;
    if (m_currentShaderHandle.Valid())
    {
        isWGSL = (m_shaderData.language == Data::ShaderLanguage::WGSL);
    }
    if (!isWGSL)
    {
        m_compileOutput = "警告: 仅支持 WGSL 的实时编译验证。SkSL 已弃用。";
        return;
    }
    if (!m_context)
    {
        LogError("ShaderEditorPanel::CompileShader - Context is null");
        m_compileOutput = "系统错误: 无法访问引擎上下文。";
        return;
    }
    try
    {
        auto nutCtx = m_context->engineContext->graphicsBackend->GetNutContext();
        if (!nutCtx)
        {
            throw std::runtime_error("NutContext 获取失败，图形后端未就绪。");
        }
        // 先自行展开一遍模块引用：拿到展开失败的错误文本，
        // 同时展开结果用于把 Tint 报告的行号映射回编辑器行号
        std::string expandError;
        std::string exportedModuleName;
        std::string expandedCode =
            Nut::ShaderModuleExpander::ExpandModules(m_shaderCodeBuffer, expandError, exportedModuleName);
        if (expandedCode.empty())
        {
            m_compileOutput = "模块展开失败: " + expandError;
            return;
        }
        LogInfo("ShaderEditorPanel: Compiling WGSL shader...");
        Nut::ShaderModule& module = Nut::ShaderManager::GetFromString(m_shaderCodeBuffer, nutCtx);
        if (!module)
        {
            m_compileOutput = "编译失败: ShaderModule 创建失败 (请查看控制台日志)。";
            return;
        }
        CollectCompileDiagnostics(module, nutCtx, expandedCode, exportedModuleName);
        bool hasError = std::any_of(m_compileMessages.begin(), m_compileMessages.end(),
                                    [](const CompileMessage& msg) { return msg.isError; });
        if (hasError)
        {
            m_compileOutput = "编译失败";
            return;
        }
        m_compileSuccess = true;
        m_compileOutput = "编译成功 (Validation Passed)";
        m_lastExpandedCode = std::move(expandedCode);
        // 快照通过验证的代码：预览材质只能用它重建，
        // 避免编译成功后继续编辑时把未验证的新代码交给渲染管线
        m_previewShaderCode = m_shaderCodeBuffer;
        module.ForeachBinding([this](const Nut::ShaderBindingInfo& info)
        {
            m_shaderBindings.push_back(info);
        });
        std::sort(m_shaderBindings.begin(), m_shaderBindings.end(),
                  [](const Nut::ShaderBindingInfo& a, const Nut::ShaderBindingInfo& b)
                  {
                      if (a.groupIndex != b.groupIndex) return a.groupIndex < b.groupIndex;
                      return a.location < b.location;
                  });
        m_previewMaterialDirty = true;
        LogInfo("ShaderEditorPanel: Compilation successful. Found {} bindings.", m_shaderBindings.size());
    }
    catch (const std::exception& e)
    {
        m_compileSuccess = false;
        m_compileOutput = std::string("编译异常:\n") + e.what();
        LogError("ShaderEditorPanel::CompileShader - Exception: {}", e.what());
    }
}
void ShaderEditorPanel::CollectCompileDiagnostics(Nut::ShaderModule& module,
                                                  const std::shared_ptr<Nut::NutContext>& nutCtx,
                                                  const std::string& expandedCode,
                                                  const std::string& exportedModuleName)
{
    std::vector<int> lineMap = BuildExpandedLineMap(expandedCode, exportedModuleName);
    struct RawMessage
    {
        std::string text;
        uint64_t line = 0;
        uint64_t column = 0;
        bool isError = false;
        bool isWarning = false;
    };
    struct DiagnosticsHolder
    {
        std::vector<RawMessage> messages;
        bool completed = false;
    };
    // WaitAny 超时后回调理论上仍可能滞后触发，shared_ptr 保证其写入目标不悬空
    auto holder = std::make_shared<DiagnosticsHolder>();
    wgpu::Future future = module.Get().GetCompilationInfo(
        wgpu::CallbackMode::WaitAnyOnly,
        [holder](wgpu::CompilationInfoRequestStatus status, wgpu::CompilationInfo const* info)
        {
            holder->completed = true;
            if (status != wgpu::CompilationInfoRequestStatus::Success || !info)
            {
                return;
            }
            for (size_t i = 0; i < info->messageCount; ++i)
            {
                const wgpu::CompilationMessage& msg = info->messages[i];
                RawMessage raw;
                raw.text = WGPUStringViewToString(msg.message);
                raw.line = msg.lineNum;
                raw.column = msg.linePos;
                raw.isError = (msg.type == wgpu::CompilationMessageType::Error);
                raw.isWarning = (msg.type == wgpu::CompilationMessageType::Warning);
                holder->messages.push_back(std::move(raw));
            }
        });
    // Shader 验证在创建时已同步完成，这里只是取回结果，2 秒余量充足
    nutCtx->GetWGPUInstance().WaitAny(future, 2'000'000'000);
    if (!holder->completed)
    {
        LogWarn("ShaderEditorPanel: GetCompilationInfo 等待超时，无法获取编译诊断");
        return;
    }
    TextEditor::ErrorMarkers markers;
    for (const auto& raw : holder->messages)
    {
        if (!raw.isError && !raw.isWarning)
        {
            continue;
        }
        CompileMessage msg;
        msg.isError = raw.isError;
        msg.text = raw.text;
        msg.column = static_cast<int>(raw.column);
        if (raw.line > 0)
        {
            int expandedIdx = static_cast<int>(raw.line) - 1;
            if (expandedIdx >= 0 && expandedIdx < static_cast<int>(lineMap.size()))
            {
                msg.editorLine = lineMap[expandedIdx];
            }
            if (msg.editorLine < 0)
            {
                // 行号落在展开出的导入模块代码里，无法映射回编辑器
                msg.text = std::format("(导入模块内 第{}行) {}", raw.line, raw.text);
            }
        }
        if (msg.isError && msg.editorLine >= 0)
        {
            auto [it, inserted] = markers.try_emplace(msg.editorLine + 1, msg.text);
            if (!inserted)
            {
                it->second += "\n" + msg.text;
            }
        }
        m_compileMessages.push_back(std::move(msg));
    }
    m_textEditor.SetErrorMarkers(markers);
}
std::vector<int> ShaderEditorPanel::BuildExpandedLineMap(const std::string& expandedCode,
                                                         const std::string& exportedModuleName) const
{
    auto splitLines = [](const std::string& text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            lines.push_back(line);
        }
        return lines;
    };
    std::vector<std::string> expandedLines = splitLines(expandedCode);
    std::vector<std::string> originalLines = splitLines(m_shaderCodeBuffer);
    std::vector<int> map(expandedLines.size(), -1);
    // 展开器把导入模块整体前置，主代码保持原有顺序放在尾部，
    // 只是删掉了其中的 import/export 行，因此从尾部向前逐行对齐即可
    int expIdx = static_cast<int>(expandedLines.size()) - 1;
    if (!exportedModuleName.empty() && expIdx >= 0 && expandedLines[expIdx].rfind("// ==========", 0) == 0)
    {
        --expIdx; // 导出模块时主代码末尾会追加一行结束注释
    }
    for (int origIdx = static_cast<int>(originalLines.size()) - 1; origIdx >= 0 && expIdx >= 0;)
    {
        if (expandedLines[expIdx] == originalLines[origIdx])
        {
            map[expIdx] = origIdx;
            --expIdx;
            --origIdx;
        }
        else
        {
            --origIdx; // 原始行是被展开器移除的 import/export 行
        }
    }
    return map;
}
void ShaderEditorPanel::RenderBindingsPanel()
{
    ImGui::Text("资源绑定 (Reflection)");
    ImGui::Separator();
    if (!m_compileSuccess && m_shaderBindings.empty())
    {
        ImGui::TextDisabled("请先编译着色器以查看绑定信息。");
        return;
    }
    if (m_shaderBindings.empty())
    {
        ImGui::TextDisabled("无绑定资源。");
        return;
    }
    int currentGroup = -1;
    for (const auto& binding : m_shaderBindings)
    {
        if (static_cast<int>(binding.groupIndex) != currentGroup)
        {
            currentGroup = static_cast<int>(binding.groupIndex);
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "@group(%d)", currentGroup);
        }
        ImGui::PushID((std::to_string(binding.groupIndex * 1000 + binding.location) + binding.name).c_str());
        std::string label = std::format("@binding({}) {}", binding.location, binding.name);
        bool nodeOpen = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
        if (nodeOpen)
        {
            ImGui::Indent();
            const char* typeStr = "Unknown";
            ImVec4 typeColor = ImVec4(1, 1, 1, 1);
            switch (binding.type)
            {
            case Nut::BindingType::UniformBuffer:
                typeStr = "Uniform Buffer";
                typeColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
                break;
            case Nut::BindingType::StorageBuffer:
                typeStr = "Storage Buffer";
                typeColor = ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
                break;
            case Nut::BindingType::Texture:
                typeStr = "Texture";
                typeColor = ImVec4(0.8f, 0.4f, 1.0f, 1.0f);
                break;
            case Nut::BindingType::Sampler:
                typeStr = "Sampler";
                typeColor = ImVec4(0.4f, 1.0f, 0.6f, 1.0f);
                break;
            }
            ImGui::Text("类型:");
            ImGui::SameLine();
            ImGui::TextColored(typeColor, "%s", typeStr);
            ImGui::Unindent();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
void ShaderEditorPanel::OpenShader(const AssetHandle& shaderHandle)
{
    if (!shaderHandle.Valid())
    {
        LogError("ShaderEditorPanel::OpenShader - Invalid shader handle");
        return;
    }
    auto metadata = AssetManager::GetInstance().GetMetadata(shaderHandle.assetGuid);
    if (!metadata || metadata->type != AssetType::Shader)
    {
        LogError("ShaderEditorPanel::OpenShader - Failed to load shader metadata");
        return;
    }
    m_currentShaderHandle = shaderHandle;
    m_shaderData = metadata->importerSettings.as<Data::ShaderData>();
    m_shaderCodeBuffer = m_shaderData.source;
    m_textEditor.SetText(m_shaderCodeBuffer);
    UpdateTextEditorLanguage();
    m_isOpen = true;
    m_isVisible = true;
    m_hasUnsavedChanges = false;
    m_codeChanged = false;
    CompileShader();
    LogInfo("ShaderEditorPanel::OpenShader - Opened: {}", metadata->assetPath.string());
}
void ShaderEditorPanel::SaveShader()
{
    if (!m_currentShaderHandle.Valid())
    {
        LogError("ShaderEditorPanel::SaveShader - No shader open");
        return;
    }
    auto metadata = AssetManager::GetInstance().GetMetadata(m_currentShaderHandle.assetGuid);
    if (!metadata) return;
    m_shaderCodeBuffer = m_textEditor.GetText();
    m_shaderData.source = m_shaderCodeBuffer;
    YAML::Node node;
    node = m_shaderData;
    std::ofstream file(AssetManager::GetInstance().GetAssetsRootPath() / metadata->assetPath);
    if (!file.is_open())
    {
        LogError("ShaderEditorPanel::SaveShader - Failed to write file: {}", metadata->assetPath.string());
        return;
    }
    file << node;
    file.close();
    AssetMetadata updatedMeta = *metadata;
    updatedMeta.importerSettings = node;
    AssetManager::GetInstance().ReImport(updatedMeta);
    m_hasUnsavedChanges = false;
    m_codeChanged = false;
    LogInfo("ShaderEditorPanel::SaveShader - Saved: {}", metadata->assetPath.string());
    // 保存成功后立即编译校验（仅 WGSL），编译失败不回滚已完成的保存
    if (m_shaderData.language == Data::ShaderLanguage::WGSL)
    {
        CompileShader();
    }
}
void ShaderEditorPanel::OpenMaterial(const AssetHandle& materialHandle)
{
    LogWarn("ShaderEditorPanel::OpenMaterial is deprecated. Please edit Shader assets directly.");
}
void ShaderEditorPanel::SaveMaterial()
{
    LogWarn("ShaderEditorPanel::SaveMaterial is deprecated.");
}
void ShaderEditorPanel::UpdateTextEditorLanguage()
{
    if (m_currentShaderHandle.Valid() && m_shaderData.language == Data::ShaderLanguage::WGSL)
    {
        m_textEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::WGSL());
    }
    else
    {
        m_textEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
    }
}
void ShaderEditorPanel::RenderToolbar()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("文件"))
        {
            if (ImGui::MenuItem("保存", "Ctrl+S", nullptr, m_hasUnsavedChanges)) SaveShader();
            if (ImGui::MenuItem("关闭")) m_isOpen = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("编辑"))
        {
            if (ImGui::MenuItem("撤销", "Ctrl+Z", nullptr, m_textEditor.CanUndo())) m_textEditor.Undo();
            if (ImGui::MenuItem("重做", "Ctrl+Y", nullptr, m_textEditor.CanRedo())) m_textEditor.Redo();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("构建"))
        {
            if (ImGui::MenuItem("编译 Shader", "F5")) CompileShader();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (ImGui::Button("保存 (Ctrl+S)")) SaveShader();
    ImGui::SameLine();
    if (ImGui::Button("编译 (F5)")) CompileShader();
    ImGui::SameLine();
    if (ImGui::Button("设置")) m_showSettingsPanel = !m_showSettingsPanel;
    ImGui::SameLine();
    ImGui::Checkbox("预览", &m_previewEnabled);
    ImGui::SameLine();
    if (m_hasUnsavedChanges)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "  * 未保存");
    else
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "  已保存");
}
void ShaderEditorPanel::RenderCompileOutput()
{
    // 主题色：错误沿用 0.62,0.24,0.24 色系（正文用提亮变体保证暗底可读），成功用低饱和绿
    const ImVec4 errorBase(0.62f, 0.24f, 0.24f, 1.0f);
    const ImVec4 errorText(0.85f, 0.45f, 0.45f, 1.0f);
    const ImVec4 successText(0.48f, 0.68f, 0.48f, 1.0f);
    const ImVec4 warningText(0.80f, 0.68f, 0.35f, 1.0f);
    ImGui::Text("输出日志:");
    if (!m_compileMessages.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(单击诊断行可跳转到对应代码)");
    }
    ImGui::Separator();
    if (!m_compileOutput.empty())
    {
        if (m_compileSuccess)
            ImGui::TextColored(successText, "[Success] %s", m_compileOutput.c_str());
        else
            ImGui::TextColored(errorBase, "[Error] %s", m_compileOutput.c_str());
    }
    for (size_t i = 0; i < m_compileMessages.size(); ++i)
    {
        const CompileMessage& msg = m_compileMessages[i];
        ImGui::PushID(static_cast<int>(i));
        std::string label;
        if (msg.editorLine >= 0)
        {
            if (msg.column > 0)
                label = std::format("{} 行 {} 列 {}: {}", msg.isError ? "错误" : "警告",
                                    msg.editorLine + 1, msg.column, msg.text);
            else
                label = std::format("{} 行 {}: {}", msg.isError ? "错误" : "警告",
                                    msg.editorLine + 1, msg.text);
        }
        else
        {
            label = std::format("{}: {}", msg.isError ? "错误" : "警告", msg.text);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, msg.isError ? errorText : warningText);
        if (ImGui::Selectable(label.c_str()) && msg.editorLine >= 0)
        {
            // Tint 的列以字符计而编辑器列按 Tab 展开计，跳到行首已足够定位
            m_textEditor.SetCursorPosition(TextEditor::Coordinates(msg.editorLine, 0));
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}
void ShaderEditorPanel::RenderPreviewPanel()
{
    ImGui::Text("实时预览");
    ImGui::Separator();
    if (!m_currentShaderHandle.Valid() ||
        m_shaderData.language != Data::ShaderLanguage::WGSL ||
        m_shaderData.type != Data::ShaderType::VertFrag)
    {
        ImGui::TextDisabled("仅支持 WGSL VertFrag 着色器的预览。");
        return;
    }
    RebuildPreviewMaterialIfNeeded();
    if (!m_previewMaterial)
    {
        if (!m_previewStatus.empty())
            ImGui::TextWrapped("%s", m_previewStatus.c_str());
        else
            ImGui::TextDisabled("编译通过后自动生成预览。");
        return;
    }
    float width = ImGui::GetContentRegionAvail().x;
    if (width < 64.0f) width = 64.0f;
    float height = width * 0.75f;
    RenderPreviewImage(width, height);
    if (!m_previewStatus.empty())
    {
        ImGui::TextWrapped("%s", m_previewStatus.c_str());
    }
}
void ShaderEditorPanel::RebuildPreviewMaterialIfNeeded()
{
    if (!m_previewMaterialDirty)
    {
        return;
    }
    if (!m_compileSuccess)
    {
        return; // 保留上一份可用的预览材质，等编译通过后再重建
    }
    m_previewMaterialDirty = false;
    m_previewStatus.clear();
    // 预览复用引擎的 Sprite 材质通路：要求标准入口与 group(0) 保留绑定（import Std 即满足）
    static const std::regex vsEntryRe(R"(\bfn\s+vs_main\b)");
    static const std::regex fsEntryRe(R"(\bfn\s+fs_main\b)");
    if (!std::regex_search(m_lastExpandedCode, vsEntryRe) ||
        !std::regex_search(m_lastExpandedCode, fsEntryRe))
    {
        m_previewMaterial.reset();
        m_previewStatus = "预览不可用: 缺少 vs_main/fs_main 入口（需使用标准 Sprite 材质模板）。";
        return;
    }
    bool hasEngineData = false;
    bool hasInstances = false;
    bool hasTexture = false;
    bool hasSampler = false;
    for (const auto& binding : m_shaderBindings)
    {
        bool isReservedSlot = (binding.groupIndex == 0 && binding.location <= 3);
        // 预览无法提供主纹理槽之外的纹理/采样器（如 Lighting 的阴影贴图），直接降级
        if (!isReservedSlot &&
            (binding.type == Nut::BindingType::Texture || binding.type == Nut::BindingType::Sampler))
        {
            m_previewMaterial.reset();
            m_previewStatus = std::format("预览不可用: 绑定 {} (group {}, binding {}) 需要引擎运行时资源。",
                                          binding.name, binding.groupIndex, binding.location);
            return;
        }
        if (binding.groupIndex != 0) continue;
        if (binding.location == 0 && binding.type == Nut::BindingType::UniformBuffer) hasEngineData = true;
        if (binding.location == 1 && binding.type == Nut::BindingType::StorageBuffer) hasInstances = true;
        if (binding.location == 2 && binding.type == Nut::BindingType::Texture) hasTexture = true;
        if (binding.location == 3 && binding.type == Nut::BindingType::Sampler) hasSampler = true;
    }
    if (!hasEngineData || !hasInstances || !hasTexture || !hasSampler)
    {
        m_previewMaterial.reset();
        m_previewStatus = "预览不可用: 缺少 group(0) 引擎保留绑定，请 import Std。";
        return;
    }
    GraphicsBackend* backend = m_context ? m_context->graphicsBackend : nullptr;
    auto nutCtx = backend ? backend->GetNutContext() : nullptr;
    if (!nutCtx)
    {
        m_previewStatus = "预览不可用: 图形后端未就绪。";
        return;
    }
    auto material = std::make_unique<RuntimeWGSLMaterial>();
    if (!material->Initialize(nutCtx, m_previewShaderCode, backend->GetSurfaceFormat(), 1))
    {
        m_previewMaterial.reset();
        m_previewStatus = "预览材质构建失败（详见控制台日志）。";
        return;
    }
    // group(0) 由每帧 SwapTexture 构建；group(1)+ 只含占位 uniform/storage，
    // 在此一次性构建，否则 SetPipeline 会绑到未 Build 的空组
    if (Nut::RenderPipeline* pipeline = material->GetPipeline(1))
    {
        pipeline->ForeachGroup([&nutCtx](size_t groupIdx, Nut::BindGroup& group)
        {
            if (groupIdx != 0)
            {
                group.Build(nutCtx);
            }
        });
    }
    m_previewMaterial = std::move(material);
}
bool ShaderEditorPanel::EnsurePreviewResources(const std::shared_ptr<Nut::NutContext>& nutCtx)
{
    if (m_previewResourcesReady)
    {
        return true;
    }
    const uint32_t whitePixel = 0xFFFFFFFFu;
    m_previewWhiteTexture = Nut::TextureBuilder()
                            .SetPixelData(&whitePixel, 1, 1, 4)
                            .SetSize(1, 1)
                            .Build(nutCtx);
    if (!m_previewWhiteTexture)
    {
        return false;
    }
    // 与 RenderSystem 的精灵四边形一致的单位几何
    std::vector<Vertex> vertices = {
        {-0.5f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.5f, 1.0f, 1.0f},
        {0.5f, -0.5f, 1.0f, 0.0f}
    };
    std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3};
    m_previewQuadVBO = Nut::BufferBuilder()
                       .SetUsage(Nut::BufferUsage::Vertex | Nut::BufferUsage::CopyDst)
                       .SetData(vertices)
                       .Build(nutCtx);
    m_previewQuadIBO = Nut::BufferBuilder()
                       .SetUsage(Nut::BufferUsage::Index | Nut::BufferUsage::CopyDst)
                       .SetData(indices)
                       .Build(nutCtx);
    m_previewSampler.SetMagFilter(wgpu::FilterMode::Linear)
                    .SetMinFilter(wgpu::FilterMode::Linear)
                    .Build(nutCtx);
    m_previewResourcesReady = true;
    return true;
}
void ShaderEditorPanel::RenderPreviewImage(float width, float height)
{
    GraphicsBackend* backend = m_context ? m_context->graphicsBackend : nullptr;
    if (!backend || !m_context->imguiRenderer)
    {
        return;
    }
    auto nutCtx = backend->GetNutContext();
    if (!nutCtx || !EnsurePreviewResources(nutCtx))
    {
        return;
    }
    auto renderTarget = backend->CreateOrGetRenderTarget("ShaderEditorPreview",
                                                         static_cast<uint16_t>(width),
                                                         static_cast<uint16_t>(height));
    if (!renderTarget)
    {
        return;
    }
    Nut::RenderPipeline* pipeline = m_previewMaterial->GetPipeline(1);
    if (!pipeline)
    {
        return;
    }
    EngineData engineData{};
    engineData.CameraPosition = {0.0f, 0.0f};
    engineData.CameraScaleX = 1.0f;
    engineData.CameraScaleY = -1.0f; // 与 GameView 相同的 Y 翻转约定
    engineData.CameraSinR = 0.0f;
    engineData.CameraCosR = 1.0f;
    engineData.ViewportSize = {width, height};
    engineData.TimeData = {m_previewTime, m_previewDeltaTime};
    engineData.MousePosition = {0.0f, 0.0f};
    // 单实例四边形铺满整个预览视口
    std::vector<InstanceData> instances(1);
    InstanceData& inst = instances[0];
    inst.position = {0.0f, 0.0f, 0.0f, 1.0f};
    inst.scaleX = 1.0f;
    inst.scaleY = 1.0f;
    inst.sinR = 0.0f;
    inst.cosR = 1.0f;
    inst.color = {1.0f, 1.0f, 1.0f, 1.0f};
    inst.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
    inst.size = {width, height};
    pipeline->SetReservedBuffers(engineData, instances, nutCtx);
    if (!pipeline->SwapTexture(m_previewWhiteTexture, &m_previewSampler, nutCtx))
    {
        ImGui::TextDisabled("预览绑定构建失败。");
        return;
    }
    auto targetTexture = Nut::TextureA::CreateTextureA(renderTarget->GetTexture(), nutCtx);
    auto attachment = Nut::ColorAttachmentBuilder()
                      .SetTexture(targetTexture)
                      .SetLoadOnOpen(Nut::LoadOnOpen::Clear)
                      .SetClearColor({0.08, 0.08, 0.10, 1.0})
                      .SetStoreOnOpen(Nut::StoreOnOpen::Store)
                      .Build();
    auto renderPass = nutCtx->BeginRenderFrame()
                            .AddColorAttachment(attachment)
                            .Build();
    if (!renderPass)
    {
        return;
    }
    renderPass.SetPipeline(*pipeline);
    m_previewMaterial->Bind(renderPass);
    renderPass.SetVertexBuffer(0, m_previewQuadVBO);
    renderPass.SetIndexBuffer(m_previewQuadIBO, wgpu::IndexFormat::Uint16);
    renderPass.DrawIndexed(6, 1, 0, 0, 0);
    nutCtx->Submit({nutCtx->EndRenderFrame(renderPass)});
    ImTextureID textureId = m_context->imguiRenderer->GetOrCreateTextureIdFor(renderTarget->GetTexture());
    ImGui::Image(textureId, ImVec2(width, height));
}
void ShaderEditorPanel::Shutdown()
{
    m_isOpen = false;
    m_compileOutput.clear();
    m_compileMessages.clear();
    m_shaderBindings.clear();
    m_previewMaterial.reset();
    m_previewWhiteTexture.reset();
    m_previewQuadVBO = Nut::Buffer(std::nullopt);
    m_previewQuadIBO = Nut::Buffer(std::nullopt);
    m_previewResourcesReady = false;
    m_context = nullptr;
}
void ShaderEditorPanel::HandleFontZoom()
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.0f)
    {
        float delta = io.MouseWheel * 2.0f;
        m_fontSize += delta;
        if (m_fontSize < m_fontSizeMin) m_fontSize = m_fontSizeMin;
        if (m_fontSize > m_fontSizeMax) m_fontSize = m_fontSizeMax;
        SaveFontSize();
        LogInfo("ShaderEditorPanel: Font size changed to {} (scale: {})", m_fontSize, m_fontSize / 16.0f);
    }
}
void ShaderEditorPanel::RenderSettingsPanel()
{
    ImGui::SetNextWindowSize(ImVec2(600, 700), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("着色器编辑器设置", &m_showSettingsPanel))
    {
        if (ImGui::BeginTabBar("##settings_tabs"))
        {
            if (ImGui::BeginTabItem("颜色"))
            {
                ImGui::Checkbox("使用自定义颜色", &m_useCustomColors);
                if (m_useCustomColors)
                {
                    ImGui::Separator();
                    ImGui::Text("编辑器配色方案：");
                    const char* colorNames[] = {
                        "默认", "关键字", "数字", "字符串", "字符字面量",
                        "标点符号", "预处理", "标识符", "已知标识符", "预处理标识符",
                        "单行注释", "多行注释", "背景", "光标", "选择",
                        "错误标记", "断点", "行号", "当前行填充", "当前行填充(非活动)", "当前行边缘"
                    };
                    for (int i = 0; i < (int)TextEditor::PaletteIndex::Max; ++i)
                    {
                        ImGui::PushID(i);
                        ImVec4 color = ImGui::ColorConvertU32ToFloat4(m_customPalette[i]);
                        if (ImGui::ColorEdit4(colorNames[i], &color.x,
                                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview))
                        {
                            m_customPalette[i] = ImGui::ColorConvertFloat4ToU32(color);
                        }
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                    if (ImGui::Button("应用"))
                    {
                        ApplyColorSettings();
                        SaveColorSettings();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("重置为暗色"))
                    {
                        m_customPalette = TextEditor::GetDarkPalette();
                        ApplyColorSettings();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("重置为亮色"))
                    {
                        m_customPalette = TextEditor::GetLightPalette();
                        ApplyColorSettings();
                    }
                }
                else
                {
                    ImGui::TextWrapped("当前使用默认暗色主题。勾选上方复选框以自定义颜色。");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("关键字"))
            {
                ImGui::Text("自定义关键字列表：");
                ImGui::Separator();
                ImGui::BeginChild("##keywords_list", ImVec2(0, -60), true);
                {
                    for (size_t i = 0; i < m_customKeywords.size(); ++i)
                    {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::Text("%s", m_customKeywords[i].c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("删除"))
                        {
                            m_customKeywords.erase(m_customKeywords.begin() + i);
                            ApplyCustomKeywords();
                            SaveCustomKeywords();
                            --i;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                ImGui::Separator();
                ImGui::InputText("新关键字", &m_newKeywordBuffer);
                ImGui::SameLine();
                if (ImGui::Button("添加"))
                {
                    if (!m_newKeywordBuffer.empty())
                    {
                        bool exists = false;
                        for (const auto& kw : m_customKeywords)
                        {
                            if (kw == m_newKeywordBuffer)
                            {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists)
                        {
                            m_customKeywords.push_back(m_newKeywordBuffer);
                            ApplyCustomKeywords();
                            SaveCustomKeywords();
                            m_newKeywordBuffer.clear();
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("清空全部"))
                {
                    m_customKeywords.clear();
                    ApplyCustomKeywords();
                    SaveCustomKeywords();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
void ShaderEditorPanel::LoadFontSize()
{
    std::ifstream file("editor_config.txt");
    if (file.is_open())
    {
        std::string line;
        while (std::getline(file, line))
        {
            if (line.find("shader_editor_font_size=") == 0)
            {
                try
                {
                    m_fontSize = std::stof(line.substr(24));
                    if (m_fontSize < m_fontSizeMin) m_fontSize = m_fontSizeMin;
                    if (m_fontSize > m_fontSizeMax) m_fontSize = m_fontSizeMax;
                }
                catch (...)
                {
                    LogWarn("ShaderEditorPanel: Failed to parse font size, using default");
                }
                break;
            }
        }
        file.close();
    }
}
void ShaderEditorPanel::SaveFontSize()
{
    std::vector<std::string> lines;
    std::ifstream inFile("editor_config.txt");
    bool found = false;
    if (inFile.is_open())
    {
        std::string line;
        while (std::getline(inFile, line))
        {
            if (line.find("shader_editor_font_size=") == 0)
            {
                lines.push_back("shader_editor_font_size=" + std::to_string(m_fontSize));
                found = true;
            }
            else lines.push_back(line);
        }
        inFile.close();
    }
    if (!found) lines.push_back("shader_editor_font_size=" + std::to_string(m_fontSize));
    std::ofstream outFile("editor_config.txt");
    if (outFile.is_open())
    {
        for (const auto& line : lines) outFile << line << "\n";
        outFile.close();
    }
}
void ShaderEditorPanel::LoadColorSettings()
{
    std::ifstream file("editor_config.txt");
    if (file.is_open())
    {
        std::string line;
        while (std::getline(file, line))
        {
            if (line.find("use_custom_colors=") == 0)
            {
                m_useCustomColors = (line.substr(18) == "true");
            }
            else if (line.find("palette_") == 0)
            {
                size_t eq = line.find('=');
                if (eq != std::string::npos)
                {
                    std::string idxStr = line.substr(8, eq - 8);
                    std::string valStr = line.substr(eq + 1);
                    try
                    {
                        int idx = std::stoi(idxStr);
                        uint32_t val = std::stoul(valStr, nullptr, 16);
                        if (idx >= 0 && idx < (int)TextEditor::PaletteIndex::Max) m_customPalette[idx] = val;
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        file.close();
    }
}
void ShaderEditorPanel::SaveColorSettings()
{
    std::vector<std::string> lines;
    std::ifstream inFile("editor_config.txt");
    if (inFile.is_open())
    {
        std::string line;
        while (std::getline(inFile, line))
        {
            if (line.find("use_custom_colors=") == 0 || line.find("palette_") == 0) continue;
            lines.push_back(line);
        }
        inFile.close();
    }
    lines.push_back("use_custom_colors=" + std::string(m_useCustomColors ? "true" : "false"));
    for (int i = 0; i < (int)TextEditor::PaletteIndex::Max; ++i)
    {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "palette_%d=0x%08x", i, m_customPalette[i]);
        lines.push_back(buffer);
    }
    std::ofstream outFile("editor_config.txt");
    if (outFile.is_open())
    {
        for (const auto& line : lines) outFile << line << "\n";
        outFile.close();
    }
}
void ShaderEditorPanel::ApplyColorSettings()
{
    if (m_useCustomColors) m_textEditor.SetPalette(m_customPalette);
    else m_textEditor.SetPalette(TextEditor::GetDarkPalette());
}
void ShaderEditorPanel::LoadCustomKeywords()
{
    std::ifstream file("editor_config.txt");
    if (file.is_open())
    {
        std::string line;
        while (std::getline(file, line))
        {
            if (line.find("custom_keyword=") == 0)
            {
                std::string kw = line.substr(15);
                if (!kw.empty()) m_customKeywords.push_back(kw);
            }
        }
        file.close();
    }
}
void ShaderEditorPanel::SaveCustomKeywords()
{
    std::vector<std::string> lines;
    std::ifstream inFile("editor_config.txt");
    if (inFile.is_open())
    {
        std::string line;
        while (std::getline(inFile, line))
        {
            if (line.find("custom_keyword=") == 0) continue;
            lines.push_back(line);
        }
        inFile.close();
    }
    for (const auto& kw : m_customKeywords) lines.push_back("custom_keyword=" + kw);
    std::ofstream outFile("editor_config.txt");
    if (outFile.is_open())
    {
        for (const auto& line : lines) outFile << line << "\n";
        outFile.close();
    }
}
void ShaderEditorPanel::ApplyCustomKeywords()
{
    if (m_customKeywords.empty()) return;
    auto langDef = m_textEditor.GetLanguageDefinition();
    for (const auto& kw : m_customKeywords) langDef.mKeywords.insert(kw);
    m_textEditor.SetLanguageDefinition(langDef);
}
void ShaderEditorPanel::RenderUniformEditor()
{
}
void ShaderEditorPanel::AddUniform()
{
}
void ShaderEditorPanel::RemoveUniform(size_t index)
{
}
