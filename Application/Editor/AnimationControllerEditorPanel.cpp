#include "AnimationControllerEditorPanel.h"
#include "../Utils/Logger.h"
#include "../Resources/Loaders/AnimationControllerLoader.h"
#include "../Utils/PopupManager.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../Resources/RuntimeAsset/RuntimeScene.h"
#include "AssetManager.h"
#include "AnimationControllerComponent.h"
#include "Path.h"
#include "Profiler.h"
#include "Input/Keyboards.h"
#include "Loaders/AnimationClipLoader.h"
AnimationControllerEditorPanel::~AnimationControllerEditorPanel()
{
    if (m_nodeEditorContext)
    {
        ed::DestroyEditor(m_nodeEditorContext);
        m_nodeEditorContext = nullptr;
    }
}
void AnimationControllerEditorPanel::Initialize(EditorContext* context)
{
    m_context = context;
    ed::Config config;
    config.SettingsFile = nullptr;
    m_nodeEditorContext = ed::CreateEditor(&config);
}
void AnimationControllerEditorPanel::Update(float deltaTime)
{
    PROFILE_FUNCTION();
    if (m_context->currentEditingAnimationControllerGuid.Valid())
    {
        OpenAnimationController(m_context->currentEditingAnimationControllerGuid);
    }
}
void AnimationControllerEditorPanel::Draw()
{
    PROFILE_FUNCTION();
    if (!m_isVisible)
        return;
    if (m_requestFocus)
    {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
    // 用 ### 固定窗口ID，标题可随脏标记变化而不重置停靠状态
    std::string windowTitle = std::string(GetPanelName()) + (m_isDirty ? " *" : "") +
        "###AnimationControllerEditorPanel";
    if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, ImGuiWindowFlags_MenuBar))
    {
        m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("文件"))
            {
                if (ImGui::MenuItem("保存", "Ctrl+S", false, m_currentController != nullptr))
                {
                    saveToControllerData();
                }
                if (ImGui::MenuItem("关闭", "Ctrl+W", false, m_currentController != nullptr))
                {
                    CloseCurrentController();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("编辑"))
            {
                if (ImGui::MenuItem("添加状态", "N", false, m_currentController != nullptr))
                {
                    createStateNode("新状态", ImVec2(100, 100));
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("视图"))
            {
                ImGui::MenuItem("变量面板", nullptr, &m_variablesPanelOpen);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (!m_currentController)
        {
            ImVec2 center = ImGui::GetContentRegionAvail();
            center.x *= 0.5f;
            center.y *= 0.5f;
            ImGui::SetCursorPos(center);
            ImGui::Text("请双击动画控制器资源以开始编辑");
        }
        else
        {
            if (ImGui::BeginChild("MainContent", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar))
            {
                static float splitterWidth = 300.0f;
                ImVec2 contentSize = ImGui::GetContentRegionAvail();
                if (ImGui::BeginChild("NodeEditor", ImVec2(contentSize.x - splitterWidth - 10, 0), true))
                {
                    drawNodeEditor();
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::Button("##splitter", ImVec2(10, -1));
                if (ImGui::IsItemActive())
                {
                    splitterWidth -= ImGui::GetIO().MouseDelta.x;
                    splitterWidth = std::clamp(splitterWidth, 200.0f, contentSize.x - 200.0f);
                }
                ImGui::SameLine();
                if (m_variablesPanelOpen)
                {
                    if (ImGui::BeginChild("VariablesPanel", ImVec2(splitterWidth, 0), true))
                    {
                        drawVariablesPanel();
                    }
                    ImGui::EndChild();
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
    handleShortcutInput();
    if (m_transitionEditWindowOpen)
    {
        drawTransitionEditor();
    }
    if (m_statePropertiesWindowOpen)
    {
        drawStatePropertiesWindow();
    }
    if (m_deleteVarConfirmOpen)
    {
        drawVariableDeleteConfirmWindow();
    }
    if (m_blendTreeWindowOpen)
    {
        drawBlendTreeEditorWindow();
    }
}
void AnimationControllerEditorPanel::Shutdown()
{
    CloseCurrentController();
    if (m_nodeEditorContext)
    {
        ed::DestroyEditor(m_nodeEditorContext);
        m_nodeEditorContext = nullptr;
    }
}
void AnimationControllerEditorPanel::OpenAnimationController(const Guid& controllerGuid)
{
    if (m_currentControllerGuid == controllerGuid && m_currentController)
        return;
    CloseCurrentController();
    auto loader = AnimationControllerLoader();
    m_currentController = loader.LoadAsset(controllerGuid);
    if (!m_currentController)
    {
        LogError("无法加载动画控制器，GUID: {}", controllerGuid.ToString());
        return;
    }
    m_currentControllerGuid = controllerGuid;
    m_currentControllerName = Path::GetFileNameWithoutExtension(
        AssetManager::GetInstance().GetMetadata(controllerGuid)->assetPath.string());
    m_controllerData = m_currentController->GetAnimationControllerData();
    initializeFromControllerData();
    m_isDirty = false;
    SetVisible(true);
    m_context->currentEditingAnimationControllerGuid = controllerGuid;
    LogInfo("打开动画控制器进行编辑: {}", m_currentControllerName);
}
void AnimationControllerEditorPanel::CloseCurrentController()
{
    if (!m_currentController)
        return;
    LogInfo("关闭动画控制器: {}", m_currentControllerName);
    m_currentController = nullptr;
    m_currentControllerGuid = Guid();
    m_currentControllerName.clear();
    m_nodes.clear();
    m_links.clear();
    m_stateToNodeIndex.clear();
    m_transitionEditWindowOpen = false;
    m_editingLinkIndex = -1;
    m_isDirty = false;
    m_statePropertiesWindowOpen = false;
    m_renamingNodeId = 0;
    m_deleteVarConfirmOpen = false;
    m_pendingDeleteVarIndex = -1;
    m_pendingDeleteVarName.clear();
    m_editingVarIndex = -1;
    m_blendTreeWindowOpen = false;
    m_blendTreeNodeId = 0;
    m_nextNodeId = 1;
    m_nextLinkId = 1;
    m_nextPinId = 1;
    m_context->currentEditingAnimationControllerGuid = Guid();
}
void AnimationControllerEditorPanel::Focus()
{
    m_requestFocus = true;
}
void AnimationControllerEditorPanel::initializeFromControllerData()
{
    m_nodes.clear();
    m_links.clear();
    m_stateToNodeIndex.clear();
    ANode entryNode;
    entryNode.id = getNextNodeId();
    entryNode.stateGuid = SpecialStateGuids::Entry();
    entryNode.name = "Entry";
    entryNode.type = NodeType::Entry;
    entryNode.position = ImVec2(50, 200);
    entryNode.color = ImVec4(0.1f, 0.6f, 0.2f, 1.0f);
    entryNode.inputPinId = 0;
    entryNode.outputPinId = getNextPinId();
    if (auto it = m_controllerData.States.find(entryNode.stateGuid);
        it != m_controllerData.States.end() && it->second.hasEditorPosition)
    {
        entryNode.position = ImVec2(it->second.editorPosX, it->second.editorPosY);
    }
    m_stateToNodeIndex[entryNode.stateGuid] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(entryNode);
    ANode anyStateNode;
    anyStateNode.id = getNextNodeId();
    anyStateNode.stateGuid = SpecialStateGuids::AnyState();
    anyStateNode.name = "Any State";
    anyStateNode.type = NodeType::AnyState;
    anyStateNode.position = ImVec2(50, 400);
    anyStateNode.color = ImVec4(0.7f, 0.2f, 0.7f, 1.0f);
    anyStateNode.inputPinId = 0;
    anyStateNode.outputPinId = getNextPinId();
    if (auto it = m_controllerData.States.find(anyStateNode.stateGuid);
        it != m_controllerData.States.end() && it->second.hasEditorPosition)
    {
        anyStateNode.position = ImVec2(it->second.editorPosX, it->second.editorPosY);
    }
    m_stateToNodeIndex[anyStateNode.stateGuid] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(anyStateNode);
    float nodeSpacing = 250.0f;
    float startX = 350.0f;
    float startY = 100.0f;
    int nodeIndex = 0;
    for (auto& [stateGuid, state] : m_controllerData.States)
    {
        if (stateGuid == SpecialStateGuids::Entry() || stateGuid == SpecialStateGuids::AnyState())
            continue;
        std::string clipName;
        for (const auto& [name, guid] : m_controllerData.Clips)
        {
            if (guid == stateGuid)
            {
                clipName = name;
                break;
            }
        }
        ANode node;
        node.id = getNextNodeId();
        node.stateGuid = stateGuid;
        // 旧数据无独立状态名时以剪辑名初始化并写回，保持向后兼容
        if (state.stateName.empty())
        {
            state.stateName = !clipName.empty() ? clipName : "状态";
        }
        node.name = state.stateName;
        node.type = NodeType::State;
        node.speed = state.speed;
        node.stateType = state.stateType;
        node.blendTree = state.blendTree;
        // 子项按阈值升序，防御手改文件产生的乱序数据
        sortBlendTreeChildren(node.blendTree);
        if (state.hasEditorPosition)
        {
            node.position = ImVec2(state.editorPosX, state.editorPosY);
        }
        else
        {
            // 旧数据无位置字段：按网格布局兜底并写回，保存时随 YAML 持久化
            node.position = ImVec2(startX + (nodeIndex % 3) * nodeSpacing, startY + (nodeIndex / 3) * nodeSpacing);
            state.editorPosX = node.position.x;
            state.editorPosY = node.position.y;
            state.hasEditorPosition = true;
        }
        node.color = state.stateType == AnimationStateType::BlendTree
                         ? ImVec4(0.2f, 0.55f, 0.6f, 1.0f)
                         : ImVec4(0.4f, 0.4f, 0.5f, 1.0f);
        node.inputPinId = getNextPinId();
        node.outputPinId = getNextPinId();
        m_stateToNodeIndex[stateGuid] = static_cast<int>(m_nodes.size());
        m_nodes.push_back(node);
        nodeIndex++;
    }
    for (const auto& [fromStateGuid, state] : m_controllerData.States)
    {
        ANode* fromNode = findNodeByStateGuid(fromStateGuid);
        if (!fromNode) continue;
        for (const auto& transition : state.Transitions)
        {
            ANode* toNode = findNodeByStateGuid(transition.ToGuid);
            if (!toNode) continue;
            ALink link;
            link.id = getNextLinkId();
            link.startPinId = fromNode->outputPinId;
            link.endPinId = toNode->inputPinId;
            link.fromStateGuid = fromStateGuid;
            link.toStateGuid = transition.ToGuid;
            link.transitionName = transition.TransitionName;
            link.duration = transition.TransitionDuration;
            link.hasExitTime = transition.hasExitTime;
            link.exitTime = transition.exitTime;
            link.conditions = transition.Conditions;
            link.priority = transition.priority;
            m_links.push_back(link);
        }
    }
    m_forceLayoutUpdate = true;
}
void AnimationControllerEditorPanel::saveToControllerData()
{
    if (!m_currentController) return;
    m_controllerData.States.clear();
    for (const ANode& node : m_nodes)
    {
        AnimationState state;
        state.stateName = node.name;
        state.speed = node.speed;
        state.stateType = node.stateType;
        state.blendTree = node.blendTree;
        // node.position 每帧与画布同步，保存时统一读取写入数据
        state.editorPosX = node.position.x;
        state.editorPosY = node.position.y;
        state.hasEditorPosition = true;
        for (const ALink& link : m_links)
        {
            if (link.fromStateGuid == node.stateGuid)
            {
                Transition transition;
                transition.ToGuid = link.toStateGuid;
                transition.TransitionName = link.transitionName;
                transition.TransitionDuration = link.duration;
                transition.Conditions = link.conditions;
                transition.priority = link.priority;
                transition.hasExitTime = link.hasExitTime;
                transition.exitTime = link.exitTime;
                state.Transitions.push_back(transition);
            }
        }
        m_controllerData.States[node.stateGuid] = state;
    }
    auto meta = AssetManager::GetInstance().GetMetadata(m_currentControllerGuid);
    auto filePath = AssetManager::GetInstance().GetAssetsRootPath() / meta->assetPath;
    std::string content = YAML::Dump(YAML::convert<AnimationControllerData>::encode(m_controllerData));
    Path::WriteFile(filePath.string(), content);
    m_isDirty = false;
    LogInfo("动画控制器数据已保存");
}
void AnimationControllerEditorPanel::drawTransitionEditor()
{
    if (!ImGui::Begin("过渡编辑器", &m_transitionEditWindowOpen))
    {
        ImGui::End();
        return;
    }
    if (m_editingLinkIndex < 0 || m_editingLinkIndex >= static_cast<int>(m_links.size()))
    {
        ImGui::Text("无效的过渡");
        ImGui::End();
        return;
    }
    ALink& link = m_links[m_editingLinkIndex];
    ImGui::Text("编辑过渡");
    ImGui::Separator();
    char nameBuffer[256];
    strncpy(nameBuffer, link.transitionName.c_str(), sizeof(nameBuffer));
    nameBuffer[sizeof(nameBuffer) - 1] = '\0';
    if (ImGui::InputText("过渡名称", nameBuffer, sizeof(nameBuffer)))
    {
        link.transitionName = nameBuffer;
        markDirty();
    }
    if (ImGui::DragFloat("持续时间", &link.duration, 0.01f, 0.0f, 10.0f, "%.2fs"))
    {
        markDirty();
    }
    if (ImGui::InputInt("优先级", &link.priority))
    {
        markDirty();
    }
    if (ImGui::Checkbox("拥有退出时间", &link.hasExitTime))
    {
        markDirty();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("如果勾选，此过渡只会在当前动画归一化播放进度达到“退出时间”后才会进行条件检查。\n如果不勾选，则会立即中断当前动画进行过渡。");
    }
    if (link.hasExitTime)
    {
        if (ImGui::SliderFloat("退出时间", &link.exitTime, 0.0f, 1.0f, "%.2f"))
        {
            markDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("归一化播放进度（0-1）达到该值后才允许过渡，1.0 表示动画播放完毕。");
        }
    }
    ImGui::Separator();
    ImGui::Text("过渡条件");
    if (drawConditionEditor(link.conditions))
    {
        markDirty();
    }
    if (ImGui::Button("保存"))
    {
        m_transitionEditWindowOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消"))
    {
        m_transitionEditWindowOpen = false;
    }
    ImGui::End();
}
void AnimationControllerEditorPanel::drawNodeContextMenu()
{
    if (ImGui::BeginPopup("NodeContextMenu"))
    {
        ANode* contextNode = findNodeById(m_contextNodeId);
        if (contextNode && contextNode->type == NodeType::State)
        {
            if (contextNode->stateType == AnimationStateType::BlendTree)
            {
                if (ImGui::MenuItem("编辑混合树"))
                {
                    openBlendTreeEditorWindow(*contextNode);
                }
            }
            if (ImGui::MenuItem("重命名"))
            {
                openStatePropertiesWindow(*contextNode);
            }
            if (ImGui::MenuItem("删除状态"))
            {
                deleteNode(m_contextNodeId);
            }
        }
        else
        {
            ImGui::TextDisabled("特殊状态无法修改");
        }
        ImGui::EndPopup();
    }
}
void AnimationControllerEditorPanel::drawLinkContextMenu()
{
    if (ImGui::BeginPopup("LinkContextMenu"))
    {
        if (ImGui::MenuItem("编辑过渡"))
        {
            ALink* contextLink = findLinkById(m_contextLinkId);
            if (contextLink)
            {
                m_editingLinkIndex = static_cast<int>(contextLink - m_links.data());
                m_transitionEditWindowOpen = true;
            }
        }
        if (ImGui::MenuItem("删除过渡"))
        {
            deleteLink(m_contextLinkId);
        }
        ImGui::EndPopup();
    }
}
bool AnimationControllerEditorPanel::drawConditionEditor(std::vector<Condition>& conditions)
{
    bool changed = false;
    for (size_t i = 0; i < conditions.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        Condition& condition = conditions[i];
        std::visit([this, &changed](auto&& arg)
        {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, FloatCondition>)
            {
                if (ImGui::BeginCombo("Float变量", arg.VarName.c_str()))
                {
                    for (const auto& var : m_controllerData.Variables)
                    {
                        if (var.Type == VariableType::VariableType_Float)
                        {
                            if (ImGui::Selectable(var.Name.c_str(), arg.VarName == var.Name))
                            {
                                arg.VarName = var.Name;
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                const char* floatOps[] = {"大于", "小于"};
                int currentOp = static_cast<int>(arg.op);
                if (ImGui::Combo("比较", &currentOp, floatOps, IM_ARRAYSIZE(floatOps)))
                {
                    arg.op = static_cast<FloatCondition::Comparison>(currentOp);
                    changed = true;
                }
                if (ImGui::DragFloat("值", &arg.Value))
                    changed = true;
            }
            else if constexpr (std::is_same_v<T, BoolCondition>)
            {
                if (ImGui::BeginCombo("Bool变量", arg.VarName.c_str()))
                {
                    for (const auto& var : m_controllerData.Variables)
                    {
                        if (var.Type == VariableType::VariableType_Bool)
                        {
                            if (ImGui::Selectable(var.Name.c_str(), arg.VarName == var.Name))
                            {
                                arg.VarName = var.Name;
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                const char* boolOps[] = {"为真", "为假"};
                int currentOp = static_cast<int>(arg.op);
                if (ImGui::Combo("比较", &currentOp, boolOps, IM_ARRAYSIZE(boolOps)))
                {
                    arg.op = static_cast<BoolCondition::Comparison>(currentOp);
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, IntCondition>)
            {
                if (ImGui::BeginCombo("Int变量", arg.VarName.c_str()))
                {
                    for (const auto& var : m_controllerData.Variables)
                    {
                        if (var.Type == VariableType::VariableType_Int)
                        {
                            if (ImGui::Selectable(var.Name.c_str(), arg.VarName == var.Name))
                            {
                                arg.VarName = var.Name;
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                const char* intOps[] = {"大于", "小于", "等于", "不等于"};
                int currentOp = static_cast<int>(arg.op);
                if (ImGui::Combo("比较", &currentOp, intOps, IM_ARRAYSIZE(intOps)))
                {
                    arg.op = static_cast<IntCondition::Comparison>(currentOp);
                    changed = true;
                }
                if (ImGui::DragInt("值", &arg.Value))
                    changed = true;
            }
            else if constexpr (std::is_same_v<T, TriggerCondition>)
            {
                if (ImGui::BeginCombo("Trigger变量", arg.VarName.c_str()))
                {
                    for (const auto& var : m_controllerData.Variables)
                    {
                        if (var.Type == VariableType::VariableType_Trigger)
                        {
                            if (ImGui::Selectable(var.Name.c_str(), arg.VarName == var.Name))
                            {
                                arg.VarName = var.Name;
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }, condition);
        ImGui::SameLine();
        if (ImGui::Button("删除"))
        {
            conditions.erase(conditions.begin() + i);
            ImGui::PopID();
            return true;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (ImGui::Button("添加Float条件"))
    {
        conditions.emplace_back(FloatCondition{FloatCondition::GreaterThan, "", 0.0f});
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("添加Bool条件"))
    {
        conditions.emplace_back(BoolCondition{BoolCondition::IsTrue, ""});
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("添加Int条件"))
    {
        conditions.emplace_back(IntCondition{IntCondition::Equal, "", 0});
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("添加Trigger条件"))
    {
        conditions.emplace_back(TriggerCondition{""});
        changed = true;
    }
    return changed;
}
AnimationControllerEditorPanel::ANode* AnimationControllerEditorPanel::findNodeByStateGuid(const Guid& stateGuid)
{
    auto it = m_stateToNodeIndex.find(stateGuid);
    if (it != m_stateToNodeIndex.end() && it->second < static_cast<int>(m_nodes.size()))
    {
        return &m_nodes[it->second];
    }
    return nullptr;
}
AnimationControllerEditorPanel::ALink* AnimationControllerEditorPanel::findLinkById(ed::LinkId linkId)
{
    for (auto& link : m_links)
    {
        if (link.id == linkId)
            return &link;
    }
    return nullptr;
}
AnimationControllerEditorPanel::ANode* AnimationControllerEditorPanel::findNodeById(ed::NodeId nodeId)
{
    for (auto& node : m_nodes)
    {
        if (node.id == nodeId)
            return &node;
    }
    return nullptr;
}
void AnimationControllerEditorPanel::createStateNode(const std::string& name, ImVec2 position, bool isEntry,
                                                     bool isDefault)
{
    ANode newNode;
    newNode.id = getNextNodeId();
    newNode.stateGuid = Guid::NewGuid();
    newNode.name = name;
    newNode.position = position;
    newNode.isEntry = isEntry;
    newNode.isDefault = isDefault;
    newNode.inputPinId = getNextPinId();
    newNode.outputPinId = getNextPinId();
    if (isEntry)
        newNode.color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    else if (isDefault)
        newNode.color = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);
    else
        newNode.color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    AnimationState newState;
    newState.stateName = name;
    m_controllerData.States[newNode.stateGuid] = newState;
    m_stateToNodeIndex[newNode.stateGuid] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(newNode);
    m_forceLayoutUpdate = true;
    markDirty();
    LogInfo("创建新状态节点: {}", name);
}
void AnimationControllerEditorPanel::createBlendTreeNode(const std::string& name, ImVec2 position)
{
    ANode newNode;
    newNode.id = getNextNodeId();
    // 混合树状态无绑定剪辑，状态GUID独立生成
    newNode.stateGuid = Guid::NewGuid();
    newNode.name = name;
    newNode.position = position;
    newNode.stateType = AnimationStateType::BlendTree;
    newNode.inputPinId = getNextPinId();
    newNode.outputPinId = getNextPinId();
    newNode.color = ImVec4(0.2f, 0.55f, 0.6f, 1.0f);
    AnimationState newState;
    newState.stateName = name;
    newState.stateType = AnimationStateType::BlendTree;
    newState.editorPosX = position.x;
    newState.editorPosY = position.y;
    newState.hasEditorPosition = true;
    m_controllerData.States[newNode.stateGuid] = newState;
    m_stateToNodeIndex[newNode.stateGuid] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(newNode);
    m_forceLayoutUpdate = true;
    markDirty();
    LogInfo("创建混合树状态节点: {}", name);
}
void AnimationControllerEditorPanel::deleteNode(ed::NodeId nodeId)
{
    ANode* nodeToDelete = findNodeById(nodeId);
    if (!nodeToDelete)
        return;
    Guid stateGuidToDelete = nodeToDelete->stateGuid;
    // 按状态唯一标识清理剪辑映射，状态名与剪辑名解耦后按名删除会误删或漏删
    for (auto it = m_controllerData.Clips.begin(); it != m_controllerData.Clips.end();)
    {
        if (it->second == stateGuidToDelete)
        {
            it = m_controllerData.Clips.erase(it);
        }
        else
        {
            ++it;
        }
    }
    std::erase_if(m_links,
                  [stateGuidToDelete](const ALink& link)
                  {
                      return link.fromStateGuid == stateGuidToDelete || link.toStateGuid ==
                          stateGuidToDelete;
                  });
    m_controllerData.States.erase(stateGuidToDelete);
    m_stateToNodeIndex.erase(stateGuidToDelete);
    std::erase_if(m_nodes,
                  [nodeId](const ANode& node)
                  {
                      return node.id == nodeId;
                  });
    m_stateToNodeIndex.clear();
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
        m_stateToNodeIndex[m_nodes[i].stateGuid] = static_cast<int>(i);
    }
    markDirty();
    LogInfo("删除状态节点");
}
void AnimationControllerEditorPanel::deleteLink(ed::LinkId linkId)
{
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                 [linkId](const ALink& link)
                                 {
                                     return link.id == linkId;
                                 }), m_links.end());
    markDirty();
    LogInfo("删除过渡连接");
}
void AnimationControllerEditorPanel::handleShortcutInput()
{
    if (!m_isFocused) return;
    // 边沿触发，避免按住 Ctrl+S 期间每帧写盘
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
    {
        saveToControllerData();
    }
    if (Keyboard::LeftCtrl.IsPressed() && Keyboard::W.IsPressed())
    {
        CloseCurrentController();
    }
}
void AnimationControllerEditorPanel::drawVariablesPanel()
{
    ImGui::Text("变量");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 240);
    if (ImGui::Button("Float"))
    {
        AnimationVariable newVar;
        newVar.Name = "新Float变量";
        newVar.Type = VariableType::VariableType_Float;
        newVar.Value = 0.0f;
        m_controllerData.Variables.push_back(newVar);
        markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Bool"))
    {
        AnimationVariable newVar;
        newVar.Name = "新Bool变量";
        newVar.Type = VariableType::VariableType_Bool;
        newVar.Value = false;
        m_controllerData.Variables.push_back(newVar);
        markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Int"))
    {
        AnimationVariable newVar;
        newVar.Name = "新Int变量";
        newVar.Type = VariableType::VariableType_Int;
        newVar.Value = 0;
        m_controllerData.Variables.push_back(newVar);
        markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Trigger"))
    {
        AnimationVariable newVar;
        newVar.Name = "新Trigger";
        newVar.Type = VariableType::VariableType_Trigger;
        newVar.Value = false;
        m_controllerData.Variables.push_back(newVar);
        markDirty();
    }
    ImGui::Separator();
    if (ImGui::BeginChild("VariablesList"))
    {
        for (size_t i = 0; i < m_controllerData.Variables.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            AnimationVariable& var = m_controllerData.Variables[i];
            // 编辑期间用成员缓冲累积输入，失焦时一次性提交改名，
            // 避免逐字符改名导致条件引用被中间名污染
            bool editingThisName = (m_editingVarIndex == static_cast<int>(i));
            char nameBuffer[256];
            char* inputBuffer;
            size_t inputBufferSize;
            if (editingThisName)
            {
                inputBuffer = m_varNameEditBuffer;
                inputBufferSize = sizeof(m_varNameEditBuffer);
            }
            else
            {
                strncpy(nameBuffer, var.Name.c_str(), sizeof(nameBuffer) - 1);
                nameBuffer[sizeof(nameBuffer) - 1] = '\0';
                inputBuffer = nameBuffer;
                inputBufferSize = sizeof(nameBuffer);
            }
            ImGui::SetNextItemWidth(150);
            ImGui::InputText("##VarName", inputBuffer, inputBufferSize);
            if (ImGui::IsItemActivated())
            {
                m_editingVarIndex = static_cast<int>(i);
                strncpy(m_varNameEditBuffer, var.Name.c_str(), sizeof(m_varNameEditBuffer) - 1);
                m_varNameEditBuffer[sizeof(m_varNameEditBuffer) - 1] = '\0';
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                if (m_editingVarIndex == static_cast<int>(i))
                {
                    std::string newName = m_varNameEditBuffer;
                    bool nameTaken = false;
                    for (size_t j = 0; j < m_controllerData.Variables.size(); ++j)
                    {
                        if (j != i && m_controllerData.Variables[j].Name == newName)
                        {
                            nameTaken = true;
                            break;
                        }
                    }
                    if (newName.empty())
                    {
                        LogWarn("变量名不能为空，重命名已取消");
                    }
                    else if (nameTaken)
                    {
                        LogWarn("变量名 {} 已存在，重命名已取消", newName);
                    }
                    else if (newName != var.Name)
                    {
                        // 同步所有过渡条件中对旧名的引用，避免改名后条件静默失效
                        renameVariableReferences(var.Name, newName);
                        var.Name = newName;
                        markDirty();
                    }
                }
                m_editingVarIndex = -1;
            }
            else if (ImGui::IsItemDeactivated() && m_editingVarIndex == static_cast<int>(i))
            {
                m_editingVarIndex = -1;
            }
            ImGui::SameLine();
            switch (var.Type)
            {
            case VariableType::VariableType_Float:
                {
                    float value = std::get<float>(var.Value);
                    if (ImGui::DragFloat("值", &value))
                    {
                        var.Value = value;
                        markDirty();
                    }
                    break;
                }
            case VariableType::VariableType_Bool:
                {
                    bool value = std::get<bool>(var.Value);
                    if (ImGui::Checkbox("值", &value))
                    {
                        var.Value = value;
                        markDirty();
                    }
                    break;
                }
            case VariableType::VariableType_Int:
                {
                    int value = std::get<int>(var.Value);
                    if (ImGui::DragInt("值", &value))
                    {
                        var.Value = value;
                        markDirty();
                    }
                    break;
                }
            case VariableType::VariableType_Trigger:
                {
                    ImGui::TextDisabled("(Trigger)");
                    break;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("删除"))
            {
                int refCount = countVariableReferences(var.Name);
                if (refCount > 0)
                {
                    // 被条件引用时先弹确认，确认后连带删除引用条件
                    m_pendingDeleteVarIndex = static_cast<int>(i);
                    m_pendingDeleteVarName = var.Name;
                    m_deleteVarConfirmOpen = true;
                }
                else
                {
                    m_controllerData.Variables.erase(m_controllerData.Variables.begin() + i);
                    markDirty();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
bool AnimationControllerEditorPanel::tryGetRuntimePlaybackStatus(
    RuntimeAnimationController::PlaybackStatus& outStatus)
{
    // 播放模式下查询选中实体的运行时播放状态，供节点高亮与混合树权重实况共用
    if (!m_context || m_context->editorState != EditorState::Playing ||
        m_context->selectionType != SelectionType::GameObject ||
        m_context->selectionList.empty() || !m_context->activeScene)
    {
        return false;
    }
    RuntimeGameObject selectedGO = m_context->activeScene->FindGameObjectByGuid(m_context->selectionList[0]);
    if (!selectedGO.IsValid() || !selectedGO.HasComponent<ECS::AnimationControllerComponent>())
    {
        return false;
    }
    auto& animComp = selectedGO.GetComponent<ECS::AnimationControllerComponent>();
    if (!animComp.runtimeController ||
        animComp.animationController.assetGuid != m_currentControllerGuid)
    {
        return false;
    }
    outStatus = animComp.runtimeController->GetPlaybackStatus();
    return true;
}
void AnimationControllerEditorPanel::drawNodeEditor()
{
    RuntimeAnimationController::PlaybackStatus runtimeStatus;
    bool runtimeStatusValid = tryGetRuntimePlaybackStatus(runtimeStatus);
    ed::SetCurrentEditor(m_nodeEditorContext);
    ed::Begin("AnimationStateMachine");
    if (m_forceLayoutUpdate)
    {
        for (const auto& node : m_nodes)
        {
            ed::SetNodePosition(node.id, node.position);
        }
        m_forceLayoutUpdate = false;
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DRAG_DROP_ASSET_HANDLE"))
        {
            AssetHandle assetHandle = *static_cast<const AssetHandle*>(payload->Data);
            if (assetHandle.assetType == AssetType::AnimationClip)
            {
                ImVec2 nodePosition = ed::ScreenToCanvas(ImGui::GetMousePos());
                handleAnimationClipDrop(assetHandle, nodePosition);
            }
        }
        ImGui::EndDragDropTarget();
    }
    for (auto& node : m_nodes)
    {
        // 当前状态绿色描边，过渡目标橙色描边
        bool highlighted = false;
        if (runtimeStatusValid && node.type == NodeType::State)
        {
            if (runtimeStatus.isTransitioning && node.stateGuid == runtimeStatus.targetStateGuid)
            {
                ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(1.0f, 0.6f, 0.1f, 1.0f));
                ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 3.5f);
                highlighted = true;
            }
            else if (node.stateGuid == runtimeStatus.currentStateGuid)
            {
                ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.2f, 1.0f, 0.3f, 1.0f));
                ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 3.5f);
                highlighted = true;
            }
        }
        ed::BeginNode(node.id);
        ImGui::PushID(node.id.Get());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, node.color);
        if (ImGui::Button(node.name.c_str(), ImVec2(120, 0)))
        {
        }
        ImGui::PopStyleColor(2);
        if (node.type == NodeType::State && ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (node.stateType == AnimationStateType::BlendTree)
            {
                openBlendTreeEditorWindow(node);
            }
            else
            {
                openStatePropertiesWindow(node);
            }
        }
        if (node.type == NodeType::State)
        {
            ed::BeginPin(node.inputPinId, ed::PinKind::Input);
            ImGui::Text("-> 输入");
            ed::EndPin();
            ImGui::SameLine();
        }
        ed::BeginPin(node.outputPinId, ed::PinKind::Output);
        ImGui::Text("输出 ->");
        ed::EndPin();
        if (node.isEntry)
        {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "[Entry]");
        }
        if (node.isDefault)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "[Default]");
        }
        if (node.stateType == AnimationStateType::BlendTree)
        {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.9f, 1.0f), "[混合树 x%d]",
                               static_cast<int>(node.blendTree.children.size()));
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DRAG_DROP_ASSET_HANDLE"))
            {
                AssetHandle assetHandle = *static_cast<const AssetHandle*>(payload->Data);
                if (assetHandle.assetType == AssetType::AnimationClip)
                {
                    handleAnimationClipDropOnNode(assetHandle, node);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        ed::EndNode();
        if (highlighted)
        {
            ed::PopStyleVar();
            ed::PopStyleColor();
        }
        // 拖动节点后同步位置并置脏，保存时随数据持久化
        ImVec2 canvasPosition = ed::GetNodePosition(node.id);
        if (std::fabs(canvasPosition.x - node.position.x) > 0.01f ||
            std::fabs(canvasPosition.y - node.position.y) > 0.01f)
        {
            node.position = canvasPosition;
            markDirty();
        }
    }
    for (const auto& link : m_links)
    {
        ed::Link(link.id, link.startPinId, link.endPinId);
    }
    if (ed::BeginCreate())
    {
        ed::PinId startPinId, endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId))
        {
            if (startPinId && endPinId)
            {
                ANode* startNode = nullptr;
                ANode* endNode = nullptr;
                for (auto& node : m_nodes)
                {
                    if (node.outputPinId == startPinId) startNode = &node;
                    if (node.inputPinId == endPinId) endNode = &node;
                }
                if (startNode && endNode && startNode != endNode)
                {
                    if (ed::AcceptNewItem())
                    {
                        ALink newLink;
                        newLink.id = getNextLinkId();
                        newLink.startPinId = startPinId;
                        newLink.endPinId = endPinId;
                        newLink.fromStateGuid = startNode->stateGuid;
                        newLink.toStateGuid = endNode->stateGuid;
                        newLink.transitionName = "新过渡";
                        newLink.duration = 0.3f;
                        m_links.push_back(newLink);
                        markDirty();
                        LogInfo("创建过渡: {} -> {}", startNode->name, endNode->name);
                    }
                }
                else
                {
                    ed::RejectNewItem();
                }
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
            if (ed::AcceptDeletedItem()) { deleteNode(deletedNodeId); }
        }
    }
    ed::EndDelete();
    ed::LinkId doubleClickedLinkId = ed::GetDoubleClickedLink();
    ALink* clickedLink = findLinkById(doubleClickedLinkId);
    if (clickedLink)
    {
        m_editingLinkIndex = static_cast<int>(clickedLink - m_links.data());
        m_transitionEditWindowOpen = true;
    }
    ed::NodeId doubleClickedNodeId = ed::GetDoubleClickedNode();
    if (ANode* doubleClickedNode = findNodeById(doubleClickedNodeId);
        doubleClickedNode && doubleClickedNode->type == NodeType::State)
    {
        if (doubleClickedNode->stateType == AnimationStateType::BlendTree)
        {
            openBlendTreeEditorWindow(*doubleClickedNode);
        }
        else
        {
            openStatePropertiesWindow(*doubleClickedNode);
        }
    }
    ed::Suspend();
    ed::NodeId contextNodeId = 0;
    ed::LinkId contextLinkId = 0;
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
    drawNodeContextMenu();
    drawLinkContextMenu();
    if (ImGui::BeginPopup("CreateNodeMenu"))
    {
        if (ImGui::MenuItem("创建状态"))
        {
            ImVec2 mousePos = ImGui::GetMousePosOnOpeningCurrentPopup();
            createStateNode("新状态", ed::ScreenToCanvas(mousePos));
        }
        if (ImGui::MenuItem("创建混合树状态"))
        {
            ImVec2 mousePos = ImGui::GetMousePosOnOpeningCurrentPopup();
            createBlendTreeNode("新混合树", ed::ScreenToCanvas(mousePos));
        }
        ImGui::EndPopup();
    }
    ed::Resume();
    ed::End();
}
void AnimationControllerEditorPanel::handleAnimationClipDrop(const AssetHandle& assetHandle, ImVec2 nodePosition)
{
    if (findNodeByStateGuid(assetHandle.assetGuid))
    {
        LogWarn("动画剪辑 {} 已经存在于状态图中", assetHandle.assetGuid.ToString());
        return;
    }
    auto loader = AnimationClipLoader();
    auto clip = loader.LoadAsset(assetHandle.assetGuid);
    std::string clipName = clip->GetName();
    ANode newNode;
    newNode.id = getNextNodeId();
    newNode.stateGuid = assetHandle.assetGuid;
    newNode.name = clipName;
    newNode.position = nodePosition;
    newNode.isEntry = false;
    newNode.isDefault = m_nodes.empty();
    newNode.inputPinId = getNextPinId();
    newNode.outputPinId = getNextPinId();
    newNode.color = newNode.isDefault ? ImVec4(0.0f, 0.0f, 1.0f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    AnimationState newState;
    newState.stateName = clipName;
    newState.editorPosX = nodePosition.x;
    newState.editorPosY = nodePosition.y;
    newState.hasEditorPosition = true;
    m_controllerData.States[newNode.stateGuid] = newState;
    m_controllerData.Clips[clipName] = assetHandle.assetGuid;
    m_stateToNodeIndex[newNode.stateGuid] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(newNode);
    m_forceLayoutUpdate = true;
    markDirty();
    if (m_nodes.size() == 3)
    {
        ANode* entryNode = findNodeByStateGuid(SpecialStateGuids::Entry());
        if (entryNode)
        {
            ALink newLink;
            newLink.id = getNextLinkId();
            newLink.startPinId = entryNode->outputPinId;
            newLink.endPinId = newNode.inputPinId;
            newLink.fromStateGuid = entryNode->stateGuid;
            newLink.toStateGuid = newNode.stateGuid;
            newLink.transitionName = "入口过渡";
            newLink.duration = 0.0f;
            m_links.push_back(newLink);
        }
    }
    LogInfo("从拖拽创建状态节点: {} (GUID: {})", clipName, assetHandle.assetGuid.ToString());
}
void AnimationControllerEditorPanel::handleAnimationClipDropOnNode(const AssetHandle& assetHandle, ANode& targetNode)
{
    // 拖剪辑到混合树节点 = 添加子项，不改变节点自身绑定
    if (targetNode.stateType == AnimationStateType::BlendTree)
    {
        BlendTreeData::Child child;
        child.clipGuid = assetHandle.assetGuid;
        child.threshold = targetNode.blendTree.children.empty()
                              ? 0.0f
                              : targetNode.blendTree.children.back().threshold + 1.0f;
        targetNode.blendTree.children.push_back(child);
        sortBlendTreeChildren(targetNode.blendTree);
        markDirty();
        LogInfo("向混合树 {} 添加子项: {}", targetNode.name, getClipDisplayName(assetHandle.assetGuid));
        return;
    }
    const Guid& newGuid = assetHandle.assetGuid;
    const Guid& oldGuid = targetNode.stateGuid;
    if (newGuid == oldGuid)
    {
        return;
    }
    ANode* existingNode = findNodeByStateGuid(newGuid);
    if (existingNode && existingNode->id != targetNode.id)
    {
        LogError("无法关联动画剪辑 {}，因为它已经关联到另一个状态节点 {}", newGuid.ToString(), existingNode->name);
        return;
    }
    auto loader = AnimationClipLoader();
    auto clip = loader.LoadAsset(newGuid);
    std::string newClipName = clip->GetName();
    targetNode.stateGuid = newGuid;
    if (m_controllerData.States.count(oldGuid))
    {
        auto stateData = m_controllerData.States.at(oldGuid);
        m_controllerData.States.erase(oldGuid);
        m_controllerData.States[newGuid] = stateData;
    }
    else
    {
        m_controllerData.States[newGuid] = AnimationState();
    }
    std::string oldClipName;
    for (auto it = m_controllerData.Clips.begin(); it != m_controllerData.Clips.end();)
    {
        if (it->second == oldGuid)
        {
            oldClipName = it->first;
            it = m_controllerData.Clips.erase(it);
        }
        else
        {
            ++it;
        }
    }
    m_controllerData.Clips[newClipName] = newGuid;
    // 仅当状态未被用户重命名（仍等于旧剪辑名或为空）时才跟随新剪辑名
    if (targetNode.name.empty() || targetNode.name == oldClipName)
    {
        targetNode.name = newClipName;
        m_controllerData.States[newGuid].stateName = newClipName;
    }
    for (ALink& link : m_links)
    {
        if (link.fromStateGuid == oldGuid)
        {
            link.fromStateGuid = newGuid;
        }
        if (link.toStateGuid == oldGuid)
        {
            link.toStateGuid = newGuid;
        }
    }
    if (m_stateToNodeIndex.count(oldGuid))
    {
        int nodeIndex = m_stateToNodeIndex.at(oldGuid);
        m_stateToNodeIndex.erase(oldGuid);
        m_stateToNodeIndex[newGuid] = nodeIndex;
    }
    markDirty();
    LogInfo("将节点 {} 的动画剪辑更新为 {}", targetNode.name, newClipName);
}

void AnimationControllerEditorPanel::openStatePropertiesWindow(const ANode& node)
{
    m_renamingNodeId = node.id;
    strncpy(m_renameBuffer, node.name.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_statePropertiesWindowOpen = true;
}

void AnimationControllerEditorPanel::drawStatePropertiesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("状态属性", &m_statePropertiesWindowOpen))
    {
        ImGui::End();
        return;
    }
    ANode* node = findNodeById(m_renamingNodeId);
    if (!node || node->type != NodeType::State)
    {
        ImGui::Text("无效的状态");
        ImGui::End();
        return;
    }
    ImGui::InputText("状态名称", m_renameBuffer, sizeof(m_renameBuffer));
    if (ImGui::DragFloat("播放速度", &node->speed, 0.01f, 0.01f, 10.0f, "%.2fx"))
    {
        markDirty();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("运行时播放该状态动画时乘上的速度倍率。");
    }
    if (ImGui::Button("确定"))
    {
        std::string newName = m_renameBuffer;
        if (!newName.empty() && newName != node->name)
        {
            node->name = newName;
            markDirty();
        }
        m_statePropertiesWindowOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消"))
    {
        m_statePropertiesWindowOpen = false;
    }
    ImGui::End();
}

void AnimationControllerEditorPanel::drawVariableDeleteConfirmWindow()
{
    // 索引可能因增删失效，先按“索引+名字”校验，失败则按名字重新定位
    int varIndex = -1;
    if (m_pendingDeleteVarIndex >= 0 &&
        m_pendingDeleteVarIndex < static_cast<int>(m_controllerData.Variables.size()) &&
        m_controllerData.Variables[m_pendingDeleteVarIndex].Name == m_pendingDeleteVarName)
    {
        varIndex = m_pendingDeleteVarIndex;
    }
    else
    {
        for (size_t i = 0; i < m_controllerData.Variables.size(); ++i)
        {
            if (m_controllerData.Variables[i].Name == m_pendingDeleteVarName)
            {
                varIndex = static_cast<int>(i);
                break;
            }
        }
    }
    if (varIndex < 0)
    {
        m_deleteVarConfirmOpen = false;
        m_pendingDeleteVarIndex = -1;
        m_pendingDeleteVarName.clear();
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("删除变量确认", &m_deleteVarConfirmOpen))
    {
        ImGui::End();
        return;
    }
    int refCount = countVariableReferences(m_pendingDeleteVarName);
    ImGui::TextWrapped("变量 \"%s\" 正被 %d 个过渡条件引用。", m_pendingDeleteVarName.c_str(), refCount);
    ImGui::TextWrapped("删除该变量将同时删除所有引用它的条件。");
    ImGui::Separator();
    if (ImGui::Button("确认删除"))
    {
        removeConditionsReferencing(m_pendingDeleteVarName);
        m_controllerData.Variables.erase(m_controllerData.Variables.begin() + varIndex);
        markDirty();
        LogInfo("删除变量 {} 及其 {} 个引用条件", m_pendingDeleteVarName, refCount);
        m_deleteVarConfirmOpen = false;
        m_pendingDeleteVarIndex = -1;
        m_pendingDeleteVarName.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消"))
    {
        m_deleteVarConfirmOpen = false;
        m_pendingDeleteVarIndex = -1;
        m_pendingDeleteVarName.clear();
    }
    ImGui::End();
}

void AnimationControllerEditorPanel::renameVariableReferences(const std::string& oldName, const std::string& newName)
{
    int updatedCount = 0;
    for (ALink& link : m_links)
    {
        for (Condition& condition : link.conditions)
        {
            std::visit([&](auto& arg)
            {
                if (arg.VarName == oldName)
                {
                    arg.VarName = newName;
                    ++updatedCount;
                }
            }, condition);
        }
    }
    if (updatedCount > 0)
    {
        LogInfo("变量重命名 {} -> {}，同步更新 {} 个条件引用", oldName, newName, updatedCount);
    }
}

int AnimationControllerEditorPanel::countVariableReferences(const std::string& name) const
{
    int count = 0;
    for (const ALink& link : m_links)
    {
        for (const Condition& condition : link.conditions)
        {
            std::visit([&](const auto& arg)
            {
                if (arg.VarName == name)
                {
                    ++count;
                }
            }, condition);
        }
    }
    return count;
}

void AnimationControllerEditorPanel::removeConditionsReferencing(const std::string& name)
{
    for (ALink& link : m_links)
    {
        std::erase_if(link.conditions, [&name](const Condition& condition)
        {
            bool matches = false;
            std::visit([&](const auto& arg)
            {
                matches = (arg.VarName == name);
            }, condition);
            return matches;
        });
    }
}

void AnimationControllerEditorPanel::sortBlendTreeChildren(BlendTreeData& tree)
{
    std::stable_sort(tree.children.begin(), tree.children.end(),
                     [](const BlendTreeData::Child& a, const BlendTreeData::Child& b)
                     {
                         return a.threshold < b.threshold;
                     });
}

std::string AnimationControllerEditorPanel::getClipDisplayName(const Guid& clipGuid) const
{
    if (!clipGuid.Valid())
    {
        return "(未设置)";
    }
    if (const AssetMetadata* meta = AssetManager::GetInstance().GetMetadata(clipGuid))
    {
        return Path::GetFileNameWithoutExtension(meta->assetPath.string());
    }
    // 资产已被删除或移动，保留GUID前缀便于排查
    return "(丢失: " + clipGuid.ToString().substr(0, 8) + ")";
}

// 与运行时 evaluateBlendTree 同一套一维混合规则，此处展开为逐子项权重供预览条使用
static std::vector<float> computeBlendTreeWeights(const BlendTreeData& tree, float value)
{
    std::vector<float> weights(tree.children.size(), 0.0f);
    if (weights.empty())
    {
        return weights;
    }
    const auto& children = tree.children;
    if (children.size() == 1 || value <= children.front().threshold)
    {
        weights.front() = 1.0f;
        return weights;
    }
    if (value >= children.back().threshold)
    {
        weights.back() = 1.0f;
        return weights;
    }
    for (size_t i = 0; i + 1 < children.size(); ++i)
    {
        if (value > children[i + 1].threshold)
        {
            continue;
        }
        float range = children[i + 1].threshold - children[i].threshold;
        float t = range > 0.0f ? (value - children[i].threshold) / range : 0.0f;
        weights[i] = 1.0f - t;
        weights[i + 1] = t;
        break;
    }
    return weights;
}

void AnimationControllerEditorPanel::openBlendTreeEditorWindow(const ANode& node)
{
    m_blendTreeNodeId = node.id;
    // 预览滑条初值取绑定参数的当前编辑值，打开即落在有意义的位置
    for (const auto& var : m_controllerData.Variables)
    {
        if (var.Type == VariableType::VariableType_Float && var.Name == node.blendTree.parameterName &&
            std::holds_alternative<float>(var.Value))
        {
            m_blendTreePreviewValue = std::get<float>(var.Value);
            break;
        }
    }
    m_blendTreeWindowOpen = true;
}

void AnimationControllerEditorPanel::drawBlendTreeEditorWindow()
{
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("混合树编辑器", &m_blendTreeWindowOpen))
    {
        ImGui::End();
        return;
    }
    ANode* node = findNodeById(m_blendTreeNodeId);
    if (!node || node->stateType != AnimationStateType::BlendTree)
    {
        ImGui::Text("无效的混合树状态");
        ImGui::End();
        return;
    }
    BlendTreeData& tree = node->blendTree;
    ImGui::Text("混合树: %s", node->name.c_str());
    ImGui::Separator();
    if (ImGui::BeginCombo("混合参数", tree.parameterName.empty() ? "(未选择)" : tree.parameterName.c_str()))
    {
        for (const auto& var : m_controllerData.Variables)
        {
            if (var.Type != VariableType::VariableType_Float)
            {
                continue;
            }
            if (ImGui::Selectable(var.Name.c_str(), tree.parameterName == var.Name))
            {
                tree.parameterName = var.Name;
                markDirty();
            }
        }
        ImGui::EndCombo();
    }
    if (tree.parameterName.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "未绑定 float 参数，运行时按参数值 0 求值");
    }
    ImGui::Separator();
    ImGui::Text("子项（剪辑 + 阈值）");
    int deleteIndex = -1;
    for (size_t i = 0; i < tree.children.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        BlendTreeData::Child& child = tree.children[i];
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("##Clip", getClipDisplayName(child.clipGuid).c_str()))
        {
            for (const auto& [key, meta] : AssetManager::GetInstance().GetAssetDatabase())
            {
                if (meta.type != AssetType::AnimationClip)
                {
                    continue;
                }
                std::string itemLabel = Path::GetFileNameWithoutExtension(meta.assetPath.string()) +
                    "##" + meta.guid.ToString();
                if (ImGui::Selectable(itemLabel.c_str(), child.clipGuid == meta.guid))
                {
                    child.clipGuid = meta.guid;
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::DragFloat("##Threshold", &child.threshold, 0.05f, 0.0f, 0.0f, "%.2f"))
        {
            markDirty();
        }
        // 拖动结束再按阈值重排，避免拖动过程中行序跳变
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            sortBlendTreeChildren(tree);
        }
        ImGui::SameLine();
        if (ImGui::Button("删除"))
        {
            deleteIndex = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (deleteIndex >= 0)
    {
        tree.children.erase(tree.children.begin() + deleteIndex);
        markDirty();
    }
    if (ImGui::Button("添加子项"))
    {
        BlendTreeData::Child child;
        child.threshold = tree.children.empty() ? 0.0f : tree.children.back().threshold + 1.0f;
        tree.children.push_back(child);
        markDirty();
    }
    ImGui::Separator();
    ImGui::Text("权重预览");
    if (tree.children.empty())
    {
        ImGui::TextDisabled("暂无子项");
        ImGui::End();
        return;
    }
    // 播放中且运行时活跃状态就是本混合树时，改用运行时实况权重
    RuntimeAnimationController::PlaybackStatus status;
    bool liveWeights = tryGetRuntimePlaybackStatus(status) && status.isBlendTreeState &&
        ((!status.isTransitioning && status.currentStateGuid == node->stateGuid) ||
         (status.isTransitioning && status.targetStateGuid == node->stateGuid));
    std::vector<float> weights;
    if (liveWeights)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "运行时实况: %s = %.3f",
                           tree.parameterName.c_str(), status.blendParameterValue);
        weights.assign(tree.children.size(), 0.0f);
        if (status.activeChildIndex >= 0 && status.activeChildIndex < static_cast<int>(weights.size()))
        {
            weights[status.activeChildIndex] = status.blendWeight;
        }
        if (status.secondaryChildIndex >= 0 && status.secondaryChildIndex < static_cast<int>(weights.size()))
        {
            weights[status.secondaryChildIndex] = 1.0f - status.blendWeight;
        }
    }
    else
    {
        float minThreshold = tree.children.front().threshold;
        float maxThreshold = tree.children.back().threshold;
        if (maxThreshold <= minThreshold)
        {
            maxThreshold = minThreshold + 1.0f;
        }
        m_blendTreePreviewValue = std::clamp(m_blendTreePreviewValue, minThreshold, maxThreshold);
        ImGui::SliderFloat("参数预览值", &m_blendTreePreviewValue, minThreshold, maxThreshold, "%.3f");
        weights = computeBlendTreeWeights(tree, m_blendTreePreviewValue);
    }
    for (size_t i = 0; i < tree.children.size(); ++i)
    {
        char overlay[160];
        snprintf(overlay, sizeof(overlay), "%s @ %.2f  |  %.1f%%",
                 getClipDisplayName(tree.children[i].clipGuid).c_str(),
                 tree.children[i].threshold, weights[i] * 100.0f);
        ImGui::ProgressBar(weights[i], ImVec2(-1, 0), overlay);
    }
    ImGui::End();
}
