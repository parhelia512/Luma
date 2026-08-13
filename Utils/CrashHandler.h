#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

#include <filesystem>

#include "Platform.h"

/**
 * @brief 崩溃处理工具类，提供进程级崩溃捕获（minidump）与会话异常退出标记。
 *
 * Windows 平台通过 SetUnhandledExceptionFilter 捕获未处理的 SEH 异常，
 * 并统一接管 std::terminate、纯虚函数调用与 CRT 非法参数错误，
 * 崩溃时向指定目录写出带时间戳的 minidump（.dmp）与同名文本报告（.txt）。
 * 非 Windows 平台全部为空实现，仅保证可编译。
 *
 * 会话标记用于"上次是否异常退出"检测：启动时写入标记文件，正常退出时删除；
 * 下次启动时若标记文件仍存在，说明上一会话未正常结束（崩溃、强杀、断电等）。
 */
class LUMA_API CrashHandler
{
public:
    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

    /**
     * @brief 安装进程级崩溃处理。
     *
     * 创建 dump 目录（不存在时）、按保留策略清理旧 dump（仅保留最近 10 份），
     * 并预生成崩溃回调所需的宽字符路径缓存，随后安装以下处理器：
     * - SetUnhandledExceptionFilter（未处理 SEH 异常）
     * - std::set_terminate（未捕获 C++ 异常等触发的 terminate）
     * - _set_purecall_handler（纯虚函数调用）
     * - _set_invalid_parameter_handler（CRT 非法参数）
     *
     * 崩溃时写出 crash_YYYYMMDD_HHMMSS.dmp 与同名 .txt（异常代码、异常地址、
     * 模块基址、线程 id 等信息）到 dumpDirectory。
     *
     * @param dumpDirectory dump 输出目录，不存在时自动创建。
     * @return 安装成功返回 true；目录创建失败或路径过长返回 false。
     *         非 Windows 平台恒返回 false。
     * @note 应在进程入口初始化阶段尽早调用。若之后某些第三方组件（如脚本宿主）
     *       覆盖了未处理异常过滤器，可再次调用本函数重新安装。
     */
    static bool Install(const std::filesystem::path& dumpDirectory);

    /**
     * @brief 写入会话标记文件，内容含启动时间戳与进程 id。
     *
     * 与 EndSessionMarker / HasAbnormalExit 配合实现"上次异常退出"检测。
     * 崩溃回调刻意不删除该文件，因此异常退出后标记会保留到下次启动。
     *
     * @param markerFile 标记文件路径，父目录不存在时自动创建。
     * @return 写入成功返回 true。非 Windows 平台恒返回 false。
     */
    static bool BeginSessionMarker(const std::filesystem::path& markerFile);

    /**
     * @brief 删除会话标记文件，表示本次会话正常结束。
     * @note 仅在进程正常退出流程中调用；崩溃路径不会删除标记。
     *       未调用过 BeginSessionMarker 时为无操作。
     */
    static void EndSessionMarker();

    /**
     * @brief 检查上一会话是否异常退出。
     * @param markerFile 标记文件路径（与 BeginSessionMarker 使用同一路径）。
     * @return 标记文件存在返回 true（上次未正常退出）。非 Windows 平台恒返回 false。
     * @note 必须在本次 BeginSessionMarker 之前调用，否则检测到的是本次会话的标记。
     */
    static bool HasAbnormalExit(const std::filesystem::path& markerFile);

private:
    CrashHandler() = default;
};

#endif
