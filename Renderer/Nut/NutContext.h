#ifndef NOAI_GRAPHICSCONTEXT_H
#define NOAI_GRAPHICSCONTEXT_H
#include <chrono>
#include "RenderPass.h"
#include "RenderTarget.h"
#include "TextureA.h"
#include "dawn/native/DawnNative.h"

class ShaderLoader;

namespace Nut
{
    /**
     * @brief 定义图形后端类型。
     */
    enum class BackendType
    {
        D3D12, ///< Direct3D 12 后端。
        D3D11, ///< Direct3D 11 后端。
        Vulkan, ///< Vulkan 后端。
        Metal, ///< Metal 后端。
        OpenGL, ///< OpenGL 后端。
        OpenGLES, ///< OpenGL ES 后端。
    };

    /**
     * @brief 定义渲染质量等级。
     */
    enum class QualityLevel
    {
        Low, ///< 低质量。
        Medium, ///< 中等质量。
        High, ///< 高质量。
    };

    /**
     * @brief 封装原生窗口句柄信息。
     *
     * 根据不同的操作系统，包含不同的原生窗口句柄类型。
     */
    struct NativeWindowHandle
    {
#if defined(_WIN32)
        void* hWnd = nullptr; ///< Windows 窗口句柄。
        void* hInst = nullptr; ///< Windows 实例句柄。
#elif defined(__APPLE__)
        void* metalLayer = nullptr; ///< Metal 层对象。
#elif defined(__linux__) && !defined(__ANDROID__)
        void* x11Display = nullptr; ///< X11 显示器句柄。
        unsigned long x11Window = 0; ///< X11 窗口ID。
#elif defined(__ANDROID__)
        void* aNativeWindow = nullptr; ///< Android 原生窗口指针。
#else

        void* placeholder = nullptr; ///< 占位符，用于未知平台。
#endif

        /**
         * @brief 检查原生窗口句柄是否有效。
         * @return 如果句柄有效则返回 true，否则返回 false。
         */
        bool IsValid() const
        {
#if defined(_WIN32)
            return hWnd != nullptr && hInst != nullptr;
#elif defined(__APPLE__)
            return metalLayer != nullptr;
#elif defined(__linux__) && !defined(__ANDROID__)
            return x11Display != nullptr && x11Window != 0;
#elif defined(__ANDROID__)
            return aNativeWindow != nullptr;
#else
            return placeholder != nullptr;
#endif
        }
    };

    struct NutContextDescriptor
    {
        std::vector<BackendType> backendTypePriority = {
            BackendType::D3D12, BackendType::Vulkan, BackendType::Metal
        }; ///< 后端类型优先级列表。
        NativeWindowHandle windowHandle; ///< 原生窗口句柄。
        uint16_t width = 1; ///< 初始宽度。
        uint16_t height = 1; ///< 初始高度。
        bool enableVSync = true; ///< 是否启用垂直同步。
        QualityLevel qualityLevel = QualityLevel::High; ///< 初始质量等级。
    };

    enum GraphicsContextCreateStatus: uint32_t
    {
        SUCCESS = 0,
        ERROR_DEVICE_CREATION = 1,
        ERROR_NONE_AVAILABLE_ADAPTER = 2,
        ERROR_SURFACE_CREATION = 3,
        ERROR_INSTANCE_CREATION = 4,
        ERROR_ALREADY_CREATED = 5,
    };

    class RenderPassBuilder;
    class ComputePassBuilder;

    class LUMA_API NutContext : public std::enable_shared_from_this<NutContext>
    {
    private:
        wgpu::SurfaceTexture m_currentSurfaceTexture;
        std::unique_ptr<dawn::native::Instance> m_instance;
        wgpu::Device m_device;
        wgpu::Surface m_surface;
        NutContextDescriptor m_descriptor;
        std::unordered_map<std::string, std::shared_ptr<RenderTarget>> m_renderTargets;
        wgpu::TextureFormat m_graphicsFormat = wgpu::TextureFormat::RGBA8Unorm;
        Size m_size = {0, 0};
        std::shared_ptr<RenderTarget> m_currentRenderTarget;
        bool m_isDeviceLost = false;
        std::vector<wgpu::CommandEncoder> m_commandEncoders;
        std::mutex m_mutex;

        std::vector<wgpu::CommandEncoder> m_computeCommandEncoders;
        std::mutex m_cmutex;
        inline static ShaderLoader* m_shaderLoader = nullptr;

