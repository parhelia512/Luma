#ifndef EDITOR_H
#define EDITOR_H
#include <memory>
#include <vector>
#include "ApplicationBase.h"
#include "Editor/EditorContext.h"
#include "Editor/IEditorPanel.h"
class ImGuiRenderer;
class SceneRenderer;
class RuntimeScene;
struct UIDrawData;
class LUMA_API Editor final : public ApplicationBase
{
public:
    static Editor* GetInstance()
    {
        return s_instance;
    }
    Editor(ApplicationConfig config);
    bool checkDotNetEnvironment();
    ~Editor() override;
    void RequestFocusInHierarchy(const Guid& guid);
    void RequestFocusInBrowser(const Guid& guid);
    void CreateNewProject();
    void CreateNewPluginProject();
    void OpenProject();
    void LoadProject(const std::filesystem::path& projectPath);
    void CreateNewProjectAtPath(const std::filesystem::path& projectPath);
    void CreatePluginProjectAtPath(const std::filesystem::path& projectPath);
    void SetPendingProjectPath(const std::filesystem::path& path) { m_pendingProjectPath = path; }
    const std::filesystem::path& GetPendingProjectPath() const { return m_pendingProjectPath; }
    IEditorPanel* GetPanelByName(const std::string& name);
    PlatformWindow* GetPlatWindow();
    EditorContext& GetEditorContext() { return m_editorContext; }

    /**
     * @brief 由入口在启动早期设置：上一编辑器会话是否异常退出（崩溃/强杀）。
     *
     * 必须在写入本次会话标记之前检测并注入，编辑器初始化完成后据此弹出自动保存恢复提示。
     */
    static void SetPreviousSessionCrashed(bool crashed) { s_previousSessionCrashed = crashed; }
protected:
    void InitializeDerived() override;
    void Update(float deltaTime) override;
    void Render() override;
    void ShutdownDerived() override;
private:
    void initializeEditorContext(); 
    void initializePanels(); 
    void registerPopups(); 
    void loadStartupScene(); 
    void drawAddComponentPopupContent(); 
    void drawFileConflictPopupContent(); 
    void drawExitConfirmPopupContent();
    void drawCrashRecoveryPopupContent();
    void checkCrashRecovery();
    void updateUps(); 
    void updateFps();
private:
    EditorContext m_editorContext; 
    std::unique_ptr<UIDrawData> m_uiCallbacks; 
    std::vector<std::unique_ptr<IEditorPanel>> m_panels; 
    std::unique_ptr<ImGuiRenderer> m_imguiRenderer; 
    std::unique_ptr<SceneRenderer> m_sceneRenderer; 
    std::filesystem::path m_pendingProjectPath; 
    bool m_undoEditActive = false; ///< 连续编辑（拖拽/输入）进行中，撤销快照推迟到编辑结束提交。
    float m_autosaveTimer = 0.0f; ///< 自动保存计时器（秒）。
    ListenerHandle m_closeRequestListener; ///< 窗口关闭请求监听（未保存确认）。
    std::filesystem::path m_latestAutosavePath; ///< 崩溃恢复提示中展示的最新自动保存文件。
    inline static bool s_previousSessionCrashed = false; ///< 上一会话是否异常退出（入口注入）。
    inline static Editor* s_instance; 
};
#endif
