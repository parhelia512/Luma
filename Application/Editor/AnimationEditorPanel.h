#ifndef ANIMATIONEDITORPANEL_H
#define ANIMATIONEDITORPANEL_H
#include "IEditorPanel.h"
#include <unordered_map>
#include <string>
#include "Loaders/TextureLoader.h"
#include "RuntimeAsset/RuntimeAnimationClip.h"
class AnimationEditorPanel : public IEditorPanel
{
private:
    sk_sp<RuntimeAnimationClip> m_currentClip; 
    Guid m_currentClipGuid; 
    std::string m_currentClipName; 
    Guid m_targetObjectGuid; 
    std::string m_targetObjectName; 
    float m_currentTime = 0.0f; 
    float m_frameRate = 60.0f; 
    int m_currentFrame = 0; 
    int m_totalFrames = 60; 
    bool m_isPlaying = false; 
    bool m_isLooping = true; 
    float m_timelineHeight = 200.0f; 
    float m_timelineZoom = 1.0f; 
    float m_timelineScrollX = 0.0f; 
    int m_selectedFrameIndex = -1; 
    bool m_frameEditWindowOpen = false; 
    int m_editingFrameIndex = -1; 
    std::vector<std::pair<int, AnimFrame>> m_copiedFrames; ///< 多帧复制缓冲：相对最小选中帧的偏移 -> 深拷贝帧数据
    bool m_componentSelectorOpen = false; 
    std::vector<std::string> m_availableComponents; 
    std::set<std::string> m_selectedComponents; 
    int m_pendingFrameIndex = -1; 
    bool m_isAddingToExistingFrame = false; 
    bool m_eventEditorOpen = false; 
    int m_editingEventIndex = -1; 
    std::string m_newEventName; 
    std::vector<std::string> m_availableEventTypes; 
    bool m_isDirty = false; ///< 剪辑数据自上次保存后是否被修改
    bool m_onionSkinEnabled = false; ///< 洋葱皮开关（降级实现：时间轴高亮播放头前后关键帧）
    std::vector<AnimationClip> m_undoStack; ///< 面板级撤销栈（剪辑数据全量快照）
    std::vector<AnimationClip> m_redoStack; ///< 面板级重做栈
    std::unordered_map<std::string, YAML::Node> m_previewComponentBackup; ///< 预览前目标对象受影响组件的快照
    Guid m_previewBackupObjectGuid; ///< 预览快照所属对象
    bool m_hasPreviewBackup = false; 
    int m_contextMenuFrame = -1; ///< 时间轴右键菜单落点帧
    int m_contextMenuKeyframe = -1; ///< 右键命中的关键帧索引（未命中为 -1）
    /// 关闭/切换动作因未保存修改而挂起时的类型
    enum class PendingCloseAction
    {
        None,
        HidePanel,
        CloseClip,
        OpenOther,
        NewClip
    };
    PendingCloseAction m_pendingCloseAction = PendingCloseAction::None; 
    Guid m_pendingOpenClipGuid; ///< 挂起的待切换剪辑
    int m_confirmPopupVisibleFrame = -1000; ///< 确认弹窗最近可见的 ImGui 帧号（检测弹窗被 X 关闭视作取消）
    // —— 属性轨道编辑状态 ——
    bool m_curveEditMode = false; ///< 轨道区显示模式：false=Dopesheet 关键帧行，true=曲线视图
    bool m_recordMode = false; ///< 录制模式：目标对象属性变化时自动在播放头帧写关键帧
    int m_selectedTrackIndex = -1; ///< 当前选中的属性轨道下标
    std::set<int> m_selectedTrackKeys; ///< 选中轨道内被选中的关键帧帧号集合
    bool m_isDraggingTrackKey = false; ///< Dopesheet 中正在水平拖动轨道关键帧
    int m_trackDragHandleFrame = -1; ///< 轨道关键帧拖拽起始命中的帧号
    int m_trackDragTargetFrame = -1; ///< 轨道关键帧拖拽当前指向的帧号
    bool m_isDraggingCurveKey = false; ///< 曲线视图中正在垂直拖动关键帧改值
    int m_curveDragKeyFrame = -1; ///< 曲线拖拽目标关键帧帧号
    float m_curveDragStartValue = 0.0f; ///< 曲线拖拽起始值
    float m_curveDragStartMouseY = 0.0f; ///< 曲线拖拽起始鼠标 Y
    float m_curveDragRangeMin = 0.0f; ///< 拖拽期间冻结的曲线值域下界（避免视图随值缩放抖动）
    float m_curveDragRangeMax = 1.0f; ///< 拖拽期间冻结的曲线值域上界
    bool m_isDraggingTangent = false; ///< 曲线视图中正在拖动 Bezier 切线柄
    int m_tangentDragKeyFrame = -1; ///< 切线拖拽目标关键帧帧号
    bool m_tangentDragIsOut = false; ///< 拖拽的是出切线柄（false=入切线柄）
    float m_tangentDragStartValue = 0.0f; ///< 拖拽起始的切线斜率，松开时无变化则不压撤销栈
    AnimationClip m_trackDragPreState; ///< 曲线连续拖拽前的剪辑快照，松开时压撤销栈
    bool m_hasTrackDragPreState = false; 
    int m_trackContextMenuTrack = -1; ///< 轨道区右键命中的轨道下标
    int m_trackContextMenuKey = -1; ///< 轨道区右键命中的关键帧帧号（未命中为 -1）
    int m_trackContextMenuFrame = -1; ///< 轨道区右键落点帧
    bool m_addTrackPopupOpen = false; 
    std::vector<std::pair<std::string, std::string>> m_addTrackCandidates; ///< 可动画属性候选：组件名 + 属性路径
    std::unordered_map<std::string, float> m_recordValueCache; ///< 录制对比缓存："组件/属性路径" -> 上次读取值
    void openAnimationClipFromContext(const Guid& clipGuid);
    void closeCurrentClipFromContext();
    void createNewAnimation();
    void drawTargetObjectSelector();
    void drawTimeline();
    void drawTimelineContextMenu();
    void drawFrameEditor();
    void drawPropertiesPanel();
    void drawControlPanel();
    void updatePlayback(float deltaTime);
    void seekToFrame(int frameIndex);
    void addKeyFrame(int frameIndex);
    void addKeyFrameFromCurrentObject(int frameIndex);
    void removeKeyFrame(int frameIndex);
    void removeSelectedKeyFrames();
    void saveCurrentClip();
    void centerTimelineOnCurrentFrame();
    void fitTimelineToAllFrames(float viewWidth);
    void applyFrameToObject(int frameIndex);
    void updateTargetObject();
    bool hasValidTargetObject() const;
    void drawComponentSelector();
    void createKeyFrameWithSelectedComponents();
    void drawEventEditor();
    void addEventTarget(AnimFrame& frame);
    void removeEventTarget(AnimFrame& frame, size_t index);
    void handleShortcutInput();
    static AnimFrame cloneFrameData(const AnimFrame& source);
    static AnimationClip cloneClipData(const AnimationClip& source);
    void pushUndoSnapshot();
    void pushUndoSnapshotFrom(AnimationClip snapshot);
    void performUndo();
    void performRedo();
    void afterClipDataRestored();
    void markDirty();
    void backupPreviewTargetState();
    void restorePreviewTargetState();
    void stopPreviewPlayback();
    void copySelectedFrames();
    void pasteFramesAt(int targetFrame);
    void drawPropertyTrackSection();
    void drawTrackContextMenu();
    void drawAddTrackPopup();
    void openAddTrackPopup();
    void createPropertyTrack(const std::string& componentName, const std::string& propertyPath);
    void removePropertyTrack(int trackIndex);
    void removeSelectedTrackKeys();
    void insertTrackKeyAt(int trackIndex, int frame);
    void applyTangentPresetToSelectedKeys(int preset);
    static void applyTangentPreset(PropertyTrack& track, size_t keyIndex, int preset);
    void updateRecording();
    void refreshRecordCache();
    /// 收集录制监听目标：已有轨道涉及的属性 ∪ Transform 常用属性（组件名 + 属性路径）
    std::vector<std::pair<std::string, std::string>> collectRecordTargets() const;
    void applyPropertyTracksToObject(float frameTime);
    void resetPropertyTrackEditState();
    void drawUnsavedChangesPopup();
    void executePendingCloseAction();
    void discardCurrentClipChanges();
    void requestCloseClip();
    void requestNewAnimation();
    void openUnsavedConfirmPopup(PendingCloseAction action);
public:
    AnimationEditorPanel() = default;
    ~AnimationEditorPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "动画编辑器"; }
    void OpenAnimationClip(const Guid& clipGuid);
    void CloseCurrentClip();
    bool HasActiveClip() const { return m_currentClip != nullptr; }
    void Focus() override;
    std::set<int> m_multiSelectedFrames; 
    bool m_isDraggingPlayhead = false; 
    bool m_isDraggingKeyframe = false; 
    int m_dragAnchorFrame = -1; 
    int m_dragFrameDelta = 0; 
    bool m_isBoxSelecting = false; 
    ImVec2 m_boxSelectionStart; 
    std::vector<int> m_dragInitialSelectionState; 
    int m_dragHandleFrame = -1; 
    std::unique_ptr<TextureLoader> m_textureLoader; 
    bool m_requestFocus = false; 
};
#endif
