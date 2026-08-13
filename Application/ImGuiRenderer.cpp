#include "ImGuiRenderer.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_wgpu.h"
#include <stdexcept>
#include "GraphicsBackend.h"
#include "../Utils/Path.h"
#include "../Utils/Logger.h"
ImGuiRenderer::ImGuiRenderer(SDL_Window* window, const wgpu::Device& device, wgpu::TextureFormat renderTargetFormat)
    : m_device(device), m_isInitialized(false)
{
    if (!window)
    {
        throw std::runtime_error("ImGuiRenderer: SDL 窗口指针为空");
    }
    if (!m_device)
    {
        throw std::runtime_error("ImGuiRenderer: WebGPU 设备无效");
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    if (!ImGui_ImplSDL3_InitForOther(window))
    {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGuiRenderer: 初始化 ImGui SDL3 后端失败");
    }
    ApplyEditorStyle();
    ImGui_ImplWGPU_InitInfo initInfo = {};
    initInfo.Device = m_device.Get();  
    initInfo.NumFramesInFlight = 1;
    initInfo.RenderTargetFormat = static_cast<WGPUTextureFormat>(renderTargetFormat);
    initInfo.DepthStencilFormat = WGPUTextureFormat_Undefined;
    if (!ImGui_ImplWGPU_Init(&initInfo))
    {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGuiRenderer: 初始化 ImGui WGPU 后端失败");
    }
    m_isInitialized = true;
    LogInfo("ImGuiRenderer 初始化成功");
}
ImGuiRenderer::~ImGuiRenderer()
{
    if (m_isInitialized)
    {
        m_textureCache.clear();
        m_activeTexturesInFrame.clear();
        ImGui_ImplWGPU_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_isInitialized = false;
        LogInfo("ImGuiRenderer 已销毁");
    }
}
void ImGuiRenderer::NewFrame()
{
    if (!m_isInitialized)
    {
        throw std::runtime_error("ImGuiRenderer::NewFrame: 渲染器未初始化");
    }
    if (!m_device)
    {
        throw std::runtime_error("ImGuiRenderer::NewFrame: WebGPU 设备无效");
    }
    if (!m_activeTexturesInFrame.empty())
    {
        for (auto it = m_textureCache.begin(); it != m_textureCache.end();)
        {
            if (std::ranges::find(m_activeTexturesInFrame, it->first) == m_activeTexturesInFrame.end())
            {
                it = m_textureCache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    m_activeTexturesInFrame.clear();
    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}
void ImGuiRenderer::Render(wgpu::RenderPassEncoder renderPass)
{
    if (!m_isInitialized)
    {
        LogWarn("ImGuiRenderer::Render: 渲染器未初始化，跳过渲染");
        return;
    }
    ImGui::Render();
    if (renderPass)
    {
        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), renderPass.Get());
    }
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}
void ImGuiRenderer::EndFrame(const GraphicsBackend& backend)
{
    if (!m_isInitialized)
    {
        return;
    }
    wgpu::TextureView frameView = backend.GetCurrentFrameView();
    if (!frameView)
    {
        LogWarn("ImGuiRenderer::EndFrame: 无法获取当前帧视图");
        return;
    }
    wgpu::CommandEncoder encoder = backend.GetDevice().CreateCommandEncoder();
    if (!encoder)
    {
        LogError("ImGuiRenderer::EndFrame: 创建命令编码器失败");
        return;
    }
    wgpu::RenderPassColorAttachment colorAttachment{};
    colorAttachment.view = frameView;
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;
    colorAttachment.clearValue = {0.15f, 0.16f, 0.18f, 1.0f};
    wgpu::RenderPassDescriptor passDesc{};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &colorAttachment;
    wgpu::RenderPassEncoder renderPass = encoder.BeginRenderPass(&passDesc);
    Render(renderPass);
    renderPass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    backend.GetDevice().GetQueue().Submit(1, &commands);
}
ImTextureID ImGuiRenderer::GetOrCreateTextureIdFor(wgpu::Texture texture)
{
    if (!texture)
    {
        return reinterpret_cast<ImTextureID>(nullptr);
    }
    WGPUTexture textureHandle = texture.Get();
    m_activeTexturesInFrame.push_back(textureHandle);
    auto it = m_textureCache.find(textureHandle);
    if (it != m_textureCache.end())
    {
        return reinterpret_cast<ImTextureID>(it->second.Get());
    }
    wgpu::TextureViewDescriptor viewDesc = {};
    viewDesc.format = texture.GetFormat();
    viewDesc.dimension = wgpu::TextureViewDimension::e2D;
    viewDesc.baseMipLevel = 0;
    viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0;
    viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = wgpu::TextureAspect::All;
    wgpu::TextureView view = texture.CreateView(&viewDesc);
    if (!view)
    {
        LogError("ImGuiRenderer::GetOrCreateTextureIdFor: 创建纹理视图失败");
        return reinterpret_cast<ImTextureID>(nullptr);
    }
    m_textureCache[textureHandle] = view;
    return reinterpret_cast<ImTextureID>(view.Get());
}
void ImGuiRenderer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}
void ImGuiRenderer::ApplyEditorStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();

#if defined(SDL_PLATFORM_ANDROID) || defined(__ANDROID__)
    float touchScale = 1.5f;
    style.WindowPadding = ImVec2(12.0f * touchScale, 12.0f * touchScale);
    style.FramePadding = ImVec2(8.0f * touchScale, 6.0f * touchScale);
    style.CellPadding = ImVec2(6.0f * touchScale, 4.0f * touchScale);
    style.ItemSpacing = ImVec2(8.0f * touchScale, 6.0f * touchScale);
    style.ItemInnerSpacing = ImVec2(6.0f * touchScale, 6.0f * touchScale);
    style.ScrollbarSize = 20.0f * touchScale;
    style.GrabMinSize = 16.0f * touchScale;
    style.TouchExtraPadding = ImVec2(8.0f, 8.0f); // 触摸额外边距
#else
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.5f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 12.0f;
#endif
    style.IndentSpacing = 14.0f;
    style.WindowTitleAlign = ImVec2(0.02f, 0.50f);
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding = ImVec2(16.0f, 2.0f);

    // 扁平化：仅保留窗口/弹窗描边，去掉控件描边。
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.DockingSeparatorSize = 2.0f;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;

    // 现代深色主题：中性深灰底 + 低饱和强调（参考 Unity 6 / Godot 4 深色风格）。
    // 选中态用 Godot 式低调蓝灰（CollapsingHeader/TreeNode 共用此色，必须足够内敛），
    // 小元素（勾选/滑块/标签强调线）用略亮的低饱和蓝，避免 ImGui 默认高饱和亮蓝。
    const ImVec4 selection(0.212f, 0.259f, 0.318f, 1.00f);       // 列表/树选中底色（蓝灰）
    const ImVec4 selectionHovered(0.247f, 0.302f, 0.369f, 1.00f);// 选中悬停
    const ImVec4 selectionActive(0.286f, 0.349f, 0.424f, 1.00f); // 选中按下
    const ImVec4 accent(0.357f, 0.557f, 0.769f, 1.00f);          // 小元素强调（勾选/滑块/强调线）
    const ImVec4 bgDeepest(0.086f, 0.090f, 0.094f, 1.00f);       // 标题栏 / 最底层
    const ImVec4 bgWindow(0.118f, 0.122f, 0.129f, 1.00f);        // 面板底
    const ImVec4 bgElevated(0.157f, 0.163f, 0.173f, 1.00f);      // 控件底 / 菜单栏
    const ImVec4 bgHovered(0.216f, 0.224f, 0.235f, 1.00f);       // 控件悬停
    const ImVec4 bgActive(0.263f, 0.275f, 0.290f, 1.00f);        // 控件按下

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.94f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.52f, 0.55f, 1.00f);
    colors[ImGuiCol_WindowBg] = bgWindow;
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.100f, 0.104f, 0.110f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.045f, 0.047f, 0.049f, 0.90f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = bgElevated;
    colors[ImGuiCol_FrameBgHovered] = bgHovered;
    colors[ImGuiCol_FrameBgActive] = bgActive;
    colors[ImGuiCol_TitleBg] = bgDeepest;
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.104f, 0.108f, 0.114f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = bgDeepest;
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.104f, 0.108f, 0.114f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.29f, 0.30f, 0.32f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.37f, 0.39f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.47f, 0.50f, 1.00f);
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = ImVec4(accent.x, accent.y, accent.z, 0.85f);
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Button] = ImVec4(0.216f, 0.224f, 0.235f, 0.85f);
    colors[ImGuiCol_ButtonHovered] = bgActive;
    colors[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.32f, 0.34f, 1.00f);
    colors[ImGuiCol_Header] = selection;
    colors[ImGuiCol_HeaderHovered] = selectionHovered;
    colors[ImGuiCol_HeaderActive] = selectionActive;
    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.24f, 0.80f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    colors[ImGuiCol_SeparatorActive] = accent;
    colors[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.18f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    colors[ImGuiCol_ResizeGripActive] = accent;
    colors[ImGuiCol_Tab] = ImVec4(0.104f, 0.108f, 0.114f, 1.00f);
    colors[ImGuiCol_TabHovered] = bgHovered;
    colors[ImGuiCol_TabSelected] = bgElevated;
    colors[ImGuiCol_TabSelectedOverline] = accent;
    colors[ImGuiCol_TabDimmed] = bgDeepest;
    colors[ImGuiCol_TabDimmedSelected] = bgWindow;
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    colors[ImGuiCol_DockingEmptyBg] = bgDeepest;
    colors[ImGuiCol_PlotLines] = ImVec4(0.55f, 0.57f, 0.60f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = accent;
    colors[ImGuiCol_PlotHistogram] = ImVec4(accent.x, accent.y, accent.z, 0.90f);
    colors[ImGuiCol_PlotHistogramHovered] = accent;
    colors[ImGuiCol_TableHeaderBg] = bgElevated;
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.045f, 0.047f, 0.049f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.19f, 0.20f, 0.21f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(selection.x, selection.y, selection.z, 0.80f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(accent.x, accent.y, accent.z, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(accent.x, accent.y, accent.z, 0.80f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.45f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
}
std::string ImGuiRenderer::LoadFonts(const std::string& fontPath, float dpiScale)
{
    if (!Path::Exists(fontPath))
    {
        LogError("ImGuiRenderer::LoadFonts: 字体文件不存在: {}", fontPath);
        return "";
    }
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig mainConfig;
    mainConfig.FontDataOwnedByAtlas = false;
    mainConfig.SizePixels = 16.0f * dpiScale;
    mainConfig.RasterizerMultiply = dpiScale;
    mainConfig.GlyphRanges = io.Fonts->GetGlyphRangesChineseFull();
    ImFont* mainFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), mainConfig.SizePixels, &mainConfig,
                                                    mainConfig.GlyphRanges);
    if (!mainFont)
    {
        LogError("ImGuiRenderer::LoadFonts: 加载主字体失败");
        return "";
    }
    ImFontConfig emojiConfig;
    emojiConfig.FontDataOwnedByAtlas = false;
    emojiConfig.SizePixels = 16.0f * dpiScale;
    emojiConfig.RasterizerMultiply = dpiScale;
    emojiConfig.MergeMode = true;
    emojiConfig.GlyphMinAdvanceX = 16.0f * dpiScale;
    static const ImWchar emojiRanges[] =
    {
        0x1, 0xFFFF,
        0,
    };
    const char* emojiFontPaths[] =
    {
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/NotoColorEmoji.ttf",
        "/System/Library/Fonts/Apple Color Emoji.ttc",
        "/usr/share/fonts/truetype/noto-color-emoji/NotoColorEmoji.ttf"
    };
    for (const char* emojiPath : emojiFontPaths)
    {
        if (Path::Exists(emojiPath))
        {
            io.Fonts->AddFontFromFileTTF(emojiPath, emojiConfig.SizePixels, &emojiConfig, emojiRanges);
        }
    }
    ImFontConfig symbolConfig;
    symbolConfig.FontDataOwnedByAtlas = false;
    symbolConfig.SizePixels = 16.0f * dpiScale;
    symbolConfig.RasterizerMultiply = dpiScale;
    symbolConfig.MergeMode = true;
    static const ImWchar basicSymbolRanges[] =
    {
        0x2000, 0x27FF,
        0,
    };
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), symbolConfig.SizePixels, &symbolConfig, basicSymbolRanges);
    io.Fonts->Build();
    auto name = Path::GetFileNameWithoutExtension(fontPath);
    m_fonts[name] = mainFont;
    LogInfo("ImGuiRenderer::LoadFonts: 成功加载字体: {}", name);
    return name;
}
void ImGuiRenderer::SetFont(const std::string& fontName)
{
    auto it = m_fonts.find(fontName);
    if (it != m_fonts.end())
    {
        ImGui::GetIO().FontDefault = it->second;
        LogInfo("ImGuiRenderer::SetFont: 设置默认字体为: {}", fontName);
    }
    else
    {
        throw std::runtime_error("ImGuiRenderer::SetFont: 字体未找到: " + fontName);
    }
}
