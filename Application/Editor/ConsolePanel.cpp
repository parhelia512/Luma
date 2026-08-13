#include "../Utils/PCH.h"
#include "Resources/RuntimeAsset/RuntimeScene.h"
#include "ConsolePanel.h"
#include "../Utils/Logger.h"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <regex>
#include <sstream>
#include "IDEIntegration.h"
#include "PreferenceSettings.h"
#include "ProjectSettings.h"
#include "Profiler.h"
ConsolePanel::~ConsolePanel()
{
    if (logListenerHandle.IsValid())
    {
        Logger::RemoveLogListener(logListenerHandle);
    }
}
void ConsolePanel::Initialize(EditorContext* context)
{
    m_context = context;
    logListenerHandle = Logger::AddLogListener([this](std::string_view message, LogLevel level)
    {
        this->onLogMessage(message, level);
    });
    filter.showInfo = infoFilterActive;
    filter.showWarning = warningFilterActive;
    filter.showError = errorFilterActive;
    filter.showDebug = true;
}
void ConsolePanel::Update(float deltaTime)
{
    PROFILE_FUNCTION();
    if (scrollToBottomB)
    {
        scrollToBottomB = false;
    }
    if (clearOnPlay && m_context && m_context->editorState == EditorState::Playing)
    {
        static bool wasPlaying = false;
        if (!wasPlaying)
        {
            ClearLogs();
        }
        wasPlaying = true;
    }
    else if (m_context && m_context->editorState != EditorState::Playing)
    {
        static bool wasPlaying = false;
        wasPlaying = false;
    }
}
void ConsolePanel::Draw()
{
    PROFILE_FUNCTION();
    ImGui::Begin(GetPanelName(), &m_isVisible);
    m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    drawToolbar();
    ImGui::Separator();
    drawLogEntries();
    ImGui::End();
}
void ConsolePanel::Shutdown()
{
    if (logListenerHandle.IsValid())
    {
        Logger::RemoveLogListener(logListenerHandle);
        logListenerHandle = ListenerHandle{};
    }
    logEntries.clear();
}
void ConsolePanel::ClearLogs()
{
    logEntries.clear();
    infoCount = 0;
    warningCount = 0;
    errorCount = 0;
    debugCount = 0;
    totalLogCount = 0;
}
void ConsolePanel::drawToolbar()
{
    if (ImGui::Button("清空"))
    {
        ClearLogs();
    }
    ImGui::SameLine();
    ImGui::Checkbox("合并", &collapseEnabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("合并相同的连续日志消息");
    }
    ImGui::SameLine();
    ImGui::Checkbox("播放时清空", &clearOnPlay);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("进入播放模式时自动清空控制台");
    }
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &autoScroll);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("自动滚动到最新日志消息");
    }
    ImGui::SameLine();
    float searchWidth = 200.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - searchWidth - 20.0f);
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::InputTextWithHint("##Search", "搜索日志...", searchBuffer, sizeof(searchBuffer));
    ImGui::Spacing();
    bool errorPressed = false;
    if (errorCount > 0 || !errorFilterActive)
    {
        if (!errorFilterActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.224f, 0.235f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.24f, 0.24f, 1.0f));
        }
        errorPressed = ImGui::Button(std::format("{}  {}", getLogLevelIcon(LogLevel::Error), errorCount).c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::BeginDisabled();
        ImGui::Button(std::format("{}  {}", getLogLevelIcon(LogLevel::Error), errorCount).c_str());
        ImGui::EndDisabled();
    }
    if (errorPressed)
    {
        errorFilterActive = !errorFilterActive;
        filter.showError = errorFilterActive;
    }
    ImGui::SameLine();
    bool warningPressed = false;
    if (warningCount > 0 || !warningFilterActive)
    {
        if (!warningFilterActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.224f, 0.235f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.66f, 0.52f, 0.14f, 1.0f));
        }
        warningPressed = ImGui::Button(std::format("{}  {}", getLogLevelIcon(LogLevel::Warning), warningCount).c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::BeginDisabled();
        ImGui::Button(std::format("{}  {}", getLogLevelIcon(LogLevel::Warning), warningCount).c_str());
        ImGui::EndDisabled();
    }
    if (warningPressed)
    {
        warningFilterActive = !warningFilterActive;
        filter.showWarning = warningFilterActive;
    }
    ImGui::SameLine();
    bool infoPressed = false;
    if (infoCount > 0 || !infoFilterActive)
    {
        if (!infoFilterActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.224f, 0.235f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.227f, 0.396f, 0.561f, 1.0f));
        }
        infoPressed = ImGui::Button(std::format("{}  {}", getLogLevelIcon(LogLevel::Info), infoCount).c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::BeginDisabled();
        ImGui::Button(std::format("{}  {}", getLogLevelIcon(LogLevel::Info), infoCount).c_str());
        ImGui::EndDisabled();
    }
    if (infoPressed)
    {
        infoFilterActive = !infoFilterActive;
        filter.showInfo = infoFilterActive;
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 120.0f);
    if (totalLogCount != static_cast<int>(logEntries.size()))
    {
        ImGui::Text("显示: %zu / 总计: %d", logEntries.size(), totalLogCount);
    }
    else
    {
        ImGui::Text("总计: %zu", logEntries.size());
    }
}
void ConsolePanel::drawLogEntries()
{
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    std::vector<int> visibleIndices;
    for (int i = 0; i < static_cast<int>(logEntries.size()); ++i)
    {
        if (shouldShowLogEntry(logEntries[i]))
        {
            visibleIndices.push_back(i);
        }
    }
    clipper.Begin(static_cast<int>(visibleIndices.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            if (i < static_cast<int>(visibleIndices.size()))
            {
                int actualIndex = visibleIndices[i];
                drawLogEntry(logEntries[actualIndex], actualIndex);
            }
        }
    }
    if (autoScroll && scrollToBottomB)
    {
        ImGui::SetScrollHereY(1.0f);
        scrollToBottomB = false;
    }
    ImGui::EndChild();
}
void ConsolePanel::drawLogEntry(const LogEntry& entry, int index)
{
    ImGui::PushID(index);
    ImVec4 levelColor = getLogLevelColor(entry.level);
    const char* levelIcon = getLogLevelIcon(entry.level);
    ImVec4 bgColor = ImVec4(0, 0, 0, 0);
    if (entry.level == LogLevel::Error)
    {
        bgColor = ImVec4(0.8f, 0.2f, 0.2f, 0.1f);
    }
    else if (entry.level == LogLevel::Warning)
    {
        bgColor = ImVec4(0.9f, 0.7f, 0.0f, 0.1f);
    }
    bool isSelected = false;
    if (bgColor.w > 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Header, bgColor);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(bgColor.x, bgColor.y, bgColor.z, bgColor.w * 1.5f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(bgColor.x, bgColor.y, bgColor.z, bgColor.w * 2.0f));
    }
    if (ImGui::Selectable(std::format("##LogEntry{}", index).c_str(), &isSelected,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
    {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            std::string jumpFile;
            int jumpLine = 0;
            if (tryParseJumpTarget(entry.message, jumpFile, jumpLine))
            {
                // 含可跳转的编译诊断位置：双击打开 IDE 并定位，不再复制到剪贴板
                openInIDE(jumpFile, jumpLine);
            }
            else
            {
                std::string clipboardText = entry.message;
                if (entry.count > 1)
                {
                    clipboardText += std::format(" (重复 {} 次)", entry.count);
                }
                ImGui::SetClipboardText(clipboardText.c_str());
            }
        }
    }
    if (bgColor.w > 0)
    {
        ImGui::PopStyleColor(3);
    }
    if (ImGui::BeginPopupContextItem())
    {
        std::string jumpFile;
        int jumpLine = 0;
        if (tryParseJumpTarget(entry.message, jumpFile, jumpLine))
        {
            if (ImGui::MenuItem("在 IDE 中打开"))
            {
                openInIDE(jumpFile, jumpLine);
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("复制消息"))
        {
            ImGui::SetClipboardText(entry.message.c_str());
        }
        if (ImGui::MenuItem("复制完整信息"))
        {
            std::string fullInfo = std::format("[{}] {} {}",
                                               formatTimestamp(entry.timestamp),
                                               getLogLevelText(entry.level),
                                               entry.message);
            if (entry.count > 1)
            {
                fullInfo += std::format(" (重复 {} 次)", entry.count);
            }
            ImGui::SetClipboardText(fullInfo.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("清空所有日志"))
        {
            ClearLogs();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, levelColor);
    ImGui::Text("%s", levelIcon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    ImGui::Text("[%s]", formatTimestamp(entry.timestamp).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", entry.message.c_str());
    if (collapseEnabled && entry.count > 1)
    {
        ImGui::SameLine();
        ImVec2 textSize = ImGui::CalcTextSize(std::to_string(entry.count).c_str());
        ImVec2 boxSize = ImVec2(textSize.x + 8, textSize.y + 4);
        ImVec2 cursorPos = ImGui::GetCursorPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 screenPos = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(
            screenPos,
            ImVec2(screenPos.x + boxSize.x, screenPos.y + boxSize.y),
            IM_COL32(100, 100, 100, 200),
            3.0f
        );
        ImGui::SetCursorPos(ImVec2(cursorPos.x + 4, cursorPos.y + 2));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::Text("%d", entry.count);
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}
void ConsolePanel::onLogMessage(std::string_view message, LogLevel level)
{
    totalLogCount++;
    if (collapseEnabled && !logEntries.empty())
    {
        LogEntry& lastEntry = logEntries.back();
        if (canCollapseWith(lastEntry, LogEntry(message, level)))
        {
            lastEntry.count++;
            lastEntry.timestamp = std::chrono::steady_clock::now();
            lastEntry.isCollapsed = true;
            return;
        }
    }
    logEntries.emplace_back(message, level);
    updateLogCounts();
    if (logEntries.size() > MAX_LOG_ENTRIES)
    {
        logEntries.erase(logEntries.begin());
        updateLogCounts();
    }
    if (autoScroll)
    {
        scrollToBottomB = true;
    }
}
bool ConsolePanel::shouldShowLogEntry(const LogEntry& entry) const
{
    switch (entry.level)
    {
    case LogLevel::Info:
        if (!filter.showInfo) return false;
        break;
    case LogLevel::Warning:
        if (!filter.showWarning) return false;
        break;
    case LogLevel::Error:
        if (!filter.showError) return false;
        break;
    case LogLevel::Debug:
        if (!filter.showDebug) return false;
        break;
    case LogLevel::Trace:
        if (!filter.showDebug) return false;
        break;
    case LogLevel::Critical:
        if (!filter.showError) return false;
        break;
    default:
        break;
    }
    if (strlen(searchBuffer) > 0)
    {
        std::string searchTerm = searchBuffer;
        std::string message = entry.message;
        std::transform(searchTerm.begin(), searchTerm.end(), searchTerm.begin(), ::tolower);
        std::transform(message.begin(), message.end(), message.begin(), ::tolower);
        if (message.find(searchTerm) == std::string::npos)
        {
            return false;
        }
    }
    return true;
}
ImVec4 ConsolePanel::getLogLevelColor(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Info:
        return ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
    case LogLevel::Warning:
        return ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
    case LogLevel::Error:
    case LogLevel::Critical: 
        return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    case LogLevel::Debug:
    case LogLevel::Trace: 
        return ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    default:
        return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    }
}
const char* ConsolePanel::getLogLevelIcon(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Info:
        return "ℹ";
    case LogLevel::Warning:
        return "⚠";
    case LogLevel::Error:
    case LogLevel::Critical: 
        return "❌";
    case LogLevel::Debug:
    case LogLevel::Trace: 
        return "🐛";
    default:
        return "?";
    }
}
const char* ConsolePanel::getLogLevelText(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Info:
        return "信息";
    case LogLevel::Warning:
        return "警告";
    case LogLevel::Error:
        return "错误";
    case LogLevel::Critical: 
        return "严重";
    case LogLevel::Debug:
        return "调试";
    case LogLevel::Trace: 
        return "追踪";
    default:
        return "未知";
    }
}
std::string ConsolePanel::formatTimestamp(const std::chrono::steady_clock::time_point& timestamp) const
{
    static auto startTime = std::chrono::steady_clock::now();
    auto duration = timestamp - startTime;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration - seconds);
    auto totalSeconds = seconds.count();
    int minutes = static_cast<int>(totalSeconds / 60);
    int secs = static_cast<int>(totalSeconds % 60);
    int ms = static_cast<int>(milliseconds.count());
    return std::format("{:02d}:{:02d}.{:03d}", minutes, secs, ms);
}
void ConsolePanel::scrollToBottom()
{
}
void ConsolePanel::updateLogCounts()
{
    infoCount = 0;
    warningCount = 0;
    errorCount = 0;
    debugCount = 0;
    for (const auto& entry : logEntries)
    {
        int multiplier = collapseEnabled ? entry.count : 1;
        switch (entry.level)
        {
        case LogLevel::Info:
            infoCount += multiplier;
            break;
        case LogLevel::Warning:
            warningCount += multiplier;
            break;
        case LogLevel::Error:
        case LogLevel::Critical: 
            errorCount += multiplier;
            break;
        case LogLevel::Debug:
        case LogLevel::Trace: 
            debugCount += multiplier;
            break;
        default:
            break;
        }
    }
}
bool ConsolePanel::canCollapseWith(const LogEntry& entry1, const LogEntry& entry2) const
{
    return entry1.message == entry2.message && entry1.level == entry2.level;
}
void ConsolePanel::collapseRepeatedMessages()
{
    if (logEntries.empty()) return;
    std::vector<LogEntry> collapsedEntries;
    collapsedEntries.reserve(logEntries.size());
    for (const auto& entry : logEntries)
    {
        if (!collapsedEntries.empty() && canCollapseWith(collapsedEntries.back(), entry))
        {
            collapsedEntries.back().count += entry.count;
            collapsedEntries.back().timestamp = entry.timestamp;
            collapsedEntries.back().isCollapsed = true;
        }
        else
        {
            collapsedEntries.push_back(entry);
        }
    }
    logEntries = std::move(collapsedEntries);
    updateLogCounts();
}
bool ConsolePanel::tryParseJumpTarget(const std::string& message, std::string& outFile, int& outLine)
{
    // 匹配 MSVC/dotnet 诊断格式：路径(行[,列]): error|warning ...
    // 路径 = 可选盘符 + 不含 Windows 非法字符与冒号的字节序列（空格、UTF-8 中文均可），
    // 且以 .cs 结尾；行/列为十进制数字。按字节匹配，不使用 icase 以避免高位字节的
    // 区域设置问题（编译器诊断中的 error/warning 恒为小写）。
    static const std::regex kJumpPattern(
        R"re(((?:[A-Za-z]:)?[^:*?"<>|\r\n]+\.[cC][sS])\((\d+)(?:\s*,\s*\d+)?\)\s*:\s*(?:fatal\s+)?(?:error|warning)\b)re");
    std::smatch match;
    if (!std::regex_search(message, match, kJumpPattern))
    {
        return false;
    }
    std::string file = match[1].str();
    const size_t firstNonSpace = file.find_first_not_of(" \t");
    if (firstNonSpace == std::string::npos)
    {
        return false;
    }
    file.erase(0, firstNonSpace);
    const std::string lineText = match[2].str();
    int line = 0;
    const std::from_chars_result parseResult =
        std::from_chars(lineText.data(), lineText.data() + lineText.size(), line);
    if (parseResult.ec != std::errc() || line <= 0)
    {
        return false;
    }
    outFile = std::move(file);
    outLine = line;
    return true;
}
void ConsolePanel::openInIDE(const std::string& filePath, int line)
{
    IDE ide = PreferenceSettings::GetInstance().GetPreferredIDE();
    if (ide == IDE::Unknown)
    {
        ide = IDEIntegration::DetectInstalledIDE();
    }
    if (ide == IDE::Unknown)
    {
        LogError("未检测到受支持的 IDE（Rider、Visual Studio、VS Code），无法跳转到 {}:{}", filePath, line);
        return;
    }
    const std::filesystem::path projectRoot = ProjectSettings::GetInstance().GetProjectRoot();
    const std::filesystem::path solutionPath = projectRoot / "LumaScripting.sln";
    // 日志消息为 UTF-8，经 char8_t 构造以保证中文路径正确转换为原生编码
    std::filesystem::path targetFile(reinterpret_cast<const char8_t*>(filePath.c_str()));
    if (targetFile.is_relative())
    {
        targetFile = projectRoot / targetFile;
    }
    if (!IDEIntegration::OpenAt(ide, solutionPath, targetFile, line))
    {
        LogError("无法启动 IDE 打开文件: {}", targetFile.string());
    }
}
