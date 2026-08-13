#ifndef TOOLBARPANEL_H
#define TOOLBARPANEL_H
#include <atomic>
#include <future>
#include <mutex>
#include <vector>
#include <array>
#include "IEditorPanel.h"
#include <string>
#include <filesystem>
enum class TargetPlatform;
class ProjectSettings;

/**
 * @brief 可跨线程读写的状态文本。
 *
 * 后台任务（脚本编译/打包）持续写入进度描述，UI 线程每帧读取显示。
 * std::string 的并发读写是未定义行为，这里用互斥锁封装。
 */
class ThreadSafeText
{
public:
    ThreadSafeText() = default;
    ThreadSafeText& operator=(const std::string& text)
    {
        Set(text);
        return *this;
    }
    ThreadSafeText& operator=(const char* text)
    {
        Set(text ? std::string(text) : std::string());
        return *this;
    }
    void Set(std::string text)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_text = std::move(text);
    }
    std::string Get() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_text;
    }

private:
    mutable std::mutex m_mutex;
    std::string m_text;
};
class ToolbarPanel : public IEditorPanel
{
public:
    ToolbarPanel() = default;
    ~ToolbarPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void drawPackagingPopup();
    void Shutdown() override;
    const char* GetPanelName() const override { return "工具栏"; }
    void OnKeystoreSavePathChosen(const std::filesystem::path& path);
private:
    void drawMainMenuBar();
    void drawViewportMenu();
    void drawProjectMenu();
    void drawWindowMenu();
    void drawFileMenu();
    void drawEditMenu();
    void drawSettingsWindow();
    void drawPlayControls();
    void drawFpsDisplay();
    void updateFps();
    void rebuildScripts();
    void launchScriptCompilation();
    bool runScriptCompilationLogic(ThreadSafeText& statusMessage, const std::filesystem::path& outPath = "");
    void drawPreferencesPopup();
    void drawScriptCompilationPopup();
    void newScene();
    void createNewSceneNow();
    void drawNewSceneConfirmPopup();
    void saveScene();
    void play();
    void pause();
    void stop();
    void undo();
    void redo();
    void drawSaveBeforePackagingPopup();
    void packageGame();
    void handleShortcuts();
    void startPackagingProcess();
    bool runScriptCompilationLogicForPackaging(ThreadSafeText& statusMessage, TargetPlatform targetPlatform);
    void updateAndroidGradleProperties(const std::filesystem::path& platformOutputDir, const ProjectSettings& settings);
    std::filesystem::path signAndroidApk(const std::filesystem::path& unsignedApk, const ProjectSettings& settings);
    void refreshKeystoreCandidates(const std::filesystem::path& projectRoot);
    void drawKeystorePickerPopup(const std::filesystem::path& projectRoot);
    void drawCreateKeystorePopup();
    void drawCreateAliasPopup();
    SDL_Window* getSDLWindow() const;
    std::atomic<bool> m_isPackaging{false};
    ThreadSafeText m_packagingStatus;
    std::atomic<float> m_packagingProgress{0.0f};
    bool m_isSettingsWindowVisible; 
    std::atomic<bool> m_isCompilingScripts{false};
    std::atomic<bool> m_compilationFinished{false};
    std::atomic<bool> m_compilationSuccess{false};
    std::atomic<bool> m_recompileQueued{false}; ///< 编译期间又有脚本变更时置位，本轮结束后自动补编一轮。
    float m_compileResultShownAt = -1.0f; ///< 编译结果角标的展示起始时间（UI 线程使用）。
    ThreadSafeText m_compilationStatus;
    ListenerHandle m_CSharpScriptUpdated; 
    std::future<void> m_packagingFuture; 
    std::future<void> m_compilationFuture; 
    std::atomic<bool> m_packagingSuccess{false};
    std::filesystem::path m_lastBuildDirectory; 
    bool m_isTransitioningPlayState = false; 
    bool m_shouldOpenKeystorePicker = false;
    std::vector<std::filesystem::path> m_keystoreCandidates;
    std::array<char, 512> m_keystorePickerBuffer{};
    struct KeystorePopupState
    {
        bool openRequested = false;
        char path[512] = "";
        char storePassword[128] = "";
        char storePasswordConfirm[128] = "";
        char alias[128] = "luma_key";
        char aliasPassword[128] = "";
        char aliasPasswordConfirm[128] = "";
        std::string errorMessage;
    } m_keystorePopupState;
    struct AliasPopupState
    {
        bool openRequested = false;
        char alias[128] = "";
        char password[128] = "";
        char passwordConfirm[128] = "";
        std::string errorMessage;
    } m_aliasPopupState;
};
#endif