        /// @brief 是否处于批量提交作用域内（由 BeginBatchedSubmission/EndBatchedSubmission 控制）。
        bool m_batchedSubmissionActive = false;
        /// @brief 批量提交作用域内共享的命令编码器（懒创建，冲刷后置空等待重建）。
        wgpu::CommandEncoder m_batchedCommandEncoder = nullptr;

        /// @brief 冲刷批量作用域内已录制的命令：Finish 共享编码器并立即提交。调用前必须持有 m_mutex。
        void flushBatchedLocked();

        // Surface reconfigure rate limiting
        std::chrono::steady_clock::time_point m_lastReconfigureTime{};
        static constexpr std::chrono::milliseconds kReconfigureCooldown{100};

    public:
        NutContext(const NutContext&) = delete;
        NutContext& operator=(const NutContext&) = delete;

        wgpu::Queue GetWGPUQueue() const
        {
            return m_device.GetQueue();
        }

        NutContext();

    private:
        bool createInstance();
        bool createDevice();
        bool createSurface();

        void configureSurface(uint32_t width, uint32_t height);

    public:
        static std::shared_ptr<NutContext> Create(const NutContextDescriptor& descriptor);
        GraphicsContextCreateStatus Initialize(const NutContextDescriptor& desc);

        std::shared_ptr<RenderTarget> CreateOrGetRenderTarget(const std::string& name, uint16_t width, uint16_t height);

        void SetActiveRenderTarget(std::shared_ptr<RenderTarget> target);
        void Resize(uint32_t width, uint32_t height);
        void SetVSync(bool enable);

        wgpu::Device& GetWGPUDevice();

        [[nodiscard]] wgpu::Surface& GetWGPUSurface();
        wgpu::Instance GetWGPUInstance() const;

        Size GetCurrentSwapChainSize() const;

        RenderPassBuilder BeginRenderFrame();

        wgpu::CommandBuffer EndRenderFrame(RenderPass& renderPass);

        void Submit(const std::vector<wgpu::CommandBuffer>& cmds);

        /**
         * @brief 开启批量提交作用域。
         *
         * 作用域内 BeginRenderFrame() 复用同一个命令编码器（WebGPU 允许一个编码器顺序录制多个
         * 渲染通道），EndRenderFrame() 只结束渲染通道而不 Finish 编码器，Submit() 收到的空命令
         * 缓冲被忽略；所有命令在 FlushBatchedSubmission()/EndBatchedSubmission() 时一次性提交。
         * 作用域外各接口行为与原先完全一致。
         * @note 该作用域为渲染线程内的局部概念，不支持跨线程共享录制。
         */
        void BeginBatchedSubmission();

        /**
         * @brief 冲刷批量作用域内已录制的命令（Finish 共享编码器并提交），作用域保持开启。
         *
         * 用于作用域内的写冲突消解：当接下来要通过 queue.WriteBuffer 覆写某个已被挂起
         * 渲染通道读取的缓冲区时，先调用本函数保证先前通道按旧数据执行。
         */
        void FlushBatchedSubmission();

        /**
         * @brief 结束批量提交作用域：冲刷剩余命令并关闭作用域。作用域未开启时为空操作。
         */
        void EndBatchedSubmission();

        void Present();

        ComputePassBuilder BeginComputeFrame();

        wgpu::CommandBuffer EndComputeFrame(ComputePass& computePass);

        void ClearCommands();

        TextureAPtr GetCurrentTexture();

        wgpu::Sampler CreateSampler(wgpu::SamplerDescriptor const* desc);

        TextureAPtr LoadTextureFromFile(const std::string& file);
        TextureAPtr CreateTextureFromMemory(const void* data, size_t size);
        TextureAPtr CreateTexture(const TextureDescriptor& descriptor);

        TextureAPtr CreateTextureFromCompressedData(const unsigned char* data, size_t size, uint32_t width,
                                                    uint32_t height,
                                                    wgpu::TextureFormat format,
                                                    uint32_t bytesPerRow = 0,
                                                    uint32_t rowsPerImage = 0);

        bool ResolveTexture(const TextureAPtr& source, const TextureAPtr& resolveTarget);

        TextureAPtr AcquireSwapChainTexture();
        static ShaderLoader* GetShaderLoader() { return m_shaderLoader; }
    };
} // namespace Nut

#endif //NOAI_GRAPHICSCONTEXT_H
