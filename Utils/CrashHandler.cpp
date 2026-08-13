#include "CrashHandler.h"

#ifdef _WIN32

// include 顺序要求：dbghelp.h 依赖 windows.h 中的类型定义，windows.h 必须在前。
// 引擎 PCH（Logger.h / Platform.h）已包含完整 windows.h（未定义 WIN32_LEAN_AND_MEAN），
// 此处显式包含以保证本文件自洽；重复包含由头文件保护宏消化。
#include <windows.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4091) // 兼容旧版 Windows SDK 中 dbghelp.h 的匿名 typedef 告警
#endif
#include <dbghelp.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define LUMA_RETURN_ADDRESS() _ReturnAddress()
#else
#define LUMA_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    // =========================================================================
    // 崩溃回调共享状态：全部在 Install 阶段预生成。
    // 崩溃回调内不做堆分配、不调用 CRT 格式化函数，仅使用下列静态缓冲
    // 与少量 Win32 API（CreateFileW / WriteFile / MiniDumpWriteDump 等）。
    // =========================================================================

    constexpr size_t kPathCapacity = 1024;      ///< 宽字符路径缓冲容量（字符数）
    constexpr size_t kReportCapacity = 4096;    ///< 崩溃文本报告缓冲容量（字节）
    constexpr size_t kMaxDumpKeepCount = 10;    ///< dump 文件保留份数
    constexpr UINT kCrashExitCode = 0xDEAD;     ///< 崩溃处理完成后的进程退出码

    constexpr DWORD kTerminateCode = 0xE0000001;        ///< std::terminate 的自定义异常代码
    constexpr DWORD kPurecallCode = 0xE0000002;         ///< 纯虚函数调用的自定义异常代码
    constexpr DWORD kInvalidParameterCode = 0xE0000003; ///< CRT 非法参数的自定义异常代码

    constexpr int kPointerHexDigits = static_cast<int>(sizeof(void*) * 2); ///< 指针十六进制位数

    wchar_t s_DumpDirectory[kPathCapacity] = {};  ///< 预生成的 dump 目录（含结尾反斜杠）
    wchar_t s_DumpFilePath[kPathCapacity] = {};   ///< 崩溃时拼接的 .dmp 完整路径
    wchar_t s_ReportFilePath[kPathCapacity] = {}; ///< 崩溃时拼接的 .txt 完整路径
    wchar_t s_ScratchWide[kPathCapacity] = {};    ///< 崩溃时复用的宽字符临时缓冲（模块路径等）
    char s_ReportText[kReportCapacity] = {};      ///< 崩溃时拼接的文本报告内容
    SYSTEMTIME s_CrashTime = {};                  ///< 崩溃发生时刻（本地时间）
    volatile LONG s_CrashInProgress = 0;          ///< 崩溃处理重入保护标志

    std::filesystem::path s_SessionMarkerFile;    ///< 会话标记文件路径（仅正常退出路径使用）

    // =========================================================================
    // 异步安全的字符串拼接工具：无堆分配、无 CRT 格式化、无锁。
    // 所有函数均保证缓冲以空字符结尾且不越界。
    // =========================================================================

    /**
     * @brief 向宽字符缓冲追加字符串。
     */
    void AppendWide(wchar_t* buffer, size_t capacity, size_t& offset, const wchar_t* text)
    {
        while (*text != L'\0' && offset + 1 < capacity)
        {
            buffer[offset] = *text;
            ++text;
            ++offset;
        }
        buffer[offset] = L'\0';
    }

    /**
     * @brief 向宽字符缓冲追加十进制无符号整数，不足 width 位时前补零。
     */
    void AppendWideUInt(wchar_t* buffer, size_t capacity, size_t& offset, unsigned int value, int width)
    {
        wchar_t digits[16];
        int count = 0;
        do
        {
            digits[count] = static_cast<wchar_t>(L'0' + static_cast<int>(value % 10u));
            ++count;
            value /= 10u;
        }
        while (value != 0u && count < 16);

        while (count < width && count < 16)
        {
            digits[count] = L'0';
            ++count;
        }

        while (count > 0 && offset + 1 < capacity)
        {
            --count;
            buffer[offset] = digits[count];
            ++offset;
        }
        buffer[offset] = L'\0';
    }

    /**
     * @brief 向窄字符缓冲追加字符串。
     */
    void AppendText(char* buffer, size_t capacity, size_t& offset, const char* text)
    {
        while (*text != '\0' && offset + 1 < capacity)
        {
            buffer[offset] = *text;
            ++text;
            ++offset;
        }
        buffer[offset] = '\0';
    }

    /**
     * @brief 向窄字符缓冲追加十进制无符号整数，不足 width 位时前补零（width 为 0 表示不补位）。
     */
    void AppendUInt(char* buffer, size_t capacity, size_t& offset, unsigned long long value, int width)
    {
        char digits[24];
        int count = 0;
        do
        {
            digits[count] = static_cast<char>('0' + static_cast<int>(value % 10ull));
            ++count;
            value /= 10ull;
        }
        while (value != 0ull && count < 24);

        while (count < width && count < 24)
        {
            digits[count] = '0';
            ++count;
        }

        while (count > 0 && offset + 1 < capacity)
        {
            --count;
            buffer[offset] = digits[count];
            ++offset;
        }
        buffer[offset] = '\0';
    }

    /**
     * @brief 向窄字符缓冲追加十六进制无符号整数（带 0x 前缀，定宽大写）。
     */
    void AppendHex(char* buffer, size_t capacity, size_t& offset, unsigned long long value, int digitCount)
    {
        AppendText(buffer, capacity, offset, "0x");
        for (int i = digitCount - 1; i >= 0; --i)
        {
            const unsigned int nibble = static_cast<unsigned int>((value >> (i * 4)) & 0xFull);
            const char digit = nibble < 10u
                                   ? static_cast<char>('0' + static_cast<int>(nibble))
                                   : static_cast<char>('A' + static_cast<int>(nibble - 10u));
            if (offset + 1 < capacity)
            {
                buffer[offset] = digit;
                ++offset;
            }
        }
        buffer[offset] = '\0';
    }

    /**
     * @brief 将宽字符串以 UTF-8 编码追加到窄字符缓冲（写入静态缓冲，无堆分配）。
     */
    void AppendWideAsUtf8(char* buffer, size_t capacity, size_t& offset, const wchar_t* text)
    {
        if (text == nullptr || text[0] == L'\0' || offset + 1 >= capacity)
        {
            return;
        }

        const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1,
                                                buffer + offset,
                                                static_cast<int>(capacity - offset - 1),
                                                nullptr, nullptr);
        if (written > 0)
        {
            // 转换结果包含结尾空字符，偏移量只前进内容部分
            offset += static_cast<size_t>(written - 1);
        }
        buffer[offset] = '\0';
    }

    // =========================================================================
    // 崩溃产物写出
    // =========================================================================

    /**
     * @brief 根据 s_CrashTime 拼接 .dmp 与 .txt 的完整路径（写入静态缓冲）。
     */
    void BuildCrashFilePaths()
    {
        size_t offset = 0;
        AppendWide(s_DumpFilePath, kPathCapacity, offset, s_DumpDirectory);
        AppendWide(s_DumpFilePath, kPathCapacity, offset, L"crash_");
        AppendWideUInt(s_DumpFilePath, kPathCapacity, offset, s_CrashTime.wYear, 4);
        AppendWideUInt(s_DumpFilePath, kPathCapacity, offset, s_CrashTime.wMonth, 2);
        AppendWideUInt(s_DumpFilePath, kPathCapacity, offset, s_CrashTime.wDay, 2);
        AppendWide(s_DumpFilePath, kPathCapacity, offset, L"_");
        AppendWideUInt(s_DumpFilePath, kPathCapacity, offset, s_CrashTime.wHour, 2);
        AppendWideUInt(s_DumpFilePath, kPathCapacity, offset, s_CrashTime.wMinute, 2);
        AppendWideUInt(s_DumpFilePath, kPathCapacity, offset, s_CrashTime.wSecond, 2);

        // 先以不含扩展名的公共前缀生成 .txt 路径，再分别追加扩展名
        size_t reportOffset = 0;
        AppendWide(s_ReportFilePath, kPathCapacity, reportOffset, s_DumpFilePath);
        AppendWide(s_ReportFilePath, kPathCapacity, reportOffset, L".txt");
        AppendWide(s_DumpFilePath, kPathCapacity, offset, L".dmp");
    }

    /**
     * @brief 写出 minidump 文件。
     */
    void WriteMiniDumpFile(EXCEPTION_POINTERS* exceptionPointers)
    {
        const HANDLE file = CreateFileW(s_DumpFilePath, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = exceptionPointers;
        exceptionInfo.ClientPointers = FALSE;

        const auto dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, dumpType,
                          exceptionPointers != nullptr ? &exceptionInfo : nullptr,
                          nullptr, nullptr);

        CloseHandle(file);
    }

    /**
     * @brief 写出与 dump 同名的文本报告（异常代码、异常地址、模块基址、线程 id 等）。
     */
    void WriteCrashReportFile(EXCEPTION_POINTERS* exceptionPointers)
    {
        DWORD exceptionCode = 0;
        const void* exceptionAddress = nullptr;
        if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
        {
            exceptionCode = exceptionPointers->ExceptionRecord->ExceptionCode;
            exceptionAddress = exceptionPointers->ExceptionRecord->ExceptionAddress;
        }

        HMODULE faultModule = nullptr;
        if (exceptionAddress != nullptr)
        {
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(exceptionAddress), &faultModule);
        }

        size_t offset = 0;
        AppendText(s_ReportText, kReportCapacity, offset, "=== LumaEngine Crash Report ===\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Time:              ");
        AppendUInt(s_ReportText, kReportCapacity, offset, s_CrashTime.wYear, 4);
        AppendText(s_ReportText, kReportCapacity, offset, "-");
        AppendUInt(s_ReportText, kReportCapacity, offset, s_CrashTime.wMonth, 2);
        AppendText(s_ReportText, kReportCapacity, offset, "-");
        AppendUInt(s_ReportText, kReportCapacity, offset, s_CrashTime.wDay, 2);
        AppendText(s_ReportText, kReportCapacity, offset, " ");
        AppendUInt(s_ReportText, kReportCapacity, offset, s_CrashTime.wHour, 2);
        AppendText(s_ReportText, kReportCapacity, offset, ":");
        AppendUInt(s_ReportText, kReportCapacity, offset, s_CrashTime.wMinute, 2);
        AppendText(s_ReportText, kReportCapacity, offset, ":");
        AppendUInt(s_ReportText, kReportCapacity, offset, s_CrashTime.wSecond, 2);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Process id:        ");
        AppendUInt(s_ReportText, kReportCapacity, offset, GetCurrentProcessId(), 0);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Thread id:         ");
        AppendUInt(s_ReportText, kReportCapacity, offset, GetCurrentThreadId(), 0);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Exception code:    ");
        AppendHex(s_ReportText, kReportCapacity, offset, exceptionCode, 8);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Exception address: ");
        AppendHex(s_ReportText, kReportCapacity, offset,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exceptionAddress)),
                  kPointerHexDigits);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Module base:       ");
        AppendHex(s_ReportText, kReportCapacity, offset,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(faultModule)),
                  kPointerHexDigits);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Faulting module:   ");
        s_ScratchWide[0] = L'\0';
        if (faultModule != nullptr &&
            GetModuleFileNameW(faultModule, s_ScratchWide, static_cast<DWORD>(kPathCapacity)) > 0)
        {
            AppendWideAsUtf8(s_ReportText, kReportCapacity, offset, s_ScratchWide);
        }
        else
        {
            AppendText(s_ReportText, kReportCapacity, offset, "<unknown>");
        }
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Executable:        ");
        s_ScratchWide[0] = L'\0';
        if (GetModuleFileNameW(nullptr, s_ScratchWide, static_cast<DWORD>(kPathCapacity)) > 0)
        {
            AppendWideAsUtf8(s_ReportText, kReportCapacity, offset, s_ScratchWide);
        }
        else
        {
            AppendText(s_ReportText, kReportCapacity, offset, "<unknown>");
        }
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset, "Minidump file:     ");
        AppendWideAsUtf8(s_ReportText, kReportCapacity, offset, s_DumpFilePath);
        AppendText(s_ReportText, kReportCapacity, offset, "\r\n");

        AppendText(s_ReportText, kReportCapacity, offset,
                   "Note: 0xE0000001 = std::terminate, 0xE0000002 = pure virtual call, "
                   "0xE0000003 = CRT invalid parameter.\r\n");

        const HANDLE file = CreateFileW(s_ReportFilePath, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        DWORD bytesWritten = 0;
        WriteFile(file, s_ReportText, static_cast<DWORD>(offset), &bytesWritten, nullptr);
        CloseHandle(file);
    }

    /**
     * @brief 崩溃处理主流程：重入保护后依次写出 .dmp 与 .txt。
     *
     * 会话标记文件刻意不在此处删除，供下次启动检测"上次异常退出"。
     */
    void WriteCrashArtifacts(EXCEPTION_POINTERS* exceptionPointers)
    {
        // 二次崩溃（或多线程同时崩溃）时直接放弃，防止递归处理
        if (InterlockedCompareExchange(&s_CrashInProgress, 1, 0) != 0)
        {
            return;
        }

        // 未成功安装（路径缓存为空）时不做任何事
        if (s_DumpDirectory[0] == L'\0')
        {
            return;
        }

        GetLocalTime(&s_CrashTime);
        BuildCrashFilePaths();

        // 目录在会话期间被删除时的最后补救；已存在则调用失败并被忽略
        CreateDirectoryW(s_DumpDirectory, nullptr);

        WriteMiniDumpFile(exceptionPointers);
        WriteCrashReportFile(exceptionPointers);
    }

    // =========================================================================
    // 各类处理器入口
    // =========================================================================

    /**
     * @brief 未处理 SEH 异常过滤器。
     */
    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exceptionPointers)
    {
        WriteCrashArtifacts(exceptionPointers);
        // 跳过系统默认的 WER 弹窗，直接结束进程
        return EXCEPTION_EXECUTE_HANDLER;
    }

    /**
     * @brief 为无异常上下文的错误路径（terminate / purecall / 非法参数）
     *        构造合成异常信息并统一转入 dump 流程，随后终止进程。
     */
    void HandleSyntheticCrash(DWORD exceptionCode)
    {
        CONTEXT context = {};
        RtlCaptureContext(&context);

        EXCEPTION_RECORD record = {};
        record.ExceptionCode = exceptionCode;
        record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        record.ExceptionAddress = LUMA_RETURN_ADDRESS();

        EXCEPTION_POINTERS pointers;
        pointers.ExceptionRecord = &record;
        pointers.ContextRecord = &context;

        WriteCrashArtifacts(&pointers);

        // 绕过 CRT 的 abort 弹窗等后续流程，立即结束进程
        TerminateProcess(GetCurrentProcess(), kCrashExitCode);
    }

    /**
     * @brief std::terminate 处理器（未捕获 C++ 异常、noexcept 违约等）。
     */
    void __cdecl OnTerminate()
    {
        HandleSyntheticCrash(kTerminateCode);
    }

    /**
     * @brief 纯虚函数调用处理器。
     */
    void __cdecl OnPureCall()
    {
        HandleSyntheticCrash(kPurecallCode);
    }

    /**
     * @brief CRT 非法参数处理器。
     */
    void __cdecl OnInvalidParameter(const wchar_t* /*expression*/, const wchar_t* /*function*/,
                                    const wchar_t* /*file*/, unsigned int /*line*/,
                                    uintptr_t /*reserved*/)
    {
        HandleSyntheticCrash(kInvalidParameterCode);
    }

    // =========================================================================
    // Install 阶段工具（正常路径，允许使用堆与 std::filesystem）
    // =========================================================================

    /**
     * @brief 清理目录中最旧的崩溃 dump，仅保留最近 kMaxDumpKeepCount 份；
     *        同名 .txt 报告随 dump 一起删除。
     */
    void PruneOldDumps(const std::filesystem::path& dumpDirectory)
    {
        namespace fs = std::filesystem;

        struct DumpEntry
        {
            fs::path path;                ///< dump 文件路径
            fs::file_time_type writeTime; ///< 最后写入时间
        };

        std::vector<DumpEntry> entries;

        std::error_code ec;
        for (fs::directory_iterator iter(dumpDirectory, ec);
             !ec && iter != fs::directory_iterator();
             iter.increment(ec))
        {
            const fs::directory_entry& entry = *iter;

            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc)
            {
                continue;
            }

            const fs::path& filePath = entry.path();
            if (filePath.extension() != L".dmp")
            {
                continue;
            }

            const std::wstring fileName = filePath.filename().wstring();
            if (fileName.rfind(L"crash_", 0) != 0)
            {
                continue;
            }

            const fs::file_time_type writeTime = entry.last_write_time(fileEc);
            if (fileEc)
            {
                continue;
            }

            entries.push_back(DumpEntry{filePath, writeTime});
        }

        if (entries.size() <= kMaxDumpKeepCount)
        {
            return;
        }

        std::sort(entries.begin(), entries.end(),
                  [](const DumpEntry& a, const DumpEntry& b)
                  {
                      return a.writeTime > b.writeTime;
                  });

        for (size_t i = kMaxDumpKeepCount; i < entries.size(); ++i)
        {
            std::error_code removeEc;
            fs::remove(entries[i].path, removeEc);

            fs::path reportPath = entries[i].path;
            reportPath.replace_extension(L".txt");
            fs::remove(reportPath, removeEc);
        }
    }
}

