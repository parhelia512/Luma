#include "FileWatcher.h"
#include "Logger.h"

#ifdef _WIN32
// 工程范围可能已通过编译选项定义这两个宏，须加守卫避免 C4005 触发 /WX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <vector>
#endif

FileWatcher::FileWatcher(const std::filesystem::path& watchDir, std::chrono::milliseconds interval)
    : m_watchDir(watchDir), m_interval(interval)
{
}

FileWatcher::~FileWatcher()
{
    Stop();
}

void FileWatcher::SetCallback(FileWatchCallback callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callback = std::move(callback);
}

bool FileWatcher::IsRunning() const
{
    return m_running.load();
}

void FileWatcher::ScanDirectory(std::unordered_map<std::string, std::filesystem::file_time_type>& snapshot)
{
    snapshot.clear();
    std::error_code ec;
    for (auto& entry : std::filesystem::recursive_directory_iterator(m_watchDir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;

        auto path = entry.path().lexically_normal().string();
        auto lwt = entry.last_write_time(ec);
        if (!ec)
            snapshot[path] = lwt;
    }
}

#ifdef _WIN32

namespace
{
    /// Modified 事件去抖窗口：同一路径在该窗口内的多次通知合并为一次
    constexpr std::chrono::milliseconds kDebounceInterval{150};

    /// 通知缓冲区大小（64KB 为跨网络路径亦安全的上限）
    constexpr DWORD kNotifyBufferSize = 64 * 1024;
}

void FileWatcher::Start()
{
    if (m_running.load())
        return;

    std::error_code ec;
    if (!std::filesystem::exists(m_watchDir, ec) || ec)
    {
        LogWarn("FileWatcher: Watch directory does not exist: {}", m_watchDir.string());
        return;
    }

    HANDLE dirHandle = CreateFileW(m_watchDir.wstring().c_str(),
                                   FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                   nullptr);
    if (dirHandle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        LogWarn("FileWatcher: Failed to open directory for watching: {} (error {})",
                m_watchDir.string(), error);
        return;
    }

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr || ioEvent == nullptr)
    {
        const DWORD error = GetLastError();
        LogWarn("FileWatcher: Failed to create event handles (error {})", error);
        if (stopEvent != nullptr)
            CloseHandle(stopEvent);
        if (ioEvent != nullptr)
            CloseHandle(ioEvent);
        CloseHandle(dirHandle);
        return;
    }

    m_dirHandle = dirHandle;
    m_stopEvent = stopEvent;
    m_ioEvent = ioEvent;

    // 建立初始快照：用于重命名/删除目录时的补报以及缓冲溢出后的差异重扫
    ScanDirectory(m_fileTimestamps);

    m_running.store(true);
    m_thread = std::thread(&FileWatcher::WatchLoop, this);
    LogInfo("FileWatcher: Started watching {} (ReadDirectoryChangesW)", m_watchDir.string());
}

void FileWatcher::Stop()
{
    // 不能仅凭 m_running 判断：监视线程可能因错误提前退出但仍需 join
    if (!m_running.load() && !m_thread.joinable())
        return;

    m_running.store(false);
    if (m_stopEvent != nullptr)
        SetEvent(static_cast<HANDLE>(m_stopEvent));

    if (m_thread.joinable())
        m_thread.join();

    if (m_dirHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_dirHandle));
        m_dirHandle = nullptr;
    }
    if (m_ioEvent != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_ioEvent));
        m_ioEvent = nullptr;
    }
    if (m_stopEvent != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_stopEvent));
        m_stopEvent = nullptr;
    }
    m_pendingModified.clear();

    LogInfo("FileWatcher: Stopped.");
}

