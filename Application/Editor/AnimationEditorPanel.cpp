#include "AnimationEditorPanel.h"
#include "../Utils/Logger.h"
#include "../Resources/Loaders/AnimationClipLoader.h"
#include "../Utils/PopupManager.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include "../Resources/RuntimeAsset/RuntimeScene.h"
#include "AnimationTrackSampler.h"
#include "AssetManager.h"
#include "ComponentRegistry.h"
#include "Profiler.h"
#include "SceneManager.h"
#include "Sprite.h"
#include "Loaders/TextureLoader.h"
void AnimationEditorPanel::Initialize(EditorContext* context)
{
    m_context = context;
    m_textureLoader = std::make_unique<TextureLoader>(*m_context->graphicsBackend);
    m_totalFrames = 60;
    PopupManager::GetInstance().Register("动画未保存修改", [this]() { this->drawUnsavedChangesPopup(); }, true,
                                         ImGuiWindowFlags_AlwaysAutoResize);
}
void AnimationEditorPanel::Update(float deltaTime)
{
    PROFILE_FUNCTION();
    if (!m_isVisible)
        return;
    // 确认弹窗被 X 直接关闭（回调不再执行）时视作"取消"，解除挂起状态
    if (m_pendingCloseAction != PendingCloseAction::None &&
        ImGui::GetFrameCount() - m_confirmPopupVisibleFrame > 2)
    {
        m_pendingCloseAction = PendingCloseAction::None;
        m_pendingOpenClipGuid = Guid();
    }
    const Guid requestedGuid = m_context->currentEditingAnimationClipGuid;
    if (requestedGuid.Valid() && requestedGuid != m_currentClipGuid)
    {
        if (m_currentClip && m_isDirty && m_pendingCloseAction == PendingCloseAction::None)
        {
            // 外部请求切换剪辑但有未保存修改：挂起请求并弹确认
            m_pendingOpenClipGuid = requestedGuid;
            m_context->currentEditingAnimationClipGuid = m_currentClipGuid;
            openUnsavedConfirmPopup(PendingCloseAction::OpenOther);
        }
        else if (m_pendingCloseAction == PendingCloseAction::None)
        {
            openAnimationClipFromContext(requestedGuid);
        }
    }
    if (!requestedGuid.Valid() && m_currentClip)
    {
        if (m_isDirty && m_pendingCloseAction == PendingCloseAction::None)
        {
            m_context->currentEditingAnimationClipGuid = m_currentClipGuid;
            openUnsavedConfirmPopup(PendingCloseAction::CloseClip);
        }
        else if (m_pendingCloseAction == PendingCloseAction::None)
        {
            closeCurrentClipFromContext();
        }
    }
    updateTargetObject();
    if (m_currentClip)
    {
        // 录制对比先于预览应用：预览写入通过缓存刷新吸收，不会被误判为用户修改
        updateRecording();
        updatePlayback(deltaTime);
    }
}
void AnimationEditorPanel::Shutdown()
{
    restorePreviewTargetState();
    CloseCurrentClip();
}
void AnimationEditorPanel::OpenAnimationClip(const Guid& clipGuid)
{
    m_context->currentEditingAnimationClipGuid = clipGuid;
}
void AnimationEditorPanel::CloseCurrentClip()
{
    m_context->currentEditingAnimationClipGuid = Guid();
}
void AnimationEditorPanel::Focus()
{
    m_isVisible = true;
    m_requestFocus = true;
}
void AnimationEditorPanel::openAnimationClipFromContext(const Guid& clipGuid)
{
    if (m_currentClipGuid == clipGuid && m_currentClip)
        return;
    closeCurrentClipFromContext();
    auto loader = AnimationClipLoader();
    m_currentClip = loader.LoadAsset(clipGuid);
    if (!m_currentClip)
    {
        LogError("无法加载动画切片，GUID: {}", clipGuid.ToString());
        m_context->currentEditingAnimationClipGuid = Guid();
        return;
    }
    m_currentClipGuid = clipGuid;
    m_currentClipName = m_currentClip->getAnimationClip().Name;
    m_targetObjectGuid = m_currentClip->getAnimationClip().TargetEntityGuid;
    m_frameRate = m_currentClip->getAnimationClip().FrameRate;
    m_isLooping = m_currentClip->getAnimationClip().IsLooping;
    m_currentTime = 0.0f;
    m_currentFrame = 0;
    m_totalFrames = 60;
    for (const auto& [frameIndex, frame] : m_currentClip->getAnimationClip().Frames)
    {
        m_totalFrames = std::max(m_totalFrames, frameIndex + 1);
    }
    for (const auto& track : m_currentClip->getAnimationClip().PropertyTracks)
    {
        for (const auto& key : track.keyframes)
        {
            m_totalFrames = std::max(m_totalFrames, key.frame + 1);
        }
    }
    m_multiSelectedFrames.clear();
    m_frameEditWindowOpen = false;
    m_isDirty = false;
    m_undoStack.clear();
    m_redoStack.clear();
    resetPropertyTrackEditState();
    LogInfo("打开动画切片进行编辑: {}", m_currentClipName);
}
void AnimationEditorPanel::closeCurrentClipFromContext()
{
    if (!m_currentClip)
        return;
    restorePreviewTargetState();
    LogInfo("关闭动画切片: {}", m_currentClipName);
    m_currentClip = nullptr;
    m_currentClipGuid = Guid();
    m_currentClipName.clear();
    m_targetObjectGuid = Guid();
    m_targetObjectName.clear();
    m_currentTime = 0.0f;
    m_currentFrame = 0;
    m_totalFrames = 60;
    m_isPlaying = false;
    m_multiSelectedFrames.clear();
    m_frameEditWindowOpen = false;
    m_isDirty = false;
    m_undoStack.clear();
    m_redoStack.clear();
    resetPropertyTrackEditState();
}
void AnimationEditorPanel::createNewAnimation()
{
    closeCurrentClipFromContext();
    AnimationClip newClip;
    newClip.Name = "新动画";
    if (!m_context->selectionList.empty() && m_context->selectionType == SelectionType::GameObject)
    {
        newClip.TargetEntityGuid = m_context->selectionList[0];
    }
    else
    {
        newClip.TargetEntityGuid = Guid::NewGuid();
    }
    Guid newGuid = Guid::NewGuid();
    m_currentClip = sk_make_sp<RuntimeAnimationClip>(newGuid, newClip);
    m_currentClipGuid = newGuid;
    m_currentClipName = newClip.Name;
    m_targetObjectGuid = newClip.TargetEntityGuid;
    m_frameRate = newClip.FrameRate;
    m_isLooping = newClip.IsLooping;
    m_currentTime = 0.0f;
    m_currentFrame = 0;
    m_totalFrames = 60;
    m_selectedFrameIndex = -1;
    // 新建剪辑尚未落盘，标记为脏以便关闭时提示
    m_isDirty = true;
    m_context->currentEditingAnimationClipGuid = newGuid;
    LogInfo("创建新动画: {}", m_currentClipName);
}
void AnimationEditorPanel::drawTargetObjectSelector()
{
    ImGui::Text("目标物体:");
    ImGui::SameLine();
    if (hasValidTargetObject())
    {
        ImGui::Text("%s", m_targetObjectName.c_str());
        ImGui::SameLine();
        if (ImGui::Button("选中"))
        {
            m_context->selectionType = SelectionType::GameObject;
            m_context->selectionList.clear();
            m_context->selectionList.push_back(m_targetObjectGuid);
            m_context->selectionAnchor = m_targetObjectGuid;
            m_context->objectToFocusInHierarchy = m_targetObjectGuid;
        }
    }
    else
    {
        ImGui::Text("没有有效的目标物体");
    }
    ImGui::SameLine();
    if (ImGui::Button("从选中设置"))
    {
        if (!m_context->selectionList.empty() && m_context->selectionType == SelectionType::GameObject)
        {
            // 切换目标前还原上一个对象被预览污染的组件状态
            restorePreviewTargetState();
            m_targetObjectGuid = m_context->selectionList[0];
            if (m_currentClip)
            {
                pushUndoSnapshot();
                m_currentClip->getAnimationClip().TargetEntityGuid = m_targetObjectGuid;
                markDirty();
            }
            LogInfo("设置目标物体为当前选中的物体");
        }
        else
        {
            LogWarn("请先在层级面板中选择一个物体");
        }
    }
}
void AnimationEditorPanel::drawControlPanel()
{
    if (m_currentClip)
    {
        char nameBuffer[256];
        strncpy(nameBuffer, m_currentClipName.c_str(), sizeof(nameBuffer));
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
        ImGui::Text("动画名称:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        const bool nameChanged = ImGui::InputText("##AnimationNameEditor", nameBuffer, sizeof(nameBuffer));
        if (ImGui::IsItemActivated())
        {
            // 开始编辑名称时压栈一次，避免每个字符一条撤销记录
            pushUndoSnapshot();
        }
        if (nameChanged)
        {
            m_currentClipName = nameBuffer;
            m_currentClip->getAnimationClip().Name = m_currentClipName;
            markDirty();
        }
    }
    else
    {
        ImGui::Text("没有打开的动画");
    }
    ImGui::SameLine();
    if (ImGui::Button(m_isPlaying ? "暂停" : "播放"))
    {
        m_isPlaying = !m_isPlaying;
    }
    ImGui::SameLine();
    if (ImGui::Button("停止"))
    {
        stopPreviewPlayback();
    }
    ImGui::SameLine();
    if (ImGui::Button("前一帧"))
    {
        seekToFrame(std::max(0, m_currentFrame - 1));
    }
    ImGui::SameLine();
    if (ImGui::Button("后一帧"))
    {
        seekToFrame(std::min(m_totalFrames - 1, m_currentFrame + 1));
    }
    ImGui::SameLine();
    bool looping = m_isLooping;
    if (ImGui::Checkbox("循环", &looping))
    {
        if (m_currentClip)
        {
            pushUndoSnapshot();
            m_currentClip->getAnimationClip().IsLooping = looping;
            markDirty();
        }
        m_isLooping = looping;
    }
    ImGui::SameLine();
    ImGui::Checkbox("洋葱皮", &m_onionSkinEnabled);
    ImGui::SetNextItemWidth(100);
    float frameRate = m_frameRate;
    const bool frameRateChanged = ImGui::DragFloat("帧率", &frameRate, 1.0f, 1.0f, 120.0f, "%.1f");
    if (m_currentClip && ImGui::IsItemActivated())
    {
        // 拖拽帧率为连续操作，按下时压栈一次
        pushUndoSnapshot();
    }
    if (frameRateChanged)
    {
        m_frameRate = std::clamp(frameRate, 1.0f, 120.0f);
        if (m_currentClip)
        {
            m_currentClip->getAnimationClip().FrameRate = m_frameRate;
            markDirty();
        }
    }
    ImGui::SameLine();
    ImGui::Text("当前帧: %d / %d", m_currentFrame, m_totalFrames);
    ImGui::SetNextItemWidth(100);
    if (ImGui::DragInt("总帧数", &m_totalFrames, 1.0f, 1, 1000))
    {
        m_totalFrames = std::clamp(m_totalFrames, 1, 1000);
        m_currentFrame = std::clamp(m_currentFrame, 0, m_totalFrames - 1);
    }
    float maxTime = static_cast<float>(m_totalFrames) / m_frameRate;
    if (ImGui::SliderFloat("时间", &m_currentTime, 0.0f, maxTime, "%.2fs"))
    {
        m_currentFrame = static_cast<int>(m_currentTime * m_frameRate);
        m_currentFrame = std::clamp(m_currentFrame, 0, m_totalFrames - 1);
    }
    if (hasValidTargetObject() && m_currentClip)
    {
        ImGui::SameLine();
        if (ImGui::Button("应用"))
        {
            applyFrameToObject(m_currentFrame);
            applyPropertyTracksToObject(static_cast<float>(m_currentFrame));
        }
    }
}
void AnimationEditorPanel::drawTimeline()
{
    ImGui::Text("时间轴 (总帧数: %d)", m_totalFrames);
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = m_timelineHeight;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float keyframeRadius = 6.0f;
    const float rulerHeight = 25.0f;
    float pixelsPerFrame = 20.0f * m_timelineZoom;
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(50, 50, 50, 255));
    int visibleFrameStart = (pixelsPerFrame > 0)
                                ? std::max(0, static_cast<int>(m_timelineScrollX / pixelsPerFrame))
                                : 0;
    int visibleFrameEnd = (pixelsPerFrame > 0)
                              ? std::min(m_totalFrames,
                                         static_cast<int>((m_timelineScrollX + canvasSize.x) / pixelsPerFrame) + 2)
                              : m_totalFrames;
    for (int frame = visibleFrameStart; frame < visibleFrameEnd; ++frame)
    {
        float x = canvasPos.x + (static_cast<float>(frame) * pixelsPerFrame) - m_timelineScrollX;
        if (x < canvasPos.x || x > canvasPos.x + canvasSize.x) continue;
        ImU32 lineColor = (frame % 10 == 0) ? IM_COL32(150, 150, 150, 255) : IM_COL32(100, 100, 100, 255);
        drawList->AddLine(ImVec2(x, canvasPos.y + rulerHeight), ImVec2(x, canvasPos.y + canvasSize.y), lineColor);
        if (frame % 5 == 0)
        {
            char buf[16];
            snprintf(buf, 16, "%d", frame);
            drawList->AddText(ImVec2(x + 2, canvasPos.y + 2), IM_COL32(200, 200, 200, 255), buf);
        }
    }
    if (m_currentClip)
    {
        for (const auto& [frameIndex, frameData] : m_currentClip->getAnimationClip().Frames)
        {
            float x = canvasPos.x + (static_cast<float>(frameIndex) * pixelsPerFrame) - m_timelineScrollX;
            if (x < canvasPos.x - keyframeRadius || x > canvasPos.x + canvasSize.x + keyframeRadius) continue;
            bool isSelected = m_multiSelectedFrames.count(frameIndex);
            ImU32 color = isSelected ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 100, 100, 255);
            drawList->AddCircleFilled(ImVec2(x, canvasPos.y + (canvasSize.y + rulerHeight) * 0.5f), keyframeRadius,
                                      color);
        }
    }
    if (m_onionSkinEnabled && m_currentClip)
    {
        // 洋葱皮降级实现：用光圈高亮播放头前后各最多 2 个关键帧（绿=前、蓝=后，离播放头越近越亮）
        std::vector<int> sortedKeyframes;
        sortedKeyframes.reserve(m_currentClip->getAnimationClip().Frames.size());
        for (const auto& [frameIndex, frameData] : m_currentClip->getAnimationClip().Frames)
        {
            sortedKeyframes.push_back(frameIndex);
        }
        std::ranges::sort(sortedKeyframes);
        auto drawOnionRing = [&](int frameIndex, ImU32 ringColor)
        {
            float x = canvasPos.x + (static_cast<float>(frameIndex) * pixelsPerFrame) - m_timelineScrollX;
            if (x < canvasPos.x - keyframeRadius || x > canvasPos.x + canvasSize.x + keyframeRadius) return;
            drawList->AddCircle(ImVec2(x, canvasPos.y + (canvasSize.y + rulerHeight) * 0.5f),
                                keyframeRadius + 4.0f, ringColor, 0, 2.0f);
        };
        auto firstAfter = std::ranges::upper_bound(sortedKeyframes, m_currentFrame);
        int drawnAfter = 0;
        for (auto it = firstAfter; it != sortedKeyframes.end() && drawnAfter < 2; ++it, ++drawnAfter)
        {
            drawOnionRing(*it, IM_COL32(100, 160, 255, drawnAfter == 0 ? 220 : 110));
        }
        auto firstBefore = std::ranges::lower_bound(sortedKeyframes, m_currentFrame);
        int drawnBefore = 0;
        for (auto it = firstBefore; it != sortedKeyframes.begin() && drawnBefore < 2;)
        {
            --it;
            drawOnionRing(*it, IM_COL32(120, 255, 120, drawnBefore == 0 ? 220 : 110));
            ++drawnBefore;
        }
    }
    ImGui::SetCursorScreenPos(canvasPos);
    ImGui::InvisibleButton("##TimelineCanvas", canvasSize);
    const bool isCanvasHovered = ImGui::IsItemHovered();
    const bool isCtrlDown = ImGui::GetIO().KeyCtrl;
    const float mouseXOnCanvas = ImGui::GetIO().MousePos.x - canvasPos.x;
    int hoveredFrame = (pixelsPerFrame > 0)
                           ? static_cast<int>(round((mouseXOnCanvas + m_timelineScrollX) / pixelsPerFrame))
                           : 0;
    hoveredFrame = std::clamp(hoveredFrame, 0, m_totalFrames - 1);
    int clickedOnFrame = -1;
    if (m_currentClip && isCanvasHovered && ImGui::GetIO().MousePos.y > canvasPos.y + rulerHeight)
    {
        for (const auto& [frameIndex, frameData] : m_currentClip->getAnimationClip().Frames)
        {
            float keyframeScreenX = canvasPos.x + (static_cast<float>(frameIndex) * pixelsPerFrame) - m_timelineScrollX;
            if (std::abs(ImGui::GetIO().MousePos.x - keyframeScreenX) < keyframeRadius)
            {
                clickedOnFrame = frameIndex;
                break;
            }
        }
    }
    if (isCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        bool clickedOnRuler = ImGui::GetIO().MousePos.y < canvasPos.y + rulerHeight;
        if (clickedOnRuler)
        {
            m_isDraggingPlayhead = true;
            seekToFrame(hoveredFrame);
        }
        else if (clickedOnFrame != -1)
        {
            m_isDraggingKeyframe = true;
            m_dragHandleFrame = clickedOnFrame;
            if (isCtrlDown)
            {
                if (m_multiSelectedFrames.count(clickedOnFrame)) m_multiSelectedFrames.erase(clickedOnFrame);
                else m_multiSelectedFrames.insert(clickedOnFrame);
            }
            else if (!m_multiSelectedFrames.count(clickedOnFrame))
            {
                m_multiSelectedFrames.clear();
                m_multiSelectedFrames.insert(clickedOnFrame);
            }
            m_dragInitialSelectionState.assign(m_multiSelectedFrames.begin(), m_multiSelectedFrames.end());
            std::ranges::sort(m_dragInitialSelectionState);
        }
        else
        {
            m_isBoxSelecting = true;
            m_boxSelectionStart = ImGui::GetIO().MousePos;
            if (!isCtrlDown) m_multiSelectedFrames.clear();
        }
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        if (m_isDraggingPlayhead)
        {
            seekToFrame(hoveredFrame);
        }
        else if (m_isDraggingKeyframe)
        {
        }
        else if (m_isBoxSelecting)
        {
            ImVec2 currentMousePos = ImGui::GetIO().MousePos;
            drawList->AddRectFilled(m_boxSelectionStart, currentMousePos, IM_COL32(100, 150, 255, 50));
            drawList->AddRect(m_boxSelectionStart, currentMousePos, IM_COL32(100, 150, 255, 150));
        }
    }
    if (m_isDraggingKeyframe && !m_dragInitialSelectionState.empty())
    {
        int draggedFrameNewPos = hoveredFrame;
        int firstSelectedFrame = m_dragInitialSelectionState.front();
        bool isTranslation = (m_dragHandleFrame == firstSelectedFrame);
        if (isTranslation)
        {
            int delta = draggedFrameNewPos - m_dragHandleFrame;
            for (int oldIndex : m_dragInitialSelectionState)
            {
                int newIndex = oldIndex + delta;
                float ghostX = canvasPos.x + (static_cast<float>(newIndex) * pixelsPerFrame) - m_timelineScrollX;
                drawList->AddCircleFilled(ImVec2(ghostX, canvasPos.y + (canvasSize.y + rulerHeight) * 0.5f),
                                          keyframeRadius,
                                          IM_COL32(255, 255, 0, 128));
            }
        }
        else
        {
            int anchorFrame = firstSelectedFrame;
            float oldSpan = static_cast<float>(m_dragHandleFrame - anchorFrame);
            float newSpan = static_cast<float>(draggedFrameNewPos - anchorFrame);
            if (std::abs(oldSpan) > 0.001f)
            {
                float scale = newSpan / oldSpan;
                for (int oldIndex : m_dragInitialSelectionState)
                {
                    int newIndex = anchorFrame + static_cast<int>(round((oldIndex - anchorFrame) * scale));
                    float ghostX = canvasPos.x + (static_cast<float>(newIndex) * pixelsPerFrame) - m_timelineScrollX;
                    drawList->AddCircleFilled(ImVec2(ghostX, canvasPos.y + (canvasSize.y + rulerHeight) * 0.5f),
                                              keyframeRadius,
                                              IM_COL32(255, 255, 0, 128));
                }
            }
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (m_isDraggingPlayhead) { m_isDraggingPlayhead = false; }
        if (m_isDraggingKeyframe)
        {
            if (m_currentClip && !m_dragInitialSelectionState.empty() && hoveredFrame != m_dragHandleFrame)
            {
                std::map<int, int> newPositions;
                int firstSelectedFrame = m_dragInitialSelectionState.front();
                bool isTranslation = (m_dragHandleFrame == firstSelectedFrame);
                if (isTranslation)
                {
                    int delta = hoveredFrame - m_dragHandleFrame;
                    for (int oldIndex : m_dragInitialSelectionState)
                    {
                        newPositions[oldIndex] = oldIndex + delta;
                    }
                }
                else
                {
                    int anchorFrame = firstSelectedFrame;
                    float oldSpan = static_cast<float>(m_dragHandleFrame - anchorFrame);
                    float newSpan = static_cast<float>(hoveredFrame - anchorFrame);
                    if (std::abs(oldSpan) > 0.001f)
                    {
                        float scale = newSpan / oldSpan;
                        for (int oldIndex : m_dragInitialSelectionState)
                        {
                            if (oldIndex == anchorFrame)
                            {
                                newPositions[oldIndex] = anchorFrame;
                            }
                            else
                            {
                                newPositions[oldIndex] = anchorFrame + static_cast<int>(round(
                                    (oldIndex - anchorFrame) * scale));
                            }
                        }
                    }
                    else
                    {
                        int delta = hoveredFrame - m_dragHandleFrame;
                        for (int oldIndex : m_dragInitialSelectionState) newPositions[oldIndex] = oldIndex + delta;
                    }
                }
                bool collision = false;
                std::set<int> finalDestinations;
                std::set<int> unselectedFrames;
                for (const auto& [frameIndex, frameData] : m_currentClip->getAnimationClip().Frames)
                {
                    if (m_multiSelectedFrames.count(frameIndex) == 0)
                    {
                        unselectedFrames.insert(frameIndex);
                    }
                }
                for (const auto& [oldIndex, newIndex] : newPositions)
                {
                    if (newIndex < 0)
                    {
                        collision = true;
                        break;
                    }
                    if (!finalDestinations.insert(newIndex).second)
                    {
                        collision = true;
                        break;
                    }
                    if (unselectedFrames.count(newIndex))
                    {
                        collision = true;
                        break;
                    }
                }
                if (!collision)
                {
                    // 拖拽移动是连续操作：仅在松开应用时压栈一次
                    pushUndoSnapshot();
                    auto& frames = m_currentClip->getAnimationClip().Frames;
                    std::vector<std::pair<int, AnimFrame>> framesToMove;
                    for (int oldIndex : m_dragInitialSelectionState)
                    {
                        framesToMove.push_back({newPositions.at(oldIndex), frames.at(oldIndex)});
                    }
                    for (int oldIndex : m_dragInitialSelectionState) frames.erase(oldIndex);
                    m_multiSelectedFrames.clear();
                    for (const auto& pair : framesToMove)
                    {
                        frames[pair.first] = pair.second;
                        m_multiSelectedFrames.insert(pair.first);
                    }
                    markDirty();
                    LogInfo("批量移动 {} 个关键帧", framesToMove.size());
                }
                else
                {
                    LogWarn("移动关键帧失败：发生碰撞或超出边界");
                }
            }
            m_isDraggingKeyframe = false;
        }
        else if (m_isBoxSelecting)
        {
            ImVec2 boxEnd = ImGui::GetIO().MousePos;
            ImVec2 boxMin(std::min(m_boxSelectionStart.x, boxEnd.x), std::min(m_boxSelectionStart.y, boxEnd.y));
            ImVec2 boxMax(std::max(m_boxSelectionStart.x, boxEnd.x), std::max(m_boxSelectionStart.y, boxEnd.y));
            if (m_currentClip)
            {
                for (const auto& [frameIndex, frameData] : m_currentClip->getAnimationClip().Frames)
                {
                    float keyframeScreenX = canvasPos.x + (static_cast<float>(frameIndex) * pixelsPerFrame) -
                        m_timelineScrollX;
                    float keyframeScreenY = canvasPos.y + (canvasSize.y + rulerHeight) * 0.5f;
                    if (keyframeScreenX >= boxMin.x && keyframeScreenX <= boxMax.x && keyframeScreenY >= boxMin.y &&
                        keyframeScreenY <= boxMax.y)
                    {
                        m_multiSelectedFrames.insert(frameIndex);
                    }
                }
            }
            m_isBoxSelecting = false;
        }
    }
    if (m_currentClip && isCanvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !m_isDraggingKeyframe && !m_isDraggingPlayhead && !m_isBoxSelecting)
    {
        // 记录右键落点：命中关键帧则弹帧菜单，否则弹空白处菜单
        m_contextMenuFrame = hoveredFrame;
        m_contextMenuKeyframe = clickedOnFrame;
        ImGui::OpenPopup("TimelineContextMenu");
    }
    drawTimelineContextMenu();
    if (isCanvasHovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            m_timelineScrollX -= ImGui::GetIO().MouseDelta.x;
            m_timelineScrollX = std::max(0.0f, m_timelineScrollX);
        }
        if (ImGui::GetIO().MouseWheel != 0.0f && pixelsPerFrame > 0)
        {
            float oldZoom = m_timelineZoom;
            m_timelineZoom += ImGui::GetIO().MouseWheel * 0.1f;
            m_timelineZoom = std::clamp(m_timelineZoom, 0.1f, 5.0f);
            if (oldZoom != m_timelineZoom)
            {
                float mouseX = mouseXOnCanvas;
                float frameAtMouse = (mouseX + m_timelineScrollX) / (20.0f * oldZoom);
                m_timelineScrollX = frameAtMouse * (20.0f * m_timelineZoom) - mouseX;
                m_timelineScrollX = std::max(0.0f, m_timelineScrollX);
            }
        }
    }
    float currentX = canvasPos.x + (static_cast<float>(m_currentFrame) * pixelsPerFrame) - m_timelineScrollX;
    if (currentX >= canvasPos.x && currentX <= canvasPos.x + canvasSize.x)
    {
        drawList->AddLine(ImVec2(currentX, canvasPos.y), ImVec2(currentX, canvasPos.y + canvasSize.y),
                          IM_COL32(255, 255, 255, 255), 2.0f);
        ImVec2 trianglePoints[3] = {
            {currentX - 5, canvasPos.y}, {currentX + 5, canvasPos.y}, {currentX, canvasPos.y + 10}
        };
        drawList->AddTriangleFilled(trianglePoints[0], trianglePoints[1], trianglePoints[2],
                                    IM_COL32(255, 255, 255, 255));
    }
    if (m_recordMode && m_currentClip)
    {
        // 录制状态给时间轴画布叠加红色描边提示
        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                          IM_COL32(220, 40, 40, 255), 0.0f, 0, 2.0f);
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DRAG_DROP_ASSET_HANDLES_MULTI"))
        {
            if (hasValidTargetObject() && m_currentClip)
            {
                size_t handleCount = payload->DataSize / sizeof(AssetHandle);
                const AssetHandle* handles = static_cast<const AssetHandle*>(payload->Data);
                auto scene = SceneManager::GetInstance().GetCurrentScene();
                auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
                const auto* compInfo = ComponentRegistry::GetInstance().Get("SpriteComponent");
                if (compInfo)
                {
                    SceneManager::GetInstance().PushUndoState(scene);
                    pushUndoSnapshot();
                    if (!targetObject.HasComponent<ECS::SpriteComponent>())
                    {
                        targetObject.AddComponent<ECS::SpriteComponent>();
                    }
                    auto& spriteComp = targetObject.GetComponent<ECS::SpriteComponent>();
                    int keyframesCreated = 0;
                    for (size_t i = 0; i < handleCount; ++i)
                    {
                        const AssetHandle& handle = handles[i];
                        if (handle.assetType != AssetType::Texture) continue;
                        int frameIndex = hoveredFrame + keyframesCreated;
                        spriteComp.textureHandle = handle;
                        AnimFrame& frame = m_currentClip->getAnimationClip().Frames[frameIndex];
                        frame.animationData["SpriteComponent"] = compInfo->serialize(
                            scene->GetRegistry(), targetObject.GetEntityHandle());
                        keyframesCreated++;
                    }
                    if (keyframesCreated > 0)
                    {
                        const AssetHandle& lastHandle = handles[handleCount - 1];
                        spriteComp.textureHandle = lastHandle;
                        markDirty();
                        LogInfo("通过批量拖拽创建了 {} 个连续的关键帧", keyframesCreated);
                    }
                }
            }
        }
        else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DRAG_DROP_ASSET_HANDLE"))
        {
            AssetHandle handle = *static_cast<const AssetHandle*>(payload->Data);
            if (handle.assetType == AssetType::Texture)
            {
                if (hasValidTargetObject() && m_currentClip)
                {
                    auto scene = SceneManager::GetInstance().GetCurrentScene();
                    SceneManager::GetInstance().PushUndoState(scene);
                    pushUndoSnapshot();
                    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
                    if (!targetObject.HasComponent<ECS::SpriteComponent>())
                    {
                        targetObject.AddComponent<ECS::SpriteComponent>();
                    }
                    auto& spriteComp = targetObject.GetComponent<ECS::SpriteComponent>();
                    spriteComp.textureHandle = handle;
                    AnimFrame& frame = m_currentClip->getAnimationClip().Frames[hoveredFrame];
                    const auto* compInfo = ComponentRegistry::GetInstance().Get("SpriteComponent");
                    if (compInfo)
                    {
                        frame.animationData["SpriteComponent"] = compInfo->serialize(
                            scene->GetRegistry(), targetObject.GetEntityHandle());
                        m_multiSelectedFrames.clear();
                        m_multiSelectedFrames.insert(hoveredFrame);
                        markDirty();
                        LogInfo("拖放纹理到第 {} 帧，已记录SpriteComponent", hoveredFrame);
                    }
                }
                else
                {
                    LogWarn("无法拖放纹理：没有动画或无效的目标物体。");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    drawPropertyTrackSection();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("缩放", &m_timelineZoom, 0.1f, 5.0f, "%.1fx");
    ImGui::SameLine();
    if (ImGui::Button("跟随播放头")) { centerTimelineOnCurrentFrame(); }
    const char* fitAllText = "适应所有";
    float fitAllButtonWidth = ImGui::CalcTextSize(fitAllText).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float spacing = ImGui::GetContentRegionAvail().x - fitAllButtonWidth;
    ImGui::SameLine(0, spacing > 0 ? spacing : 0);
    if (ImGui::Button(fitAllText)) { fitTimelineToAllFrames(canvasSize.x); }
}
void AnimationEditorPanel::updatePlayback(float deltaTime)
{
    if (!m_isPlaying || m_totalFrames == 0)
        return;
    m_currentTime += deltaTime;
    float maxTime = static_cast<float>(m_totalFrames) / m_frameRate;
    if (m_currentTime >= maxTime)
    {
        if (m_isLooping)
        {
            m_currentTime = 0.0f;
        }
        else
        {
            m_currentTime = maxTime;
            m_isPlaying = false;
        }
    }
    m_currentFrame = static_cast<int>(m_currentTime * m_frameRate);
    m_currentFrame = std::clamp(m_currentFrame, 0, m_totalFrames - 1);
    if (hasValidTargetObject() && m_currentClip)
    {
        applyFrameToObject(m_currentFrame);
        // 属性轨道用连续时间采样，播放中帧间也能平滑插值
        applyPropertyTracksToObject(m_currentTime * m_frameRate);
    }
}
void AnimationEditorPanel::seekToFrame(int frameIndex)
{
    int newFrame = std::clamp(frameIndex, 0, m_totalFrames - 1);
    if (newFrame != m_currentFrame)
    {
        m_currentFrame = newFrame;
        if (m_frameRate > 0)
        {
            m_currentTime = static_cast<float>(m_currentFrame) / m_frameRate;
        }
        else
        {
            m_currentTime = 0.0f;
        }
    }
    if (hasValidTargetObject() && m_currentClip)
    {
        applyFrameToObject(m_currentFrame);
        applyPropertyTracksToObject(static_cast<float>(m_currentFrame));
    }
}
void AnimationEditorPanel::addKeyFrame(int frameIndex)
{
    if (!m_currentClip)
    {
        LogWarn("没有打开的动画切片，无法添加关键帧");
        return;
    }
    if (m_currentClip->getAnimationClip().Frames.find(frameIndex) != m_currentClip->getAnimationClip().Frames.end())
    {
        LogWarn("帧 {} 已存在关键帧", frameIndex);
        return;
    }
    pushUndoSnapshot();
    AnimFrame newFrame;
    m_currentClip->getAnimationClip().Frames[frameIndex] = newFrame;
    m_multiSelectedFrames.clear();
    m_multiSelectedFrames.insert(frameIndex);
    markDirty();
    LogInfo("添加空关键帧: {}", frameIndex);
}
void AnimationEditorPanel::removeKeyFrame(int frameIndex)
{
    // 不在此处压撤销栈：批量删除等手势由调用方统一压栈一次
    if (!m_currentClip) return;
    auto it = m_currentClip->getAnimationClip().Frames.find(frameIndex);
    if (it == m_currentClip->getAnimationClip().Frames.end())
    {
        LogWarn("帧 {} 不存在关键帧", frameIndex);
        return;
    }
    m_currentClip->getAnimationClip().Frames.erase(it);
    m_multiSelectedFrames.erase(frameIndex);
    markDirty();
    LogInfo("删除关键帧: {}", frameIndex);
}
void AnimationEditorPanel::removeSelectedKeyFrames()
{
    if (!m_currentClip || m_multiSelectedFrames.empty()) return;
    pushUndoSnapshot();
    // removeKeyFrame 会修改选中集合，先拷贝一份再遍历
    std::vector<int> framesToRemove(m_multiSelectedFrames.begin(), m_multiSelectedFrames.end());
    for (int frameIndex : framesToRemove)
    {
        removeKeyFrame(frameIndex);
    }
    m_multiSelectedFrames.clear();
}
AnimFrame AnimationEditorPanel::cloneFrameData(const AnimFrame& source)
{
    AnimFrame result;
    result.eventTargets = source.eventTargets;
    for (const auto& [componentName, componentData] : source.animationData)
    {
        // YAML::Node 是引用语义，必须 Clone 深拷贝，否则快照会随原数据一同被修改
        result.animationData[componentName] = YAML::Clone(componentData);
    }
    return result;
}
AnimationClip AnimationEditorPanel::cloneClipData(const AnimationClip& source)
{
    AnimationClip result;
    result.Name = source.Name;
    result.TargetEntityGuid = source.TargetEntityGuid;
    result.FrameRate = source.FrameRate;
    result.IsLooping = source.IsLooping;
    for (const auto& [frameIndex, frame] : source.Frames)
    {
        result.Frames[frameIndex] = cloneFrameData(frame);
    }
    // 属性轨道是纯值类型，vector 拷贝即深拷贝
    result.PropertyTracks = source.PropertyTracks;
    return result;
}
void AnimationEditorPanel::pushUndoSnapshot()
{
    if (!m_currentClip) return;
    pushUndoSnapshotFrom(cloneClipData(m_currentClip->getAnimationClip()));
}
void AnimationEditorPanel::pushUndoSnapshotFrom(AnimationClip snapshot)
{
    constexpr size_t kUndoStackLimit = 64;
    m_undoStack.push_back(std::move(snapshot));
    if (m_undoStack.size() > kUndoStackLimit)
    {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
}
void AnimationEditorPanel::performUndo()
{
    if (!m_currentClip || m_undoStack.empty()) return;
    m_redoStack.push_back(cloneClipData(m_currentClip->getAnimationClip()));
    m_currentClip->getAnimationClip() = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    afterClipDataRestored();
    LogInfo("动画编辑器: 撤销");
}
void AnimationEditorPanel::performRedo()
{
    if (!m_currentClip || m_redoStack.empty()) return;
    m_undoStack.push_back(cloneClipData(m_currentClip->getAnimationClip()));
    m_currentClip->getAnimationClip() = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    afterClipDataRestored();
    LogInfo("动画编辑器: 重做");
}
void AnimationEditorPanel::afterClipDataRestored()
{
    const AnimationClip& clipData = m_currentClip->getAnimationClip();
    m_currentClipName = clipData.Name;
    m_targetObjectGuid = clipData.TargetEntityGuid;
    m_frameRate = clipData.FrameRate > 0.0f ? clipData.FrameRate : 60.0f;
    m_isLooping = clipData.IsLooping;
    m_totalFrames = 60;
    for (const auto& [frameIndex, frame] : clipData.Frames)
    {
        m_totalFrames = std::max(m_totalFrames, frameIndex + 1);
    }
    for (const auto& track : clipData.PropertyTracks)
    {
        for (const auto& key : track.keyframes)
        {
            m_totalFrames = std::max(m_totalFrames, key.frame + 1);
        }
    }
    m_currentFrame = std::clamp(m_currentFrame, 0, m_totalFrames - 1);
    // 丢弃指向已不存在关键帧的选中项
    for (auto it = m_multiSelectedFrames.begin(); it != m_multiSelectedFrames.end();)
    {
        if (!clipData.Frames.contains(*it))
        {
            it = m_multiSelectedFrames.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // 轨道选中状态同样可能因撤销/重做失效
    if (m_selectedTrackIndex >= static_cast<int>(clipData.PropertyTracks.size()))
    {
        m_selectedTrackIndex = -1;
        m_selectedTrackKeys.clear();
    }
    else if (m_selectedTrackIndex >= 0)
    {
        const auto& keys = clipData.PropertyTracks[m_selectedTrackIndex].keyframes;
        for (auto it = m_selectedTrackKeys.begin(); it != m_selectedTrackKeys.end();)
        {
            const bool exists = std::any_of(keys.begin(), keys.end(),
                                            [frame = *it](const PropertyKey& key) { return key.frame == frame; });
            it = exists ? std::next(it) : m_selectedTrackKeys.erase(it);
        }
    }
    if (m_recordMode)
    {
        refreshRecordCache();
    }
    markDirty();
}
void AnimationEditorPanel::markDirty()
{
    m_isDirty = true;
}
void AnimationEditorPanel::copySelectedFrames()
{
    if (!m_currentClip || m_multiSelectedFrames.empty()) return;
    const auto& frames = m_currentClip->getAnimationClip().Frames;
    // std::set 有序，首元素即最小选中帧，作为相对偏移基准
    const int baseFrame = *m_multiSelectedFrames.begin();
    m_copiedFrames.clear();
    for (int frameIndex : m_multiSelectedFrames)
    {
        auto it = frames.find(frameIndex);
        if (it == frames.end()) continue;
        m_copiedFrames.emplace_back(frameIndex - baseFrame, cloneFrameData(it->second));
    }
    LogInfo("复制 {} 个关键帧", m_copiedFrames.size());
}
void AnimationEditorPanel::pasteFramesAt(int targetFrame)
{
    if (!m_currentClip || m_copiedFrames.empty()) return;
    targetFrame = std::max(0, targetFrame);
    pushUndoSnapshot();
    auto& frames = m_currentClip->getAnimationClip().Frames;
    m_multiSelectedFrames.clear();
    for (const auto& [offset, frameData] : m_copiedFrames)
    {
        int destIndex = targetFrame + offset;
        // 再次克隆，保证多次粘贴产生的帧互不共享节点
        frames[destIndex] = cloneFrameData(frameData);
        m_multiSelectedFrames.insert(destIndex);
        m_totalFrames = std::max(m_totalFrames, destIndex + 1);
    }
    markDirty();
    LogInfo("粘贴 {} 个关键帧到第 {} 帧", m_copiedFrames.size(), targetFrame);
}
void AnimationEditorPanel::drawPropertyTrackSection()
{
    if (!m_currentClip)
        return;
    ImGui::Spacing();
    const bool recordOn = m_recordMode;
    if (recordOn)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.10f, 0.10f, 1.0f));
    }
    if (ImGui::Button("● 录制"))
    {
        m_recordMode = !m_recordMode;
        if (m_recordMode)
        {
            refreshRecordCache();
        }
    }
    if (recordOn)
    {
        ImGui::PopStyleColor(3);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("录制模式：修改目标物体属性时，自动在播放头帧写入关键帧\n监听已有轨道属性与 Transform 常用属性（位置/旋转/缩放），无轨道时自动创建");
    }
    ImGui::SameLine();
    if (ImGui::Button(m_curveEditMode ? "关键帧视图" : "曲线视图"))
    {
        m_curveEditMode = !m_curveEditMode;
    }
    ImGui::SameLine();
    if (ImGui::Button("添加属性轨道"))
    {
        openAddTrackPopup();
    }
    auto& clipData = m_currentClip->getAnimationClip();
    ImGui::SameLine();
    ImGui::TextDisabled("属性轨道: %zu", clipData.PropertyTracks.size());
    if (clipData.PropertyTracks.empty())
        return;

    const float sidebarWidth = 190.0f;
    const float rowHeight = 22.0f;
    const int trackCount = static_cast<int>(clipData.PropertyTracks.size());
    const float pixelsPerFrame = 20.0f * m_timelineZoom;
    const float areaHeight = m_curveEditMode
                                 ? std::max(180.0f, rowHeight * static_cast<float>(trackCount))
                                 : rowHeight * static_cast<float>(trackCount);
    const ImVec2 areaPos = ImGui::GetCursorScreenPos();
    // 面板极窄时保证画布最小宽度，避免零尺寸 InvisibleButton 触发断言
    const ImVec2 areaSize = ImVec2(std::max(ImGui::GetContentRegionAvail().x, sidebarWidth + 60.0f), areaHeight);
    const float laneX = areaPos.x + sidebarWidth;
    const float areaRight = areaPos.x + areaSize.x;
    const float laneWidth = std::max(0.0f, areaSize.x - sidebarWidth);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(areaPos, ImVec2(areaRight, areaPos.y + areaSize.y), IM_COL32(45, 45, 45, 255));
    drawList->AddRectFilled(areaPos, ImVec2(laneX, areaPos.y + areaSize.y), IM_COL32(36, 36, 36, 255));
    auto frameToX = [&](float frame) { return laneX + frame * pixelsPerFrame - m_timelineScrollX; };
    // 网格竖线与上方时间轴共用滚动/缩放，保证帧对齐
    if (pixelsPerFrame > 0.0f)
    {
        const int gridStart = std::max(0, static_cast<int>(m_timelineScrollX / pixelsPerFrame));
        const int gridEnd = std::min(m_totalFrames,
                                     static_cast<int>((m_timelineScrollX + laneWidth) / pixelsPerFrame) + 2);
        for (int frame = gridStart; frame < gridEnd; ++frame)
        {
            const float x = frameToX(static_cast<float>(frame));
            if (x < laneX || x > areaRight)
                continue;
            const ImU32 lineColor = (frame % 10 == 0) ? IM_COL32(85, 85, 85, 255) : IM_COL32(62, 62, 62, 255);
            drawList->AddLine(ImVec2(x, areaPos.y), ImVec2(x, areaPos.y + areaSize.y), lineColor);
        }
    }
    // 侧栏轨道行
    for (int i = 0; i < trackCount; ++i)
    {
        const auto& track = clipData.PropertyTracks[i];
        const float rowY = areaPos.y + static_cast<float>(i) * rowHeight;
        if (i == m_selectedTrackIndex)
        {
            drawList->AddRectFilled(ImVec2(areaPos.x, rowY), ImVec2(laneX, rowY + rowHeight),
                                    IM_COL32(70, 95, 140, 255));
        }
        const std::string label = track.targetComponent + "." + track.propertyPath;
        drawList->PushClipRect(ImVec2(areaPos.x, rowY), ImVec2(laneX - 4.0f, rowY + rowHeight), true);
        drawList->AddText(ImVec2(areaPos.x + 6.0f, rowY + 4.0f), IM_COL32(215, 215, 215, 255), label.c_str());
        drawList->PopClipRect();
        // 曲线模式下行分隔线只画在侧栏，避免切割曲线画布
        const float separatorRight = m_curveEditMode ? laneX : areaRight;
        drawList->AddLine(ImVec2(areaPos.x, rowY + rowHeight), ImVec2(separatorRight, rowY + rowHeight),
                          IM_COL32(56, 56, 56, 255));
    }
    drawList->AddLine(ImVec2(laneX, areaPos.y), ImVec2(laneX, areaPos.y + areaSize.y), IM_COL32(80, 80, 80, 255));

    ImGui::SetCursorScreenPos(areaPos);
    ImGui::InvisibleButton("##PropertyTrackCanvas", areaSize);
    const bool areaHovered = ImGui::IsItemHovered();
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const bool mouseInLane = areaHovered && mousePos.x >= laneX;
    const bool mouseInSidebar = areaHovered && mousePos.x < laneX;
    const int hoveredRow = static_cast<int>((mousePos.y - areaPos.y) / rowHeight);
    int hoveredTrackFrame = (pixelsPerFrame > 0.0f)
                                ? static_cast<int>(round((mousePos.x - laneX + m_timelineScrollX) / pixelsPerFrame))
                                : 0;
    hoveredTrackFrame = std::clamp(hoveredTrackFrame, 0, m_totalFrames - 1);
    if (mouseInSidebar && hoveredRow >= 0 && hoveredRow < trackCount)
    {
        const auto& track = clipData.PropertyTracks[hoveredRow];
        ImGui::SetTooltip("%s.%s", track.targetComponent.c_str(), track.propertyPath.c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (m_selectedTrackIndex != hoveredRow)
            {
                m_selectedTrackKeys.clear();
            }
            m_selectedTrackIndex = hoveredRow;
        }
    }
    auto drawDiamond = [&](const ImVec2& center, float radius, ImU32 color)
    {
        drawList->AddQuadFilled(ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y),
                                ImVec2(center.x, center.y + radius), ImVec2(center.x - radius, center.y), color);
    };

    int hitTrack = -1;
    int hitKeyFrame = -1;
    if (!m_curveEditMode)
    {
        // —— Dopesheet：每轨道一行，关键帧画菱形 ——
        for (int i = 0; i < trackCount; ++i)
        {
            const auto& track = clipData.PropertyTracks[i];
            const float centerY = areaPos.y + static_cast<float>(i) * rowHeight + rowHeight * 0.5f;
            for (const auto& key : track.keyframes)
            {
                const float x = frameToX(static_cast<float>(key.frame));
                if (x < laneX - 6.0f || x > areaRight + 6.0f)
                    continue;
                const bool selected = (i == m_selectedTrackIndex && m_selectedTrackKeys.count(key.frame));
                drawDiamond(ImVec2(x, centerY), 5.0f,
                            selected ? IM_COL32(255, 220, 60, 255) : IM_COL32(120, 190, 255, 255));
                if (mouseInLane && std::abs(mousePos.x - x) <= 6.0f &&
                    std::abs(mousePos.y - centerY) <= rowHeight * 0.5f)
                {
                    hitTrack = i;
                    hitKeyFrame = key.frame;
                }
            }
        }
        if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseInLane)
        {
            if (hitKeyFrame != -1)
            {
                if (m_selectedTrackIndex != hitTrack)
                {
                    m_selectedTrackKeys.clear();
                }
                m_selectedTrackIndex = hitTrack;
                if (ImGui::GetIO().KeyCtrl)
                {
                    if (m_selectedTrackKeys.count(hitKeyFrame))
                        m_selectedTrackKeys.erase(hitKeyFrame);
                    else
                        m_selectedTrackKeys.insert(hitKeyFrame);
                }
                else if (!m_selectedTrackKeys.count(hitKeyFrame))
                {
                    m_selectedTrackKeys.clear();
                    m_selectedTrackKeys.insert(hitKeyFrame);
                }
                m_isDraggingTrackKey = true;
                m_trackDragHandleFrame = hitKeyFrame;
                m_trackDragTargetFrame = hitKeyFrame;
            }
            else if (hoveredRow >= 0 && hoveredRow < trackCount)
            {
                if (ImGui::GetIO().KeyCtrl)
                {
                    // Ctrl+点击空白处在该帧直接加 key（Unity 习惯），双击同样可用
                    insertTrackKeyAt(hoveredRow, hoveredTrackFrame);
                }
                else
                {
                    m_selectedTrackIndex = hoveredRow;
                    m_selectedTrackKeys.clear();
                }
            }
        }
        if (mouseInLane && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            hitKeyFrame == -1 && hoveredRow >= 0 && hoveredRow < trackCount)
        {
            insertTrackKeyAt(hoveredRow, hoveredTrackFrame);
        }
        if (m_isDraggingTrackKey && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            m_trackDragTargetFrame = hoveredTrackFrame;
            const int delta = m_trackDragTargetFrame - m_trackDragHandleFrame;
            if (delta != 0 && m_selectedTrackIndex >= 0 && m_selectedTrackIndex < trackCount)
            {
                const float centerY = areaPos.y + static_cast<float>(m_selectedTrackIndex) * rowHeight +
                    rowHeight * 0.5f;
                for (int frame : m_selectedTrackKeys)
                {
                    drawDiamond(ImVec2(frameToX(static_cast<float>(frame + delta)), centerY), 5.0f,
                                IM_COL32(255, 220, 60, 128));
                }
            }
        }
        if (m_isDraggingTrackKey && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const int delta = m_trackDragTargetFrame - m_trackDragHandleFrame;
            if (delta != 0 && m_selectedTrackIndex >= 0 && m_selectedTrackIndex < trackCount &&
                !m_selectedTrackKeys.empty())
            {
                PropertyTrack& track = clipData.PropertyTracks[m_selectedTrackIndex];
                bool collision = false;
                for (int frame : m_selectedTrackKeys)
                {
                    const int dest = frame + delta;
                    if (dest < 0)
                    {
                        collision = true;
                        break;
                    }
                    // 目标位置已有未选中的关键帧则视为碰撞（整体平移不会产生选中间碰撞）
                    for (const auto& key : track.keyframes)
                    {
                        if (key.frame == dest && !m_selectedTrackKeys.count(key.frame))
                        {
                            collision = true;
                            break;
                        }
                    }
                    if (collision)
                        break;
                }
                if (!collision)
                {
                    // 先按下标收集再改帧号，避免移动过程中的帧号误匹配
                    std::vector<size_t> movedIndices;
                    for (size_t k = 0; k < track.keyframes.size(); ++k)
                    {
                        if (m_selectedTrackKeys.count(track.keyframes[k].frame))
                        {
                            movedIndices.push_back(k);
                        }
                    }
                    pushUndoSnapshot();
                    std::set<int> newSelection;
                    for (size_t idx : movedIndices)
                    {
                        track.keyframes[idx].frame += delta;
                        newSelection.insert(track.keyframes[idx].frame);
                    }
                    AnimationTrackSampler::SortKeys(track);
                    m_selectedTrackKeys = std::move(newSelection);
                    for (int frame : m_selectedTrackKeys)
                    {
                        m_totalFrames = std::max(m_totalFrames, frame + 1);
                    }
                    markDirty();
                }
                else
                {
                    LogWarn("移动轨道关键帧失败：目标位置冲突或越界");
                }
            }
            m_isDraggingTrackKey = false;
        }
    }
    else
    {
        // —— 曲线视图：绘制选中轨道的值-时间曲线 ——
        if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= trackCount)
        {
            m_selectedTrackIndex = 0;
            m_selectedTrackKeys.clear();
        }
        PropertyTrack& track = clipData.PropertyTracks[m_selectedTrackIndex];
        float minValue;
        float maxValue;
        if (m_isDraggingCurveKey)
        {
            minValue = m_curveDragRangeMin;
            maxValue = m_curveDragRangeMax;
        }
        else
        {
            minValue = std::numeric_limits<float>::max();
            maxValue = std::numeric_limits<float>::lowest();
            for (const auto& key : track.keyframes)
            {
                minValue = std::min(minValue, key.value);
                maxValue = std::max(maxValue, key.value);
            }
            if (track.keyframes.empty())
            {
                minValue = 0.0f;
                maxValue = 1.0f;
            }
            if (maxValue - minValue < 1e-4f)
            {
                minValue -= 1.0f;
                maxValue += 1.0f;
            }
            const float padding = (maxValue - minValue) * 0.15f;
            minValue -= padding;
            maxValue += padding;
        }
        auto valueToY = [&](float value)
        {
            return areaPos.y + (1.0f - (value - minValue) / (maxValue - minValue)) * areaSize.y;
        };
        for (int gi = 0; gi <= 4; ++gi)
        {
            const float value = minValue + (maxValue - minValue) * static_cast<float>(gi) / 4.0f;
            const float y = valueToY(value);
            drawList->AddLine(ImVec2(laneX, y), ImVec2(areaRight, y), IM_COL32(62, 62, 62, 255));
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", value);
            drawList->AddText(ImVec2(laneX + 4.0f, std::min(y, areaPos.y + areaSize.y - 16.0f)),
                              IM_COL32(150, 150, 150, 255), buf);
        }
        if (!track.keyframes.empty() && laneWidth > 1.0f && pixelsPerFrame > 0.0f)
        {
            const float step = 3.0f;
            float prevX = laneX;
            float prevY = valueToY(AnimationTrackSampler::Evaluate(track, m_timelineScrollX / pixelsPerFrame));
            for (float px = step; px <= laneWidth; px += step)
            {
                const float frameTime = (px + m_timelineScrollX) / pixelsPerFrame;
                const float x = laneX + px;
                const float y = valueToY(AnimationTrackSampler::Evaluate(track, frameTime));
                drawList->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), IM_COL32(130, 220, 130, 255), 1.5f);
                prevX = x;
                prevY = y;
            }
        }
        for (const auto& key : track.keyframes)
        {
            const float x = frameToX(static_cast<float>(key.frame));
            if (x < laneX - 6.0f || x > areaRight + 6.0f)
                continue;
            const float y = valueToY(key.value);
            const bool selected = m_selectedTrackKeys.count(key.frame) != 0;
            drawList->AddCircleFilled(ImVec2(x, y), 5.0f,
                                      selected ? IM_COL32(255, 220, 60, 255) : IM_COL32(160, 230, 160, 255));
            if (selected || (m_isDraggingCurveKey && key.frame == m_curveDragKeyFrame))
            {
                char buf[48];
                snprintf(buf, sizeof(buf), "%d: %.3f", key.frame, key.value);
                drawList->AddText(ImVec2(x + 8.0f, y - 16.0f), IM_COL32(255, 255, 180, 255), buf);
            }
            if (mouseInLane && std::abs(mousePos.x - x) <= 7.0f && std::abs(mousePos.y - y) <= 7.0f)
            {
                hitTrack = m_selectedTrackIndex;
                hitKeyFrame = key.frame;
            }
        }
        // —— Bezier 切线柄：选中的 CubicBezier 关键帧两侧显示可拖动手柄 ——
        const float pixelScaleY = areaSize.y / (maxValue - minValue);
        int hitTangentKeyFrame = -1;
        bool hitTangentIsOut = false;
        for (const auto& key : track.keyframes)
        {
            if (key.interp != AnimInterpMode::CubicBezier || !m_selectedTrackKeys.count(key.frame))
                continue;
            const float x = frameToX(static_cast<float>(key.frame));
            const float y = valueToY(key.value);
            if (x < laneX - 60.0f || x > areaRight + 60.0f)
                continue;
            // 斜率（值/帧）换算为像素空间方向，归一化到固定柄长，保证任意缩放下手柄可点
            auto handleEnd = [&](float tangent, bool outSide)
            {
                float dirX = pixelsPerFrame;
                float dirY = -tangent * pixelScaleY;
                const float length = std::sqrt(dirX * dirX + dirY * dirY);
                if (length > 1e-5f)
                {
                    dirX /= length;
                    dirY /= length;
                }
                const float sign = outSide ? 1.0f : -1.0f;
                constexpr float kHandleLength = 42.0f;
                return ImVec2(x + dirX * kHandleLength * sign, y + dirY * kHandleLength * sign);
            };
            const ImVec2 inEnd = handleEnd(key.inTangent, false);
            const ImVec2 outEnd = handleEnd(key.outTangent, true);
            drawList->AddLine(ImVec2(x, y), inEnd, IM_COL32(200, 160, 255, 200));
            drawList->AddLine(ImVec2(x, y), outEnd, IM_COL32(200, 160, 255, 200));
            drawList->AddCircleFilled(inEnd, 4.0f, IM_COL32(200, 160, 255, 255));
            drawList->AddCircleFilled(outEnd, 4.0f, IM_COL32(200, 160, 255, 255));
            if (mouseInLane)
            {
                if (std::abs(mousePos.x - inEnd.x) <= 6.0f && std::abs(mousePos.y - inEnd.y) <= 6.0f)
                {
                    hitTangentKeyFrame = key.frame;
                    hitTangentIsOut = false;
                }
                else if (std::abs(mousePos.x - outEnd.x) <= 6.0f && std::abs(mousePos.y - outEnd.y) <= 6.0f)
                {
                    hitTangentKeyFrame = key.frame;
                    hitTangentIsOut = true;
                }
            }
        }
        if (mouseInLane && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hitTangentKeyFrame != -1)
        {
            m_isDraggingTangent = true;
            m_tangentDragKeyFrame = hitTangentKeyFrame;
            m_tangentDragIsOut = hitTangentIsOut;
            for (const auto& key : track.keyframes)
            {
                if (key.frame == hitTangentKeyFrame)
                {
                    m_tangentDragStartValue = hitTangentIsOut ? key.outTangent : key.inTangent;
                    break;
                }
            }
            m_trackDragPreState = cloneClipData(clipData);
            m_hasTrackDragPreState = true;
        }
        if (m_isDraggingTangent && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            for (auto& key : track.keyframes)
            {
                if (key.frame != m_tangentDragKeyFrame)
                    continue;
                const float keyX = frameToX(static_cast<float>(key.frame));
                const float keyY = valueToY(key.value);
                // 鼠标相对关键帧的位移换算回 值/帧 斜率；水平分量钳制在手柄一侧，避免斜率爆炸
                float dxFrames = (pixelsPerFrame > 0.0f) ? (mousePos.x - keyX) / pixelsPerFrame : 0.0f;
                const float dyValue = (keyY - mousePos.y) / pixelScaleY;
                constexpr float kMinDxFrames = 0.05f;
                if (m_tangentDragIsOut)
                {
                    dxFrames = std::max(dxFrames, kMinDxFrames);
                }
                else
                {
                    dxFrames = std::min(dxFrames, -kMinDxFrames);
                }
                const float slope = std::clamp(dyValue / dxFrames, -1000.0f, 1000.0f);
                if (m_tangentDragIsOut)
                {
                    key.outTangent = slope;
                }
                else
                {
                    key.inTangent = slope;
                }
                break;
            }
        }
        if (m_isDraggingTangent && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_isDraggingTangent = false;
            bool tangentChanged = false;
            for (const auto& key : track.keyframes)
            {
                if (key.frame == m_tangentDragKeyFrame)
                {
                    const float current = m_tangentDragIsOut ? key.outTangent : key.inTangent;
                    tangentChanged = std::abs(current - m_tangentDragStartValue) > 1e-6f;
                    break;
                }
            }
            if (m_hasTrackDragPreState && tangentChanged)
            {
                pushUndoSnapshotFrom(std::move(m_trackDragPreState));
                markDirty();
            }
            m_hasTrackDragPreState = false;
        }
        if (mouseInLane && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hitKeyFrame != -1 &&
            hitTangentKeyFrame == -1)
        {
            if (!ImGui::GetIO().KeyCtrl && !m_selectedTrackKeys.count(hitKeyFrame))
            {
                m_selectedTrackKeys.clear();
            }
            m_selectedTrackKeys.insert(hitKeyFrame);
            m_isDraggingCurveKey = true;
            m_curveDragKeyFrame = hitKeyFrame;
            m_curveDragStartMouseY = mousePos.y;
            m_curveDragRangeMin = minValue;
            m_curveDragRangeMax = maxValue;
            for (const auto& key : track.keyframes)
            {
                if (key.frame == hitKeyFrame)
                {
                    m_curveDragStartValue = key.value;
                    break;
                }
            }
            // 连续拖值只在松开时压一次撤销：先克隆拖拽前状态
            m_trackDragPreState = cloneClipData(clipData);
            m_hasTrackDragPreState = true;
        }
        if (m_isDraggingCurveKey && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            // 垂直位移换算为值增量（向上为正），值域用拖拽起始时冻结的区间
            const float deltaValue = (m_curveDragStartMouseY - mousePos.y) / areaSize.y * (maxValue - minValue);
            for (auto& key : track.keyframes)
            {
                if (key.frame == m_curveDragKeyFrame)
                {
                    key.value = m_curveDragStartValue + deltaValue;
                    break;
                }
            }
        }
        if (m_isDraggingCurveKey && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_isDraggingCurveKey = false;
            bool valueChanged = false;
            for (const auto& key : track.keyframes)
            {
                if (key.frame == m_curveDragKeyFrame)
                {
                    valueChanged = std::abs(key.value - m_curveDragStartValue) > 1e-6f;
                    break;
                }
            }
            if (m_hasTrackDragPreState && valueChanged)
            {
                pushUndoSnapshotFrom(std::move(m_trackDragPreState));
                markDirty();
            }
            m_hasTrackDragPreState = false;
        }
        // 空白处双击或 Ctrl+点击均可加 key，取该时刻曲线值以保形
        if (mouseInLane && hitKeyFrame == -1 && hitTangentKeyFrame == -1 &&
            (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
             (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl)))
        {
            insertTrackKeyAt(m_selectedTrackIndex, hoveredTrackFrame);
        }
    }
    // 右键菜单：命中关键帧弹关键帧菜单，否则弹轨道菜单
    if (areaHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        if (hitKeyFrame != -1)
        {
            if (m_selectedTrackIndex != hitTrack)
            {
                m_selectedTrackKeys.clear();
            }
            m_selectedTrackIndex = hitTrack;
            if (!m_selectedTrackKeys.count(hitKeyFrame))
            {
                m_selectedTrackKeys.clear();
                m_selectedTrackKeys.insert(hitKeyFrame);
            }
            m_trackContextMenuTrack = hitTrack;
        }
        else if (m_curveEditMode)
        {
            m_trackContextMenuTrack = m_selectedTrackIndex;
        }
        else
        {
            m_trackContextMenuTrack = (hoveredRow >= 0 && hoveredRow < trackCount) ? hoveredRow : -1;
        }
        m_trackContextMenuKey = hitKeyFrame;
        m_trackContextMenuFrame = hoveredTrackFrame;
        if (m_trackContextMenuTrack != -1)
        {
            ImGui::OpenPopup("PropertyTrackContextMenu");
        }
    }
    drawTrackContextMenu();
    // 播放头竖线延伸到轨道区
    const float playheadX = frameToX(static_cast<float>(m_currentFrame));
    if (playheadX >= laneX && playheadX <= areaRight)
    {
        drawList->AddLine(ImVec2(playheadX, areaPos.y), ImVec2(playheadX, areaPos.y + areaSize.y),
                          IM_COL32(255, 255, 255, 170), 2.0f);
    }
    if (m_recordMode)
    {
        // 录制状态给轨道区叠加红色描边提示
        drawList->AddRect(areaPos, ImVec2(areaRight, areaPos.y + areaSize.y), IM_COL32(220, 40, 40, 255),
                          0.0f, 0, 2.0f);
    }
}
void AnimationEditorPanel::drawTrackContextMenu()
{
    if (!ImGui::BeginPopup("PropertyTrackContextMenu"))
        return;
    if (!m_currentClip)
    {
        ImGui::EndPopup();
        return;
    }
    auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    if (m_trackContextMenuTrack < 0 || m_trackContextMenuTrack >= static_cast<int>(tracks.size()))
    {
        ImGui::EndPopup();
        return;
    }
    if (m_trackContextMenuKey != -1)
    {
        ImGui::TextDisabled("插值预设");
        if (ImGui::MenuItem("自动平滑"))
        {
            applyTangentPresetToSelectedKeys(0);
        }
        if (ImGui::MenuItem("线性"))
        {
            applyTangentPresetToSelectedKeys(1);
        }
        if (ImGui::MenuItem("阶梯"))
        {
            applyTangentPresetToSelectedKeys(2);
        }
        if (ImGui::MenuItem("缓入 (EaseIn)"))
        {
            applyTangentPresetToSelectedKeys(3);
        }
        if (ImGui::MenuItem("缓出 (EaseOut)"))
        {
            applyTangentPresetToSelectedKeys(4);
        }
        if (ImGui::MenuItem("缓入缓出 (EaseInOut)"))
        {
            applyTangentPresetToSelectedKeys(5);
        }
        ImGui::Separator();
        const std::string deleteLabel = m_selectedTrackKeys.size() > 1
                                            ? std::format("删除选中的 {} 个关键帧", m_selectedTrackKeys.size())
                                            : "删除关键帧";
        if (ImGui::MenuItem(deleteLabel.c_str()))
        {
            removeSelectedTrackKeys();
        }
    }
    else
    {
        if (ImGui::MenuItem("在此添加关键帧"))
        {
            insertTrackKeyAt(m_trackContextMenuTrack, m_trackContextMenuFrame);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("删除轨道"))
    {
        removePropertyTrack(m_trackContextMenuTrack);
    }
    ImGui::EndPopup();
}
void AnimationEditorPanel::openAddTrackPopup()
{
    if (!m_currentClip)
        return;
    if (!hasValidTargetObject())
    {
        LogWarn("请先设置有效的目标物体，再添加属性轨道");
        return;
    }
    m_addTrackCandidates.clear();
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    auto& registry = ComponentRegistry::GetInstance();
    auto& sceneRegistry = scene->GetRegistry();
    const auto entityHandle = targetObject.GetEntityHandle();
    for (const auto& componentName : registry.GetAllRegisteredNames())
    {
        const auto* componentInfo = registry.Get(componentName);
        if (!componentInfo || !componentInfo->isExposedInEditor ||
            !componentInfo->has(sceneRegistry, entityHandle))
        {
            continue;
        }
        for (const auto& prop : componentInfo->properties)
        {
            if (!prop.isExposedInEditor || !prop.get || !prop.set)
            {
                continue;
            }
            // 按属性实际类型拆分可动画的 float 标量通道（Unity 同款：多分量拆多轨）
            const std::any value = prop.get(sceneRegistry, entityHandle);
            if (std::any_cast<float>(&value) || std::any_cast<int>(&value))
            {
                m_addTrackCandidates.emplace_back(componentName, prop.name);
            }
            else if (std::any_cast<ECS::Vector2f>(&value))
            {
                m_addTrackCandidates.emplace_back(componentName, prop.name + ".x");
                m_addTrackCandidates.emplace_back(componentName, prop.name + ".y");
            }
            else if (std::any_cast<ECS::Color>(&value))
            {
                m_addTrackCandidates.emplace_back(componentName, prop.name + ".r");
                m_addTrackCandidates.emplace_back(componentName, prop.name + ".g");
                m_addTrackCandidates.emplace_back(componentName, prop.name + ".b");
                m_addTrackCandidates.emplace_back(componentName, prop.name + ".a");
            }
            else if (std::any_cast<bool>(&value))
            {
                m_addTrackCandidates.emplace_back(componentName, prop.name);
            }
        }
    }
    std::stable_sort(m_addTrackCandidates.begin(), m_addTrackCandidates.end(),
                     [](const auto& a, const auto& b)
                     {
                         const bool aIsTransform = a.first == "TransformComponent";
                         const bool bIsTransform = b.first == "TransformComponent";
                         if (aIsTransform != bIsTransform)
                         {
                             return aIsTransform;
                         }
                         return a.first < b.first;
                     });
    m_addTrackPopupOpen = true;
}
void AnimationEditorPanel::drawAddTrackPopup()
{
    if (!ImGui::Begin("添加属性轨道", &m_addTrackPopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    if (!m_currentClip)
    {
        m_addTrackPopupOpen = false;
        ImGui::End();
        return;
    }
    ImGui::Text("选择要动画化的属性:");
    ImGui::Separator();
    if (m_addTrackCandidates.empty())
    {
        ImGui::TextDisabled("目标物体没有可动画的数值属性");
    }
    const auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    std::string lastComponent;
    for (size_t i = 0; i < m_addTrackCandidates.size(); ++i)
    {
        const auto& [componentName, propertyPath] = m_addTrackCandidates[i];
        if (componentName != lastComponent)
        {
            if (!lastComponent.empty())
            {
                ImGui::Separator();
            }
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", componentName.c_str());
            lastComponent = componentName;
        }
        bool alreadyExists = false;
        for (const auto& track : tracks)
        {
            if (track.targetComponent == componentName && track.propertyPath == propertyPath)
            {
                alreadyExists = true;
                break;
            }
        }
        ImGui::PushID(static_cast<int>(i));
        if (alreadyExists)
        {
            ImGui::TextDisabled("  %s (已添加)", propertyPath.c_str());
        }
        else if (ImGui::Selectable(("  " + propertyPath).c_str()))
        {
            createPropertyTrack(componentName, propertyPath);
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button("关闭"))
    {
        m_addTrackPopupOpen = false;
    }
    ImGui::End();
}
void AnimationEditorPanel::createPropertyTrack(const std::string& componentName, const std::string& propertyPath)
{
    if (!m_currentClip)
        return;
    auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        if (tracks[i].targetComponent == componentName && tracks[i].propertyPath == propertyPath)
        {
            m_selectedTrackIndex = i;
            return;
        }
    }
    pushUndoSnapshot();
    PropertyTrack track;
    track.targetComponent = componentName;
    track.propertyPath = propertyPath;
    // 在播放头帧记录一个初始关键帧，取对象当前值
    PropertyKey key;
    key.frame = m_currentFrame;
    if (hasValidTargetObject())
    {
        auto scene = SceneManager::GetInstance().GetCurrentScene();
        auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
        float currentValue = 0.0f;
        if (AnimationTrackSampler::GetValue(scene->GetRegistry(), targetObject.GetEntityHandle(),
                                            componentName, propertyPath, currentValue))
        {
            key.value = currentValue;
        }
        if (AnimationTrackSampler::IsBoolProperty(scene->GetRegistry(), targetObject.GetEntityHandle(),
                                                  componentName, propertyPath))
        {
            // bool 属性不可插值，默认阶梯
            key.interp = AnimInterpMode::Step;
        }
    }
    track.keyframes.push_back(key);
    tracks.push_back(std::move(track));
    m_selectedTrackIndex = static_cast<int>(tracks.size()) - 1;
    m_selectedTrackKeys.clear();
    m_selectedTrackKeys.insert(key.frame);
    if (m_recordMode)
    {
        refreshRecordCache();
    }
    markDirty();
    LogInfo("添加属性轨道: {}.{}", componentName, propertyPath);
}
void AnimationEditorPanel::removePropertyTrack(int trackIndex)
{
    if (!m_currentClip)
        return;
    auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    pushUndoSnapshot();
    LogInfo("删除属性轨道: {}.{}", tracks[trackIndex].targetComponent, tracks[trackIndex].propertyPath);
    tracks.erase(tracks.begin() + trackIndex);
    if (m_selectedTrackIndex == trackIndex)
    {
        m_selectedTrackIndex = -1;
        m_selectedTrackKeys.clear();
    }
    else if (m_selectedTrackIndex > trackIndex)
    {
        --m_selectedTrackIndex;
    }
    markDirty();
}
void AnimationEditorPanel::removeSelectedTrackKeys()
{
    if (!m_currentClip || m_selectedTrackKeys.empty())
        return;
    auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(tracks.size()))
    {
        // 选中轨道已失效时清空残留选中，避免 Delete 快捷键被持续拦截
        m_selectedTrackKeys.clear();
        return;
    }
    pushUndoSnapshot();
    auto& keys = tracks[m_selectedTrackIndex].keyframes;
    std::erase_if(keys, [this](const PropertyKey& key) { return m_selectedTrackKeys.count(key.frame) != 0; });
    LogInfo("删除 {} 个轨道关键帧", m_selectedTrackKeys.size());
    m_selectedTrackKeys.clear();
    markDirty();
}
void AnimationEditorPanel::insertTrackKeyAt(int trackIndex, int frame)
{
    if (!m_currentClip)
        return;
    auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    frame = std::max(0, frame);
    PropertyTrack& track = tracks[trackIndex];
    for (const auto& key : track.keyframes)
    {
        if (key.frame == frame)
        {
            // 同帧已有关键帧则只切换选中
            m_selectedTrackIndex = trackIndex;
            m_selectedTrackKeys.clear();
            m_selectedTrackKeys.insert(frame);
            return;
        }
    }
    pushUndoSnapshot();
    PropertyKey key;
    key.frame = frame;
    if (!track.keyframes.empty())
    {
        // 取该时刻的曲线值，插入后不改变现有曲线形状；插值模式继承前一关键帧
        key.value = AnimationTrackSampler::Evaluate(track, static_cast<float>(frame));
        const PropertyKey* prevKey = nullptr;
        for (const auto& existing : track.keyframes)
        {
            if (existing.frame <= frame && (!prevKey || existing.frame > prevKey->frame))
            {
                prevKey = &existing;
            }
        }
        key.interp = prevKey ? prevKey->interp : track.keyframes.front().interp;
    }
    else if (hasValidTargetObject())
    {
        auto scene = SceneManager::GetInstance().GetCurrentScene();
        auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
        float currentValue = 0.0f;
        if (AnimationTrackSampler::GetValue(scene->GetRegistry(), targetObject.GetEntityHandle(),
                                            track.targetComponent, track.propertyPath, currentValue))
        {
            key.value = currentValue;
        }
        if (AnimationTrackSampler::IsBoolProperty(scene->GetRegistry(), targetObject.GetEntityHandle(),
                                                  track.targetComponent, track.propertyPath))
        {
            key.interp = AnimInterpMode::Step;
        }
    }
    track.keyframes.push_back(key);
    AnimationTrackSampler::SortKeys(track);
    m_selectedTrackIndex = trackIndex;
    m_selectedTrackKeys.clear();
    m_selectedTrackKeys.insert(frame);
    m_totalFrames = std::max(m_totalFrames, frame + 1);
    markDirty();
}
void AnimationEditorPanel::applyTangentPreset(PropertyTrack& track, size_t keyIndex, int preset)
{
    if (keyIndex >= track.keyframes.size())
        return;
    auto& keys = track.keyframes;
    PropertyKey& key = keys[keyIndex];
    if (preset == 1)
    {
        key.interp = AnimInterpMode::Linear;
        return;
    }
    if (preset == 2)
    {
        key.interp = AnimInterpMode::Step;
        return;
    }
    auto slopeBetween = [](const PropertyKey& a, const PropertyKey& b)
    {
        const float frameSpan = static_cast<float>(b.frame - a.frame);
        return (frameSpan != 0.0f) ? (b.value - a.value) / frameSpan : 0.0f;
    };
    if (preset >= 3 && preset <= 5)
    {
        // 缓动预设作用于本关键帧到下一关键帧的区间，通过 Hermite 切线精确表达：
        // EaseIn: m0=0, m1=2Δ → v0+Δt²；EaseOut: m0=2Δ, m1=0 → v0+Δ(1-(1-t)²)；
        // EaseInOut: m0=m1=0 → 平滑步 v0+Δt²(3-2t)
        key.interp = AnimInterpMode::CubicBezier;
        if (keyIndex + 1 >= keys.size())
        {
            // 最后一个关键帧没有出段，只标记模式
            key.outTangent = 0.0f;
            return;
        }
        PropertyKey& next = keys[keyIndex + 1];
        const float chord = slopeBetween(key, next);
        if (preset == 3)
        {
            key.outTangent = 0.0f;
            next.inTangent = 2.0f * chord;
        }
        else if (preset == 4)
        {
            key.outTangent = 2.0f * chord;
            next.inTangent = 0.0f;
        }
        else
        {
            key.outTangent = 0.0f;
            next.inTangent = 0.0f;
        }
        return;
    }
    // 自动平滑：Catmull-Rom 风格取相邻关键帧连线斜率，端点用单侧斜率
    key.interp = AnimInterpMode::CubicBezier;
    float slope = 0.0f;
    if (keys.size() >= 2)
    {
        if (keyIndex == 0)
        {
            slope = slopeBetween(keys[0], keys[1]);
        }
        else if (keyIndex == keys.size() - 1)
        {
            slope = slopeBetween(keys[keys.size() - 2], keys[keys.size() - 1]);
        }
        else
        {
            slope = slopeBetween(keys[keyIndex - 1], keys[keyIndex + 1]);
        }
    }
    key.inTangent = slope;
    key.outTangent = slope;
}
void AnimationEditorPanel::applyTangentPresetToSelectedKeys(int preset)
{
    if (!m_currentClip || m_selectedTrackKeys.empty())
        return;
    auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(tracks.size()))
        return;
    pushUndoSnapshot();
    PropertyTrack& track = tracks[m_selectedTrackIndex];
    for (size_t k = 0; k < track.keyframes.size(); ++k)
    {
        if (m_selectedTrackKeys.count(track.keyframes[k].frame))
        {
            applyTangentPreset(track, k, preset);
        }
    }
    markDirty();
}
std::vector<std::pair<std::string, std::string>> AnimationEditorPanel::collectRecordTargets() const
{
    std::vector<std::pair<std::string, std::string>> targets;
    if (!m_currentClip)
        return targets;
    const auto& tracks = m_currentClip->getAnimationClip().PropertyTracks;
    targets.reserve(tracks.size() + 5);
    for (const auto& track : tracks)
    {
        targets.emplace_back(track.targetComponent, track.propertyPath);
    }
    // Transform 常用属性即使没有轨道也纳入监听，检测到变化时自动建轨（Unity 录制语义）
    static constexpr const char* kTransformRecordPaths[] = {
        "position.x", "position.y", "rotation", "scale.x", "scale.y"
    };
    for (const char* path : kTransformRecordPaths)
    {
        const bool exists = std::any_of(targets.begin(), targets.end(),
                                        [path](const auto& entry)
                                        {
                                            return entry.first == "TransformComponent" && entry.second == path;
                                        });
        if (!exists)
        {
            targets.emplace_back("TransformComponent", path);
        }
    }
    return targets;
}
void AnimationEditorPanel::updateRecording()
{
    if (!m_recordMode || !m_currentClip || !hasValidTargetObject())
        return;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (!scene)
        return;
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    if (!targetObject.IsValid())
        return;
    auto& clipData = m_currentClip->getAnimationClip();
    auto& sceneRegistry = scene->GetRegistry();
    const auto entityHandle = targetObject.GetEntityHandle();
    const auto monitored = collectRecordTargets();
    struct PendingWrite
    {
        std::string component;
        std::string propertyPath;
        float value;
    };
    std::vector<PendingWrite> writes;
    for (const auto& [componentName, propertyPath] : monitored)
    {
        float currentValue = 0.0f;
        if (!AnimationTrackSampler::GetValue(sceneRegistry, entityHandle,
                                             componentName, propertyPath, currentValue))
        {
            continue;
        }
        const std::string cacheKey = componentName + "/" + propertyPath;
        auto it = m_recordValueCache.find(cacheKey);
        if (it == m_recordValueCache.end())
        {
            // 首次见到的属性只记基线，不产生关键帧
            m_recordValueCache[cacheKey] = currentValue;
            continue;
        }
        if (std::abs(currentValue - it->second) <= 1e-5f)
        {
            continue;
        }
        writes.push_back({componentName, propertyPath, currentValue});
        it->second = currentValue;
    }
    if (writes.empty())
        return;
    auto findTrack = [&clipData](const std::string& component, const std::string& path) -> PropertyTrack*
    {
        for (auto& track : clipData.PropertyTracks)
        {
            if (track.targetComponent == component && track.propertyPath == path)
            {
                return &track;
            }
        }
        return nullptr;
    };
    // 仅在新建轨道/新建关键帧时压撤销栈：同帧连续改值（如拖 Inspector 滑条）聚合为一次撤销
    bool needSnapshot = false;
    for (const auto& write : writes)
    {
        const PropertyTrack* track = findTrack(write.component, write.propertyPath);
        if (!track)
        {
            needSnapshot = true;
            break;
        }
        const bool hasKeyAtPlayhead = std::any_of(track->keyframes.begin(), track->keyframes.end(),
                                                  [this](const PropertyKey& key)
                                                  {
                                                      return key.frame == m_currentFrame;
                                                  });
        if (!hasKeyAtPlayhead)
        {
            needSnapshot = true;
            break;
        }
    }
    if (needSnapshot)
    {
        pushUndoSnapshot();
    }
    for (const auto& write : writes)
    {
        PropertyTrack* track = findTrack(write.component, write.propertyPath);
        if (!track)
        {
            // 无轨道自动建轨：关键帧随后由统一写入逻辑落到播放头帧
            PropertyTrack newTrack;
            newTrack.targetComponent = write.component;
            newTrack.propertyPath = write.propertyPath;
            clipData.PropertyTracks.push_back(std::move(newTrack));
            track = &clipData.PropertyTracks.back();
            LogInfo("录制: 自动创建属性轨道 {}.{}", write.component, write.propertyPath);
        }
        bool updated = false;
        for (auto& key : track->keyframes)
        {
            if (key.frame == m_currentFrame)
            {
                key.value = write.value;
                updated = true;
                break;
            }
        }
        if (!updated)
        {
            PropertyKey key;
            key.frame = m_currentFrame;
            key.value = write.value;
            if (!track->keyframes.empty())
            {
                // 插值模式继承轨道内前一关键帧（bool 轨道的 Step 由此传递）
                const PropertyKey* prevKey = nullptr;
                for (const auto& existing : track->keyframes)
                {
                    if (existing.frame <= m_currentFrame && (!prevKey || existing.frame > prevKey->frame))
                    {
                        prevKey = &existing;
                    }
                }
                key.interp = prevKey ? prevKey->interp : track->keyframes.front().interp;
            }
            track->keyframes.push_back(key);
            AnimationTrackSampler::SortKeys(*track);
        }
    }
    markDirty();
}
void AnimationEditorPanel::refreshRecordCache()
{
    m_recordValueCache.clear();
    if (!m_currentClip || !hasValidTargetObject())
        return;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    if (!targetObject.IsValid())
        return;
    auto& sceneRegistry = scene->GetRegistry();
    const auto entityHandle = targetObject.GetEntityHandle();
    // 基线覆盖全部监听目标（含暂无轨道的 Transform 常用属性），预览写入后立即刷新可避免误判
    for (const auto& [componentName, propertyPath] : collectRecordTargets())
    {
        float value = 0.0f;
        if (AnimationTrackSampler::GetValue(sceneRegistry, entityHandle, componentName, propertyPath, value))
        {
            m_recordValueCache[componentName + "/" + propertyPath] = value;
        }
    }
}
void AnimationEditorPanel::applyPropertyTracksToObject(float frameTime)
{
    if (!m_currentClip || !hasValidTargetObject())
        return;
    const AnimationClip& clipData = m_currentClip->getAnimationClip();
    if (clipData.PropertyTracks.empty())
        return;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (!scene)
        return;
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    if (!targetObject.IsValid())
        return;
    // 轨道写入同样属于预览污染，先备份以便停止/关闭时还原
    backupPreviewTargetState();
    AnimationTrackSampler::ApplyTracks(clipData, scene->GetRegistry(), targetObject.GetEntityHandle(), frameTime);
    if (m_recordMode)
    {
        refreshRecordCache();
    }
}
void AnimationEditorPanel::resetPropertyTrackEditState()
{
    m_recordMode = false;
    m_selectedTrackIndex = -1;
    m_selectedTrackKeys.clear();
    m_isDraggingTrackKey = false;
    m_trackDragHandleFrame = -1;
    m_trackDragTargetFrame = -1;
    m_isDraggingCurveKey = false;
    m_curveDragKeyFrame = -1;
    m_isDraggingTangent = false;
    m_tangentDragKeyFrame = -1;
    m_hasTrackDragPreState = false;
    m_addTrackPopupOpen = false;
    m_addTrackCandidates.clear();
    m_recordValueCache.clear();
}
void AnimationEditorPanel::drawTimelineContextMenu()
{
    if (ImGui::BeginPopup("TimelineContextMenu"))
    {
        if (m_contextMenuKeyframe != -1)
        {
            // 右键未选中的帧时改选它，使复制/删除作用范围直观
            if (!m_multiSelectedFrames.count(m_contextMenuKeyframe))
            {
                m_multiSelectedFrames.clear();
                m_multiSelectedFrames.insert(m_contextMenuKeyframe);
            }
            const size_t selectedCount = m_multiSelectedFrames.size();
            std::string copyLabel = selectedCount > 1 ? std::format("复制选中的 {} 帧", selectedCount) : "复制帧";
            std::string deleteLabel = selectedCount > 1 ? std::format("删除选中的 {} 帧", selectedCount) : "删除帧";
            if (ImGui::MenuItem(copyLabel.c_str()))
            {
                copySelectedFrames();
            }
            if (ImGui::MenuItem(deleteLabel.c_str()))
            {
                removeSelectedKeyFrames();
            }
            if (ImGui::MenuItem("跳到该帧"))
            {
                seekToFrame(m_contextMenuKeyframe);
            }
        }
        else
        {
            if (ImGui::MenuItem("粘贴到此处", nullptr, false, !m_copiedFrames.empty()))
            {
                pasteFramesAt(m_contextMenuFrame);
            }
            if (ImGui::MenuItem("在此添加关键帧", nullptr, false, hasValidTargetObject()))
            {
                addKeyFrameFromCurrentObject(m_contextMenuFrame);
            }
        }
        ImGui::EndPopup();
    }
}
void AnimationEditorPanel::saveCurrentClip()
{
    if (!m_currentClip)
        return;
    AnimationClip& clipData = m_currentClip->getAnimationClip();
    if (clipData.Name.empty())
    {
        clipData.Name = m_currentClipName.empty() ? "未命名动画" : m_currentClipName;
    }
    if (!clipData.TargetEntityGuid.Valid() && m_targetObjectGuid.Valid())
    {
        clipData.TargetEntityGuid = m_targetObjectGuid;
    }
    auto meta = AssetManager::GetInstance().GetMetadata(m_currentClipGuid);
    if (!meta)
    {
        LogError("无法找到动画切片的元数据，GUID: {}", m_currentClipGuid.ToString());
        return;
    }
    auto path = AssetManager::GetInstance().GetAssetsRootPath() / meta->assetPath.filename();
    std::ofstream fout(path);
    if (!fout.is_open())
    {
        LogError("无法打开文件保存动画切片: {}", path.c_str());
        return;
    }
    std::string content = YAML::Dump(YAML::convert<AnimationClip>::encode(clipData));
    fout << content;
    fout.close();
    m_isDirty = false;
    LogInfo("保存动画切片: {} (包含 {} 个关键帧)", clipData.Name, clipData.Frames.size());
}
void AnimationEditorPanel::centerTimelineOnCurrentFrame()
{
    float pixelsPerFrame = 20.0f * m_timelineZoom;
    m_timelineScrollX = m_currentFrame * pixelsPerFrame - 200.0f;
    m_timelineScrollX = std::max(0.0f, m_timelineScrollX);
}
void AnimationEditorPanel::fitTimelineToAllFrames(float viewWidth)
{
    if (m_totalFrames <= 0)
        return;
    float neededPixelsPerFrame = viewWidth / static_cast<float>(m_totalFrames);
    m_timelineZoom = neededPixelsPerFrame / 20.0f;
    m_timelineZoom = std::clamp(m_timelineZoom, 0.1f, 5.0f);
    m_timelineScrollX = 0.0f;
}
void AnimationEditorPanel::applyFrameToObject(int frameIndex)
{
    if (!m_currentClip || !hasValidTargetObject())
        return;
    auto frameIt = m_currentClip->getAnimationClip().Frames.find(frameIndex);
    if (frameIt == m_currentClip->getAnimationClip().Frames.end())
        return;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (!scene)
        return;
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    if (!targetObject.IsValid())
        return;
    // 首次向对象写入预览帧前备份其组件状态，停止/关闭/切换时精确还原
    backupPreviewTargetState();
    const AnimFrame& frame = frameIt->second;
    auto& registry = ComponentRegistry::GetInstance();
    for (const auto& [componentName, componentData] : frame.animationData)
    {
        const auto* componentInfo = registry.Get(componentName);
        if (componentInfo && componentInfo->deserialize)
        {
            try
            {
                componentInfo->deserialize(scene->GetRegistry(), targetObject.GetEntityHandle(), componentData);
                EventBus::GetInstance().Publish(ComponentUpdatedEvent{
                    scene->GetRegistry(), targetObject.GetEntityHandle()
                });
            }
            catch (const std::exception& e)
            {
                LogError("应用组件数据失败 {}: {}", componentName, e.what());
            }
        }
    }
    if (m_recordMode)
    {
        // 快照写入属于预览行为，刷新录制基线
        refreshRecordCache();
    }
}
void AnimationEditorPanel::backupPreviewTargetState()
{
    if (m_hasPreviewBackup && m_previewBackupObjectGuid == m_targetObjectGuid)
        return;
    // 目标对象变了：先把上一个对象还原，再为新对象建快照
    restorePreviewTargetState();
    if (!m_currentClip || !hasValidTargetObject())
        return;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    // 备份范围取剪辑所有帧涉及的组件并集，保证任意帧的写入都可还原
    std::set<std::string> affectedComponents;
    for (const auto& [frameIndex, frame] : m_currentClip->getAnimationClip().Frames)
    {
        for (const auto& [componentName, componentData] : frame.animationData)
        {
            affectedComponents.insert(componentName);
        }
    }
    // 属性轨道写入的组件同样纳入备份，停止预览时一并还原
    for (const auto& track : m_currentClip->getAnimationClip().PropertyTracks)
    {
        affectedComponents.insert(track.targetComponent);
    }
    if (affectedComponents.empty())
        return;
    auto& registry = ComponentRegistry::GetInstance();
    auto& sceneRegistry = scene->GetRegistry();
    auto entityHandle = targetObject.GetEntityHandle();
    m_previewComponentBackup.clear();
    for (const auto& componentName : affectedComponents)
    {
        const auto* componentInfo = registry.Get(componentName);
        if (componentInfo && componentInfo->serialize && componentInfo->has(sceneRegistry, entityHandle))
        {
            m_previewComponentBackup[componentName] = componentInfo->serialize(sceneRegistry, entityHandle);
        }
    }
    if (m_previewComponentBackup.empty())
        return;
    m_previewBackupObjectGuid = m_targetObjectGuid;
    m_hasPreviewBackup = true;
}
void AnimationEditorPanel::restorePreviewTargetState()
{
    if (!m_hasPreviewBackup)
        return;
    m_hasPreviewBackup = false;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (scene)
    {
        auto targetObject = scene->FindGameObjectByGuid(m_previewBackupObjectGuid);
        if (targetObject.IsValid())
        {
            auto& registry = ComponentRegistry::GetInstance();
            for (const auto& [componentName, componentData] : m_previewComponentBackup)
            {
                const auto* componentInfo = registry.Get(componentName);
                if (componentInfo && componentInfo->deserialize)
                {
                    try
                    {
                        componentInfo->deserialize(scene->GetRegistry(), targetObject.GetEntityHandle(),
                                                   componentData);
                        EventBus::GetInstance().Publish(ComponentUpdatedEvent{
                            scene->GetRegistry(), targetObject.GetEntityHandle()
                        });
                    }
                    catch (const std::exception& e)
                    {
                        LogError("还原预览组件状态失败 {}: {}", componentName, e.what());
                    }
                }
            }
            LogInfo("已还原预览目标对象的组件状态");
        }
    }
    m_previewComponentBackup.clear();
    m_previewBackupObjectGuid = Guid();
    if (m_recordMode)
    {
        // 还原写回了旧值，刷新录制基线避免被误判为用户修改
        refreshRecordCache();
    }
}
void AnimationEditorPanel::stopPreviewPlayback()
{
    // 停止预览：还原对象到进入预览前的状态，播放头归零但不再应用帧（避免重新污染）
    m_isPlaying = false;
    restorePreviewTargetState();
    m_currentFrame = 0;
    m_currentTime = 0.0f;
}
void AnimationEditorPanel::updateTargetObject()
{
    if (m_targetObjectGuid.Valid())
    {
        auto scene = SceneManager::GetInstance().GetCurrentScene();
        if (scene)
        {
            auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
            if (targetObject.IsValid())
            {
                m_targetObjectName = targetObject.GetName();
            }
            else
            {
                m_targetObjectName = "无效物体";
            }
        }
        else
        {
            m_targetObjectName = "没有场景";
        }
    }
    else
    {
        m_targetObjectName.clear();
    }
}
bool AnimationEditorPanel::hasValidTargetObject() const
{
    if (!m_targetObjectGuid.Valid())
        return false;
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (!scene)
        return false;
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    return targetObject.IsValid();
}
void AnimationEditorPanel::addKeyFrameFromCurrentObject(int frameIndex)
{
    if (!m_currentClip || !hasValidTargetObject())
    {
        LogWarn("没有打开的动画切片或没有有效的目标物体，无法添加关键帧");
        return;
    }
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (!scene)
        return;
    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
    if (!targetObject.IsValid())
    {
        LogError("找不到目标物体: {}", m_targetObjectGuid.ToString());
        return;
    }
    std::vector<std::string> availableComponents;
    const auto& registry = ComponentRegistry::GetInstance();
    auto entityHandle = targetObject.GetEntityHandle();
    auto& sceneRegistry = scene->GetRegistry();
    for (const auto& componentName : registry.GetAllRegisteredNames())
    {
        const auto* componentInfo = registry.Get(componentName);
        if (componentInfo && componentInfo->has(sceneRegistry, entityHandle))
        {
            availableComponents.push_back(componentName);
        }
    }
    if (availableComponents.empty())
    {
        LogWarn("目标物体没有任何组件可以记录");
        return;
    }
    m_componentSelectorOpen = true;
    m_selectedComponents.clear();
    m_availableComponents = availableComponents;
    m_pendingFrameIndex = frameIndex;
    for (const auto& componentName : m_availableComponents)
    {
        if (componentName == "Transform")
        {
            m_selectedComponents.insert(componentName);
        }
    }
}
void AnimationEditorPanel::drawComponentSelector()
{
    if (!ImGui::Begin("选择要记录的组件", &m_componentSelectorOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    ImGui::Text("选择要在关键帧中记录的组件:");
    ImGui::Separator();
    if (ImGui::Button("全选"))
    {
        m_selectedComponents.clear();
        for (const auto& componentName : m_availableComponents)
        {
            m_selectedComponents.insert(componentName);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("全不选"))
    {
        m_selectedComponents.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("仅Transform"))
    {
        m_selectedComponents.clear();
        for (const auto& componentName : m_availableComponents)
        {
            if (componentName == "Transform")
            {
                m_selectedComponents.insert(componentName);
                break;
            }
        }
    }
    ImGui::Separator();
    for (const auto& componentName : m_availableComponents)
    {
        bool isSelected = m_selectedComponents.find(componentName) != m_selectedComponents.end();
        if (ImGui::Checkbox(componentName.c_str(), &isSelected))
        {
            if (isSelected)
            {
                m_selectedComponents.insert(componentName);
            }
            else
            {
                m_selectedComponents.erase(componentName);
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button("确认添加关键帧"))
    {
        createKeyFrameWithSelectedComponents();
        m_componentSelectorOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消"))
    {
        m_componentSelectorOpen = false;
    }
    ImGui::End();
}
void AnimationEditorPanel::createKeyFrameWithSelectedComponents()
{
    if (m_selectedComponents.empty())
    {
        LogWarn("没有选择任何组件，无法创建关键帧");
        return;
    }
    pushUndoSnapshot();
    markDirty();
    bool frameExists = m_currentClip->getAnimationClip().Frames.find(m_pendingFrameIndex) != m_currentClip->
        getAnimationClip().Frames.end();
    AnimFrame& frame = m_currentClip->getAnimationClip().Frames[m_pendingFrameIndex];
    if (frameExists)
    {
        for (const auto& componentName : m_selectedComponents)
        {
            frame.animationData.erase(componentName);
        }
    }
    auto scene = SceneManager::GetInstance().GetCurrentScene();
    if (scene)
    {
        auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
        if (targetObject.IsValid())
        {
            const auto& registry = ComponentRegistry::GetInstance();
            auto entityHandle = targetObject.GetEntityHandle();
            auto& sceneRegistry = scene->GetRegistry();
            for (const auto& componentName : m_selectedComponents)
            {
                const auto* componentInfo = registry.Get(componentName);
                if (componentInfo && componentInfo->has(sceneRegistry, entityHandle))
                {
                    YAML::Node componentNode = componentInfo->serialize(sceneRegistry, entityHandle);
                    frame.animationData[componentName] = componentNode;
                }
            }
            m_multiSelectedFrames.clear();
            m_multiSelectedFrames.insert(m_pendingFrameIndex);
            LogInfo("添加关键帧: {}，包含 {} 个选中的组件", m_pendingFrameIndex, m_selectedComponents.size());
        }
    }
}
void AnimationEditorPanel::drawFrameEditor()
{
    if (!ImGui::Begin("帧编辑器", &m_frameEditWindowOpen))
    {
        ImGui::End();
        return;
    }
    if (m_editingFrameIndex < 0 || !m_currentClip)
    {
        ImGui::Text("无效的编辑帧");
        ImGui::End();
        return;
    }
    auto it = m_currentClip->getAnimationClip().Frames.find(m_editingFrameIndex);
    if (it == m_currentClip->getAnimationClip().Frames.end())
    {
        ImGui::Text("帧数据不存在");
        ImGui::End();
        return;
    }
    ImGui::Text("编辑帧 %d", m_editingFrameIndex);
    ImGui::Separator();
    AnimFrame& frame = it->second;
    // 帧编辑器里的控件原地修改数据，先克隆前置状态，本帧发生修改时统一压栈一次
    AnimationClip preEditState = cloneClipData(m_currentClip->getAnimationClip());
    bool clipMutated = false;
    if (ImGui::CollapsingHeader("组件数据", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("此帧记录了 %zu 个组件的数据:", frame.animationData.size());
        ImGui::Separator();
        for (auto animDataIt = frame.animationData.begin(); animDataIt != frame.animationData.end();)
        {
            auto& componentName = animDataIt->first;
            auto& componentData = animDataIt->second;
            if (ImGui::TreeNode(componentName.c_str()))
            {
                ImGui::Text("组件类型: %s", componentName.c_str());
                if (componentData.IsMap())
                {
                    ImGui::Text("记录的属性数量: %zu", componentData.size());
                }
                else if (componentData.IsScalar())
                {
                    ImGui::Text("数据: %s", componentData.as<std::string>().c_str());
                }
                if (ImGui::Button("删除组件数据"))
                {
                    animDataIt = frame.animationData.erase(animDataIt);
                    clipMutated = true;
                    ImGui::TreePop();
                    continue;
                }
                ImGui::TreePop();
            }
            ++animDataIt;
        }
        ImGui::Separator();
        if (ImGui::Button("添加更多组件"))
        {
            if (hasValidTargetObject())
            {
                auto scene = SceneManager::GetInstance().GetCurrentScene();
                if (scene)
                {
                    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
                    if (targetObject.IsValid())
                    {
                        std::vector<std::string> unrecordedComponents;
                        const auto& registry = ComponentRegistry::GetInstance();
                        auto entityHandle = targetObject.GetEntityHandle();
                        auto& sceneRegistry = scene->GetRegistry();
                        for (const auto& componentName : registry.GetAllRegisteredNames())
                        {
                            const auto* componentInfo = registry.Get(componentName);
                            if (componentInfo && componentInfo->has(sceneRegistry, entityHandle))
                            {
                                if (frame.animationData.find(componentName) == frame.animationData.end())
                                {
                                    unrecordedComponents.push_back(componentName);
                                }
                            }
                        }
                        if (!unrecordedComponents.empty())
                        {
                            m_componentSelectorOpen = true;
                            m_availableComponents = unrecordedComponents;
                            m_selectedComponents.clear();
                            m_pendingFrameIndex = m_editingFrameIndex;
                            m_isAddingToExistingFrame = true;
                        }
                        else
                        {
                            LogInfo("所有组件都已记录");
                        }
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("从物体刷新已有组件"))
        {
            if (hasValidTargetObject())
            {
                auto scene = SceneManager::GetInstance().GetCurrentScene();
                if (scene)
                {
                    auto targetObject = scene->FindGameObjectByGuid(m_targetObjectGuid);
                    if (targetObject.IsValid())
                    {
                        const auto& registry = ComponentRegistry::GetInstance();
                        auto entityHandle = targetObject.GetEntityHandle();
                        auto& sceneRegistry = scene->GetRegistry();
                        int refreshedCount = 0;
                        for (auto& [componentName, componentData] : frame.animationData)
                        {
                            const auto* componentInfo = registry.Get(componentName);
                            if (componentInfo && componentInfo->has(sceneRegistry, entityHandle))
                            {
                                componentData = componentInfo->serialize(sceneRegistry, entityHandle);
                                refreshedCount++;
                            }
                        }
                        if (refreshedCount > 0)
                        {
                            clipMutated = true;
                        }
                        LogInfo("刷新了 {} 个组件的数据", refreshedCount);
                    }
                }
            }
        }
    }
    if (ImGui::CollapsingHeader("动画事件", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("在此帧触发的事件:");
        ImGui::Separator();
        std::vector<int> indicesToRemove;
        for (size_t i = 0; i < frame.eventTargets.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            std::string eventLabel = std::format("事件 {} [{}]", i,
                                                 frame.eventTargets[i].targetMethodName.empty()
                                                     ? "未设置"
                                                     : frame.eventTargets[i].targetMethodName);
            if (ImGui::TreeNode(eventLabel.c_str()))
            {
                ECS::SerializableEventTarget& target = frame.eventTargets[i];
                if (ImGui::Button("详细编辑"))
                {
                    m_editingEventIndex = static_cast<int>(i);
                    m_eventEditorOpen = true;
                }
                if (CustomDrawing::WidgetDrawer<Guid>::Draw("目标实体", target.targetEntityGuid, *m_context->uiCallbacks))
                {
                    target.targetComponentName = "ScriptComponent";
                    target.targetMethodName.clear();
                    clipMutated = true;
                    m_context->uiCallbacks->onValueChanged.Invoke();
                }
                ImGui::Text("组件名称: ScriptComponent");
                target.targetComponentName = "ScriptComponent";
                ImGui::Text("方法名称:");
                ImGui::SameLine();
                auto availableMethods = CustomDrawing::ScriptMetadataHelper::GetAvailableMethods(
                    target.targetEntityGuid, "");
                std::string currentMethodDisplay = target.targetMethodName;
                if (!target.targetMethodName.empty())
                {
                    for (const auto& [methodName, signature] : availableMethods)
                    {
                        if (methodName == target.targetMethodName)
                        {
                            currentMethodDisplay = std::format("{}({})", methodName,
                                                               signature == "void" ? "" : signature);
                            break;
                        }
                    }
                }
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::BeginCombo("##MethodSelector",
                                      target.targetMethodName.empty() ? "选择方法" : currentMethodDisplay.c_str()))
                {
                    for (const auto& [methodName, signature] : availableMethods)
                    {
                        bool isSelected = (target.targetMethodName == methodName);
                        std::string methodDisplay = std::format("{}({})", methodName,
                                                                signature == "void" ? "" : signature);
                        if (ImGui::Selectable(methodDisplay.c_str(), isSelected))
                        {
                            target.targetMethodName = methodName;
                            clipMutated = true;
                            m_context->uiCallbacks->onValueChanged.Invoke();
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("删除事件"))
                {
                    indicesToRemove.push_back(static_cast<int>(i));
                    m_context->uiCallbacks->onValueChanged.Invoke();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        for (int i = static_cast<int>(indicesToRemove.size()) - 1; i >= 0; --i)
        {
            frame.eventTargets.erase(frame.eventTargets.begin() + indicesToRemove[i]);
            clipMutated = true;
        }
        if (ImGui::Button("添加动画事件"))
        {
            addEventTarget(frame);
            clipMutated = true;
        }
    }
    if (clipMutated)
    {
        pushUndoSnapshotFrom(std::move(preEditState));
        markDirty();
    }
    ImGui::Separator();
    if (ImGui::Button("确认"))
    {
        saveCurrentClip();
        m_frameEditWindowOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭"))
    {
        m_frameEditWindowOpen = false;
    }
    ImGui::End();
}
void AnimationEditorPanel::addEventTarget(AnimFrame& frame)
{
    ECS::SerializableEventTarget newTarget;
    newTarget.targetComponentName = "ScriptComponent";
    if (hasValidTargetObject())
    {
        newTarget.targetEntityGuid = m_targetObjectGuid;
    }
    frame.eventTargets.push_back(newTarget);
    m_context->uiCallbacks->onValueChanged.Invoke();
    LogInfo("添加新的动画事件目标");
}
void AnimationEditorPanel::removeEventTarget(AnimFrame& frame, size_t index)
{
    if (index < frame.eventTargets.size())
    {
        frame.eventTargets.erase(frame.eventTargets.begin() + index);
        m_context->uiCallbacks->onValueChanged.Invoke();
        LogInfo("删除动画事件目标");
    }
}
void AnimationEditorPanel::handleShortcutInput()
{
    if (!m_isFocused) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
    {
        return; // 正在文本输入，快捷键留给输入框
    }
    // 边沿触发，修复按住 Ctrl+S 每帧连发写盘
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
    {
        saveCurrentClip();
    }
    if (m_currentClip && !ImGui::IsAnyItemActive())
    {
        // 面板聚焦时优先处理剪辑数据撤销/重做（工具栏全局撤销已通过帧号标记让路）
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
        {
            performUndo();
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
        {
            performRedo();
        }
    }
    if (m_currentClip)
    {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
        {
            copySelectedFrames();
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
        {
            pasteFramesAt(m_currentFrame);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        // 轨道关键帧选中时 Delete 优先作用于轨道
        if (!m_selectedTrackKeys.empty())
        {
            removeSelectedTrackKeys();
        }
        else
        {
            removeSelectedKeyFrames();
        }
    }
}
void AnimationEditorPanel::drawEventEditor()
{
    if (!ImGui::Begin("动画事件编辑器", &m_eventEditorOpen))
    {
        ImGui::End();
        return;
    }
    if (!m_currentClip || m_editingEventIndex < 0)
    {
        ImGui::Text("没有选中的事件进行编辑");
        ImGui::End();
        return;
    }
    auto frameIt = m_currentClip->getAnimationClip().Frames.find(m_editingFrameIndex);
    if (frameIt == m_currentClip->getAnimationClip().Frames.end())
    {
        ImGui::Text("无效的帧数据");
        ImGui::End();
        return;
    }
    AnimFrame& frame = frameIt->second;
    if (m_editingEventIndex >= static_cast<int>(frame.eventTargets.size()))
    {
        ImGui::Text("无效的事件索引");
        ImGui::End();
        return;
    }
    ECS::SerializableEventTarget& target = frame.eventTargets[m_editingEventIndex];
    ImGui::Text("编辑帧 %d 的事件 %d", m_editingFrameIndex, m_editingEventIndex);
    ImGui::Separator();
    // 与帧编辑器相同：先克隆前置状态，本帧发生修改时统一压栈
    AnimationClip preEditState = cloneClipData(m_currentClip->getAnimationClip());
    bool changed = false;
    if (CustomDrawing::WidgetDrawer<Guid>::Draw("目标实体", target.targetEntityGuid, *m_context->uiCallbacks))
    {
        changed = true;
        target.targetComponentName = "ScriptComponent";
        target.targetMethodName.clear();
    }
    ImGui::Text("组件名称:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "ScriptComponent");
    target.targetComponentName = "ScriptComponent";
    ImGui::Text("方法名称:");
    ImGui::SameLine();
    auto availableMethods = CustomDrawing::ScriptMetadataHelper::GetAvailableMethods(
        target.targetEntityGuid, "");
    std::string currentMethodDisplay = target.targetMethodName;
    if (!target.targetMethodName.empty())
    {
        for (const auto& [methodName, signature] : availableMethods)
        {
            if (methodName == target.targetMethodName)
            {
                currentMethodDisplay = std::format("{}({})", methodName,
                                                   signature == "void" ? "" : signature);
                break;
            }
        }
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##MethodSelector",
                          target.targetMethodName.empty() ? "选择方法" : currentMethodDisplay.c_str()))
    {
        if (availableMethods.empty())
        {
            ImGui::TextDisabled("无可用方法");
        }
        else
        {
            for (const auto& [methodName, signature] : availableMethods)
            {
                bool isSelected = (target.targetMethodName == methodName);
                std::string methodDisplay = std::format("{}({})", methodName,
                                                        signature == "void" ? "" : signature);
                if (ImGui::Selectable(methodDisplay.c_str(), isSelected))
                {
                    target.targetMethodName = methodName;
                    changed = true;
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("方法名: %s", methodName.c_str());
                    ImGui::Text("参数: %s", signature == "void" ? "无" : signature.c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndCombo();
    }
    RuntimeGameObject targetObject = CustomDrawing::ScriptMetadataHelper::GetGameObjectByGuid(
        target.targetEntityGuid);
    if (targetObject.IsValid())
    {
        ImGui::Text("目标对象: %s", targetObject.GetName().c_str());
        ImGui::Separator();
        ImGui::Text("对象详情:");
        ImGui::Text("  GUID: %s", target.targetEntityGuid.ToString().c_str());
        if (targetObject.HasComponent<ECS::ScriptsComponent>())
        {
            auto& scriptsComp = targetObject.GetComponent<ECS::ScriptsComponent>();
            if (scriptsComp.scripts.empty())
            {
                ImGui::TextDisabled("  (无脚本)");
            }
            else
            {
                for (const auto& script : scriptsComp.scripts)
                {
                    if (script.metadata)
                    {
                        ImGui::Text("  脚本类: %s", script.metadata->name.c_str());
                        ImGui::Text("    可用方法数: %zu", script.metadata->publicMethods.size());
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.6f, 1.0f), "  一个脚本组件无元数据");
                    }
                }
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "  对象没有脚本组件");
        }
    }
    else if (target.targetEntityGuid.Valid())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "目标对象无效或不存在");
        ImGui::Text("GUID: %s", target.targetEntityGuid.ToString().c_str());
    }
    else
    {
        ImGui::TextDisabled("请选择目标实体");
    }
    ImGui::Separator();
    if (ImGui::CollapsingHeader("事件预览"))
    {
        if (!target.targetEntityGuid.Valid())
        {
            ImGui::TextDisabled("请先选择目标实体");
        }
        else if (target.targetMethodName.empty())
        {
            ImGui::TextDisabled("请先选择目标方法");
        }
        else
        {
            ImGui::Text("事件调用:");
            ImGui::Text("  实体: %s", targetObject.IsValid() ? targetObject.GetName().c_str() : "无效对象");
            ImGui::Text("  组件: %s", target.targetComponentName.c_str());
            ImGui::Text("  方法: %s", target.targetMethodName.c_str());
            ImGui::Text("  触发帧: %d", m_editingFrameIndex);
        }
    }
    ImGui::Separator();
    bool eventDeleted = false;
    if (ImGui::Button("保存"))
    {
        if (changed)
        {
            m_context->uiCallbacks->onValueChanged.Invoke();
        }
        m_eventEditorOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("取消"))
    {
        m_eventEditorOpen = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("删除此事件"))
    {
        frame.eventTargets.erase(frame.eventTargets.begin() + m_editingEventIndex);
        m_context->uiCallbacks->onValueChanged.Invoke();
        m_eventEditorOpen = false;
        eventDeleted = true;
        LogInfo("删除动画事件目标");
    }
    if (changed || eventDeleted)
    {
        pushUndoSnapshotFrom(std::move(preEditState));
        markDirty();
    }
    ImGui::End();
}
void AnimationEditorPanel::Draw()
{
    PROFILE_FUNCTION();
    if (!m_isVisible) return;
    if (m_requestFocus)
    {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
    // 脏标记显示在标题，###后缀保持窗口 ID 稳定（不破坏停靠布局）
    std::string windowTitle = std::string(GetPanelName()) + (m_isDirty ? " *" : "") + "###动画编辑器";
    // 录制状态用红色窗口描边提示（Unity 录制红框语义）
    const bool recordingBorder = m_recordMode && m_currentClip != nullptr;
    if (recordingBorder)
    {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    }
    if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, ImGuiWindowFlags_MenuBar))
    {
        m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("文件"))
            {
                if (ImGui::MenuItem("保存", "Ctrl+S", false, m_currentClip != nullptr)) { saveCurrentClip(); }
                if (ImGui::MenuItem("关闭", "Ctrl+W", false, m_currentClip != nullptr)) { requestCloseClip(); }
                if (ImGui::MenuItem("新建动画", "Ctrl+N")) { requestNewAnimation(); }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("编辑"))
            {
                bool hasSelection = !m_multiSelectedFrames.empty();
                if (ImGui::MenuItem("撤销", "Ctrl+Z", false, !m_undoStack.empty()))
                {
                    performUndo();
                }
                if (ImGui::MenuItem("重做", "Ctrl+Shift+Z", false, !m_redoStack.empty()))
                {
                    performRedo();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("添加关键帧", "K", false, hasValidTargetObject()))
                {
                    addKeyFrameFromCurrentObject(m_currentFrame);
                }
                if (ImGui::MenuItem("删除关键帧", "Delete", false, hasSelection))
                {
                    removeSelectedKeyFrames();
                }
                if (ImGui::MenuItem("复制帧数据", "Ctrl+C", false, hasSelection))
                {
                    copySelectedFrames();
                }
                if (ImGui::MenuItem("粘贴帧数据", "Ctrl+V", false, !m_copiedFrames.empty() && m_currentFrame >= 0))
                {
                    pasteFramesAt(m_currentFrame);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("播放"))
            {
                if (ImGui::MenuItem(m_isPlaying ? "暂停" : "播放", "Space")) { m_isPlaying = !m_isPlaying; }
                if (ImGui::MenuItem("停止", "Shift+Space"))
                {
                    stopPreviewPlayback();
                }
                if (ImGui::MenuItem("跳到开头", "Home")) { seekToFrame(0); }
                if (ImGui::MenuItem("跳到结尾", "End")) { seekToFrame(m_totalFrames - 1); }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        drawTargetObjectSelector();
        ImGui::Separator();
        if (!m_currentClip)
        {
            ImGui::Text("当前没有打开的动画切片");
            ImGui::Text("双击资源浏览器中的动画切片文件以开始编辑");
            ImGui::Separator();
            drawControlPanel();
            ImGui::Separator();
            drawTimeline();
        }
        else
        {
            drawControlPanel();
            ImGui::Separator();
            drawTimeline();
            ImGui::Separator();
            drawPropertiesPanel();
        }
    }
    ImGui::End();
    if (recordingBorder)
    {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    if (!m_isVisible)
    {
        // 用户点了窗口 X：有未保存修改时先弹确认，否则直接还原预览并停止播放
        if (m_currentClip && m_isDirty && m_pendingCloseAction == PendingCloseAction::None)
        {
            m_isVisible = true;
            m_isPlaying = false;
            openUnsavedConfirmPopup(PendingCloseAction::HidePanel);
        }
        else if (m_pendingCloseAction == PendingCloseAction::None)
        {
            m_isPlaying = false;
            restorePreviewTargetState();
        }
    }
    if (m_isVisible && m_isFocused && m_currentClip)
    {
        // 声明本面板接管撤销/重做快捷键（工具栏在下一帧让路）
        m_context->animationEditorUndoCaptureFrame = ImGui::GetFrameCount();
    }
    handleShortcutInput();
    if (m_frameEditWindowOpen) { drawFrameEditor(); }
    if (m_componentSelectorOpen) { drawComponentSelector(); }
    if (m_eventEditorOpen) { drawEventEditor(); }
    if (m_addTrackPopupOpen) { drawAddTrackPopup(); }
}
void AnimationEditorPanel::drawPropertiesPanel()
{
    if (!m_currentClip)
    {
        ImGui::Text("没有打开的动画切片");
        return;
    }
    ImGui::Text("属性面板");
    if (m_multiSelectedFrames.size() == 1)
    {
        int selectedFrame = *m_multiSelectedFrames.begin();
        auto it = m_currentClip->getAnimationClip().Frames.find(selectedFrame);
        if (it != m_currentClip->getAnimationClip().Frames.end())
        {
            ImGui::Text("关键帧 %d", selectedFrame);
            if (ImGui::Button("编辑帧数据"))
            {
                m_editingFrameIndex = selectedFrame;
                m_frameEditWindowOpen = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("删除关键帧"))
            {
                removeSelectedKeyFrames();
                return;
            }
            ImGui::SameLine();
            if (ImGui::Button("复制帧"))
            {
                copySelectedFrames();
            }
            ImGui::SameLine();
            if (ImGui::Button("应用到物体"))
            {
                applyFrameToObject(selectedFrame);
                applyPropertyTracksToObject(static_cast<float>(selectedFrame));
            }
            const AnimFrame& frame = it->second;
            ImGui::Text("记录的组件: %zu 个", frame.animationData.size());
            if (ImGui::BeginChild("ComponentData", ImVec2(0, 150), true))
            {
                for (const auto& [name, data] : frame.animationData)
                {
                    if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_Leaf))
                    {
                        ImGui::TreePop();
                    }
                }
                ImGui::EndChild();
            }
            ImGui::Text("事件目标: %zu 个", frame.eventTargets.size());
        }
    }
    else if (m_multiSelectedFrames.size() > 1)
    {
        ImGui::Text("已选择 %zu 个关键帧", m_multiSelectedFrames.size());
        if (ImGui::Button("删除所有选中的关键帧"))
        {
            removeSelectedKeyFrames();
        }
        ImGui::SameLine();
        if (ImGui::Button("复制选中帧"))
        {
            copySelectedFrames();
        }
    }
    else
    {
        ImGui::Text("当前帧: %d", m_currentFrame);
        auto it = m_currentClip->getAnimationClip().Frames.find(m_currentFrame);
        if (it != m_currentClip->getAnimationClip().Frames.end())
        {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "这是一个关键帧");
            if (ImGui::Button("编辑此关键帧"))
            {
                m_multiSelectedFrames.insert(m_currentFrame);
            }
        }
        else
        {
            if (hasValidTargetObject())
            {
                if (ImGui::Button("添加关键帧"))
                {
                    addKeyFrameFromCurrentObject(m_currentFrame);
                }
            }
            else
            {
                ImGui::Text("请先设置目标物体");
            }
        }
        if (!m_copiedFrames.empty())
        {
            if (ImGui::Button("粘贴帧数据"))
            {
                pasteFramesAt(m_currentFrame);
            }
        }
    }
}
void AnimationEditorPanel::openUnsavedConfirmPopup(PendingCloseAction action)
{
    m_pendingCloseAction = action;
    m_isPlaying = false;
    m_confirmPopupVisibleFrame = ImGui::GetFrameCount();
    PopupManager::GetInstance().Open("动画未保存修改");
}
void AnimationEditorPanel::drawUnsavedChangesPopup()
{
    // 弹窗可见帧号持续刷新；若用户用 X 关掉弹窗，Update 里检测到帧号过期即视作取消
    m_confirmPopupVisibleFrame = ImGui::GetFrameCount();
    ImGui::Text("动画 \"%s\" 有未保存的修改。", m_currentClipName.c_str());
    ImGui::Separator();
    if (ImGui::Button("保存", ImVec2(80, 0)))
    {
        saveCurrentClip();
        executePendingCloseAction();
        PopupManager::GetInstance().Close("动画未保存修改");
    }
    ImGui::SameLine();
    if (ImGui::Button("丢弃", ImVec2(80, 0)))
    {
        discardCurrentClipChanges();
        executePendingCloseAction();
        PopupManager::GetInstance().Close("动画未保存修改");
    }
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(80, 0)))
    {
        m_pendingCloseAction = PendingCloseAction::None;
        m_pendingOpenClipGuid = Guid();
        PopupManager::GetInstance().Close("动画未保存修改");
    }
}
void AnimationEditorPanel::discardCurrentClipChanges()
{
    if (!m_currentClip)
        return;
    // 剪辑实例被运行时缓存共享，丢弃时从资产元数据（磁盘版本）恢复数据，避免未保存修改残留
    const AssetMetadata* meta = AssetManager::GetInstance().GetMetadata(m_currentClipGuid);
    if (meta && meta->importerSettings.IsDefined())
    {
        try
        {
            m_currentClip->getAnimationClip() = meta->importerSettings.as<AnimationClip>();
            // 同步面板镜像字段（名称/帧率/循环/选中集）；其内部的置脏随后被清除
            afterClipDataRestored();
        }
        catch (const std::exception& e)
        {
            LogError("丢弃修改时恢复动画数据失败: {}", e.what());
        }
    }
    m_undoStack.clear();
    m_redoStack.clear();
    m_isDirty = false;
}
void AnimationEditorPanel::executePendingCloseAction()
{
    const PendingCloseAction action = m_pendingCloseAction;
    m_pendingCloseAction = PendingCloseAction::None;
    switch (action)
    {
    case PendingCloseAction::HidePanel:
        m_isPlaying = false;
        restorePreviewTargetState();
        m_isVisible = false;
        break;
    case PendingCloseAction::CloseClip:
        m_context->currentEditingAnimationClipGuid = Guid();
        closeCurrentClipFromContext();
        break;
    case PendingCloseAction::OpenOther:
        {
            const Guid targetGuid = m_pendingOpenClipGuid;
            m_context->currentEditingAnimationClipGuid = targetGuid;
            openAnimationClipFromContext(targetGuid);
            break;
        }
    case PendingCloseAction::NewClip:
        createNewAnimation();
        break;
    default:
        break;
    }
    m_pendingOpenClipGuid = Guid();
}
void AnimationEditorPanel::requestCloseClip()
{
    if (!m_currentClip)
        return;
    if (m_isDirty)
    {
        openUnsavedConfirmPopup(PendingCloseAction::CloseClip);
    }
    else
    {
        CloseCurrentClip();
    }
}
void AnimationEditorPanel::requestNewAnimation()
{
    if (m_currentClip && m_isDirty)
    {
        openUnsavedConfirmPopup(PendingCloseAction::NewClip);
    }
    else
    {
        createNewAnimation();
    }
}