bool CrashHandler::Install(const std::filesystem::path& dumpDirectory)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::path absoluteDir = fs::absolute(dumpDirectory, ec);
    if (ec)
    {
        absoluteDir = dumpDirectory;
        ec.clear();
    }

    fs::create_directories(absoluteDir, ec);
    if (ec)
    {
        return false;
    }

    PruneOldDumps(absoluteDir);

    // 预生成宽字符目录缓存（含结尾分隔符），崩溃回调内不再进行路径运算与堆分配
    std::wstring wideDir = absoluteDir.wstring();
    if (wideDir.empty())
    {
        return false;
    }
    if (wideDir.back() != L'\\' && wideDir.back() != L'/')
    {
        wideDir.push_back(L'\\');
    }
    // 预留 crash_YYYYMMDD_HHMMSS.dmp 文件名所需空间（约 26 字符）
    if (wideDir.size() + 32 >= kPathCapacity)
    {
        return false;
    }

    size_t offset = 0;
    s_DumpDirectory[0] = L'\0';
    AppendWide(s_DumpDirectory, kPathCapacity, offset, wideDir.c_str());

    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);

    return true;
}

bool CrashHandler::BeginSessionMarker(const std::filesystem::path& markerFile)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path parentDir = markerFile.parent_path();
    if (!parentDir.empty())
    {
        // 创建失败时下方打开文件会自然失败，无需单独处理
        fs::create_directories(parentDir, ec);
    }

    std::ofstream out(markerFile, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        return false;
    }

    // 打开成功即记录路径，保证 EndSessionMarker 可清理不完整的标记文件
    s_SessionMarkerFile = markerFile;

    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm timeInfo = {};
    char timeBuffer[32] = "unknown";
    if (localtime_s(&timeInfo, &nowTime) == 0)
    {
        std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
    }

    out << "pid=" << GetCurrentProcessId() << '\n';
    out << "start=" << timeBuffer << '\n';
    out.flush();

    return out.good();
}

void CrashHandler::EndSessionMarker()
{
    if (s_SessionMarkerFile.empty())
    {
        return;
    }

    std::error_code ec;
    std::filesystem::remove(s_SessionMarkerFile, ec);
    s_SessionMarkerFile.clear();
}

bool CrashHandler::HasAbnormalExit(const std::filesystem::path& markerFile)
{
    std::error_code ec;
    return std::filesystem::exists(markerFile, ec);
}

#else // 非 Windows 平台：空实现，仅保证可编译

bool CrashHandler::Install(const std::filesystem::path& /*dumpDirectory*/)
{
    return false;
}

bool CrashHandler::BeginSessionMarker(const std::filesystem::path& /*markerFile*/)
{
    return false;
}

void CrashHandler::EndSessionMarker()
{
}

bool CrashHandler::HasAbnormalExit(const std::filesystem::path& /*markerFile*/)
{
    return false;
}

#endif
