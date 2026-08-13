#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include <string>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

enum class FileChangeType
{
    Created,
    Modified,
    Deleted
};

struct FileChangeEvent
{
    std::filesystem::path path;
    FileChangeType type;
};

using FileWatchCallback = std::function<void(const FileChangeEvent&)>;

/**
 * @brief 递归目录文件变更监视器。
 *        Windows 下基于 ReadDirectoryChangesW 原生通知实现（低开销、事件驱动，
 *        对同一路径 150ms 内的多次 Modified 通知做去抖合并，通知缓冲溢出时
 *        降级为一次全量重扫）；其他平台保留基于时间戳快照的轮询实现。
 *        回调在后台线程中触发，路径为经 lexically_normal 规范化的绝对路径。
 */
class FileWatcher
{
public:
    /**
     * @brief 构造监视器。
     * @param watchDir 要递归监视的目录
     * @param interval 轮询间隔（仅非 Windows 的轮询实现使用）
     */
    explicit FileWatcher(const std::filesystem::path& watchDir,
                         std::chrono::milliseconds interval = std::chrono::milliseconds(500));
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /**
     * @brief 设置文件变更回调（后台线程中触发）。
     * @param callback 变更事件回调
     */
    void SetCallback(FileWatchCallback callback);

    /**
     * @brief 开始监视。目录不存在或原生句柄创建失败时不会启动。
     */
    void Start();

    /**
     * @brief 停止监视并等待后台线程退出（不悬挂）。可重复调用。
     */
    void Stop();

    /**
     * @brief 查询监视器是否处于运行状态。
     * @return 运行中返回 true
     */
    bool IsRunning() const;

private:
    void WatchLoop();

    /**
     * @brief 全量扫描监视目录，记录所有常规文件的最后写入时间。
     * @param snapshot 输出的 路径 -> 最后写入时间 快照
     */
    void ScanDirectory(std::unordered_map<std::string, std::filesystem::file_time_type>& snapshot);

#ifdef _WIN32
    /**
     * @brief 解析 ReadDirectoryChangesW 填充的 FILE_NOTIFY_INFORMATION 缓冲区。
     * @param buffer 通知缓冲区首地址
     * @param bytes 缓冲区内有效字节数
     */
    void ProcessNotifyBuffer(const unsigned char* buffer, unsigned long bytes);

    /**
     * @brief 处理单条原生变更事件：维护快照、目录级补报以及 Modified 去抖登记。
     * @param path 规范化后的绝对路径
     * @param type 变更类型（重命名已在上层映射为 Deleted + Created）
     */
    void HandleNativeEvent(const std::filesystem::path& path, FileChangeType type);

    /**
     * @brief 派发去抖窗口已到期的 Modified 事件。
     * @param force 为 true 时忽略去抖窗口，立即派发全部挂起事件（用于退出前排空）
     */
    void FlushPendingModified(bool force);

    /**
     * @brief 通知缓冲溢出时的降级路径：全量重扫一次并按快照差异补发事件。
     */
    void RescanAndEmitDiff();

    /**
     * @brief 在回调锁保护下取出回调并派发一条事件。
     * @param path 事件路径
     * @param type 事件类型
     */
    void EmitEvent(const std::filesystem::path& path, FileChangeType type);

    /**
     * @brief 计算距最近一条挂起 Modified 事件到期的等待毫秒数。
     * @return 无挂起事件时返回 INFINITE，否则返回 [1, 去抖窗口] 内的毫秒数
     */
    unsigned long ComputeWaitTimeoutMs() const;
#endif

    std::filesystem::path m_watchDir;
    std::chrono::milliseconds m_interval;
    FileWatchCallback m_callback;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_callbackMutex;

    std::unordered_map<std::string, std::filesystem::file_time_type> m_fileTimestamps;

#ifdef _WIN32
    void* m_dirHandle = nullptr; ///< 被监视目录句柄（HANDLE，避免在头文件引入 windows.h）
    void* m_stopEvent = nullptr; ///< 停止事件句柄（HANDLE）
    void* m_ioEvent = nullptr;   ///< 重叠 IO 完成事件句柄（HANDLE）

    /**
     * @brief 去抖挂起项：记录最近一次通知时间与精确路径（避免经窄字符串往返丢失信息）。
     */
    struct PendingEntry
    {
        std::chrono::steady_clock::time_point lastNotify; ///< 最近一次 Modified 通知时间
        std::filesystem::path path;                       ///< 事件的精确路径
    };

    /// 去抖登记表：规范化路径字符串 -> 挂起项（仅监视线程访问，无需加锁）
    std::unordered_map<std::string, PendingEntry> m_pendingModified;
#endif
};

#endif
