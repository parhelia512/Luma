#ifndef CONSOLEPANEL_H
#define CONSOLEPANEL_H
#include "IEditorPanel.h"
#include "../../Utils/Logger.h"
#include "../../Event/LumaEvent.h"
#include <vector>
#include <string>
#include <chrono>
class ConsolePanel : public IEditorPanel
{
public:
    ConsolePanel() = default;
    ~ConsolePanel() override;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "控制台"; }
    void ClearLogs();
    void SetAutoScroll(bool enabled) { autoScroll = enabled; }
    size_t GetLogCount() const { return logEntries.size(); }
private:
    struct LogEntry
    {
        std::string message; 
        LogLevel level; 
        std::chrono::steady_clock::time_point timestamp; 
        int count; 
        bool isCollapsed; 
        LogEntry(std::string_view msg, LogLevel lvl)
            : message(msg), level(lvl), timestamp(std::chrono::steady_clock::now()), count(1), isCollapsed(false)
        {
        }
    };
    struct LogFilter
    {
        bool showInfo = true; 
        bool showWarning = true; 
        bool showError = true; 
        bool showDebug = true; 
    };
    void drawToolbar();
    void drawLogEntries();
    void drawLogEntry(const LogEntry& entry, int index);
    void onLogMessage(std::string_view message, LogLevel level);
    bool shouldShowLogEntry(const LogEntry& entry) const;
    ImVec4 getLogLevelColor(LogLevel level) const;
    const char* getLogLevelIcon(LogLevel level) const;
    const char* getLogLevelText(LogLevel level) const;
    std::string formatTimestamp(const std::chrono::steady_clock::time_point& timestamp) const;

    /**
     * @brief 尝试从日志消息中解析可跳转的源码位置。
     *        识别 MSVC/dotnet 诊断格式：文件路径(行[,列]): error|warning ...，
     *        路径可包含空格与中文；仅对扩展名为 .cs 的路径启用跳转。
     * @param message 日志消息文本（UTF-8）
     * @param outFile 解析出的 .cs 文件路径（UTF-8）
     * @param outLine 解析出的行号（从 1 开始）
     * @return 消息中含有可跳转位置时返回 true
     */
    static bool tryParseJumpTarget(const std::string& message, std::string& outFile, int& outLine);

    /**
     * @brief 在用户偏好的 IDE 中打开指定文件并定位到行。
     *        IDE 选择顺序：PreferenceSettings 偏好 -> 自动检测；
     *        解决方案为项目根目录下的 LumaScripting.sln。
     * @param filePath 目标文件路径（UTF-8；相对路径按项目根目录解析）
     * @param line 目标行号（从 1 开始）
     */
    static void openInIDE(const std::string& filePath, int line);
    void scrollToBottom();
    void updateLogCounts();
    bool canCollapseWith(const LogEntry& entry1, const LogEntry& entry2) const;
    void collapseRepeatedMessages();
private:
    std::vector<LogEntry> logEntries; 
    LogFilter filter; 
    ListenerHandle logListenerHandle; 
    bool autoScroll = true; 
    bool scrollToBottomB = false; 
    bool collapseEnabled = true; 
    bool clearOnPlay = false; 
    char searchBuffer[256] = {0}; 
    int infoCount = 0; 
    int warningCount = 0; 
    int errorCount = 0; 
    int debugCount = 0; 
    int totalLogCount = 0; 
    static constexpr size_t MAX_LOG_ENTRIES = 2000; 
    mutable std::string timestampCache; 
    bool errorFilterActive = true; 
    bool warningFilterActive = true; 
    bool infoFilterActive = true; 
};
#endif