void FileWatcher::WatchLoop()
{
    HANDLE dirHandle = static_cast<HANDLE>(m_dirHandle);
    HANDLE stopEvent = static_cast<HANDLE>(m_stopEvent);
    HANDLE ioEvent = static_cast<HANDLE>(m_ioEvent);

    constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

    std::vector<unsigned char> buffer(kNotifyBufferSize);
    OVERLAPPED overlapped{};
    overlapped.hEvent = ioEvent;
    bool ioPending = false;

    while (m_running.load())
    {
        if (!ioPending)
        {
            ResetEvent(ioEvent);
            if (!ReadDirectoryChangesW(dirHandle, buffer.data(), kNotifyBufferSize, TRUE,
                                       kNotifyFilter, nullptr, &overlapped, nullptr))
            {
                const DWORD error = GetLastError();
                if (error == ERROR_NOTIFY_ENUM_DIR)
                {
                    // 系统积压过多变更：降级为一次全量重扫
                    RescanAndEmitDiff();
                    continue;
                }
                LogWarn("FileWatcher: ReadDirectoryChangesW failed (error {}), watcher thread exiting.", error);
                break;
            }
            ioPending = true;
        }

        HANDLE waitHandles[2] = {stopEvent, ioEvent};
        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, ComputeWaitTimeoutMs());

        if (waitResult == WAIT_OBJECT_0)
        {
            // 停止事件
            break;
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            FlushPendingModified(false);
            continue;
        }
        if (waitResult != WAIT_OBJECT_0 + 1)
        {
            LogWarn("FileWatcher: WaitForMultipleObjects failed (error {}), watcher thread exiting.", GetLastError());
            break;
        }

        DWORD bytesTransferred = 0;
        ioPending = false;
        if (!GetOverlappedResult(dirHandle, &overlapped, &bytesTransferred, FALSE))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_NOTIFY_ENUM_DIR)
            {
                RescanAndEmitDiff();
                continue;
            }
            if (error == ERROR_OPERATION_ABORTED)
                break;
            LogWarn("FileWatcher: GetOverlappedResult failed (error {}), watcher thread exiting.", error);
            break;
        }

        if (bytesTransferred == 0)
        {
            // 缓冲区溢出（变更太多装不下）：降级为一次全量重扫
            RescanAndEmitDiff();
            continue;
        }

        ProcessNotifyBuffer(buffer.data(), bytesTransferred);
        FlushPendingModified(false);
    }

    // 干净退出：取消未完成的重叠 IO 并等待其真正结束，防止悬挂请求写已释放的缓冲区
    if (ioPending)
    {
        CancelIoEx(dirHandle, &overlapped);
        DWORD bytesTransferred = 0;
        GetOverlappedResult(dirHandle, &overlapped, &bytesTransferred, TRUE);
    }
    FlushPendingModified(true);
}

void FileWatcher::ProcessNotifyBuffer(const unsigned char* buffer, unsigned long bytes)
{
    if (bytes == 0)
        return;

    size_t offset = 0;
    for (;;)
    {
        const FILE_NOTIFY_INFORMATION* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
        const std::wstring relative(info->FileName, info->FileNameLength / sizeof(WCHAR));
        const std::filesystem::path fullPath = (m_watchDir / relative).lexically_normal();

        switch (info->Action)
        {
        case FILE_ACTION_ADDED:
        case FILE_ACTION_RENAMED_NEW_NAME: // 重命名映射为 Deleted(旧) + Created(新)
            HandleNativeEvent(fullPath, FileChangeType::Created);
            break;
        case FILE_ACTION_REMOVED:
        case FILE_ACTION_RENAMED_OLD_NAME:
            HandleNativeEvent(fullPath, FileChangeType::Deleted);
            break;
        case FILE_ACTION_MODIFIED:
            HandleNativeEvent(fullPath, FileChangeType::Modified);
            break;
        default:
            break;
        }

        if (info->NextEntryOffset == 0)
            break;
        offset += info->NextEntryOffset;
    }
}

