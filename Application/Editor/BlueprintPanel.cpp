#include "Resources/RuntimeAsset/RuntimeScene.h"
#include "BlueprintPanel.h"
#include "BlueprintNodeRegistry.h"
#include "ScriptMetadataRegistry.h"
#include "Loaders/BlueprintLoader.h"
#include "Resources/AssetManager.h"
#include "Utils/Path.h"
#include "Utils/Logger.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <implot.h>
#include "imgui_internal.h"
#include "Profiler.h"
#include "Input/Cursor.h"
#include "Input/Keyboards.h"
#include "BlueprintEditorNav.h"
struct PairHash
{
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const
    {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};
namespace
{
    // 主图上函数占位卡片的固定尺寸（画布单位），函数区域折叠后按此绘制与命中
    constexpr float kFunctionCardWidth = 260.0f;
    constexpr float kFunctionCardHeight = 96.0f;
    // C# 类型别名 → 完整类型名，连线校验与配色共用同一套规范化
    std::string_view canonicalPinType(std::string_view typeName)
    {
        if (typeName == "float") return "System.Single";
        if (typeName == "double") return "System.Double";
        if (typeName == "int") return "System.Int32";
        if (typeName == "long") return "System.Int64";
        if (typeName == "bool") return "System.Boolean";
        if (typeName == "string") return "System.String";
        if (typeName == "object") return "System.Object";
        if (typeName == "short") return "System.Int16";
        if (typeName == "byte") return "System.Byte";
        if (typeName == "char") return "System.Char";
        if (typeName == "DynamicObject") return "System.Object";
        return typeName;
    }
    // 输出→输入的类型连接规则：Exec 只连 Exec，System.Object 入参接受任意数据
    bool arePinTypesCompatible(const std::string& outputType, const std::string& inputType)
    {
        if (outputType == "Exec" || inputType == "Exec")
        {
            return outputType == "Exec" && inputType == "Exec";
        }
        std::string_view inputCanonical = canonicalPinType(inputType);
        if (inputCanonical == "System.Object")
        {
            return true;
        }
        return canonicalPinType(outputType) == inputCanonical;
    }
    // 伪类型引脚只作为节点内嵌控件（下拉、按钮等），不参与连线
    bool isConnectablePinType(const std::string& type)
    {
        return type != "SelectType" && type != "TemplateType" && type != "NodeInputText" &&
            type != "FunctionSelection" && type != "Args";
    }
    // 引脚类型 → 显示颜色（参考 UE 蓝图配色并降低饱和度，贴合深色主题）
    ImVec4 getPinTypeColor(const std::string& type)
    {
        std::string_view typeView(type);
        if (typeView.size() > 2 && typeView.substr(typeView.size() - 2) == "[]")
        {
            typeView = typeView.substr(0, typeView.size() - 2); // 数组沿用元素类型的颜色
        }
        std::string_view canonical = canonicalPinType(typeView);
        if (canonical == "Exec") return ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
        if (canonical == "System.Boolean") return ImVec4(0.75f, 0.29f, 0.29f, 1.0f);
        if (canonical == "System.Int16" || canonical == "System.Int32" || canonical == "System.Int64" ||
            canonical == "System.Byte")
        {
            return ImVec4(0.33f, 0.72f, 0.70f, 1.0f);
        }
        if (canonical == "System.Single" || canonical == "System.Double") return ImVec4(0.45f, 0.76f, 0.42f, 1.0f);
        if (canonical == "System.String" || canonical == "System.Char") return ImVec4(0.76f, 0.42f, 0.72f, 1.0f);
        if (canonical == "System.Object") return ImVec4(0.58f, 0.58f, 0.58f, 1.0f); // 通配
        return ImVec4(0.44f, 0.55f, 0.69f, 1.0f); // 其他对象类型
    }
    // 大小写不敏感的子串匹配（仅处理 ASCII，UTF-8 中文按字节原样比较）
    bool containsIgnoreCase(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) return true;
        if (needle.size() > haystack.size()) return false;
        auto toLower = [](unsigned char c) -> unsigned char
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
        };
        for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        {
            size_t j = 0;
            while (j < needle.size() &&
                toLower(static_cast<unsigned char>(haystack[i + j])) ==
                toLower(static_cast<unsigned char>(needle[j])))
            {
                ++j;
            }
            if (j == needle.size()) return true;
        }
        return false;
    }
    // 节点归属图页的唯一访问口（0=主图）：字段本身由 BlueprintData.h 序列化，
    // 生成器不读取该字段，改动只影响编辑器视图层
    uint32_t readNodeOwnerFunction(const BlueprintNode& node)
    {
        return node.OwnerFunctionID;
    }
    void writeNodeOwnerFunction(BlueprintNode& node, uint32_t functionId)
    {
        node.OwnerFunctionID = functionId;
    }
    // 平铺搜索结果行：左侧节点名、右侧分类灰字，返回是否被点击
    bool drawFlatNodeMenuItem(const char* name, const char* category, bool enabled)
    {
        bool clicked = false;
        if (enabled)
        {
            clicked = ImGui::Selectable(name, false, 0, ImVec2(260, 0));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::Selectable(name, false, ImGuiSelectableFlags_Disabled, ImVec2(260, 0));
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(280);
        ImGui::TextDisabled("%s", category);
        return clicked;
    }
}
BlueprintPanel::~BlueprintPanel()
{
    ImPlot::DestroyContext();
    if (m_nodeEditorContext)
    {
        ed::DestroyEditor(m_nodeEditorContext);
        m_nodeEditorContext = nullptr;
    }
}
void BlueprintPanel::Initialize(EditorContext* context)
{
    m_context = context;
    ed::Config config;
    config.SettingsFile = nullptr;
    m_nodeEditorContext = ed::CreateEditor(&config);
    ImPlot::CreateContext();
    m_scriptCompiledListener = EventBus::GetInstance().Subscribe<CSharpScriptCompiledEvent>(
        [this](const CSharpScriptCompiledEvent& e)
        {
            BlueprintNodeRegistry::GetInstance().RegisterAll();
            if (m_currentBlueprint)
            {
                OpenBlueprint(m_currentBlueprintGuid);
            }
        });
}
void BlueprintPanel::Update(float deltaTime)
{
    PROFILE_FUNCTION();
    if (m_context->currentEditingBlueprintGuid.Valid() && m_context->currentEditingBlueprintGuid !=
        m_currentBlueprintGuid)
    {
        OpenBlueprint(m_context->currentEditingBlueprintGuid);
    }
    else if (!m_context->currentEditingBlueprintGuid.Valid() && m_currentBlueprintGuid.Valid())
    {
        CloseCurrentBlueprint();
    }
}
void BlueprintPanel::Shutdown()
{
    CloseCurrentBlueprint();
    if (m_nodeEditorContext)
    {
        ed::DestroyEditor(m_nodeEditorContext);
        m_nodeEditorContext = nullptr;
    }
}
void BlueprintPanel::OpenBlueprint(const Guid& blueprintGuid)
{
    if (m_currentBlueprintGuid == blueprintGuid && m_currentBlueprint)
        return;
    CloseCurrentBlueprint();
    auto loader = BlueprintLoader();
    m_currentBlueprint = loader.LoadAsset(blueprintGuid);
    if (!m_currentBlueprint)
    {
        LogError("无法加载蓝图，GUID: {}", blueprintGuid.ToString());
        return;
    }
    m_currentBlueprintGuid = blueprintGuid;
    m_currentBlueprintName = m_currentBlueprint->GetBlueprintData().Name;
    strncpy(m_blueprintNameBuffer, m_currentBlueprintName.c_str(), sizeof(m_blueprintNameBuffer));
    m_blueprintNameBuffer[sizeof(m_blueprintNameBuffer) - 1] = '\0';
    m_currentViewFunction = 0; // 打开蓝图总是从主图开始（归属迁移在 initializeFromBlueprintData 内进行）
    m_pendingViewFunction = kNoPendingView;
    m_viewJustSwitched = false;
    m_pendingNavKind = PendingNavKind::None;
    m_pendingFocusNodeId = 0;
    m_findReferences.isOpen = false;
    m_findReferences.items.clear();
    initializeFromBlueprintData();
    m_undoStack.clear();
    m_redoStack.clear();
    m_clipboardNodes.clear();
    m_clipboardLinks.clear();
    m_hasMoveCandidate = false;
    m_hasPendingEditSnapshot = false;
    m_pendingLinkPin = ed::PinId(0);
    const auto& allTypes = ScriptMetadataRegistry::GetInstance().GetAvailableTypes();
    m_sortedTypeNames = allTypes;
    std::sort(m_sortedTypeNames.begin(), m_sortedTypeNames.end(), [](const std::string& a, const std::string& b)
    {
        if (a.length() != b.length())
        {
            return a.length() < b.length();
        }
        return a < b;
    });
    SetVisible(true);
    m_context->currentEditingBlueprintGuid = blueprintGuid;
    LogInfo("打开蓝图进行编辑: {}", m_currentBlueprintName);
}
void BlueprintPanel::ClearEditorState()
{
    m_nodes.clear();
    m_links.clear();
    m_regions.clear();
    m_nodeMap.clear();
    m_pinMap.clear();
    m_inputStringWindows.clear();
    m_nextNodeId = 1;
    m_nextPinId = 1;
    m_nextLinkId = 1;
    m_nextFunctionId = 1;
    m_nextRegionId = 1;
}
void BlueprintPanel::CloseCurrentBlueprint()
{
    if (!m_currentBlueprint) return;
    LogInfo("关闭蓝图: {}", m_currentBlueprintName);
    ClearEditorState();
    m_undoStack.clear();
    m_redoStack.clear();
    m_clipboardNodes.clear();
    m_clipboardLinks.clear();
    m_hasMoveCandidate = false;
    m_hasPendingEditSnapshot = false;
    m_pendingLinkPin = ed::PinId(0);
    m_currentViewFunction = 0;
    m_pendingViewFunction = kNoPendingView;
    m_viewJustSwitched = false;
    m_pendingNavKind = PendingNavKind::None;
    m_pendingFocusNodeId = 0;
    m_findReferences.isOpen = false;
    m_findReferences.items.clear();
    m_currentBlueprint = nullptr;
    m_currentBlueprintGuid = Guid();
    m_currentBlueprintName.clear();
    if (m_context)
    {
        m_context->currentEditingBlueprintGuid = Guid();
    }
}
void BlueprintPanel::Focus()
{
    m_requestFocus = true;
}
void BlueprintPanel::Draw()
{
    PROFILE_FUNCTION();
    if (!m_isVisible) return;
    if (m_requestFocus)
    {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
    std::string panelName = m_currentBlueprint
                                ? (m_currentBlueprintName + "###BlueprintEditor")
                                : "蓝图编辑器###BlueprintEditor";
    if (ImGui::Begin(panelName.c_str(), &m_isVisible, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse))
    {
        m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        drawMenuBar();
        if (!m_currentBlueprint)
        {
            ImVec2 center = ImGui::GetContentRegionAvail();
            const char* text = "请双击蓝图资源以开始编辑";
            ImVec2 textSize = ImGui::CalcTextSize(text);
            ImGui::SetCursorPos(ImVec2((center.x - textSize.x) * 0.5f, (center.y - textSize.y) * 0.5f));
            ImGui::TextUnformatted(text);
        }
        else
        {
            if (ImGui::BeginChild("MainContent", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar))
            {
                static float splitterWidth = 350.0f;
                ImVec2 contentSize = ImGui::GetContentRegionAvail();
                if (ImGui::BeginChild("NodeEditorWrapper", ImVec2(contentSize.x - splitterWidth - 10, 0), true,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove))
                {
                    drawNodeEditor();
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::Button("##splitter", ImVec2(10, -1));
                if (ImGui::IsItemActive())
                {
                    splitterWidth -= ImGui::GetIO().MouseDelta.x;
                    splitterWidth = std::clamp(splitterWidth, 250.0f, contentSize.x - 250.0f);
                }
                ImGui::SameLine();
                if (m_variablesPanelOpen)
                {
                    if (ImGui::BeginChild("SidePanel", ImVec2(splitterWidth, 0), true))
                    {
                        ImGui::Text("蓝图名称:");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##BlueprintName", m_blueprintNameBuffer, sizeof(m_blueprintNameBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            pushUndoSnapshot();
                            m_currentBlueprintName = m_blueprintNameBuffer;
                            m_currentBlueprint->GetBlueprintData().Name = m_currentBlueprintName;
                            updateSelfNodePinTypes();
                        }
                        ImGui::Separator();
                        if (ImGui::BeginTabBar("SidePanelTabs"))
                        {
                            if (ImGui::BeginTabItem("节点列表"))
                            {
                                drawNodeListPanel();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("变量"))
                            {
                                drawVariablesPanel();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("函数"))
                            {
                                drawFunctionsPanel();
                                ImGui::EndTabItem();
                            }
                            ImGui::EndTabBar();
                        }
                    }
                    ImGui::EndChild();
                }
            }
            ImGui::EndChild();
            updateInputStringWindows();
        }
    }
    ImGui::End();
    handleShortcutInput();
    if (m_currentBlueprint)
    {
        drawInputStringWindows();
        drawSelectTypeWindows();
        drawCreateFunctionPopup();
        drawSelectFunctionWindows();
        drawCreateRegionPopup();
        drawFindReferencesWindow();
    }
}
void BlueprintPanel::updateSelfNodePinTypes()
{
    if (!m_currentBlueprint) return;
    std::string selfType = "GameScripts." + m_currentBlueprint->GetBlueprintData().Name;
    for (auto& node : m_nodes)
    {
        BlueprintNode* sourceData = findSourceDataById(node.sourceDataID);
        if (sourceData && sourceData->TargetMemberName == "GetSelf")
        {
            for (auto& pin : node.outputPins)
            {
                if (pin.name == "自身")
                {
                    pin.type = selfType;
                }
            }
        }
    }
}
void BlueprintPanel::drawSelectTypeWindows()
{
    std::erase_if(m_selectTypeWindows, [](const auto& window) { return !window.isOpen; });
    for (auto& window : m_selectTypeWindows)
    {
        if (!window.isOpen) continue;
        ImGui::SetNextWindowSize(ImVec2(250, 350), ImGuiCond_FirstUseEver);
        if (window.needsFocus)
        {
            ImGui::SetNextWindowFocus();
            window.needsFocus = false;
        }
        if (ImGui::Begin(window.windowId.c_str(), &window.isOpen,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
        {
            BlueprintNode* sourceData = findSourceDataById(window.nodeId);
            if (sourceData)
            {
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere(0);
                ImGui::InputText("搜索", window.searchBuffer, sizeof(window.searchBuffer));
                ImGui::Separator();
                if (ImGui::BeginChild("##TypeScrollingRegion"))
                {
                    // 压栈时 captureStateToData 可能改动 InputDefaults，取值拷贝并经 map 写回，避免引用失效
                    std::string selectedType = sourceData->InputDefaults[window.pinName];
                    std::string_view search_sv(window.searchBuffer);
                    if (std::string_view("void").find(search_sv) != std::string_view::npos)
                    {
                        if (ImGui::Selectable("void", selectedType == "void"))
                        {
                            pushUndoSnapshot();
                            sourceData->InputDefaults[window.pinName] = "void";
                            window.isOpen = false;
                        }
                    }
                    for (const auto& typeName : m_sortedTypeNames)
                    {
                        if (typeName == "void")
                        {
                            continue;
                        }
                        if (search_sv.empty() || std::string_view(typeName).find(search_sv) != std::string_view::npos)
                        {
                            if (ImGui::Selectable(typeName.c_str(), selectedType == typeName))
                            {
                                pushUndoSnapshot();
                                sourceData->InputDefaults[window.pinName] = typeName;
                                window.isOpen = false;
                            }
                        }
                    }
                    ImGui::EndChild();
                }
            }
            else
            {
                ImGui::Text("错误: 找不到源节点数据。");
                if (ImGui::Button("关闭"))
                {
                    window.isOpen = false;
                }
            }
        }
        ImGui::End();
    }
}
BlueprintPanel::SelectTypeWindow* BlueprintPanel::findSelectTypeWindow(uint32_t nodeId, const std::string& pinName)
{
    for (auto& window : m_selectTypeWindows)
    {
        if (window.nodeId == nodeId && window.pinName == pinName)
        {
            return &window;
        }
    }
    return nullptr;
}
void BlueprintPanel::drawMenuBar()
{
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("文件"))
        {
            if (ImGui::MenuItem("保存", "Ctrl+S", false, m_currentBlueprint != nullptr))
            {
                saveToBlueprintData();
            }
            if (ImGui::MenuItem("关闭", "Ctrl+W", false, m_currentBlueprint != nullptr))
            {
                CloseCurrentBlueprint();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("创建"))
        {
            // 子图内禁止创建嵌套函数；普通注释区域各图页均可创建
            if (ImGui::MenuItem("创建函数...", nullptr, false, m_currentViewFunction == 0))
            {
                m_isEditingFunction = false;
                m_functionEditorBuffer = {};
                m_functionEditorBuffer.Name = "NewFunction";
                m_functionEditorBuffer.ReturnType = "void";
                m_functionEditorBuffer.Visibility = "public";
                m_showCreateFunctionPopup = true;
            }
            if (ImGui::MenuItem("创建逻辑区域..."))
            {
                m_showCreateRegionPopup = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图"))
        {
            ImGui::MenuItem("侧边栏", nullptr, &m_variablesPanelOpen);
            ImGui::Separator();
            // 有多选时整理选中节点，否则整理当前图页全部节点
            if (ImGui::MenuItem("整理节点", nullptr, false, m_currentBlueprint != nullptr))
            {
                arrangeNodes();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}
void BlueprintPanel::drawNodeEditor()
{
    applyPendingViewSwitch();
    drawGraphBreadcrumb();
    ed::SetCurrentEditor(m_nodeEditorContext);
    rebuildPinConnections();
    ed::Begin("BlueprintEditor");
    handleRegionInteraction();
    drawRegions();
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BLUEPRINT_NODE_DEFINITION"))
        {
            const char* nodeFullName = (const char*)payload->Data;
            const auto* definition = BlueprintNodeRegistry::GetInstance().GetDefinition(nodeFullName);
            if (definition)
            {
                ImVec2 nodePosition = ed::ScreenToCanvas({
                    static_cast<float>(LumaCursor::GetPosition().x), static_cast<float>(LumaCursor::GetPosition().y)
                });
                createNodeFromDefinition(definition, nodePosition);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BLUEPRINT_FUNCTION_CALL"))
        {
            const char* funcName = (const char*)payload->Data;
            const auto& funcs = m_currentBlueprint->GetBlueprintData().Functions;
            auto it = std::find_if(funcs.begin(), funcs.end(), [&](const BlueprintFunction& f)
            {
                return f.Name == funcName;
            });
            if (it != funcs.end())
            {
                ImVec2 nodePosition = ed::ScreenToCanvas(ImGui::GetMousePos());
                createFunctionCallNode(*it, nodePosition);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BLUEPRINT_VARIABLE"))
        {
            // 松开后先弹 Get/Set 选择菜单，选中后再创建节点
            m_varDropName = (const char*)payload->Data;
            m_varDropCanvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
            m_openVarDropMenu = true;
        }
    }
    ImGui::EndDragDropTarget();
    for (auto& node : m_nodes)
    {
        BlueprintNode* sourceData = findSourceDataById(node.sourceDataID);
        if (!sourceData) continue;
        // 只提交当前视图域内的节点：未提交的节点不绘制、不可命中，但状态仍保留在编辑器上下文中
        if (readNodeOwnerFunction(*sourceData) != m_currentViewFunction) continue;
        if (sourceData->TargetMemberName == "MakeArray")
        {
            std::string elementType = "System.Object";
            if (sourceData->InputDefaults.count("元素类型") && !sourceData->InputDefaults.at("元素类型").empty())
            {
                elementType = sourceData->InputDefaults.at("元素类型");
            }
            for (auto& pin : node.outputPins)
            {
                if (pin.name == "数组")
                {
                    pin.type = elementType + "[]";
                }
            }
            for (auto& pin : node.inputPins)
            {
                if (pin.name.rfind("_dyn_element_", 0) == 0)
                {
                    pin.type = elementType;
                }
            }
        }
        ed::BeginNode(node.id);
        ImGui::TextUnformatted(node.name.c_str());
        ImGui::Spacing();
        ImGui::BeginGroup();
        bool requestAddParameter = false;
        ed::PinId pinIdToDelete = 0;
        for (auto& pin : node.inputPins)
        {
            if (sourceData->Type == BlueprintNodeType::VariableSet && pin.name == "值")
            {
                ed::BeginPin(pin.id, ed::PinKind::Input);
                ImGui::TextColored(getPinTypeColor(pin.type), "-> %s", pin.name.c_str());
                ed::EndPin();
                if (!pin.isConnected)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    std::string& value = sourceData->InputDefaults[pin.name];
                    char buffer[256];
                    strncpy(buffer, value.c_str(), sizeof(buffer));
                    buffer[sizeof(buffer) - 1] = '\0';
                    if (ImGui::InputText(("##" + pin.name + std::to_string(pin.id.Get())).c_str(), buffer,
                                         sizeof(buffer)))
                    {
                        value = buffer;
                    }
                    trackItemEditUndo();
                }
            }
            else if (pin.type == "SelectType" || pin.type == "TemplateType")
            {
                ImGui::TextUnformatted(pin.name.c_str());
                std::string& selectedType = sourceData->InputDefaults[pin.name];
                if (selectedType.empty()) { selectedType = "void"; }
                std::string buttonText = selectedType;
                if (ImGui::Button((buttonText + "##" + std::to_string(pin.id.Get())).c_str(), ImVec2(150, 0)))
                {
                    SelectTypeWindow* window = findSelectTypeWindow(node.sourceDataID, pin.name);
                    if (!window)
                    {
                        SelectTypeWindow newWindow;
                        newWindow.nodeId = node.sourceDataID;
                        newWindow.pinName = pin.name;
                        newWindow.isOpen = true;
                        newWindow.needsFocus = true;
                        newWindow.windowId = "选择类型##" + std::to_string(node.sourceDataID) + "_" + pin.name;
                        m_selectTypeWindows.push_back(newWindow);
                    }
                    else
                    {
                        window->isOpen = true;
                        window->needsFocus = true;
                    }
                }
            }
            else if (sourceData->TargetMemberName == "Return" && pin.name == "输入值")
            {
                std::string returnType = sourceData->InputDefaults["返回类型"];
                if (returnType == "void") continue;
                pin.type = returnType;
                ed::BeginPin(pin.id, ed::PinKind::Input);
                ImGui::TextColored(getPinTypeColor(pin.type), "-> %s (%s)", pin.name.c_str(), pin.type.c_str());
                ed::EndPin();
            }
            else if (pin.type == "NodeInputText")
            {
                ImGui::TextUnformatted(pin.name.c_str());
                ImGui::SetNextItemWidth(150);
                std::string& value = sourceData->InputDefaults[pin.name];
                char buffer[256];
                strncpy(buffer, value.c_str(), sizeof(buffer));
                buffer[sizeof(buffer) - 1] = '\0';
                if (ImGui::InputText(("##" + pin.name + std::to_string(pin.id.Get())).c_str(), buffer, sizeof(buffer)))
                {
                    value = buffer;
                }
                trackItemEditUndo();
            }
            else if (sourceData->TargetClassFullName == "Luma.SDK.Debug" && pin.name == "message")
            {
                ed::BeginPin(pin.id, ed::PinKind::Input);
                ImGui::TextColored(getPinTypeColor(pin.type), "-> %s", pin.name.c_str());
                ed::EndPin();
                if (!pin.isConnected)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    std::string& value = sourceData->InputDefaults[pin.name];
                    char buffer[256];
                    strncpy(buffer, value.c_str(), sizeof(buffer));
                    buffer[sizeof(buffer) - 1] = '\0';
                    std::string inputId = "##" + pin.name + std::to_string(pin.id.Get());
                    if (ImGui::InputText(inputId.c_str(), buffer, sizeof(buffer)))
                    {
                        value = buffer;
                    }
                    trackItemEditUndo();
                }
            }
            else if (sourceData->TargetMemberName == "If" && pin.name == "条件")
            {
                ed::BeginPin(pin.id, ed::PinKind::Input);
                ImGui::TextColored(getPinTypeColor(pin.type), "-> %s", pin.name.c_str());
                ed::EndPin();
                if (!pin.isConnected)
                {
                    ImGui::SameLine();
                    std::string& value = sourceData->InputDefaults[pin.name];
                    std::string buttonText = value.empty()
                                                 ? "编辑条件"
                                                 : (value.length() > 15 ? value.substr(0, 12) + "..." : value);
                    if (ImGui::Button((buttonText + "##" + std::to_string(pin.id.Get())).c_str(), ImVec2(100, 0)))
                    {
                        InputStringWindow* window = findInputStringWindow(node.sourceDataID, pin.name);
                        if (!window)
                        {
                            InputStringWindow newWindow;
                            newWindow.nodeId = node.sourceDataID;
                            newWindow.pinName = pin.name;
                            newWindow.isOpen = true;
                            newWindow.needsFocus = true;
                            newWindow.size = ImVec2(300, 200);
                            newWindow.windowId = "InputString##" + std::to_string(node.sourceDataID) + "_" + pin.name;
                            m_inputStringWindows.push_back(newWindow);
                        }
                        else
                        {
                            window->isOpen = true;
                            window->needsFocus = true;
                        }
                    }
                }
            }
            else if (pin.type == "FunctionSelection")
            {
                ImGui::TextUnformatted(pin.name.c_str());
                ImGui::SameLine();
                std::string& selectedFunction = sourceData->InputDefaults[pin.name];
                const char* buttonText = selectedFunction.empty() ? "(选择回调函数)" : selectedFunction.c_str();
                if (ImGui::Button((std::string(buttonText) + "##" + std::to_string(pin.id.Get())).c_str(),
                                  ImVec2(180, 0)))
                {
                    SelectFunctionWindow* window = findSelectFunctionWindow(node.sourceDataID, pin.name);
                    if (!window)
                    {
                        SelectFunctionWindow newWindow;
                        newWindow.nodeId = node.sourceDataID;
                        newWindow.pinName = pin.name;
                        newWindow.isOpen = true;
                        newWindow.needsFocus = true;
                        newWindow.windowId = "选择函数##" + std::to_string(node.sourceDataID) + "_" + pin.name;
                        m_selectFunctionWindows.push_back(newWindow);
                    }
                    else
                    {
                        window->isOpen = true;
                        window->needsFocus = true;
                    }
                }
            }
            else if (pin.name == "参数列表" && pin.type == "Args")
            {
                ImGui::PushID(pin.id.Get());
                if (ImGui::Button("添加元素"))
                {
                    requestAddParameter = true;
                }
                ImGui::PopID();
            }
            else if (pin.name.rfind("_dyn_element_", 0) == 0)
            {
                ed::BeginPin(pin.id, ed::PinKind::Input);
                std::string displayName = pin.name.substr(13);
                ImGui::TextColored(getPinTypeColor(pin.type), "-> %s (%s)", displayName.c_str(), pin.type.c_str());
                ed::EndPin();
                ImGui::SameLine();
                ImGui::PushID(pin.id.Get());
                if (ImGui::Button("X"))
                {
                    pinIdToDelete = pin.id;
                }
                ImGui::PopID();
            }
            else
            {
                ed::BeginPin(pin.id, ed::PinKind::Input);
                ImGui::TextColored(getPinTypeColor(pin.type), "-> %s", pin.name.c_str());
                ed::EndPin();
            }
        }
        if (requestAddParameter)
        {
            pushUndoSnapshot();
            size_t insertPos = 0;
            size_t dynamicCount = 0;
            for (size_t i = 0; i < node.inputPins.size(); ++i)
            {
                if (node.inputPins[i].type == "Args")
                {
                    insertPos = i;
                }
                else if (node.inputPins[i].name.rfind("_dyn_element_", 0) == 0)
                {
                    dynamicCount++;
                }
            }
            std::string elementType = "System.Object";
            if (sourceData->InputDefaults.count("元素类型") && !sourceData->InputDefaults.at("元素类型").empty())
            {
                elementType = sourceData->InputDefaults.at("元素类型");
            }
            BPin newPin;
            newPin.id = getNextPinId();
            newPin.nodeId = node.id;
            newPin.name = "_dyn_element_" + std::to_string(dynamicCount);
            newPin.type = elementType;
            newPin.kind = ed::PinKind::Input;
            node.inputPins.insert(node.inputPins.begin() + insertPos, newPin);
        }
        if (pinIdToDelete.Get() != 0)
        {
            pushUndoSnapshot();
            std::vector<ed::LinkId> linksToDelete;
            for (const auto& link : m_links)
            {
                if (link.endPinId == pinIdToDelete)
                {
                    linksToDelete.push_back(link.id);
                }
            }
            for (auto linkId : linksToDelete)
            {
                deleteLink(linkId);
            }
            std::erase_if(node.inputPins, [pinIdToDelete](const BPin& pin)
            {
                return pin.id == pinIdToDelete;
            });
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        for (auto& pin : node.outputPins)
        {
            if (sourceData->TargetMemberName == "Declare" && pin.name == "输出变量")
            {
                std::string varType = sourceData->InputDefaults["变量类型"];
                if (varType.empty()) varType = "System.Object";
                pin.type = varType;
            }
            else if (sourceData->TargetClassFullName == "Utility" && sourceData->TargetMemberName == "Input" && pin.name
                == "输出")
            {
                std::string selectedType = sourceData->InputDefaults["类型"];
                if (selectedType.empty()) selectedType = "System.Object";
                pin.type = selectedType;
            }
            else if ((sourceData->TargetMemberName == "GetComponent" || sourceData->TargetMemberName == "AddComponent")
                && pin.name == "返回值")
            {
                std::string componentType = sourceData->InputDefaults["组件类型"];
                if (componentType.empty() || componentType == "选择类型")
                {
                    pin.type = "System.Object";
                }
                else
                {
                    pin.type = componentType;
                }
            }
            ed::BeginPin(pin.id, ed::PinKind::Output);
            if (pin.type == "Exec")
            {
                ImGui::TextColored(getPinTypeColor(pin.type), "%s ->", pin.name.c_str());
            }
            else
            {
                ImGui::TextColored(getPinTypeColor(pin.type), "%s (%s) ->", pin.name.c_str(), pin.type.c_str());
            }
            ed::EndPin();
        }
        ImGui::EndGroup();
        ed::EndNode();
        node.position = ed::GetNodePosition(node.id);
    }
    for (const auto& link : m_links)
    {
        const BPin* linkStartPin = findPinById(link.startPinId);
        const BPin* linkEndPin = findPinById(link.endPinId);
        // 两端都在当前视图域内才提交（跨域连线已被 canCreateLink 拒绝，此处兜底过滤）
        if (getPinOwnerFunction(linkStartPin) != m_currentViewFunction ||
            getPinOwnerFunction(linkEndPin) != m_currentViewFunction)
        {
            continue;
        }
        // 连线颜色取源输出引脚的类型色
        ImVec4 linkColor = linkStartPin ? getPinTypeColor(linkStartPin->type) : ImVec4(1, 1, 1, 1);
        ed::Link(link.id, link.startPinId, link.endPinId, linkColor);
    }
    // 待定导航统一在节点提交完成后应用，保证目标对象已存在于编辑器中；
    // 优先级：节点居中（查找引用跳转）> 精确矩形（书签）> 内容自适应（切页）
    if (m_pendingFocusNodeId != 0)
    {
        BNode* focusNode = nullptr;
        for (auto& candidate : m_nodes)
        {
            if (candidate.sourceDataID == m_pendingFocusNodeId)
            {
                focusNode = &candidate;
                break;
            }
        }
        if (focusNode)
        {
            ed::SelectNode(focusNode->id, false);
            ed::CenterNodeOnScreen(focusNode->id);
        }
        m_pendingFocusNodeId = 0;
        m_pendingNavKind = PendingNavKind::None;
        m_viewJustSwitched = false;
    }
    else if (m_pendingNavKind == PendingNavKind::Rect)
    {
        navigateToViewRect(m_pendingNavRect);
        m_pendingNavKind = PendingNavKind::None;
        m_viewJustSwitched = false;
    }
    else if (m_viewJustSwitched || m_pendingNavKind == PendingNavKind::Content)
    {
        // 本帧已按新视图提交内容，把镜头对准当前域
        ed::NavigateToContent(0.0f);
        m_pendingNavKind = PendingNavKind::None;
        m_viewJustSwitched = false;
    }
    if (ed::BeginCreate())
    {
        ed::PinId startPinId, endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId))
        {
            BPin* startPin = findPinById(startPinId);
            BPin* endPin = findPinById(endPinId);
            if (startPin && endPin && startPin->nodeId != endPin->nodeId)
            {
                if (canCreateLink(startPin, endPin))
                {
                    if (ed::AcceptNewItem())
                    {
                        pushUndoSnapshot();
                        BLink newLink{getNextLinkId(), startPin->id, endPin->id};
                        if (startPin->kind == ed::PinKind::Input)
                        {
                            std::swap(newLink.startPinId, newLink.endPinId);
                        }
                        m_links.push_back(newLink);
                    }
                }
                else
                {
                    ed::RejectNewItem(ImVec4(1, 0, 0, 1), 2.0f);
                }
            }
        }
        ed::PinId newNodePinId = 0;
        if (ed::QueryNewNode(&newNodePinId))
        {
            // 拖线到空白处松开：记录起点引脚，弹出按类型过滤的节点创建菜单
            BPin* draggedPin = findPinById(newNodePinId);
            if (draggedPin && ed::AcceptNewItem())
            {
                m_pendingLinkPin = newNodePinId;
                m_pinMenuSearchBuffer[0] = '\0';
                ed::Suspend();
                ImGui::OpenPopup("CreateNodeFromPinMenu");
                ed::Resume();
            }
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete())
    {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId))
        {
            if (ed::AcceptDeletedItem()) { deleteLink(deletedLinkId); }
        }
        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId))
        {
            BNode* nodeToCheck = findNodeById(deletedNodeId);
            BlueprintNode* nodeSourceData = nodeToCheck ? findSourceDataById(nodeToCheck->sourceDataID) : nullptr;
            if (nodeSourceData && nodeSourceData->Type == BlueprintNodeType::FunctionEntry)
            {
                // 函数入口节点只能随函数一起删除
                ed::RejectDeletedItem();
                continue;
            }
            if (ed::AcceptDeletedItem()) { deleteNode(deletedNodeId); }
        }
    }
    ed::EndDelete();
    // 双击函数入口/函数调用节点（本蓝图函数以 TargetMemberName 标识、无类名）进入对应函数子图
    if (ed::NodeId doubleClickedNodeId = ed::GetDoubleClickedNode())
    {
        BNode* doubleClickedNode = findNodeById(doubleClickedNodeId);
        BlueprintNode* doubleClickedSource = doubleClickedNode
                                                 ? findSourceDataById(doubleClickedNode->sourceDataID)
                                                 : nullptr;
        if (doubleClickedSource && doubleClickedSource->TargetClassFullName.empty() &&
            (doubleClickedSource->Type == BlueprintNodeType::FunctionEntry ||
                doubleClickedSource->Type == BlueprintNodeType::FunctionCall))
        {
            const auto& funcs = m_currentBlueprint->GetBlueprintData().Functions;
            auto it = std::find_if(funcs.begin(), funcs.end(), [&](const BlueprintFunction& f)
            {
                return f.Name == doubleClickedSource->TargetMemberName;
            });
            if (it != funcs.end())
            {
                requestViewSwitch(it->ID);
            }
        }
    }
    // 节点/区域拖动的撤销检测：按下时记录基准与前置快照，松开且发生位移时将该快照入栈
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ed::IsActive())
    {
        m_moveCandidateSnapshot = makeSnapshot();
        m_moveStartNodePositions.clear();
        m_moveStartRegionRects.clear();
        for (const auto& node : m_nodes)
        {
            m_moveStartNodePositions[node.id.Get()] = node.position;
        }
        for (const auto& region : m_regions)
        {
            m_moveStartRegionRects[region.id] = {region.position, region.size};
        }
        m_hasMoveCandidate = true;
    }
    if (m_hasMoveCandidate && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        bool anyMoved = false;
        for (const auto& node : m_nodes)
        {
            auto it = m_moveStartNodePositions.find(node.id.Get());
            if (it == m_moveStartNodePositions.end()) continue;
            if (std::fabs(node.position.x - it->second.x) > 0.5f ||
                std::fabs(node.position.y - it->second.y) > 0.5f)
            {
                anyMoved = true;
                break;
            }
        }
        if (!anyMoved)
        {
            for (const auto& region : m_regions)
            {
                auto it = m_moveStartRegionRects.find(region.id);
                if (it == m_moveStartRegionRects.end()) continue;
                if (std::fabs(region.position.x - it->second.first.x) > 0.5f ||
                    std::fabs(region.position.y - it->second.first.y) > 0.5f ||
                    std::fabs(region.size.x - it->second.second.x) > 0.5f ||
                    std::fabs(region.size.y - it->second.second.y) > 0.5f)
                {
                    anyMoved = true;
                    break;
                }
            }
        }
        if (anyMoved)
        {
            pushUndoSnapshotDirect(std::move(m_moveCandidateSnapshot));
        }
        m_hasMoveCandidate = false;
    }
    ed::Suspend();
    ed::NodeId contextNodeId;
    ed::LinkId contextLinkId;
    if (ed::ShowNodeContextMenu(&contextNodeId))
    {
        m_contextNodeId = contextNodeId;
        ImGui::OpenPopup("NodeContextMenu");
    }
    else if (ed::ShowLinkContextMenu(&contextLinkId))
    {
        m_contextLinkId = contextLinkId;
        ImGui::OpenPopup("LinkContextMenu");
    }
    else if (ed::ShowBackgroundContextMenu())
    {
        ImGui::OpenPopup("CreateNodeMenu");
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ed::IsActive() && !ed::GetHoveredNode() && !
        ed::GetHoveredLink())
    {
        bool onRegion = false;
        for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it)
        {
            const auto& region = *it;
            if (region.ownerFunctionId != m_currentViewFunction) continue; // 不可见区域不响应右键
            // 函数区域按占位卡片尺寸命中，与绘制保持一致
            const ImVec2 displaySize = region.functionId != 0
                                           ? ImVec2(kFunctionCardWidth, kFunctionCardHeight)
                                           : region.size;
            ImVec2 canvas_br = ImVec2(region.position.x + displaySize.x, region.position.y + displaySize.y);
            ImRect regionRect(ed::CanvasToScreen(region.position), ed::CanvasToScreen(canvas_br));
            if (regionRect.Contains(ImGui::GetMousePos()))
            {
                m_contextRegionId = region.id;
                onRegion = true;
                break;
            }
        }
        if (onRegion)
        {
            ImGui::OpenPopup("RegionContextMenu");
        }
    }
    if (m_openVarDropMenu)
    {
        ImGui::OpenPopup("VariableDropMenu");
        m_openVarDropMenu = false;
    }
    drawNodeContextMenu();
    drawLinkContextMenu();
    drawRegionContextMenu();
    drawBackgroundContextMenu();
    drawCreateNodeFromPinMenu();
    drawVariableDropMenu();
    ed::Resume();
    ed::End();
}
void BlueprintPanel::drawNodeListPanel()
{
    static char searchBuffer[128] = "";
    ImGui::InputText("搜索", searchBuffer, sizeof(searchBuffer));
    ImGui::Separator();
    if (ImGui::BeginChild("NodeListScroll"))
    {
        const auto& categorizedNodes = BlueprintNodeRegistry::GetInstance().GetCategorizedDefinitions();
        for (const auto& [category, definitions] : categorizedNodes)
        {
            if (ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto* def : definitions)
                {
                    if (searchBuffer[0] != '\0' && std::string_view(def->DisplayName).find(searchBuffer) ==
                        std::string_view::npos)
                    {
                        continue;
                    }
                    bool eventExists = (def->NodeType == BlueprintNodeType::Event && doesEventNodeExist(def->FullName));
                    if (eventExists)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        ImGui::Selectable(def->DisplayName.c_str(), false, ImGuiSelectableFlags_Disabled);
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::Selectable(def->DisplayName.c_str());
                        if (ImGui::BeginDragDropSource())
                        {
                            ImGui::SetDragDropPayload("BLUEPRINT_NODE_DEFINITION", def->FullName.c_str(),
                                                      def->FullName.length() + 1);
                            ImGui::Text("创建 %s", def->DisplayName.c_str());
                            ImGui::EndDragDropSource();
                        }
                    }
                }
            }
        }
    }
    ImGui::EndChild();
}
void BlueprintPanel::drawVariablesPanel()
{
    ImGui::TextUnformatted("蓝图变量");
    ImGui::SameLine(
        ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize("添加变量").x -
        ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::Button("添加变量"))
    {
        pushUndoSnapshot();
        BlueprintVariable newVar;
        newVar.Name = "NewVar" + std::to_string(m_currentBlueprint->GetBlueprintData().Variables.size());
        newVar.Type = "System.Single";
        m_currentBlueprint->GetBlueprintData().Variables.push_back(newVar);
    }
    ImGui::Separator();
    if (ImGui::BeginChild("VariableList"))
    {
        auto& variables = m_currentBlueprint->GetBlueprintData().Variables;
        for (size_t i = 0; i < variables.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            auto& var = variables[i];
            // 拖拽手柄：拖到画布空白处松开可生成 Get/Set 节点
            ImGui::Selectable("::", false, 0, ImVec2(14, 0));
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("BLUEPRINT_VARIABLE", var.Name.c_str(), var.Name.length() + 1);
                ImGui::Text("变量 %s", var.Name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("VariableItemContext"))
            {
                if (ImGui::MenuItem("查找引用"))
                {
                    openVariableReferences(var.Name);
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            char nameBuffer[256];
            strncpy(nameBuffer, var.Name.c_str(), sizeof(nameBuffer) - 1);
            nameBuffer[sizeof(nameBuffer) - 1] = '\0';
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("##VarName", nameBuffer, sizeof(nameBuffer)))
            {
                var.Name = nameBuffer;
            }
            trackItemEditUndo();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##VarType", var.Type.c_str()))
            {
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere(0);
                ImGui::InputText("搜索", m_variableTypeSearchBuffer, sizeof(m_variableTypeSearchBuffer));
                ImGui::Separator();
                for (const auto& typeName : m_sortedTypeNames)
                {
                    std::string_view search_sv(m_variableTypeSearchBuffer);
                    if (search_sv.empty() || std::string_view(typeName).find(search_sv) != std::string_view::npos)
                    {
                        if (ImGui::Selectable(typeName.c_str(), var.Type == typeName))
                        {
                            pushUndoSnapshot();
                            var.Type = typeName;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("X"))
            {
                pushUndoSnapshot();
                variables.erase(variables.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
void BlueprintPanel::drawFunctionsPanel()
{
    if (!m_currentBlueprint) return;
    ImGui::BeginDisabled(m_currentViewFunction != 0); // 子图内禁止创建嵌套函数
    if (ImGui::Button("创建函数"))
    {
        m_isEditingFunction = false;
        m_functionEditorBuffer = {};
        m_functionEditorBuffer.Name = "NewFunction" + std::to_string(
            m_currentBlueprint->GetBlueprintData().Functions.size());
        m_functionEditorBuffer.ReturnType = "void";
        m_functionEditorBuffer.Visibility = "public";
        m_functionEditorBuffer.IsStatic = false;
        strncpy(m_functionNameBuffer, m_functionEditorBuffer.Name.c_str(), sizeof(m_functionNameBuffer));
        m_functionNameBuffer[sizeof(m_functionNameBuffer) - 1] = '\0';
        m_functionTypeSearchBuffer[0] = '\0';
        m_showCreateFunctionPopup = true;
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::BeginChild("FunctionsList"))
    {
        auto& functions = m_currentBlueprint->GetBlueprintData().Functions;
        for (size_t i = 0; i < functions.size();)
        {
            ImGui::PushID(static_cast<int>(i));
            auto& func = functions[i];
            // 弹出菜单里可能触发删除使 func 引用失效，先拷贝要用的字段
            const std::string funcName = func.Name;
            const uint32_t funcId = func.ID;
            std::string signature = funcName + "()  [" + std::to_string(countNodesOwnedByFunction(funcId)) +
                " 节点]";
            const float buttons_width = 100.0f;
            float selectableWidth = ImGui::GetContentRegionAvail().x - buttons_width;
            if (selectableWidth < 1.0f) selectableWidth = 1.0f;
            ImGui::Selectable(signature.c_str(), m_currentViewFunction == funcId, 0, ImVec2(selectableWidth, 0));
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("BLUEPRINT_FUNCTION_CALL", funcName.c_str(), funcName.length() + 1);
                ImGui::Text("调用函数 %s", funcName.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                requestViewSwitch(funcId); // 双击函数名进入子图
            }
            bool wantDelete = false;
            bool wantEdit = false;
            if (ImGui::BeginPopupContextItem("FunctionItemContext"))
            {
                if (ImGui::MenuItem("进入编辑"))
                {
                    requestViewSwitch(funcId);
                }
                if (ImGui::MenuItem("查找引用"))
                {
                    openFunctionReferences(funcName);
                }
                if (ImGui::MenuItem("重命名/修改签名..."))
                {
                    wantEdit = true;
                }
                if (ImGui::MenuItem("删除函数"))
                {
                    wantDelete = true;
                }
                ImGui::EndPopup();
            }
            if (wantDelete)
            {
                deleteFunction(funcName);
                ImGui::PopID();
                continue;
            }
            ImGui::SameLine();
            if (ImGui::Button(("编辑##" + std::to_string(funcId)).c_str()))
            {
                wantEdit = true;
            }
            if (wantEdit)
            {
                m_contextFunctionName = funcName;
                m_isEditingFunction = true;
                m_functionEditorBuffer = func;
                strncpy(m_functionNameBuffer, m_functionEditorBuffer.Name.c_str(), sizeof(m_functionNameBuffer));
                m_functionNameBuffer[sizeof(m_functionNameBuffer) - 1] = '\0';
                m_functionTypeSearchBuffer[0] = '\0';
                m_showCreateFunctionPopup = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(("X##" + std::to_string(funcId)).c_str()))
            {
                deleteFunction(funcName);
                ImGui::PopID();
                continue;
            }
            ImGui::PopID();
            i++;
        }
    }
    ImGui::EndChild();
}
void BlueprintPanel::deleteFunction(const std::string& functionName)
{
    LogInfo("删除函数: {}", functionName);
    pushUndoSnapshot();
    auto& functions = m_currentBlueprint->GetBlueprintData().Functions;
    uint32_t funcIdToDelete = 0;
    auto funcIt = std::find_if(functions.begin(), functions.end(),
                               [&](const BlueprintFunction& f) { return f.Name == functionName; });
    if (funcIt != functions.end())
    {
        funcIdToDelete = funcIt->ID;
    }
    std::vector<ed::NodeId> nodesToDelete;
    for (const auto& n : m_nodes)
    {
        auto* sourceData = findSourceDataById(n.sourceDataID);
        if (sourceData &&
            (sourceData->Type == BlueprintNodeType::FunctionEntry || sourceData->Type ==
                BlueprintNodeType::FunctionCall) &&
            sourceData->TargetMemberName == functionName)
        {
            nodesToDelete.push_back(n.id);
        }
    }
    for (auto nodeId : nodesToDelete)
    {
        deleteNode(nodeId, true);
    }
    if (funcIdToDelete != 0)
    {
        std::erase_if(m_regions, [&](const BRegion& region) { return region.functionId == funcIdToDelete; });
        std::erase_if(m_currentBlueprint->GetBlueprintData().CommentRegions,
                      [&](const BlueprintCommentRegion& region) { return region.FunctionID == funcIdToDelete; });
        // 函数体剩余节点（Return、变量节点等）与子图内的普通注释区域回到主图，避免带着失效归属永久隐藏
        for (auto& bpNode : m_currentBlueprint->GetBlueprintData().Nodes)
        {
            if (readNodeOwnerFunction(bpNode) == funcIdToDelete)
            {
                writeNodeOwnerFunction(bpNode, 0);
            }
        }
        for (auto& region : m_regions)
        {
            if (region.ownerFunctionId == funcIdToDelete) region.ownerFunctionId = 0;
        }
        for (auto& regionData : m_currentBlueprint->GetBlueprintData().CommentRegions)
        {
            if (regionData.OwnerFunctionID == funcIdToDelete) regionData.OwnerFunctionID = 0;
        }
        if (m_currentViewFunction == funcIdToDelete)
        {
            m_currentViewFunction = 0;
            m_viewJustSwitched = true;
        }
    }
    std::erase_if(functions, [&](const BlueprintFunction& f) { return f.Name == functionName; });
}
void BlueprintPanel::rebuildFunctionNodePins(const std::string& oldName, const BlueprintFunction& updatedFunc)
{
    if (oldName != updatedFunc.Name)
    {
        for (auto& region : m_regions)
        {
            if (region.functionId == updatedFunc.ID)
            {
                region.title = updatedFunc.Name;
                break;
            }
        }
        for (auto& region_data : m_currentBlueprint->GetBlueprintData().CommentRegions)
        {
            if (region_data.FunctionID == updatedFunc.ID)
            {
                region_data.Title = updatedFunc.Name;
                break;
            }
        }
    }
    for (auto& node : m_nodes)
    {
        auto* sourceData = findSourceDataById(node.sourceDataID);
        if (!sourceData || (sourceData->TargetMemberName != oldName && sourceData->TargetMemberName != updatedFunc.
            Name))
            continue;
        if (sourceData->Type != BlueprintNodeType::FunctionEntry && sourceData->Type != BlueprintNodeType::FunctionCall)
            continue;
        sourceData->TargetMemberName = updatedFunc.Name;
        node.name = updatedFunc.Name;
        std::vector<ed::LinkId> linksToDelete;
        for (const auto& pin : node.inputPins)
        {
            for (const auto& link : m_links) if (link.endPinId == pin.id) linksToDelete.push_back(link.id);
        }
        for (const auto& pin : node.outputPins)
        {
            for (const auto& link : m_links) if (link.startPinId == pin.id) linksToDelete.push_back(link.id);
        }
        for (auto linkId : linksToDelete) deleteLink(linkId);
        node.inputPins.clear();
        node.outputPins.clear();
        if (sourceData->Type == BlueprintNodeType::FunctionEntry)
        {
            node.outputPins.push_back({getNextPinId(), node.id, "然后", "Exec", ed::PinKind::Output});
            for (const auto& param : updatedFunc.Parameters)
            {
                node.outputPins.push_back({getNextPinId(), node.id, param.Name, param.Type, ed::PinKind::Output});
            }
        }
        else
        {
            node.inputPins.push_back({getNextPinId(), node.id, "", "Exec", ed::PinKind::Input});
            for (const auto& param : updatedFunc.Parameters)
            {
                node.inputPins.push_back({getNextPinId(), node.id, param.Name, param.Type, ed::PinKind::Input});
            }
            node.outputPins.push_back({getNextPinId(), node.id, "然后", "Exec", ed::PinKind::Output});
            if (updatedFunc.ReturnType != "void")
            {
                node.outputPins.push_back({
                    getNextPinId(), node.id, "返回值", updatedFunc.ReturnType, ed::PinKind::Output
                });
            }
        }
    }
    rebuildPinConnections();
}
void BlueprintPanel::drawCreateFunctionPopup()
{
    if (!m_showCreateFunctionPopup) return;
    const char* popupTitle = m_isEditingFunction ? "修改函数签名" : "创建新函数";
    ImGui::OpenPopup(popupTitle);
    if (ImGui::BeginPopupModal(popupTitle, &m_showCreateFunctionPopup, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::InputText("函数名", m_functionNameBuffer, sizeof(m_functionNameBuffer)))
        {
            m_functionEditorBuffer.Name = m_functionNameBuffer;
        }
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("返回类型", m_functionEditorBuffer.ReturnType.c_str()))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere(0);
            ImGui::InputText("搜索", m_functionTypeSearchBuffer, sizeof(m_functionTypeSearchBuffer));
            ImGui::Separator();
            std::string_view search_sv(m_functionTypeSearchBuffer);
            if (std::string_view("void").find(search_sv) != std::string_view::npos)
            {
                if (ImGui::Selectable("void", m_functionEditorBuffer.ReturnType == "void"))
                {
                    m_functionEditorBuffer.ReturnType = "void";
                }
            }
            for (const auto& typeName : m_sortedTypeNames)
            {
                if (typeName == "void")
                {
                    continue;
                }
                if (search_sv.empty() || std::string_view(typeName).find(search_sv) != std::string_view::npos)
                {
                    if (ImGui::Selectable(typeName.c_str(), m_functionEditorBuffer.ReturnType == typeName))
                    {
                        m_functionEditorBuffer.ReturnType = typeName;
                    }
                }
            }
            ImGui::EndCombo();
        }
        const char* visibilities[] = {"公开", "私有", "受保护"};
        const char* visibilities_en[] = {"public", "private", "protected"};
        int currentVisibility = 0;
        if (m_functionEditorBuffer.Visibility == "private") currentVisibility = 1;
        if (m_functionEditorBuffer.Visibility == "protected") currentVisibility = 2;
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("可见性", &currentVisibility, visibilities, IM_ARRAYSIZE(visibilities)))
        {
            m_functionEditorBuffer.Visibility = visibilities_en[currentVisibility];
        }
        ImGui::SeparatorText("参数列表");
        for (size_t i = 0; i < m_functionEditorBuffer.Parameters.size();)
        {
            ImGui::PushID(static_cast<int>(i));
            auto& param = m_functionEditorBuffer.Parameters[i];
            char paramNameBuffer[128];
            strncpy(paramNameBuffer, param.Name.c_str(), sizeof(paramNameBuffer) - 1);
            paramNameBuffer[sizeof(paramNameBuffer) - 1] = '\0';
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputText("##ParamName", paramNameBuffer, sizeof(paramNameBuffer)))
            {
                param.Name = paramNameBuffer;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##ParamType", param.Type.c_str()))
            {
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere(0);
                ImGui::InputText("搜索", m_functionTypeSearchBuffer, sizeof(m_functionTypeSearchBuffer));
                ImGui::Separator();
                for (const auto& typeName : m_sortedTypeNames)
                {
                    std::string_view search_sv_param(m_functionTypeSearchBuffer);
                    if (search_sv_param.empty() || std::string_view(typeName).find(search_sv_param) !=
                        std::string_view::npos)
                    {
                        if (ImGui::Selectable(typeName.c_str(), param.Type == typeName))
                        {
                            param.Type = typeName;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("移除"))
            {
                m_functionEditorBuffer.Parameters.erase(m_functionEditorBuffer.Parameters.begin() + i);
                ImGui::PopID();
                continue;
            }
            ImGui::PopID();
            i++;
        }
        if (ImGui::Button("添加参数"))
        {
            m_functionEditorBuffer.Parameters.push_back({
                "newParam" + std::to_string(m_functionEditorBuffer.Parameters.size()), "System.Int32"
            });
        }
        ImGui::Separator();
        const char* buttonText = m_isEditingFunction ? "应用修改" : "创建";
        if (ImGui::Button(buttonText, ImVec2(120, 0)))
        {
            pushUndoSnapshot();
            if (m_isEditingFunction)
            {
                auto& funcs = m_currentBlueprint->GetBlueprintData().Functions;
                auto it = std::find_if(funcs.begin(), funcs.end(),
                                       [&](const BlueprintFunction& f) { return f.Name == m_contextFunctionName; });
                if (it != funcs.end())
                {
                    std::string oldName = it->Name;
                    *it = m_functionEditorBuffer;
                    rebuildFunctionNodePins(oldName, *it);
                }
            }
            else
            {
                m_functionEditorBuffer.ID = getNextFunctionId();
                m_functionEditorBuffer.IsStatic = false;
                m_currentBlueprint->GetBlueprintData().Functions.push_back(m_functionEditorBuffer);
                BlueprintCommentRegion regionData;
                regionData.ID = getNextRegionId();
                regionData.Title = m_functionEditorBuffer.Name;
                regionData.FunctionID = m_functionEditorBuffer.ID;
                regionData.Position = {100.0f, 100.0f};
                regionData.Size = {600.0f, 400.0f};
                m_currentBlueprint->GetBlueprintData().CommentRegions.push_back(regionData);
                BRegion region;
                region.id = regionData.ID;
                region.title = regionData.Title;
                region.position = ImVec2(regionData.Position.x, regionData.Position.y);
                region.size = ImVec2(regionData.Size.w, regionData.Size.h);
                region.functionId = regionData.FunctionID;
                ImGuiID hash = ImHashStr(region.title.c_str(), 0, 0);
                region.color = ImPlot::GetColormapColor(((hash & 0xFF)) % ImPlot::GetColormapSize(ImPlotColormap_Deep),
                                                        ImPlotColormap_Deep);
                region.color.w = 0.4f;
                m_regions.push_back(region);
                BlueprintNode bpNode;
                bpNode.ID = getNextNodeId();
                bpNode.Type = BlueprintNodeType::FunctionEntry;
                bpNode.TargetMemberName = m_functionEditorBuffer.Name;
                ImVec2 entryNodePos = {regionData.Position.x + 20, regionData.Position.y + 40};
                bpNode.Position = {entryNodePos.x, entryNodePos.y};
                writeNodeOwnerFunction(bpNode, m_functionEditorBuffer.ID);
                m_currentBlueprint->GetBlueprintData().Nodes.push_back(bpNode);
                BNode editorNode;
                editorNode.id = ed::NodeId(bpNode.ID);
                editorNode.sourceDataID = bpNode.ID;
                editorNode.name = bpNode.TargetMemberName;
                editorNode.position = {bpNode.Position.x, bpNode.Position.y};
                editorNode.outputPins.push_back({getNextPinId(), editorNode.id, "然后", "Exec", ed::PinKind::Output});
                for (const auto& param : m_functionEditorBuffer.Parameters)
                {
                    editorNode.outputPins.push_back({
                        getNextPinId(), editorNode.id, param.Name, param.Type, ed::PinKind::Output
                    });
                }
                m_nodes.push_back(editorNode);
                if (m_functionEditorBuffer.ReturnType != "void")
                {
                    const auto* returnDef = BlueprintNodeRegistry::GetInstance().GetDefinition("FlowControl.Return");
                    if (returnDef)
                    {
                        BlueprintNode bpReturnNode;
                        bpReturnNode.ID = getNextNodeId();
                        bpReturnNode.Type = returnDef->NodeType;
                        bpReturnNode.Position = {entryNodePos.x + 400, entryNodePos.y};
                        std::string_view fullName(returnDef->FullName);
                        size_t lastDot = fullName.find_last_of('.');
                        if (lastDot != std::string_view::npos)
                        {
                            bpReturnNode.TargetClassFullName = std::string(fullName.substr(0, lastDot));
                            bpReturnNode.TargetMemberName = std::string(fullName.substr(lastDot + 1));
                        }
                        bpReturnNode.InputDefaults["返回类型"] = m_functionEditorBuffer.ReturnType;
                        writeNodeOwnerFunction(bpReturnNode, m_functionEditorBuffer.ID);
                        m_currentBlueprint->GetBlueprintData().Nodes.push_back(bpReturnNode);
                        BNode editorReturnNode;
                        editorReturnNode.id = ed::NodeId(bpReturnNode.ID);
                        editorReturnNode.sourceDataID = bpReturnNode.ID;
                        editorReturnNode.name = returnDef->DisplayName;
                        editorReturnNode.position = {bpReturnNode.Position.x, bpReturnNode.Position.y};
                        for (const auto& pinDef : returnDef->InputPins)
                        {
                            BPin pin = {
                                getNextPinId(), editorReturnNode.id, pinDef.Name, pinDef.Type, ed::PinKind::Input
                            };
                            if (pin.name == "输入值")
                            {
                                pin.type = m_functionEditorBuffer.ReturnType;
                            }
                            editorReturnNode.inputPins.push_back(pin);
                        }
                        m_nodes.push_back(editorReturnNode);
                    }
                }
            }
            m_showCreateFunctionPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0)))
        {
            m_showCreateFunctionPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
void BlueprintPanel::drawCreateRegionPopup()
{
    if (!m_showCreateRegionPopup) return;
    ImGui::OpenPopup("创建逻辑区域");
    if (ImGui::BeginPopupModal("创建逻辑区域", &m_showCreateRegionPopup, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("标题", m_newRegionTitleBuffer, sizeof(m_newRegionTitleBuffer));
        ImGui::ColorEdit3("颜色", m_newRegionColorBuffer);
        ImGui::DragFloat2("大小", m_newRegionSizeBuffer, 1.0f, 50.0f, 2000.0f);
        ImGui::Separator();
        if (ImGui::Button("创建", ImVec2(120, 0)))
        {
            pushUndoSnapshot();
            BlueprintCommentRegion regionData;
            regionData.ID = getNextRegionId();
            regionData.Title = m_newRegionTitleBuffer;
            regionData.FunctionID = 0;
            regionData.OwnerFunctionID = m_currentViewFunction; // 普通注释区域归属当前视图域
            ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
            regionData.Position.x = canvasPos.x;
            regionData.Position.y = canvasPos.y;
            regionData.Size = {m_newRegionSizeBuffer[0], m_newRegionSizeBuffer[1]};
            m_currentBlueprint->GetBlueprintData().CommentRegions.push_back(regionData);
            BRegion region;
            region.id = regionData.ID;
            region.title = regionData.Title;
            region.position = ImVec2(regionData.Position.x, regionData.Position.y);
            region.size = ImVec2(regionData.Size.w, regionData.Size.h);
            region.functionId = regionData.FunctionID;
            region.ownerFunctionId = regionData.OwnerFunctionID;
            region.color = ImVec4(m_newRegionColorBuffer[0], m_newRegionColorBuffer[1], m_newRegionColorBuffer[2],
                                  0.4f);
            m_regions.push_back(region);
            m_showCreateRegionPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0)))
        {
            m_showCreateRegionPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
BlueprintPanel::SelectFunctionWindow* BlueprintPanel::findSelectFunctionWindow(
    uint32_t nodeId, const std::string& pinName)
{
    for (auto& window : m_selectFunctionWindows)
    {
        if (window.nodeId == nodeId && window.pinName == pinName)
        {
            return &window;
        }
    }
    return nullptr;
}
void BlueprintPanel::handleShortcutInput()
{
    if (!m_isFocused) return;
    if (Keyboard::LeftCtrl.IsPressed() && Keyboard::S.IsPressed())
    {
        saveToBlueprintData();
    }
    if (Keyboard::LeftCtrl.IsPressed() && Keyboard::W.IsPressed())
    {
        CloseCurrentBlueprint();
    }
    if (!m_currentBlueprint) return;
    // 文本输入期间不响应编辑类快捷键，避免与输入框冲突
    if (ImGui::GetIO().WantTextInput) return;
    const bool ctrlDown = Keyboard::LeftCtrl.IsPressed() || Keyboard::RightCtrl.IsPressed();
    const bool shiftDown = Keyboard::LeftShift.IsPressed() || Keyboard::RightShift.IsPressed();
    if (ctrlDown && Keyboard::Z.IsDown())
    {
        if (shiftDown)
        {
            performRedo();
        }
        else
        {
            performUndo();
        }
    }
    if (ctrlDown && Keyboard::C.IsDown())
    {
        copySelectionToClipboard();
    }
    if (ctrlDown && Keyboard::V.IsDown())
    {
        pasteClipboardAtMouse();
    }
    if (ctrlDown && Keyboard::D.IsDown())
    {
        duplicateSelection();
    }
    if (Keyboard::Delete.IsDown())
    {
        deleteSelectedObjects();
    }
    // 画布书签：Ctrl+1/2/3 设置当前视角，Shift+1/2/3 跳转（Ctrl+Shift 同按视为无效组合）
    const Key* bookmarkKeys[] = {&Keyboard::Num1, &Keyboard::Num2, &Keyboard::Num3};
    for (int slot = 0; slot < 3; ++slot)
    {
        if (!bookmarkKeys[slot]->IsDown()) continue;
        if (ctrlDown && !shiftDown)
        {
            setBookmark(slot + 1);
        }
        else if (shiftDown && !ctrlDown)
        {
            jumpToBookmark(slot + 1);
        }
    }
    if (m_currentViewFunction != 0 && Keyboard::Escape.IsDown() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        requestViewSwitch(0); // Esc 返回主图；弹窗打开时让 Esc 先去关闭弹窗
    }
}
void BlueprintPanel::drawSelectFunctionWindows()
{
    std::erase_if(m_selectFunctionWindows, [](const auto& window) { return !window.isOpen; });
    for (auto& window : m_selectFunctionWindows)
    {
        if (!window.isOpen) continue;
        ImGui::SetNextWindowSize(ImVec2(250, 350), ImGuiCond_FirstUseEver);
        if (window.needsFocus)
        {
            ImGui::SetNextWindowFocus();
            window.needsFocus = false;
        }
        if (ImGui::Begin(window.windowId.c_str(), &window.isOpen,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
        {
            BlueprintNode* sourceData = findSourceDataById(window.nodeId);
            if (sourceData)
            {
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
                ImGui::InputText("搜索", window.searchBuffer, sizeof(window.searchBuffer));
                ImGui::Separator();
                if (ImGui::BeginChild("##FunctionScrollingRegion"))
                {
                    // 压栈时 captureStateToData 可能改动 InputDefaults，取值拷贝并经 map 写回，避免引用失效
                    std::string selectedFunction = sourceData->InputDefaults[window.pinName];
                    std::string_view search_sv(window.searchBuffer);
                    if (ImGui::Selectable("(无)", selectedFunction.empty()))
                    {
                        pushUndoSnapshot();
                        sourceData->InputDefaults[window.pinName].clear();
                        window.isOpen = false;
                    }
                    const auto& functions = m_currentBlueprint->GetBlueprintData().Functions;
                    for (const auto& func : functions)
                    {
                        if (search_sv.empty() || std::string_view(func.Name).find(search_sv) != std::string_view::npos)
                        {
                            if (ImGui::Selectable(func.Name.c_str(), selectedFunction == func.Name))
                            {
                                pushUndoSnapshot();
                                sourceData->InputDefaults[window.pinName] = func.Name;
                                window.isOpen = false;
                            }
                        }
                    }
                    ImGui::EndChild();
                }
            }
            else
            {
                ImGui::Text("错误: 找不到源节点数据。");
                if (ImGui::Button("关闭"))
                {
                    window.isOpen = false;
                }
            }
        }
        ImGui::End();
    }
}
void BlueprintPanel::drawRegions()
{
    ed::Suspend();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float headerHeight = 30.0f;
    for (const auto& region : m_regions)
    {
        if (region.ownerFunctionId != m_currentViewFunction) continue; // 只绘制当前图页的区域
        // 函数区域折叠为占位卡片：函数体节点已按归属隐藏，主图只保留一个可双击进入的入口
        const bool isFunctionCard = (region.functionId != 0);
        const ImVec2 displaySize = isFunctionCard
                                       ? ImVec2(kFunctionCardWidth, kFunctionCardHeight)
                                       : region.size;
        ImVec2 canvas_br = ImVec2(region.position.x + displaySize.x, region.position.y + displaySize.y);
        ImVec2 screen_tl = ed::CanvasToScreen(region.position);
        ImVec2 screen_br = ed::CanvasToScreen(canvas_br);
        ImVec2 screenSize = ImVec2(screen_br.x - screen_tl.x, screen_br.y - screen_tl.y);
        ImU32 headerColor = ImGui::GetColorU32(ImVec4(region.color.x, region.color.y, region.color.z,
                                                      region.color.w + 0.3f));
        ImU32 bodyColor = ImGui::GetColorU32(region.color);
        drawList->AddRectFilled(screen_tl, screen_br, bodyColor, 8.0f);
        drawList->AddRectFilled(screen_tl, ImVec2(screen_tl.x + screenSize.x, screen_tl.y + headerHeight), headerColor,
                                8.0f, ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight);
        ImVec2 textSize = ImGui::CalcTextSize(region.title.c_str());
        drawList->AddText(ImVec2(screen_tl.x + (screenSize.x - textSize.x) * 0.5f,
                                 screen_tl.y + (headerHeight - textSize.y) * 0.5f),
                          IM_COL32_WHITE, region.title.c_str());
        if (isFunctionCard)
        {
            // 描边模拟节点外观，正文提示进入方式
            drawList->AddRect(screen_tl, screen_br, IM_COL32(255, 255, 255, 90), 8.0f, 0, 1.5f);
            const char* hint = "双击编辑";
            ImVec2 hintSize = ImGui::CalcTextSize(hint);
            drawList->AddText(ImVec2(screen_tl.x + (screenSize.x - hintSize.x) * 0.5f,
                                     screen_tl.y + headerHeight + (screenSize.y - headerHeight - hintSize.y) * 0.5f),
                              IM_COL32(255, 255, 255, 170), hint);
        }
        else
        {
            ImVec2 resizeHandlePos = ImVec2(screen_br.x - 15, screen_br.y - 15);
            drawList->AddTriangleFilled(resizeHandlePos, ImVec2(resizeHandlePos.x + 15, resizeHandlePos.y),
                                        ImVec2(resizeHandlePos.x + 15, resizeHandlePos.y + 15),
                                        IM_COL32(255, 255, 255, 128));
        }
    }
    ed::Resume();
}
void BlueprintPanel::handleRegionInteraction()
{
    ed::Suspend();
    const float headerHeight = 30.0f;
    const float resizeHandleSize = 15.0f;
    const ImVec2 mousePos = ImGui::GetMousePos();
    const ImVec2 canvasMousePos = ed::ScreenToCanvas(mousePos);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_regionInteraction.type == ERegionInteractionType::None)
    {
        for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it)
        {
            BRegion& region = *it;
            if (region.ownerFunctionId != m_currentViewFunction) continue; // 不可见区域不参与交互
            const bool isFunctionCard = (region.functionId != 0);
            const ImVec2 displaySize = isFunctionCard
                                           ? ImVec2(kFunctionCardWidth, kFunctionCardHeight)
                                           : region.size;
            ImVec2 canvas_br = ImVec2(region.position.x + displaySize.x, region.position.y + displaySize.y);
            ImVec2 screen_tl = ed::CanvasToScreen(region.position);
            ImVec2 screen_br = ed::CanvasToScreen(canvas_br);
            ImRect headerRect(screen_tl, ImVec2(screen_br.x, screen_tl.y + headerHeight));
            ImRect resizeRect(ImVec2(screen_br.x - resizeHandleSize, screen_br.y - resizeHandleSize), screen_br);
            if (isFunctionCard)
            {
                // 占位卡片：双击任意位置进入子图；按住可拖动，函数体（隐藏）节点整体随动保持相对布局
                ImRect cardRect(screen_tl, screen_br);
                if (!cardRect.Contains(mousePos)) continue;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    requestViewSwitch(region.functionId);
                    break;
                }
                m_regionInteraction.type = ERegionInteractionType::Dragging;
                m_regionInteraction.activeRegion = &region;
                m_regionInteraction.startMousePos = canvasMousePos;
                m_regionInteraction.nodesToDrag.clear();
                for (auto& node : m_nodes)
                {
                    BlueprintNode* nodeSource = findSourceDataById(node.sourceDataID);
                    if (nodeSource && readNodeOwnerFunction(*nodeSource) == region.functionId)
                    {
                        m_regionInteraction.nodesToDrag.push_back(&node);
                    }
                }
                break;
            }
            if (resizeRect.Contains(mousePos))
            {
                m_regionInteraction.type = ERegionInteractionType::Resizing;
                m_regionInteraction.activeRegion = &region;
                m_regionInteraction.startMousePos = canvasMousePos;
                break;
            }
            if (headerRect.Contains(mousePos))
            {
                m_regionInteraction.type = ERegionInteractionType::Dragging;
                m_regionInteraction.activeRegion = &region;
                m_regionInteraction.startMousePos = canvasMousePos;
                m_regionInteraction.nodesToDrag.clear();
                ImRect regionCanvasRect(region.position, canvas_br);
                // 普通区域只带上本图页、落在区域矩形内的节点
                for (auto& node : m_nodes)
                {
                    if (!regionCanvasRect.Contains(node.position)) continue;
                    BlueprintNode* nodeSource = findSourceDataById(node.sourceDataID);
                    if (nodeSource && readNodeOwnerFunction(*nodeSource) != region.ownerFunctionId) continue;
                    m_regionInteraction.nodesToDrag.push_back(&node);
                }
                break;
            }
        }
    }
    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && m_regionInteraction.activeRegion)
    {
        ImVec2 delta = ImVec2(canvasMousePos.x - m_regionInteraction.startMousePos.x,
                              canvasMousePos.y - m_regionInteraction.startMousePos.y);
        if (m_regionInteraction.type == ERegionInteractionType::Dragging)
        {
            m_regionInteraction.activeRegion->position.x += delta.x;
            m_regionInteraction.activeRegion->position.y += delta.y;
            for (auto* node : m_regionInteraction.nodesToDrag)
            {
                node->position.x += delta.x;
                node->position.y += delta.y;
                ed::SetNodePosition(node->id, node->position);
            }
        }
        else if (m_regionInteraction.type == ERegionInteractionType::Resizing)
        {
            m_regionInteraction.activeRegion->size.x += delta.x;
            m_regionInteraction.activeRegion->size.y += delta.y;
            m_regionInteraction.activeRegion->size.x = std::max(100.0f, m_regionInteraction.activeRegion->size.x);
            m_regionInteraction.activeRegion->size.y = std::max(100.0f, m_regionInteraction.activeRegion->size.y);
        }
        m_regionInteraction.startMousePos = canvasMousePos;
    }
    else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_regionInteraction.type = ERegionInteractionType::None;
        m_regionInteraction.activeRegion = nullptr;
        m_regionInteraction.nodesToDrag.clear();
    }
    ed::Resume();
}
void BlueprintPanel::drawBackgroundContextMenu()
{
    ImVec2 open_position = ImGui::GetMousePosOnOpeningCurrentPopup();
    if (ImGui::BeginPopup("CreateNodeMenu"))
    {
        ImVec2 open_position_canvas = ed::ScreenToCanvas(open_position);
        const auto& categorizedNodes = BlueprintNodeRegistry::GetInstance().GetCategorizedDefinitions();
        static char searchBuffer[128] = "";
        if (ImGui::IsWindowAppearing())
        {
            searchBuffer[0] = '\0';
            ImGui::SetKeyboardFocusHere(0);
        }
        ImGui::InputText("搜索", searchBuffer, sizeof(searchBuffer));
        ImGui::Separator();
        std::string_view search_sv(searchBuffer);
        if (!search_sv.empty())
        {
            // 有搜索词时跳过分类层级，平铺显示所有匹配项（匹配名称或分类名）
            if (ImGui::BeginChild("##FlatSearchResults", ImVec2(420, 320)))
            {
                int rowId = 0;
                if (m_currentBlueprint)
                {
                    for (const auto& func : m_currentBlueprint->GetBlueprintData().Functions)
                    {
                        if (!containsIgnoreCase(func.Name, search_sv) &&
                            !containsIgnoreCase("函数调用", search_sv))
                        {
                            continue;
                        }
                        ImGui::PushID(rowId++);
                        if (drawFlatNodeMenuItem(func.Name.c_str(), "函数调用", true))
                        {
                            createFunctionCallNode(func, open_position_canvas);
                        }
                        ImGui::PopID();
                    }
                    for (const auto& var : m_currentBlueprint->GetBlueprintData().Variables)
                    {
                        std::string getLabel = "获取 " + var.Name;
                        std::string setLabel = "设置 " + var.Name;
                        if (containsIgnoreCase(getLabel, search_sv) || containsIgnoreCase("变量", search_sv))
                        {
                            ImGui::PushID(rowId++);
                            if (drawFlatNodeMenuItem(getLabel.c_str(), "变量", true))
                            {
                                createVariableNode(var, BlueprintNodeType::VariableGet, open_position_canvas);
                            }
                            ImGui::PopID();
                        }
                        if (containsIgnoreCase(setLabel, search_sv) || containsIgnoreCase("变量", search_sv))
                        {
                            ImGui::PushID(rowId++);
                            if (drawFlatNodeMenuItem(setLabel.c_str(), "变量", true))
                            {
                                createVariableNode(var, BlueprintNodeType::VariableSet, open_position_canvas);
                            }
                            ImGui::PopID();
                        }
                    }
                }
                for (const auto& [category, definitions] : categorizedNodes)
                {
                    for (const auto* def : definitions)
                    {
                        if (!containsIgnoreCase(def->DisplayName, search_sv) &&
                            !containsIgnoreCase(category, search_sv))
                        {
                            continue;
                        }
                        bool eventExists = (def->NodeType == BlueprintNodeType::Event && doesEventNodeExist(
                            def->FullName));
                        ImGui::PushID(rowId++);
                        if (drawFlatNodeMenuItem(def->DisplayName.c_str(), category.c_str(), !eventExists))
                        {
                            createNodeFromDefinition(def, open_position_canvas);
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
        else
        {
            if (m_currentBlueprint && !m_currentBlueprint->GetBlueprintData().Functions.empty())
            {
                if (ImGui::BeginMenu("函数调用"))
                {
                    for (const auto& func : m_currentBlueprint->GetBlueprintData().Functions)
                    {
                        if (ImGui::MenuItem(func.Name.c_str()))
                        {
                            createFunctionCallNode(func, open_position_canvas);
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
            }
            if (ImGui::BeginMenu("变量"))
            {
                if (m_currentBlueprint)
                {
                    auto& variables = m_currentBlueprint->GetBlueprintData().Variables;
                    if (variables.empty())
                    {
                        ImGui::MenuItem("(无可用变量)", nullptr, false, false);
                    }
                    else
                    {
                        if (ImGui::BeginMenu("获取"))
                        {
                            for (const auto& var : variables)
                            {
                                if (ImGui::MenuItem(var.Name.c_str()))
                                {
                                    createVariableNode(var, BlueprintNodeType::VariableGet,
                                                       ed::ScreenToCanvas(open_position));
                                }
                            }
                            ImGui::EndMenu();
                        }
                        if (ImGui::BeginMenu("设置"))
                        {
                            for (const auto& var : variables)
                            {
                                if (ImGui::MenuItem(var.Name.c_str()))
                                {
                                    createVariableNode(var, BlueprintNodeType::VariableSet,
                                                       ed::ScreenToCanvas(open_position));
                                }
                            }
                            ImGui::EndMenu();
                        }
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            for (const auto& [category, definitions] : categorizedNodes)
            {
                if (ImGui::BeginMenu(category.c_str()))
                {
                    for (const auto* def : definitions)
                    {
                        bool eventExists = (def->NodeType == BlueprintNodeType::Event && doesEventNodeExist(
                            def->FullName));
                        if (ImGui::MenuItem(def->DisplayName.c_str(), nullptr, false, !eventExists))
                        {
                            createNodeFromDefinition(def, ed::ScreenToCanvas(open_position));
                        }
                    }
                    ImGui::EndMenu();
                }
            }
        }
        ImGui::EndPopup();
    }
}
void BlueprintPanel::drawCreateNodeFromPinMenu()
{
    ImVec2 open_position = ImGui::GetMousePosOnOpeningCurrentPopup();
    if (ImGui::BeginPopup("CreateNodeFromPinMenu"))
    {
        BPin* startPin = findPinById(m_pendingLinkPin);
        if (!startPin || !m_currentBlueprint)
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImVec2 open_position_canvas = ed::ScreenToCanvas(open_position);
        ImGui::TextDisabled("从 %s 创建连接", startPin->name.empty() ? "执行" : startPin->name.c_str());
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere(0);
        }
        ImGui::InputText("搜索", m_pinMenuSearchBuffer, sizeof(m_pinMenuSearchBuffer));
        ImGui::Separator();
        std::string_view search_sv(m_pinMenuSearchBuffer);
        const ed::PinId startPinId = m_pendingLinkPin;
        // 只做方向与类型的兼容判断，引脚占用等约束由 canCreateLink 在真正连线时兜底
        auto pinCompatible = [startPin](const std::string& candidateType, ed::PinKind candidateKind)
        {
            if (candidateKind == startPin->kind) return false;
            const bool startIsOutput = (startPin->kind == ed::PinKind::Output);
            const std::string& outType = startIsOutput ? startPin->type : candidateType;
            const std::string& inType = startIsOutput ? candidateType : startPin->type;
            return arePinTypesCompatible(outType, inType);
        };
        bool created = false;
        if (ImGui::BeginChild("##PinMenuResults", ImVec2(420, 320)))
        {
            int rowId = 0;
            for (const auto& func : m_currentBlueprint->GetBlueprintData().Functions)
            {
                bool compatible = pinCompatible("Exec", ed::PinKind::Input) ||
                    pinCompatible("Exec", ed::PinKind::Output);
                if (!compatible)
                {
                    for (const auto& param : func.Parameters)
                    {
                        if (pinCompatible(param.Type, ed::PinKind::Input))
                        {
                            compatible = true;
                            break;
                        }
                    }
                }
                if (!compatible && func.ReturnType != "void")
                {
                    compatible = pinCompatible(func.ReturnType, ed::PinKind::Output);
                }
                if (!compatible) continue;
                if (!search_sv.empty() && !containsIgnoreCase(func.Name, search_sv) &&
                    !containsIgnoreCase("函数调用", search_sv))
                {
                    continue;
                }
                ImGui::PushID(rowId++);
                if (drawFlatNodeMenuItem(func.Name.c_str(), "函数调用", true))
                {
                    createFunctionCallNode(func, open_position_canvas);
                    created = true;
                }
                ImGui::PopID();
                if (created) break;
            }
            if (!created)
            {
                for (const auto& var : m_currentBlueprint->GetBlueprintData().Variables)
                {
                    std::string getLabel = "获取 " + var.Name;
                    std::string setLabel = "设置 " + var.Name;
                    bool getCompatible = pinCompatible(var.Type, ed::PinKind::Output);
                    bool setCompatible = pinCompatible("Exec", ed::PinKind::Input) ||
                        pinCompatible(var.Type, ed::PinKind::Input) ||
                        pinCompatible("Exec", ed::PinKind::Output);
                    bool searchOk = search_sv.empty() || containsIgnoreCase("变量", search_sv);
                    if (getCompatible && (searchOk || containsIgnoreCase(getLabel, search_sv)))
                    {
                        ImGui::PushID(rowId++);
                        if (drawFlatNodeMenuItem(getLabel.c_str(), "变量", true))
                        {
                            createVariableNode(var, BlueprintNodeType::VariableGet, open_position_canvas);
                            created = true;
                        }
                        ImGui::PopID();
                        if (created) break;
                    }
                    if (setCompatible && (searchOk || containsIgnoreCase(setLabel, search_sv)))
                    {
                        ImGui::PushID(rowId++);
                        if (drawFlatNodeMenuItem(setLabel.c_str(), "变量", true))
                        {
                            createVariableNode(var, BlueprintNodeType::VariableSet, open_position_canvas);
                            created = true;
                        }
                        ImGui::PopID();
                        if (created) break;
                    }
                }
            }
            if (!created)
            {
                const auto& categorizedNodes = BlueprintNodeRegistry::GetInstance().GetCategorizedDefinitions();
                for (const auto& [category, definitions] : categorizedNodes)
                {
                    for (const auto* def : definitions)
                    {
                        if (!nodeDefinitionHasCompatiblePin(def, startPin)) continue;
                        if (!search_sv.empty() && !containsIgnoreCase(def->DisplayName, search_sv) &&
                            !containsIgnoreCase(category, search_sv))
                        {
                            continue;
                        }
                        bool eventExists = (def->NodeType == BlueprintNodeType::Event && doesEventNodeExist(
                            def->FullName));
                        ImGui::PushID(rowId++);
                        if (drawFlatNodeMenuItem(def->DisplayName.c_str(), category.c_str(), !eventExists))
                        {
                            createNodeFromDefinition(def, open_position_canvas);
                            created = true;
                        }
                        ImGui::PopID();
                        if (created) break;
                    }
                    if (created) break;
                }
            }
        }
        ImGui::EndChild();
        if (created)
        {
            // 创建节点会使 m_nodes 扩容，重新查找起点引脚后再自动连线
            BPin* freshStartPin = findPinById(startPinId);
            if (freshStartPin && !m_nodes.empty())
            {
                connectPinToFirstCompatiblePin(freshStartPin, m_nodes.back());
            }
            m_pendingLinkPin = ed::PinId(0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    else
    {
        m_pendingLinkPin = ed::PinId(0);
    }
}
void BlueprintPanel::drawVariableDropMenu()
{
    if (ImGui::BeginPopup("VariableDropMenu"))
    {
        const BlueprintVariable* variable = nullptr;
        if (m_currentBlueprint)
        {
            const auto& variables = m_currentBlueprint->GetBlueprintData().Variables;
            auto it = std::find_if(variables.begin(), variables.end(),
                                   [this](const BlueprintVariable& var) { return var.Name == m_varDropName; });
            if (it != variables.end())
            {
                variable = &(*it);
            }
        }
        if (!variable)
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            if (ImGui::MenuItem(("获取 " + variable->Name).c_str()))
            {
                createVariableNode(*variable, BlueprintNodeType::VariableGet, m_varDropCanvasPos);
            }
            if (ImGui::MenuItem(("设置 " + variable->Name).c_str()))
            {
                createVariableNode(*variable, BlueprintNodeType::VariableSet, m_varDropCanvasPos);
            }
        }
        ImGui::EndPopup();
    }
}
void BlueprintPanel::drawNodeContextMenu()
{
    if (ImGui::BeginPopup("NodeContextMenu"))
    {
        const size_t selectedCount = collectSelectedViewNodes().size();
        if (selectedCount >= 2)
        {
            if (ImGui::MenuItem("左对齐")) alignSelectedNodes(NodeAlignMode::Left);
            if (ImGui::MenuItem("右对齐")) alignSelectedNodes(NodeAlignMode::Right);
            if (ImGui::MenuItem("顶对齐")) alignSelectedNodes(NodeAlignMode::Top);
            if (ImGui::MenuItem("底对齐")) alignSelectedNodes(NodeAlignMode::Bottom);
            ImGui::Separator();
            // 均布需要至少 3 个节点才有中间项可挪
            if (ImGui::MenuItem("横向均布", nullptr, false, selectedCount >= 3)) distributeSelectedNodes(true);
            if (ImGui::MenuItem("纵向均布", nullptr, false, selectedCount >= 3)) distributeSelectedNodes(false);
            ImGui::Separator();
        }
        if (ImGui::MenuItem(selectedCount >= 2 ? "整理选中节点" : "整理节点（全图）"))
        {
            arrangeNodes();
        }
        if (m_contextNodeId)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("删除节点"))
            {
                deleteNode(m_contextNodeId);
            }
        }
        ImGui::EndPopup();
    }
}
void BlueprintPanel::drawLinkContextMenu()
{
    if (ImGui::BeginPopup("LinkContextMenu"))
    {
        if (m_contextLinkId)
        {
            if (ImGui::MenuItem("删除连接"))
            {
                deleteLink(m_contextLinkId);
            }
        }
        ImGui::EndPopup();
    }
}
void BlueprintPanel::drawRegionContextMenu()
{
    if (ImGui::BeginPopup("RegionContextMenu"))
    {
        auto regionIt = std::find_if(m_regions.begin(), m_regions.end(),
                                     [&](const BRegion& region) { return region.id == m_contextRegionId; });
        const bool isFunctionCard = regionIt != m_regions.end() && regionIt->functionId != 0;
        if (isFunctionCard)
        {
            // 占位卡片是函数在主图的入口，不提供单独删除；删除函数走函数面板（含节点归属级联处理）
            if (ImGui::MenuItem("编辑函数图"))
            {
                requestViewSwitch(regionIt->functionId);
            }
        }
        else if (ImGui::MenuItem("删除区域"))
        {
            pushUndoSnapshot();
            std::erase_if(m_regions, [&](const BRegion& region) { return region.id == m_contextRegionId; });
            std::erase_if(m_currentBlueprint->GetBlueprintData().CommentRegions,
                          [&](const BlueprintCommentRegion& region)
                          {
                              return region.ID == m_contextRegionId;
                          });
        }
        ImGui::EndPopup();
    }
}
void BlueprintPanel::updateInputStringWindows()
{
    for (auto& window : m_inputStringWindows)
    {
        if (!window.isOpen) continue;
        BNode* node = nullptr;
        for (auto& n : m_nodes)
        {
            if (n.sourceDataID == window.nodeId)
            {
                node = &n;
                break;
            }
        }
        if (node)
        {
            ed::SetCurrentEditor(m_nodeEditorContext);
            ImVec2 nodeScreenPos = ed::CanvasToScreen(node->position);
            window.position = ImVec2(nodeScreenPos.x + 200, nodeScreenPos.y);
        }
        else
        {
            window.isOpen = false;
        }
    }
    m_inputStringWindows.erase(
        std::remove_if(m_inputStringWindows.begin(), m_inputStringWindows.end(),
                       [](const InputStringWindow& w) { return !w.isOpen; }),
        m_inputStringWindows.end());
}
void BlueprintPanel::drawInputStringWindows()
{
    for (auto& window : m_inputStringWindows)
    {
        if (!window.isOpen) continue;
        ImGui::SetNextWindowPos(window.position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(window.size, ImGuiCond_FirstUseEver);
        if (window.needsFocus)
        {
            ImGui::SetNextWindowFocus();
            window.needsFocus = false;
        }
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin(window.windowId.c_str(), &window.isOpen, flags))
        {
            BlueprintNode* sourceData = findSourceDataById(window.nodeId);
            if (sourceData)
            {
                std::string& value = sourceData->InputDefaults[window.pinName];
                static char buffer[4096];
                strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
                buffer[sizeof(buffer) - 1] = '\0';
                ImGui::Text("编辑 %s:", window.pinName.c_str());
                ImGui::Separator();
                if (ImGui::InputTextMultiline("##input", buffer, sizeof(buffer),
                                              ImVec2(280, 150),
                                              ImGuiInputTextFlags_AllowTabInput))
                {
                    value = buffer;
                }
                trackItemEditUndo();
                ImGui::Separator();
                if (ImGui::Button("完成"))
                {
                    window.isOpen = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("清空"))
                {
                    pushUndoSnapshot();
                    // 压栈时 captureStateToData 可能改动 InputDefaults，经 map 重新取值再清空
                    sourceData->InputDefaults[window.pinName].clear();
                    buffer[0] = '\0';
                }
            }
            else
            {
                ImGui::Text("错误: 找不到对应的节点数据");
                if (ImGui::Button("关闭"))
                {
                    window.isOpen = false;
                }
            }
        }
        ImGui::End();
    }
}
BlueprintPanel::InputStringWindow* BlueprintPanel::findInputStringWindow(uint32_t nodeId, const std::string& pinName)
{
    for (auto& window : m_inputStringWindows)
    {
        if (window.nodeId == nodeId && window.pinName == pinName)
        {
            return &window;
        }
    }
    return nullptr;
}
void BlueprintPanel::initializeFromBlueprintData()
{
    ClearEditorState();
    auto& blueprintData = m_currentBlueprint->GetBlueprintData();
    std::unordered_map<uint32_t, ed::NodeId> bpNodeIdToEditorNodeId;
    std::unordered_map<std::pair<uint32_t, std::string>, ed::PinId, PairHash> bpPinToEditorPinId;
    for (auto& bpNodeData : blueprintData.Nodes)
    {
        BNode node;
        node.id = ed::NodeId(bpNodeData.ID);
        m_nextNodeId = std::max(m_nextNodeId, bpNodeData.ID + 1);
        node.sourceDataID = bpNodeData.ID;
        node.position = {bpNodeData.Position.x, bpNodeData.Position.y};
        const auto* definition = BlueprintNodeRegistry::GetInstance().GetDefinition(
            bpNodeData.TargetClassFullName + "." + bpNodeData.TargetMemberName);
        if (!definition)
        {
            if (bpNodeData.Type == BlueprintNodeType::VariableGet)
            {
                node.name = "获取 " + bpNodeData.VariableName;
                std::string varType = "System.Object";
                const auto& allVars = blueprintData.Variables;
                auto it = std::find_if(allVars.begin(), allVars.end(),
                                       [&](const BlueprintVariable& var)
                                       {
                                           return var.Name == bpNodeData.VariableName;
                                       });
                if (it != allVars.end())
                {
                    varType = it->Type;
                }
                BPin pin = {getNextPinId(), node.id, "值", varType, ed::PinKind::Output};
                node.outputPins.push_back(pin);
                bpPinToEditorPinId[{bpNodeData.ID, pin.name}] = pin.id;
            }
            else if (bpNodeData.Type == BlueprintNodeType::VariableSet)
            {
                node.name = "设置 " + bpNodeData.VariableName;
                std::string varType = "System.Object";
                const auto& allVars = blueprintData.Variables;
                auto it = std::find_if(allVars.begin(), allVars.end(),
                                       [&](const BlueprintVariable& var)
                                       {
                                           return var.Name == bpNodeData.VariableName;
                                       });
                if (it != allVars.end())
                {
                    varType = it->Type;
                }
                BPin execInPin = {getNextPinId(), node.id, "", "Exec", ed::PinKind::Input};
                BPin valueInPin = {getNextPinId(), node.id, "值", varType, ed::PinKind::Input};
                BPin thenOutPin = {getNextPinId(), node.id, "然后", "Exec", ed::PinKind::Output};
                node.inputPins.push_back(execInPin);
                node.inputPins.push_back(valueInPin);
                node.outputPins.push_back(thenOutPin);
                bpPinToEditorPinId[{bpNodeData.ID, execInPin.name}] = execInPin.id;
                bpPinToEditorPinId[{bpNodeData.ID, valueInPin.name}] = valueInPin.id;
                bpPinToEditorPinId[{bpNodeData.ID, thenOutPin.name}] = thenOutPin.id;
            }
            else if (bpNodeData.Type == BlueprintNodeType::FunctionEntry || bpNodeData.Type ==
                BlueprintNodeType::FunctionCall)
            {
                const auto& funcs = blueprintData.Functions;
                auto it = std::find_if(funcs.begin(), funcs.end(), [&](const BlueprintFunction& func)
                {
                    return func.Name == bpNodeData.TargetMemberName;
                });
                if (it != funcs.end())
                {
                    const auto& func = *it;
                    node.name = func.Name;
                    if (bpNodeData.Type == BlueprintNodeType::FunctionEntry)
                    {
                        BPin thenPin = {getNextPinId(), node.id, "然后", "Exec", ed::PinKind::Output};
                        node.outputPins.push_back(thenPin);
                        bpPinToEditorPinId[{bpNodeData.ID, thenPin.name}] = thenPin.id;
                        for (const auto& param : func.Parameters)
                        {
                            BPin paramPin = {getNextPinId(), node.id, param.Name, param.Type, ed::PinKind::Output};
                            node.outputPins.push_back(paramPin);
                            bpPinToEditorPinId[{bpNodeData.ID, paramPin.name}] = paramPin.id;
                        }
                    }
                    else
                    {
                        BPin execInPin = {getNextPinId(), node.id, "", "Exec", ed::PinKind::Input};
                        node.inputPins.push_back(execInPin);
                        bpPinToEditorPinId[{bpNodeData.ID, execInPin.name}] = execInPin.id;
                        for (const auto& param : func.Parameters)
                        {
                            BPin paramPin = {getNextPinId(), node.id, param.Name, param.Type, ed::PinKind::Input};
                            node.inputPins.push_back(paramPin);
                            bpPinToEditorPinId[{bpNodeData.ID, paramPin.name}] = paramPin.id;
                        }
                        BPin thenPin = {getNextPinId(), node.id, "然后", "Exec", ed::PinKind::Output};
                        node.outputPins.push_back(thenPin);
                        bpPinToEditorPinId[{bpNodeData.ID, thenPin.name}] = thenPin.id;
                        if (func.ReturnType != "void")
                        {
                            BPin returnPin = {
                                getNextPinId(), node.id, "返回值", func.ReturnType, ed::PinKind::Output
                            };
                            node.outputPins.push_back(returnPin);
                            bpPinToEditorPinId[{bpNodeData.ID, returnPin.name}] = returnPin.id;
                        }
                    }
                }
                else
                {
                    node.name = "未知函数: " + bpNodeData.TargetMemberName;
                }
            }
            else continue;
        }
        else
        {
            node.name = definition->DisplayName;
            for (const auto& pinDef : definition->InputPins)
            {
                BPin pin = {getNextPinId(), node.id, pinDef.Name, pinDef.Type, ed::PinKind::Input};
                node.inputPins.push_back(pin);
                bpPinToEditorPinId[{bpNodeData.ID, pin.name}] = pin.id;
            }
            for (const auto& pinDef : definition->OutputPins)
            {
                BPin pin = {getNextPinId(), node.id, pinDef.Name, pinDef.Type, ed::PinKind::Output};
                node.outputPins.push_back(pin);
                bpPinToEditorPinId[{bpNodeData.ID, pin.name}] = pin.id;
            }
            if (definition->FullName == "Utility.GetSelf")
            {
                std::string selfType = "GameScripts." + m_currentBlueprint->GetBlueprintData().Name;
                for (auto& pin : node.outputPins)
                {
                    if (pin.name == "自身")
                    {
                        pin.type = selfType;
                    }
                }
            }
        }
        auto insert_it = std::find_if(node.inputPins.begin(), node.inputPins.end(), [](const BPin& pin)
        {
            return pin.type == "Args";
        });
        if (insert_it != node.inputPins.end())
        {
            if (bpNodeData.InputDefaults.count("_DynamicArgsCount"))
            {
                try
                {
                    int count = std::stoi(bpNodeData.InputDefaults.at("_DynamicArgsCount"));
                    std::vector<BPin> dynamic_pins_to_add;
                    for (int i = 0; i < count; ++i)
                    {
                        BPin dynamicPin;
                        dynamicPin.id = getNextPinId();
                        dynamicPin.nodeId = node.id;
                        dynamicPin.name = bpNodeData.InputDefaults.at("_DynamicArg_" + std::to_string(i) + "_Name");
                        dynamicPin.type = bpNodeData.InputDefaults.at("_DynamicArg_" + std::to_string(i) + "_Type");
                        dynamicPin.kind = ed::PinKind::Input;
                        dynamic_pins_to_add.push_back(dynamicPin);
                        // 动态引脚也要注册进映射，否则其连线在重建时会丢失
                        bpPinToEditorPinId[{bpNodeData.ID, dynamicPin.name}] = dynamicPin.id;
                    }
                    if (!dynamic_pins_to_add.empty())
                    {
                        node.inputPins.insert(insert_it, dynamic_pins_to_add.begin(), dynamic_pins_to_add.end());
                    }
                }
                catch (const std::exception& e)
                {
                    LogWarn("为节点 {} 加载动态参数失败: {}", bpNodeData.ID, e.what());
                }
            }
        }
        m_nodes.push_back(node);
        bpNodeIdToEditorNodeId[bpNodeData.ID] = node.id;
    }
    for (const auto& bpLink : blueprintData.Links)
    {
        auto startIt = bpPinToEditorPinId.find({bpLink.FromNodeID, bpLink.FromPinName});
        auto endIt = bpPinToEditorPinId.find({bpLink.ToNodeID, bpLink.ToPinName});
        if (startIt != bpPinToEditorPinId.end() && endIt != bpPinToEditorPinId.end())
        {
            m_links.push_back({getNextLinkId(), startIt->second, endIt->second});
        }
    }
    rebuildPinConnections();
    ed::SetCurrentEditor(m_nodeEditorContext);
    for (const auto& node : m_nodes)
    {
        ed::SetNodePosition(node.id, node.position);
    }
    m_nextFunctionId = 1;
    for (const auto& func : blueprintData.Functions)
    {
        m_nextFunctionId = std::max(m_nextFunctionId, func.ID + 1);
    }
    m_nextRegionId = 1;
    for (const auto& regionData : blueprintData.CommentRegions)
    {
        BRegion region;
        region.id = regionData.ID;
        region.title = regionData.Title;
        region.position = {regionData.Position.x, regionData.Position.y};
        region.size = {regionData.Size.w, regionData.Size.h};
        region.functionId = regionData.FunctionID;
        region.ownerFunctionId = regionData.OwnerFunctionID;
        ImGuiID hash = ImHashStr(region.title.c_str(), 0, 0);
        region.color = ImPlot::GetColormapColor(((hash & 0xFF)) % ImPlot::GetColormapSize(ImPlotColormap_Deep),
                                                ImPlotColormap_Deep);
        region.color.w = 0.4f;
        m_regions.push_back(region);
        m_nextRegionId = std::max(m_nextRegionId, region.id + 1);
    }
    // 区域构建完成后再迁移归属（旧文件按区域几何包含推断），并校验当前视图仍有效
    migrateNodeOwnership();
}
void BlueprintPanel::captureStateToData()
{
    if (!m_currentBlueprint) return;
    auto& blueprintData = m_currentBlueprint->GetBlueprintData();
    for (const auto& node : m_nodes)
    {
        BlueprintNode* sourceData = findSourceDataById(node.sourceDataID);
        if (sourceData)
        {
            sourceData->Position.x = node.position.x;
            sourceData->Position.y = node.position.y;
            std::vector<std::string> keysToRemove;
            for (const auto& pair : sourceData->InputDefaults)
            {
                if (pair.first.rfind("_dyn_element_", 0) == 0 || pair.first == "_DynamicArgsCount")
                {
                    keysToRemove.push_back(pair.first);
                }
            }
            for (const auto& key : keysToRemove)
            {
                sourceData->InputDefaults.erase(key);
            }
            int dynamicArgCount = 0;
            for (const auto& pin : node.inputPins)
            {
                if (pin.name.rfind("_dyn_element_", 0) == 0)
                {
                    std::string baseKey = "_DynamicArg_" + std::to_string(dynamicArgCount);
                    sourceData->InputDefaults[baseKey + "_Name"] = pin.name;
                    sourceData->InputDefaults[baseKey + "_Type"] = pin.type;
                    dynamicArgCount++;
                }
            }
            if (dynamicArgCount > 0)
            {
                sourceData->InputDefaults["_DynamicArgsCount"] = std::to_string(dynamicArgCount);
            }
        }
    }
    blueprintData.Links.clear();
    std::unordered_map<ed::PinId, std::pair<uint32_t, std::string>> editorPinToBpPin;
    for (const auto& node : m_nodes)
    {
        BlueprintNode* sourceData = findSourceDataById(node.sourceDataID);
        if (!sourceData) continue;
        for (const auto& pin : node.inputPins) { editorPinToBpPin[pin.id] = {sourceData->ID, pin.name}; }
        for (const auto& pin : node.outputPins) { editorPinToBpPin[pin.id] = {sourceData->ID, pin.name}; }
    }
    for (const auto& link : m_links)
    {
        auto startIt = editorPinToBpPin.find(link.startPinId);
        auto endIt = editorPinToBpPin.find(link.endPinId);
        if (startIt != editorPinToBpPin.end() && endIt != editorPinToBpPin.end())
        {
            blueprintData.Links.push_back({
                startIt->second.first, startIt->second.second,
                endIt->second.first, endIt->second.second
            });
        }
    }
    blueprintData.CommentRegions.clear();
    for (const auto& region : m_regions)
    {
        BlueprintCommentRegion regionData;
        regionData.ID = region.id;
        regionData.Title = region.title;
        regionData.FunctionID = region.functionId;
        regionData.OwnerFunctionID = region.ownerFunctionId;
        regionData.Position = {region.position.x, region.position.y};
        regionData.Size = {region.size.x, region.size.y};
        blueprintData.CommentRegions.push_back(regionData);
    }
}
void BlueprintPanel::saveToBlueprintData()
{
    if (!m_currentBlueprint) return;
    captureStateToData();
    auto meta = AssetManager::GetInstance().GetMetadata(m_currentBlueprintGuid);
    auto filePath = AssetManager::GetInstance().GetAssetsRootPath() / meta->assetPath;
    std::string content = YAML::Dump(YAML::convert<Blueprint>::encode(m_currentBlueprint->GetBlueprintData()));
    Path::WriteFile(filePath.string(), content);
    LogInfo("蓝图数据已保存: {}", filePath.string());
}
void BlueprintPanel::createVariableNode(const BlueprintVariable& variable, BlueprintNodeType type, ImVec2 position)
{
    if (!m_currentBlueprint) return;
    pushUndoSnapshot();
    BlueprintNode bpNode;
    bpNode.ID = getNextNodeId();
    bpNode.Type = type;
    bpNode.VariableName = variable.Name;
    bpNode.Position = {position.x, position.y};
    writeNodeOwnerFunction(bpNode, m_currentViewFunction); // 新节点归属当前视图域
    m_currentBlueprint->GetBlueprintData().Nodes.push_back(bpNode);
    BNode editorNode;
    editorNode.id = ed::NodeId(bpNode.ID);
    editorNode.sourceDataID = bpNode.ID;
    editorNode.position = position;
    if (type == BlueprintNodeType::VariableGet)
    {
        editorNode.name = "获取 " + variable.Name;
        editorNode.outputPins.push_back({getNextPinId(), editorNode.id, "值", variable.Type, ed::PinKind::Output});
    }
    else if (type == BlueprintNodeType::VariableSet)
    {
        editorNode.name = "设置 " + variable.Name;
        editorNode.inputPins.push_back({getNextPinId(), editorNode.id, "", "Exec", ed::PinKind::Input});
        editorNode.inputPins.push_back({getNextPinId(), editorNode.id, "值", variable.Type, ed::PinKind::Input});
        editorNode.outputPins.push_back({getNextPinId(), editorNode.id, "然后", "Exec", ed::PinKind::Output});
    }
    m_nodes.push_back(editorNode);
    ed::SetNodePosition(editorNode.id, position);
}
void BlueprintPanel::createNodeFromDefinition(const BlueprintNodeDefinition* definition, ImVec2 position)
{
    if (!m_currentBlueprint) return;
    if (definition->NodeType == BlueprintNodeType::Event)
    {
        if (doesEventNodeExist(definition->FullName))
        {
            LogWarn("无法创建事件节点 '{}'，因为它已存在于蓝图中。", definition->DisplayName);
            return;
        }
    }
    pushUndoSnapshot();
    BlueprintNode bpNode;
    bpNode.ID = getNextNodeId();
    bpNode.Type = definition->NodeType;
    bpNode.Position = {position.x, position.y};
    std::string_view fullName(definition->FullName);
    size_t lastDot = fullName.find_last_of('.');
    if (lastDot != std::string_view::npos)
    {
        bpNode.TargetClassFullName = std::string(fullName.substr(0, lastDot));
        bpNode.TargetMemberName = std::string(fullName.substr(lastDot + 1));
    }
    writeNodeOwnerFunction(bpNode, m_currentViewFunction); // 新节点归属当前视图域
    m_currentBlueprint->GetBlueprintData().Nodes.push_back(bpNode);
    BNode editorNode;
    editorNode.id = ed::NodeId(bpNode.ID);
    editorNode.sourceDataID = bpNode.ID;
    editorNode.name = definition->DisplayName;
    editorNode.position = position;
    for (const auto& pinDef : definition->InputPins)
    {
        editorNode.inputPins.push_back({getNextPinId(), editorNode.id, pinDef.Name, pinDef.Type, ed::PinKind::Input});
    }
    for (const auto& pinDef : definition->OutputPins)
    {
        editorNode.outputPins.push_back({getNextPinId(), editorNode.id, pinDef.Name, pinDef.Type, ed::PinKind::Output});
    }
    if (definition->FullName == "Utility.GetSelf")
    {
        std::string selfType = "GameScripts." + m_currentBlueprint->GetBlueprintData().Name;
        for (auto& pin : editorNode.outputPins)
        {
            if (pin.name == "自身")
            {
                pin.type = selfType;
            }
        }
    }
    m_nodes.push_back(editorNode);
    ed::SetNodePosition(editorNode.id, position);
}
void BlueprintPanel::createFunctionCallNode(const BlueprintFunction& func, ImVec2 position)
{
    if (!m_currentBlueprint) return;
    pushUndoSnapshot();
    BlueprintNode bpNode;
    bpNode.ID = getNextNodeId();
    bpNode.Type = BlueprintNodeType::FunctionCall;
    bpNode.TargetMemberName = func.Name;
    bpNode.Position = {position.x, position.y};
    writeNodeOwnerFunction(bpNode, m_currentViewFunction); // 新节点归属当前视图域
    m_currentBlueprint->GetBlueprintData().Nodes.push_back(bpNode);
    BNode editorNode;
    editorNode.id = ed::NodeId(bpNode.ID);
    editorNode.sourceDataID = bpNode.ID;
    editorNode.name = func.Name;
    editorNode.position = position;
    editorNode.inputPins.push_back({getNextPinId(), editorNode.id, "", "Exec", ed::PinKind::Input});
    for (const auto& param : func.Parameters)
    {
        editorNode.inputPins.push_back({getNextPinId(), editorNode.id, param.Name, param.Type, ed::PinKind::Input});
    }
    editorNode.outputPins.push_back({getNextPinId(), editorNode.id, "然后", "Exec", ed::PinKind::Output});
    if (func.ReturnType != "void")
    {
        editorNode.outputPins.push_back({
            getNextPinId(), editorNode.id, "返回值", func.ReturnType, ed::PinKind::Output
        });
    }
    m_nodes.push_back(editorNode);
    ed::SetNodePosition(editorNode.id, position);
}
void BlueprintPanel::deleteNode(ed::NodeId nodeId, bool allowProtected)
{
    BNode* nodeToDelete = findNodeById(nodeId);
    if (!nodeToDelete) return;
    if (!allowProtected)
    {
        const BlueprintNode* protectedCheck = findSourceDataById(nodeToDelete->sourceDataID);
        if (protectedCheck && protectedCheck->Type == BlueprintNodeType::FunctionEntry)
        {
            // 函数入口节点只能随函数一起删除
            return;
        }
    }
    pushUndoSnapshot();
    std::vector<ed::LinkId> linksToDelete;
    for (const auto& link : m_links)
    {
        bool isConnected = false;
        for (const auto& pin : nodeToDelete->inputPins)
        {
            if (link.endPinId == pin.id)
            {
                isConnected = true;
                break;
            }
        }
        if (isConnected)
        {
            linksToDelete.push_back(link.id);
            continue;
        }
        for (const auto& pin : nodeToDelete->outputPins)
        {
            if (link.startPinId == pin.id)
            {
                linksToDelete.push_back(link.id);
                break;
            }
        }
    }
    for (ed::LinkId linkId : linksToDelete)
    {
        deleteLink(linkId);
    }
    uint32_t sourceIdToDelete = nodeToDelete->sourceDataID;
    auto& bpNodes = m_currentBlueprint->GetBlueprintData().Nodes;
    std::erase_if(bpNodes, [sourceIdToDelete](const BlueprintNode& bpNode)
    {
        return bpNode.ID == sourceIdToDelete;
    });
    std::erase_if(m_nodes, [nodeId](const BNode& node)
    {
        return node.id == nodeId;
    });
}
void BlueprintPanel::deleteLink(ed::LinkId linkId)
{
    BLink* linkToDelete = findLinkById(linkId);
    if (!linkToDelete) return;
    pushUndoSnapshot();
    BPin* startPin = findPinById(linkToDelete->startPinId);
    BPin* endPin = findPinById(linkToDelete->endPinId);
    std::erase_if(m_links, [linkId](const BLink& link)
    {
        return link.id == linkId;
    });
    if (startPin)
    {
        bool stillConnected = false;
        for (const auto& link : m_links)
        {
            if (link.startPinId == startPin->id)
            {
                stillConnected = true;
                break;
            }
        }
        startPin->isConnected = stillConnected;
    }
    if (endPin)
    {
        bool stillConnected = false;
        for (const auto& link : m_links)
        {
            if (link.endPinId == endPin->id)
            {
                stillConnected = true;
                break;
            }
        }
        endPin->isConnected = stillConnected;
    }
}
bool BlueprintPanel::canCreateLink(const BPin* startPin, const BPin* endPin) const
{
    if (!startPin || !endPin || startPin == endPin) return false;
    if (startPin->nodeId == endPin->nodeId) return false;
    if (startPin->kind == endPin->kind) return false;
    // 主图与函数子图之间不允许连线，函数边界只能通过 Entry/Return 表达
    if (getPinOwnerFunction(startPin) != getPinOwnerFunction(endPin)) return false;
    const BPin* pOut = (startPin->kind == ed::PinKind::Output) ? startPin : endPin;
    const BPin* pIn = (startPin->kind == ed::PinKind::Output) ? endPin : startPin;
    if (pIn->isConnected) return false;
    return arePinTypesCompatible(pOut->type, pIn->type);
}
bool BlueprintPanel::nodeDefinitionHasCompatiblePin(const BlueprintNodeDefinition* definition,
                                                    const BPin* startPin) const
{
    const bool startIsOutput = (startPin->kind == ed::PinKind::Output);
    const auto& candidatePins = startIsOutput ? definition->InputPins : definition->OutputPins;
    for (const auto& pinDef : candidatePins)
    {
        if (!isConnectablePinType(pinDef.Type)) continue;
        const std::string& outType = startIsOutput ? startPin->type : pinDef.Type;
        const std::string& inType = startIsOutput ? pinDef.Type : startPin->type;
        if (arePinTypesCompatible(outType, inType)) return true;
    }
    return false;
}
void BlueprintPanel::connectPinToFirstCompatiblePin(BPin* startPin, BNode& newNode)
{
    auto& candidatePins = (startPin->kind == ed::PinKind::Output) ? newNode.inputPins : newNode.outputPins;
    for (auto& pin : candidatePins)
    {
        if (!isConnectablePinType(pin.type)) continue;
        if (!canCreateLink(startPin, &pin)) continue;
        BLink newLink{getNextLinkId(), startPin->id, pin.id};
        if (startPin->kind == ed::PinKind::Input)
        {
            std::swap(newLink.startPinId, newLink.endPinId);
        }
        m_links.push_back(newLink);
        rebuildPinConnections();
        return;
    }
}
void BlueprintPanel::rebuildPinConnections()
{
    for (auto& node : m_nodes)
    {
        for (auto& pin : node.inputPins) pin.isConnected = false;
        for (auto& pin : node.outputPins) pin.isConnected = false;
    }
    for (const auto& link : m_links)
    {
        BPin* startPin = findPinById(link.startPinId);
        BPin* endPin = findPinById(link.endPinId);
        if (startPin) startPin->isConnected = true;
        if (endPin) endPin->isConnected = true;
    }
}
BlueprintPanel::BNode* BlueprintPanel::findNodeById(ed::NodeId nodeId)
{
    for (auto& node : m_nodes)
    {
        if (node.id == nodeId)
            return &node;
    }
    return nullptr;
}
BlueprintPanel::BPin* BlueprintPanel::findPinById(ed::PinId pinId)
{
    if (!pinId) return nullptr;
    for (auto& node : m_nodes)
    {
        for (auto& pin : node.inputPins)
        {
            if (pin.id == pinId)
                return &pin;
        }
        for (auto& pin : node.outputPins)
        {
            if (pin.id == pinId)
                return &pin;
        }
    }
    return nullptr;
}
BlueprintPanel::BLink* BlueprintPanel::findLinkById(ed::LinkId linkId)
{
    for (auto& link : m_links)
    {
        if (link.id == linkId)
            return &link;
    }
    return nullptr;
}
BlueprintNode* BlueprintPanel::findSourceDataById(uint32_t id)
{
    if (!m_currentBlueprint) return nullptr;
    auto& bpNodes = m_currentBlueprint->GetBlueprintData().Nodes;
    auto it = std::find_if(bpNodes.begin(), bpNodes.end(), [id](const BlueprintNode& node)
    {
        return node.ID == id;
    });
    if (it != bpNodes.end())
    {
        return &(*it);
    }
    return nullptr;
}
bool BlueprintPanel::doesEventNodeExist(const std::string& fullName)
{
    for (const auto& node : m_nodes)
    {
        const BlueprintNode* sourceData = findSourceDataById(node.sourceDataID);
        if (sourceData && sourceData->Type == BlueprintNodeType::Event)
        {
            std::string existingNodeFullName = sourceData->TargetClassFullName + "." + sourceData->TargetMemberName;
            if (existingNodeFullName == fullName)
            {
                return true;
            }
        }
    }
    return false;
}
const BlueprintFunction* BlueprintPanel::findFunctionById(uint32_t functionId) const
{
    if (!m_currentBlueprint || functionId == 0) return nullptr;
    const auto& funcs = m_currentBlueprint->GetBlueprintData().Functions;
    auto it = std::find_if(funcs.begin(), funcs.end(),
                           [functionId](const BlueprintFunction& f) { return f.ID == functionId; });
    return it != funcs.end() ? &(*it) : nullptr;
}
uint32_t BlueprintPanel::getPinOwnerFunction(const BPin* pin) const
{
    if (!pin || !m_currentBlueprint) return 0;
    for (const auto& node : m_nodes)
    {
        if (node.id != pin->nodeId) continue;
        const auto& bpNodes = m_currentBlueprint->GetBlueprintData().Nodes;
        auto it = std::find_if(bpNodes.begin(), bpNodes.end(),
                               [&node](const BlueprintNode& bpNode) { return bpNode.ID == node.sourceDataID; });
        return it != bpNodes.end() ? readNodeOwnerFunction(*it) : 0;
    }
    return 0;
}
int BlueprintPanel::countNodesOwnedByFunction(uint32_t functionId) const
{
    if (!m_currentBlueprint) return 0;
    int count = 0;
    for (const auto& bpNode : m_currentBlueprint->GetBlueprintData().Nodes)
    {
        if (readNodeOwnerFunction(bpNode) == functionId) count++;
    }
    return count;
}
void BlueprintPanel::requestViewSwitch(uint32_t functionId)
{
    m_pendingViewFunction = functionId;
}
void BlueprintPanel::applyPendingViewSwitch()
{
    if (m_pendingViewFunction == kNoPendingView) return;
    uint32_t target = m_pendingViewFunction;
    m_pendingViewFunction = kNoPendingView;
    if (target != 0 && !findFunctionById(target))
    {
        target = 0; // 目标函数已不存在（如被撤销/删除），退回主图
    }
    if (target == m_currentViewFunction) return;
    m_currentViewFunction = target;
    m_viewJustSwitched = true;
    // 残留选择集属于旧视图域，若不清空会被 Delete/复制隔空命中
    ed::SetCurrentEditor(m_nodeEditorContext);
    ed::ClearSelection();
    m_regionInteraction.type = ERegionInteractionType::None;
    m_regionInteraction.activeRegion = nullptr;
    m_regionInteraction.nodesToDrag.clear();
}
void BlueprintPanel::drawGraphBreadcrumb()
{
    if (m_currentViewFunction == 0)
    {
        ImGui::TextDisabled("主图");
        return;
    }
    if (ImGui::SmallButton("主图##Breadcrumb"))
    {
        requestViewSwitch(0);
    }
    ImGui::SameLine();
    const BlueprintFunction* func = findFunctionById(m_currentViewFunction);
    ImGui::Text("> %s", func ? func->Name.c_str() : "未知函数");
    ImGui::SameLine();
    ImGui::TextDisabled("(Esc 返回主图)");
}
void BlueprintPanel::migrateNodeOwnership()
{
    auto& blueprintData = m_currentBlueprint->GetBlueprintData();
    // 版本 0 的旧数据没有 OwnerFunctionID，需按几何位置推断一次；
    // 版本 1 起 owner==0 即明确表示主图，不能再按区域包含改写
    const bool legacyData = blueprintData.GraphSchemaVersion < 1;
    for (auto& bpNode : blueprintData.Nodes)
    {
        if (bpNode.Type == BlueprintNodeType::FunctionEntry)
        {
            // 入口节点归属始终以函数名反查为准（改名时 TargetMemberName 已同步）
            const auto& funcs = blueprintData.Functions;
            auto it = std::find_if(funcs.begin(), funcs.end(), [&bpNode](const BlueprintFunction& f)
            {
                return f.Name == bpNode.TargetMemberName;
            });
            writeNodeOwnerFunction(bpNode, it != funcs.end() ? it->ID : 0);
            continue;
        }
        uint32_t owner = readNodeOwnerFunction(bpNode);
        if (owner != 0)
        {
            if (!findFunctionById(owner))
            {
                writeNodeOwnerFunction(bpNode, 0); // 归属的函数已不存在，节点回到主图
            }
            continue;
        }
        if (!legacyData) continue;
        // 旧文件迁移：落在某函数注释区域内的节点视为该函数的节点（沿用原坐标，不重排）
        for (const auto& region : m_regions)
        {
            if (region.functionId == 0) continue;
            if (bpNode.Position.x >= region.position.x && bpNode.Position.y >= region.position.y &&
                bpNode.Position.x <= region.position.x + region.size.x &&
                bpNode.Position.y <= region.position.y + region.size.y)
            {
                writeNodeOwnerFunction(bpNode, region.functionId);
                break;
            }
        }
    }
    // 区域归属的函数已不存在时回到主图（数据与编辑器镜像一并修正）
    for (auto& regionData : blueprintData.CommentRegions)
    {
        if (regionData.OwnerFunctionID != 0 && !findFunctionById(regionData.OwnerFunctionID))
        {
            regionData.OwnerFunctionID = 0;
        }
    }
    for (auto& region : m_regions)
    {
        if (region.ownerFunctionId != 0 && !findFunctionById(region.ownerFunctionId))
        {
            region.ownerFunctionId = 0;
        }
    }
    blueprintData.GraphSchemaVersion = 1;
    if (m_currentViewFunction != 0 && !findFunctionById(m_currentViewFunction))
    {
        m_currentViewFunction = 0; // 撤销/重做可能移除了正在查看的函数
        m_viewJustSwitched = true;
    }
}
BlueprintPanel::UndoRecord BlueprintPanel::makeSnapshot()
{
    // 先把编辑器实况（位置、连线、动态引脚、区域）同步进数据，再取全量副本；
    // 快照同时记录所在图页，撤销/重做时把视图带回修改现场
    captureStateToData();
    return {m_currentBlueprint->GetBlueprintData(), m_currentViewFunction};
}
void BlueprintPanel::pushUndoSnapshot()
{
    if (!m_currentBlueprint) return;
    // 同一帧内的批量修改（如框选删除多个节点）合并为一个撤销步骤
    if (ImGui::GetFrameCount() == m_lastUndoPushFrame) return;
    pushUndoSnapshotDirect(makeSnapshot());
}
void BlueprintPanel::pushUndoSnapshotDirect(UndoRecord&& snapshot)
{
    if (!m_currentBlueprint) return;
    m_undoStack.push_back(std::move(snapshot));
    while (m_undoStack.size() > kUndoStackLimit)
    {
        m_undoStack.pop_front();
    }
    m_redoStack.clear();
    m_lastUndoPushFrame = ImGui::GetFrameCount();
}
void BlueprintPanel::restoreFromSnapshot(const UndoRecord& snapshot)
{
    m_currentBlueprint->GetBlueprintData() = snapshot.data;
    m_currentBlueprintName = snapshot.data.Name;
    strncpy(m_blueprintNameBuffer, m_currentBlueprintName.c_str(), sizeof(m_blueprintNameBuffer));
    m_blueprintNameBuffer[sizeof(m_blueprintNameBuffer) - 1] = '\0';
    // 撤销跨图页的修改时切回快照所在图页，避免"看不见的变化"；
    // 若该函数在快照里已不存在，initializeFromBlueprintData 内的迁移会兜底退回主图
    if (m_currentViewFunction != snapshot.activeGraphId)
    {
        m_currentViewFunction = snapshot.activeGraphId;
        m_viewJustSwitched = true;
    }
    m_pendingViewFunction = kNoPendingView;
    // 与打开蓝图相同的重建路径，编辑器状态完全由数据再生
    initializeFromBlueprintData();
    m_hasMoveCandidate = false;
    m_hasPendingEditSnapshot = false;
}
void BlueprintPanel::performUndo()
{
    if (!m_currentBlueprint || m_undoStack.empty()) return;
    m_redoStack.push_back(makeSnapshot());
    UndoRecord snapshot = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    restoreFromSnapshot(snapshot);
}
void BlueprintPanel::performRedo()
{
    if (!m_currentBlueprint || m_redoStack.empty()) return;
    // 不走 pushUndoSnapshotDirect，避免清空重做栈
    m_undoStack.push_back(makeSnapshot());
    while (m_undoStack.size() > kUndoStackLimit)
    {
        m_undoStack.pop_front();
    }
    UndoRecord snapshot = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    restoreFromSnapshot(snapshot);
}
void BlueprintPanel::trackItemEditUndo()
{
    if (!m_currentBlueprint) return;
    if (ImGui::IsItemActivated())
    {
        // 编辑开始时暂存前置状态，提交（失焦且有修改）时才真正入栈
        m_pendingEditSnapshot = makeSnapshot();
        m_hasPendingEditSnapshot = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        if (m_hasPendingEditSnapshot)
        {
            pushUndoSnapshotDirect(std::move(m_pendingEditSnapshot));
            m_hasPendingEditSnapshot = false;
        }
    }
    else if (ImGui::IsItemDeactivated())
    {
        m_hasPendingEditSnapshot = false;
    }
}
void BlueprintPanel::deleteSelectedObjects()
{
    if (!m_currentBlueprint) return;
    ed::SetCurrentEditor(m_nodeEditorContext);
    int selectedCount = ed::GetSelectedObjectCount();
    if (selectedCount <= 0) return;
    std::vector<ed::NodeId> selectedNodes(selectedCount);
    std::vector<ed::LinkId> selectedLinks(selectedCount);
    int nodeCount = ed::GetSelectedNodes(selectedNodes.data(), selectedCount);
    int linkCount = ed::GetSelectedLinks(selectedLinks.data(), selectedCount);
    selectedNodes.resize(std::max(nodeCount, 0));
    selectedLinks.resize(std::max(linkCount, 0));
    if (selectedNodes.empty() && selectedLinks.empty()) return;
    for (ed::LinkId linkId : selectedLinks)
    {
        deleteLink(linkId);
    }
    for (ed::NodeId nodeId : selectedNodes)
    {
        BNode* node = findNodeById(nodeId);
        BlueprintNode* sourceData = node ? findSourceDataById(node->sourceDataID) : nullptr;
        if (sourceData && readNodeOwnerFunction(*sourceData) != m_currentViewFunction)
        {
            continue; // 其他视图域的残留选择不参与删除
        }
        deleteNode(nodeId); // 函数入口节点由 deleteNode 内部保护
    }
    ed::ClearSelection();
}
bool BlueprintPanel::collectSelectionForClipboard(std::vector<ClipboardNode>& outNodes,
                                                  std::vector<ClipboardLink>& outLinks, ImVec2& outTopLeft)
{
    if (!m_currentBlueprint) return false;
    ed::SetCurrentEditor(m_nodeEditorContext);
    int selectedCount = ed::GetSelectedObjectCount();
    if (selectedCount <= 0) return false;
    std::vector<ed::NodeId> selectedNodes(selectedCount);
    int nodeCount = ed::GetSelectedNodes(selectedNodes.data(), selectedCount);
    selectedNodes.resize(std::max(nodeCount, 0));
    if (selectedNodes.empty()) return false;
    // 同步位置与动态引脚信息，保证复制的数据副本完整
    captureStateToData();
    std::unordered_map<uint32_t, int> sourceIdToIndex;
    for (ed::NodeId nodeId : selectedNodes)
    {
        BNode* node = findNodeById(nodeId);
        if (!node) continue;
        BlueprintNode* sourceData = findSourceDataById(node->sourceDataID);
        if (!sourceData) continue;
        if (sourceData->Type == BlueprintNodeType::FunctionEntry) continue; // 入口节点不参与复制
        if (readNodeOwnerFunction(*sourceData) != m_currentViewFunction) continue; // 只复制当前视图域内的节点
        sourceIdToIndex[sourceData->ID] = static_cast<int>(outNodes.size());
        outNodes.push_back({*sourceData, ImVec2(0, 0)});
    }
    if (outNodes.empty()) return false;
    outTopLeft = ImVec2(outNodes[0].data.Position.x, outNodes[0].data.Position.y);
    for (const auto& clipNode : outNodes)
    {
        outTopLeft.x = std::min(outTopLeft.x, clipNode.data.Position.x);
        outTopLeft.y = std::min(outTopLeft.y, clipNode.data.Position.y);
    }
    for (auto& clipNode : outNodes)
    {
        clipNode.relativePosition = ImVec2(clipNode.data.Position.x - outTopLeft.x,
                                           clipNode.data.Position.y - outTopLeft.y);
    }
    for (const auto& link : m_links)
    {
        BPin* startPin = findPinById(link.startPinId);
        BPin* endPin = findPinById(link.endPinId);
        if (!startPin || !endPin) continue;
        BNode* startNode = findNodeById(startPin->nodeId);
        BNode* endNode = findNodeById(endPin->nodeId);
        if (!startNode || !endNode) continue;
        auto fromIt = sourceIdToIndex.find(startNode->sourceDataID);
        auto toIt = sourceIdToIndex.find(endNode->sourceDataID);
        if (fromIt == sourceIdToIndex.end() || toIt == sourceIdToIndex.end()) continue;
        outLinks.push_back({fromIt->second, startPin->name, toIt->second, endPin->name});
    }
    return true;
}
void BlueprintPanel::copySelectionToClipboard()
{
    std::vector<ClipboardNode> nodes;
    std::vector<ClipboardLink> links;
    ImVec2 topLeft;
    if (!collectSelectionForClipboard(nodes, links, topLeft)) return;
    m_clipboardNodes = std::move(nodes);
    m_clipboardLinks = std::move(links);
}
void BlueprintPanel::pasteFromClipboard(const std::vector<ClipboardNode>& nodes,
                                        const std::vector<ClipboardLink>& links, ImVec2 basePosition)
{
    if (!m_currentBlueprint || nodes.empty()) return;
    // 先把编辑器实况落进数据再整体重建，避免重建丢失未保存的修改
    captureStateToData();
    pushUndoSnapshot();
    auto& blueprintData = m_currentBlueprint->GetBlueprintData();
    std::vector<int64_t> newNodeIds(nodes.size(), -1);
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto& clipNode = nodes[i];
        if (clipNode.data.Type == BlueprintNodeType::Event &&
            doesEventNodeExist(clipNode.data.TargetClassFullName + "." + clipNode.data.TargetMemberName))
        {
            continue; // 事件节点唯一，跳过已存在的
        }
        BlueprintNode newNode = clipNode.data;
        newNode.ID = getNextNodeId();
        newNode.Position.x = basePosition.x + clipNode.relativePosition.x;
        newNode.Position.y = basePosition.y + clipNode.relativePosition.y;
        // 粘贴目标域=当前视图：覆盖剪贴板里带来的旧归属，支持跨图页复制粘贴
        writeNodeOwnerFunction(newNode, m_currentViewFunction);
        blueprintData.Nodes.push_back(newNode);
        newNodeIds[i] = newNode.ID;
    }
    for (const auto& clipLink : links)
    {
        if (clipLink.fromNodeIndex < 0 || clipLink.fromNodeIndex >= static_cast<int>(newNodeIds.size())) continue;
        if (clipLink.toNodeIndex < 0 || clipLink.toNodeIndex >= static_cast<int>(newNodeIds.size())) continue;
        if (newNodeIds[clipLink.fromNodeIndex] < 0 || newNodeIds[clipLink.toNodeIndex] < 0) continue;
        blueprintData.Links.push_back({
            static_cast<uint32_t>(newNodeIds[clipLink.fromNodeIndex]), clipLink.fromPinName,
            static_cast<uint32_t>(newNodeIds[clipLink.toNodeIndex]), clipLink.toPinName
        });
    }
    initializeFromBlueprintData();
    ed::SetCurrentEditor(m_nodeEditorContext);
    ed::ClearSelection();
    for (int64_t newId : newNodeIds)
    {
        if (newId >= 0)
        {
            ed::SelectNode(ed::NodeId(static_cast<uint32_t>(newId)), true);
        }
    }
}
void BlueprintPanel::pasteClipboardAtMouse()
{
    if (!m_currentBlueprint || m_clipboardNodes.empty()) return;
    ed::SetCurrentEditor(m_nodeEditorContext);
    ImVec2 basePosition = ed::ScreenToCanvas(ImGui::GetMousePos());
    pasteFromClipboard(m_clipboardNodes, m_clipboardLinks, basePosition);
}
void BlueprintPanel::duplicateSelection()
{
    std::vector<ClipboardNode> nodes;
    std::vector<ClipboardLink> links;
    ImVec2 topLeft;
    if (!collectSelectionForClipboard(nodes, links, topLeft)) return;
    pasteFromClipboard(nodes, links, ImVec2(topLeft.x + 40.0f, topLeft.y + 40.0f));
}
std::vector<BlueprintPanel::BNode*> BlueprintPanel::collectSelectedViewNodes()
{
    std::vector<BNode*> result;
    if (!m_currentBlueprint) return result;
    ed::SetCurrentEditor(m_nodeEditorContext);
    int selectedCount = ed::GetSelectedObjectCount();
    if (selectedCount <= 0) return result;
    std::vector<ed::NodeId> selectedNodes(selectedCount);
    int nodeCount = ed::GetSelectedNodes(selectedNodes.data(), selectedCount);
    selectedNodes.resize(std::max(nodeCount, 0));
    for (ed::NodeId nodeId : selectedNodes)
    {
        BNode* node = findNodeById(nodeId);
        BlueprintNode* sourceData = node ? findSourceDataById(node->sourceDataID) : nullptr;
        if (!sourceData || readNodeOwnerFunction(*sourceData) != m_currentViewFunction)
        {
            continue; // 其他图页的残留选择不参与本页布局操作
        }
        result.push_back(node);
    }
    return result;
}
void BlueprintPanel::alignSelectedNodes(NodeAlignMode mode)
{
    std::vector<BNode*> nodes = collectSelectedViewNodes();
    if (nodes.size() < 2) return;
    pushUndoSnapshot();
    ed::SetCurrentEditor(m_nodeEditorContext);
    // 以选区包围盒的对应边为基准
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (const BNode* node : nodes)
    {
        const ImVec2 size = ed::GetNodeSize(node->id);
        minX = std::min(minX, node->position.x);
        minY = std::min(minY, node->position.y);
        maxX = std::max(maxX, node->position.x + size.x);
        maxY = std::max(maxY, node->position.y + size.y);
    }
    for (BNode* node : nodes)
    {
        const ImVec2 size = ed::GetNodeSize(node->id);
        ImVec2 pos = node->position;
        switch (mode)
        {
        case NodeAlignMode::Left: pos.x = minX;
            break;
        case NodeAlignMode::Right: pos.x = maxX - size.x;
            break;
        case NodeAlignMode::Top: pos.y = minY;
            break;
        case NodeAlignMode::Bottom: pos.y = maxY - size.y;
            break;
        }
        node->position = pos;
        ed::SetNodePosition(node->id, pos);
    }
}
void BlueprintPanel::distributeSelectedNodes(bool horizontal)
{
    std::vector<BNode*> nodes = collectSelectedViewNodes();
    if (nodes.size() < 3) return;
    pushUndoSnapshot();
    ed::SetCurrentEditor(m_nodeEditorContext);
    // 首尾节点不动，中间节点重排使相邻间隙相等
    std::sort(nodes.begin(), nodes.end(), [horizontal](const BNode* a, const BNode* b)
    {
        return horizontal ? a->position.x < b->position.x : a->position.y < b->position.y;
    });
    float totalSize = 0.0f;
    for (const BNode* node : nodes)
    {
        const ImVec2 size = ed::GetNodeSize(node->id);
        totalSize += horizontal ? size.x : size.y;
    }
    const ImVec2 lastSize = ed::GetNodeSize(nodes.back()->id);
    const float spanStart = horizontal ? nodes.front()->position.x : nodes.front()->position.y;
    const float spanEnd = horizontal
                              ? nodes.back()->position.x + lastSize.x
                              : nodes.back()->position.y + lastSize.y;
    const float gap = (spanEnd - spanStart - totalSize) / static_cast<float>(nodes.size() - 1);
    float cursor = spanStart;
    for (BNode* node : nodes)
    {
        const ImVec2 size = ed::GetNodeSize(node->id);
        ImVec2 pos = node->position;
        if (horizontal) pos.x = cursor;
        else pos.y = cursor;
        node->position = pos;
        ed::SetNodePosition(node->id, pos);
        cursor += (horizontal ? size.x : size.y) + gap;
    }
}
void BlueprintPanel::arrangeNodes()
{
    if (!m_currentBlueprint) return;
    ed::SetCurrentEditor(m_nodeEditorContext);
    // 有效多选（>=2）时整理选中节点，否则整理当前图页全部节点
    std::vector<BNode*> targets = collectSelectedViewNodes();
    if (targets.size() < 2)
    {
        targets.clear();
        for (auto& node : m_nodes)
        {
            BlueprintNode* sourceData = findSourceDataById(node.sourceDataID);
            if (sourceData && readNodeOwnerFunction(*sourceData) == m_currentViewFunction)
            {
                targets.push_back(&node);
            }
        }
    }
    if (targets.size() < 2) return;
    pushUndoSnapshot();
    const int count = static_cast<int>(targets.size());
    std::unordered_map<uint64_t, int> indexOfNode;
    for (int i = 0; i < count; ++i)
    {
        indexOfNode[targets[i]->id.Get()] = i;
    }
    // 目标集合内部的有向边（输出端 → 输入端），exec 与数据连线同权参与分层
    struct ArrangeEdge
    {
        int from = 0;
        int to = 0;
    };
    std::vector<ArrangeEdge> edges;
    for (const auto& link : m_links)
    {
        const BPin* startPin = findPinById(link.startPinId);
        const BPin* endPin = findPinById(link.endPinId);
        if (!startPin || !endPin) continue;
        auto fromIt = indexOfNode.find(startPin->nodeId.Get());
        auto toIt = indexOfNode.find(endPin->nodeId.Get());
        if (fromIt == indexOfNode.end() || toIt == indexOfNode.end()) continue;
        if (fromIt->second == toIt->second) continue;
        edges.push_back({fromIt->second, toIt->second});
    }
    // 分列：沿有向边做最长路松弛，轮数受限以容忍数据环
    std::vector<int> column(count, 0);
    for (int round = 0; round < count; ++round)
    {
        bool changed = false;
        for (const ArrangeEdge& edge : edges)
        {
            if (column[edge.to] < column[edge.from] + 1)
            {
                column[edge.to] = column[edge.from] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    // 纯数据节点（无任何 Exec 引脚）贴到最早消费者的前一列，避免全部堆在第 0 列
    for (int i = 0; i < count; ++i)
    {
        const BNode* node = targets[i];
        bool hasExecPin = false;
        for (const auto& pin : node->inputPins)
        {
            if (pin.type == "Exec")
            {
                hasExecPin = true;
                break;
            }
        }
        if (!hasExecPin)
        {
            for (const auto& pin : node->outputPins)
            {
                if (pin.type == "Exec")
                {
                    hasExecPin = true;
                    break;
                }
            }
        }
        if (hasExecPin) continue;
        int minConsumer = count; // 列号必小于节点数，count 作"无消费者"哨兵
        for (const ArrangeEdge& edge : edges)
        {
            if (edge.from == i) minConsumer = std::min(minConsumer, column[edge.to]);
        }
        if (minConsumer != count)
        {
            column[i] = std::max(0, minConsumer - 1);
        }
    }
    int maxColumn = 0;
    for (int c : column) maxColumn = std::max(maxColumn, c);
    std::vector<std::vector<int>> columns(static_cast<size_t>(maxColumn) + 1);
    for (int i = 0; i < count; ++i)
    {
        columns[column[i]].push_back(i);
    }
    // 同列排序：有前驱的按前驱行号重心，无前驱的保持原 y 相对次序
    std::vector<float> rowOrder(count, 0.0f);
    for (auto& columnNodes : columns)
    {
        std::sort(columnNodes.begin(), columnNodes.end(), [&](int a, int b)
        {
            return targets[a]->position.y < targets[b]->position.y;
        });
        std::vector<std::pair<float, int>> keyed;
        keyed.reserve(columnNodes.size());
        for (size_t r = 0; r < columnNodes.size(); ++r)
        {
            const int index = columnNodes[r];
            float sum = 0.0f;
            int predCount = 0;
            for (const ArrangeEdge& edge : edges)
            {
                if (edge.to == index && column[edge.from] < column[index])
                {
                    sum += rowOrder[edge.from];
                    ++predCount;
                }
            }
            keyed.emplace_back(predCount > 0 ? sum / static_cast<float>(predCount) : static_cast<float>(r), index);
        }
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t r = 0; r < keyed.size(); ++r)
        {
            columnNodes[r] = keyed[r].second;
            rowOrder[keyed[r].second] = static_cast<float>(r);
        }
    }
    // 布局原点取目标集原包围盒左上角，整理后整体位置大致不变
    ImVec2 origin(FLT_MAX, FLT_MAX);
    for (const BNode* node : targets)
    {
        origin.x = std::min(origin.x, node->position.x);
        origin.y = std::min(origin.y, node->position.y);
    }
    constexpr float kColumnSpacing = 250.0f;
    constexpr float kRowSpacing = 120.0f;
    float columnX = origin.x;
    for (const auto& columnNodes : columns)
    {
        float y = origin.y;
        float maxWidth = 0.0f;
        for (int index : columnNodes)
        {
            BNode* node = targets[index];
            const ImVec2 pos(columnX, y);
            node->position = pos;
            ed::SetNodePosition(node->id, pos);
            const ImVec2 size = ed::GetNodeSize(node->id);
            maxWidth = std::max(maxWidth, size.x);
            // 行距 120 起步，超高节点按实际高度让位避免重叠
            y += std::max(kRowSpacing, size.y + 24.0f);
        }
        // 列距 250 起步，超宽节点会把下一列推远
        columnX += std::max(kColumnSpacing, maxWidth + 40.0f);
    }
}
void BlueprintPanel::setBookmark(int slot)
{
    if (!m_currentBlueprint) return;
    auto& bookmarks = m_currentBlueprint->GetBlueprintData().Bookmarks;
    auto it = std::find_if(bookmarks.begin(), bookmarks.end(),
                           [slot](const BlueprintBookmark& bookmark) { return bookmark.Slot == slot; });
    if (it == bookmarks.end())
    {
        bookmarks.emplace_back();
        it = std::prev(bookmarks.end());
        it->Slot = slot;
    }
    it->GraphID = m_currentViewFunction;
    const ImVec4 rect = captureCurrentViewRect();
    it->ViewRect.x = rect.x;
    it->ViewRect.y = rect.y;
    it->ViewRect.w = rect.z - rect.x;
    it->ViewRect.h = rect.w - rect.y;
    LogInfo("画布书签 {} 已设置（{}）", slot, graphDisplayName(m_currentViewFunction));
}
void BlueprintPanel::jumpToBookmark(int slot)
{
    if (!m_currentBlueprint) return;
    const auto& bookmarks = m_currentBlueprint->GetBlueprintData().Bookmarks;
    auto it = std::find_if(bookmarks.begin(), bookmarks.end(),
                           [slot](const BlueprintBookmark& bookmark) { return bookmark.Slot == slot; });
    if (it == bookmarks.end() || it->ViewRect.w <= 0.0f || it->ViewRect.h <= 0.0f) return;
    if (it->GraphID != 0 && !findFunctionById(it->GraphID))
    {
        LogWarn("画布书签 {} 指向的函数已被删除", slot);
        return;
    }
    requestViewSwitch(it->GraphID); // 目标与当前页相同时切换为空操作
    m_pendingFocusNodeId = 0;
    m_pendingNavKind = PendingNavKind::Rect;
    m_pendingNavRect = ImVec4(it->ViewRect.x, it->ViewRect.y,
                              it->ViewRect.x + it->ViewRect.w, it->ViewRect.y + it->ViewRect.h);
}
ImVec4 BlueprintPanel::captureCurrentViewRect() const
{
    return BlueprintEditorNav::GetViewRect(m_nodeEditorContext);
}
void BlueprintPanel::navigateToViewRect(const ImVec4& rect)
{
    if (rect.z - rect.x <= 0.0f || rect.w - rect.y <= 0.0f) return;
    BlueprintEditorNav::NavigateToRect(m_nodeEditorContext, rect, 0.25f);
}
std::string BlueprintPanel::graphDisplayName(uint32_t graphId) const
{
    if (graphId == 0) return "主图";
    const BlueprintFunction* func = findFunctionById(graphId);
    return func ? ("函数 " + func->Name) : "未知图页";
}
void BlueprintPanel::openVariableReferences(const std::string& variableName)
{
    if (!m_currentBlueprint) return;
    m_findReferences.items.clear();
    m_findReferences.title = "变量 \"" + variableName + "\" 的引用";
    for (const auto& bpNode : m_currentBlueprint->GetBlueprintData().Nodes)
    {
        const bool isVariableNode = bpNode.Type == BlueprintNodeType::VariableGet ||
            bpNode.Type == BlueprintNodeType::VariableSet;
        if (!isVariableNode || bpNode.VariableName != variableName) continue;
        ReferenceItem item;
        item.nodeID = bpNode.ID;
        item.graphId = readNodeOwnerFunction(bpNode);
        item.nodeTitle = (bpNode.Type == BlueprintNodeType::VariableGet ? "获取 " : "设置 ") + variableName;
        item.graphTitle = graphDisplayName(item.graphId);
        m_findReferences.items.push_back(std::move(item));
    }
    m_findReferences.isOpen = true;
}
void BlueprintPanel::openFunctionReferences(const std::string& functionName)
{
    if (!m_currentBlueprint) return;
    m_findReferences.items.clear();
    m_findReferences.title = "函数 \"" + functionName + "\" 的引用";
    for (const auto& bpNode : m_currentBlueprint->GetBlueprintData().Nodes)
    {
        std::string title;
        if (bpNode.Type == BlueprintNodeType::FunctionCall &&
            bpNode.TargetClassFullName.empty() && bpNode.TargetMemberName == functionName)
        {
            title = "调用 " + functionName;
        }
        else
        {
            // 函数选择引脚（FunctionSelection）以默认值存函数名，也算引用
            const auto* definition = BlueprintNodeRegistry::GetInstance().GetDefinition(
                bpNode.TargetClassFullName + "." + bpNode.TargetMemberName);
            if (definition)
            {
                for (const auto& pinDef : definition->InputPins)
                {
                    if (pinDef.Type != "FunctionSelection") continue;
                    auto defaultIt = bpNode.InputDefaults.find(pinDef.Name);
                    if (defaultIt != bpNode.InputDefaults.end() && defaultIt->second == functionName)
                    {
                        title = definition->DisplayName + "（引脚 " + pinDef.Name + "）";
                        break;
                    }
                }
            }
        }
        if (title.empty()) continue;
        ReferenceItem item;
        item.nodeID = bpNode.ID;
        item.graphId = readNodeOwnerFunction(bpNode);
        item.nodeTitle = std::move(title);
        item.graphTitle = graphDisplayName(item.graphId);
        m_findReferences.items.push_back(std::move(item));
    }
    m_findReferences.isOpen = true;
}
void BlueprintPanel::drawFindReferencesWindow()
{
    if (!m_findReferences.isOpen || !m_currentBlueprint) return;
    ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("查找引用##BlueprintFindReferences", &m_findReferences.isOpen))
    {
        ImGui::TextUnformatted(m_findReferences.title.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("（%d 处）", static_cast<int>(m_findReferences.items.size()));
        ImGui::Separator();
        if (m_findReferences.items.empty())
        {
            ImGui::TextDisabled("没有找到引用。");
        }
        else
        {
            if (ImGui::BeginChild("##ReferenceList"))
            {
                for (size_t i = 0; i < m_findReferences.items.size(); ++i)
                {
                    const ReferenceItem& item = m_findReferences.items[i];
                    ImGui::PushID(static_cast<int>(i));
                    const std::string label = item.nodeTitle + "  [" + item.graphTitle + "]##ref";
                    if (ImGui::Selectable(label.c_str()))
                    {
                        jumpToNode(item.nodeID);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}
void BlueprintPanel::jumpToNode(uint32_t nodeDataId)
{
    BlueprintNode* sourceData = findSourceDataById(nodeDataId);
    if (!sourceData)
    {
        LogWarn("引用的节点已不存在（ID={}），请重新查找", nodeDataId);
        return;
    }
    // 先切到节点所在图页（同页为空操作），居中在节点提交后的待定导航中执行
    requestViewSwitch(readNodeOwnerFunction(*sourceData));
    m_pendingNavKind = PendingNavKind::None;
    m_pendingFocusNodeId = nodeDataId;
}