void FileWatcher::HandleNativeEvent(const std::filesystem::path& path, FileChangeType type)
{
    const std::string key = path.string();
    std::error_code ec;

    switch (type)
    {
    case FileChangeType::Created:
        {
            if (std::filesystem::is_directory(path, ec))
            {
                // 目录被创建/移入：系统只通知目录本身，为其中的文件补报 Created（与轮询行为一致）
                std::error_code iterEc;
                for (auto& entry : std::filesystem::recursive_directory_iterator(path, iterEc))
                {
                    if (iterEc)
                        break;
                    std::error_code entryEc;
                    if (!entry.is_regular_file(entryEc))
                        continue;
                    const auto childPath = entry.path().lexically_normal();
                    const auto lwt = entry.last_write_time(entryEc);
                    if (!entryEc)
                        m_fileTimestamps[childPath.string()] = lwt;
                    EmitEvent(childPath, FileChangeType::Created);
                }
                return;
            }
            const auto lwt = std::filesystem::last_write_time(path, ec);
            if (!ec)
                m_fileTimestamps[key] = lwt;
            m_pendingModified.erase(key);
            EmitEvent(path, FileChangeType::Created);
            return;
        }
    case FileChangeType::Deleted:
        {
            m_pendingModified.erase(key);
            m_fileTimestamps.erase(key);

            // 目录被删除/移出时系统只通知目录本身：按快照前缀为其中的文件补报 Deleted
            const std::string prefix = key + static_cast<char>(std::filesystem::path::preferred_separator);
            for (auto it = m_fileTimestamps.begin(); it != m_fileTimestamps.end();)
            {
                if (it->first.compare(0, prefix.size(), prefix) == 0)
                {
                    m_pendingModified.erase(it->first);
                    EmitEvent(std::filesystem::path(it->first), FileChangeType::Deleted);
                    it = m_fileTimestamps.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            EmitEvent(path, FileChangeType::Deleted);
            return;
        }
    case FileChangeType::Modified:
        {
            // 目录自身的写入时间变化不作为资源修改上报（轮询实现只跟踪常规文件）
            if (!std::filesystem::is_regular_file(path, ec) || ec)
                return;
            const auto lwt = std::filesystem::last_write_time(path, ec);
            if (!ec)
                m_fileTimestamps[key] = lwt;
            // 去抖：只登记时间，待 150ms 内无后续通知时再派发
            m_pendingModified[key] = PendingEntry{std::chrono::steady_clock::now(), path};
            return;
        }
    default:
        return;
    }
}

void FileWatcher::FlushPendingModified(bool force)
{
    if (m_pendingModified.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_pendingModified.begin(); it != m_pendingModified.end();)
    {
        if (force || now - it->second.lastNotify >= kDebounceInterval)
        {
            EmitEvent(it->second.path, FileChangeType::Modified);
            it = m_pendingModified.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FileWatcher::RescanAndEmitDiff()
{
    LogWarn("FileWatcher: Change notification overflow, performing a full rescan of {}", m_watchDir.string());

    std::unordered_map<std::string, std::filesystem::file_time_type> current;
    ScanDirectory(current);

    for (auto& [path, time] : current)
    {
        auto it = m_fileTimestamps.find(path);
        if (it == m_fileTimestamps.end())
        {
            EmitEvent(std::filesystem::path(path), FileChangeType::Created);
        }
        else if (it->second != time)
        {
            // Modified 仍走去抖，避免与后续原生通知重复派发
            m_pendingModified[path] = PendingEntry{std::chrono::steady_clock::now(), std::filesystem::path(path)};
        }
    }

    for (auto& [path, time] : m_fileTimestamps)
    {
        if (current.find(path) == current.end())
        {
            m_pendingModified.erase(path);
            EmitEvent(std::filesystem::path(path), FileChangeType::Deleted);
        }
    }

    m_fileTimestamps = std::move(current);
}

void FileWatcher::EmitEvent(const std::filesystem::path& path, FileChangeType type)
{
    FileWatchCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        cb = m_callback;
    }
    if (cb)
    {
        cb({path, type});
    }
}

unsigned long FileWatcher::ComputeWaitTimeoutMs() const
{
    if (m_pendingModified.empty())
        return INFINITE;

    auto earliest = std::chrono::steady_clock::time_point::max();
    for (const auto& pending : m_pendingModified)
    {
        earliest = std::min(earliest, pending.second.lastNotify);
    }

    const auto now = std::chrono::steady_clock::now();
    const auto due = earliest + kDebounceInterval;
    if (due <= now)
        return 1;

    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count() + 1;
    const long long clamped = std::clamp(ms, static_cast<long long>(1),
                                         static_cast<long long>(kDebounceInterval.count()));
    return static_cast<unsigned long>(clamped);
}

#else

void FileWatcher::Start()
{
    if (m_running.load())
        return;

    if (!std::filesystem::exists(m_watchDir))
    {
        LogWarn("FileWatcher: Watch directory does not exist: {}", m_watchDir.string());
        return;
    }

    ScanDirectory(m_fileTimestamps);
    m_running.store(true);
    m_thread = std::thread(&FileWatcher::WatchLoop, this);
    LogInfo("FileWatcher: Started watching {}", m_watchDir.string());
}

void FileWatcher::Stop()
{
    if (!m_running.load())
        return;

    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();

    LogInfo("FileWatcher: Stopped.");
}

void FileWatcher::WatchLoop()
{
    while (m_running.load())
    {
        std::this_thread::sleep_for(m_interval);
        if (!m_running.load())
            break;

        std::unordered_map<std::string, std::filesystem::file_time_type> current;
        ScanDirectory(current);

        FileWatchCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            cb = m_callback;
        }
        if (!cb)
        {
            m_fileTimestamps = std::move(current);
            continue;
        }

        for (auto& [path, time] : current)
        {
            auto it = m_fileTimestamps.find(path);
            if (it == m_fileTimestamps.end())
            {
                cb({std::filesystem::path(path), FileChangeType::Created});
            }
            else if (it->second != time)
            {
                cb({std::filesystem::path(path), FileChangeType::Modified});
            }
        }

        for (auto& [path, time] : m_fileTimestamps)
        {
            if (current.find(path) == current.end())
            {
                cb({std::filesystem::path(path), FileChangeType::Deleted});
            }
        }

        m_fileTimestamps = std::move(current);
    }
}

#endif
