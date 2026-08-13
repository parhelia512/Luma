#include "../Utils/PCH.h"
#include "GameViewPanel.h"
#include "ImGuiRenderer.h"
#include "RenderableManager.h"
#include "../Renderer/GraphicsBackend.h"
#include "../Renderer/RenderSystem.h"
#include "Renderer/Camera.h"
#include "SceneRenderer.h"
#include "../Utils/Profiler.h"
#include "RuntimeAsset/RuntimeScene.h"
#include "../ProjectSettings.h"
#include "../../Components/ParticleComponent.h"
#include "../Renderer/Nut/ShaderStruct.h"
#include "../Renderer/Nut/TextureA.h"
#include "../Renderer/Nut/RenderPass.h"
#include <cstdio>
void GameViewPanel::Initialize(EditorContext* context)
{
    m_context = context;
    m_particleRenderer = std::make_unique<Particles::ParticleRenderer>();
}
void GameViewPanel::Update(float deltaTime)
{
}
void GameViewPanel::Draw()
{
    PROFILE_FUNCTION();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(GetPanelName(), &m_isVisible);
    m_isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    drawToolbar();
    const ImVec2 regionPos = ImGui::GetCursorScreenPos();
    const ImVec2 regionSize = ImGui::GetContentRegionAvail();
    // 按选定宽高比在可用区域内求居中 letterbox 显示区（自由模式铺满；-1 表示按设计分辨率，需运行时取值）
    static constexpr float kAspectRatios[] = {0.0f, 16.0f / 9.0f, 9.0f / 16.0f, 4.0f / 3.0f, 1.0f, -1.0f};
    float aspect = kAspectRatios[m_aspectModeIndex];
    if (aspect < 0.0f)
    {
        const float designWidth = static_cast<float>(ProjectSettings::GetInstance().GetDesignWidth());
        const float designHeight = static_cast<float>(ProjectSettings::GetInstance().GetDesignHeight());
        aspect = (designWidth > 0.0f && designHeight > 0.0f) ? designWidth / designHeight : 0.0f;
    }
    ImVec2 displaySize = regionSize;
    if (aspect > 0.0f && regionSize.x > 0 && regionSize.y > 0)
    {
        if (regionSize.x / regionSize.y > aspect)
        {
            displaySize = ImVec2(regionSize.y * aspect, regionSize.y);
        }
        else
        {
            displaySize = ImVec2(regionSize.x, regionSize.x / aspect);
        }
    }
    const ImVec2 imagePos(regionPos.x + (regionSize.x - displaySize.x) * 0.5f,
                          regionPos.y + (regionSize.y - displaySize.y) * 0.5f);
    if (m_context->editorState != EditorState::Editing)
    {
        // 运行态输入映射区域取实际图像区（letterbox 后与铺满不再一致）
        m_context->engineContext->sceneViewRect = ECS::RectF(imagePos.x, imagePos.y, displaySize.x,
                                                             displaySize.y);
        m_context->engineContext->isSceneViewFocused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    }
    if (displaySize.x > 0 && displaySize.y > 0)
    {
        m_gameViewTarget = m_context->graphicsBackend->CreateOrGetRenderTarget("GameView",
                                                                               (uint16_t)displaySize.x,
                                                                               (uint16_t)displaySize.y);
        if (m_gameViewTarget)
        {
            if (m_context->editorState != EditorState::Editing && m_context->activeScene)
            {
                auto cameraProperties = m_context->activeScene->GetCameraProperties();
                float windowWidth = displaySize.x;
                float windowHeight = displaySize.y;
                auto scaleMode = ProjectSettings::GetInstance().GetViewportScaleMode();
                float designWidth = static_cast<float>(ProjectSettings::GetInstance().GetDesignWidth());
                float designHeight = static_cast<float>(ProjectSettings::GetInstance().GetDesignHeight());
                cameraProperties.zoomFactor = {1.0f, 1.0f};
                switch (scaleMode)
                {
                case ViewportScaleMode::None:
                    cameraProperties.viewport = SkRect::MakeWH(windowWidth, windowHeight);
                    break;
                case ViewportScaleMode::FixedAspect:
                    {
                        float designAspect = designWidth / designHeight;
                        float windowAspect = windowWidth / windowHeight;
                        if (windowAspect > designAspect)
                        {
                            float scale = windowHeight / designHeight;
                            float scaledWidth = designWidth * scale;
                            float offsetX = (windowWidth - scaledWidth) * 0.5f;
                            cameraProperties.viewport = SkRect::MakeXYWH(offsetX, 0, scaledWidth, windowHeight);
                            cameraProperties.zoomFactor = {scale, scale};
                        }
                        else
                        {
                            float scale = windowWidth / designWidth;
                            float scaledHeight = designHeight * scale;
                            float offsetY = (windowHeight - scaledHeight) * 0.5f;
                            cameraProperties.viewport = SkRect::MakeXYWH(0, offsetY, windowWidth, scaledHeight);
                            cameraProperties.zoomFactor = {scale, scale};
                        }
                    }
                    break;
                case ViewportScaleMode::FixedWidth:
                    {
                        float scale = windowWidth / designWidth;
                        cameraProperties.viewport = SkRect::MakeWH(windowWidth, windowHeight);
                        cameraProperties.zoomFactor = {scale, scale};
                    }
                    break;
                case ViewportScaleMode::FixedHeight:
                    {
                        float scale = windowHeight / designHeight;
                        cameraProperties.viewport = SkRect::MakeWH(windowWidth, windowHeight);
                        cameraProperties.zoomFactor = {scale, scale};
                    }
                    break;
                case ViewportScaleMode::Expand:
                    {
                        float scaleX = windowWidth / designWidth;
                        float scaleY = windowHeight / designHeight;
                        cameraProperties.viewport = SkRect::MakeWH(windowWidth, windowHeight);
                        cameraProperties.zoomFactor = {scaleX, scaleY};
                    }
                    break;
                }
                CameraManager::GetInstance().GetActiveCamera().SetProperties(cameraProperties);
                
                auto& uiCamera = CameraManager::GetInstance().GetUICamera();
                CameraProperties uiCamProps = uiCamera.GetProperties();
                uiCamProps.viewport = cameraProperties.viewport;
                uiCamProps.zoomFactor = cameraProperties.zoomFactor;
                uiCamera.SetProperties(uiCamProps);
                
                m_context->graphicsBackend->SetActiveRenderTarget(m_gameViewTarget);
                if (m_context->renderQueue)
                {
                    for (const auto& packet : *m_context->renderQueue)
                    {
                        m_context->engineContext->renderSystem->Submit(packet);
                    }
                }
                m_context->engineContext->renderSystem->Flush();
                m_context->graphicsBackend->Submit();
                renderParticlesGPU();
            }
            else
            {
                m_context->graphicsBackend->SetActiveRenderTarget(m_gameViewTarget);
                m_context->engineContext->renderSystem->Clear({0.0f, 0.0f, 0.0f, 1.0f});
                m_context->graphicsBackend->Submit();
            }
            ImTextureID textureId = m_context->imguiRenderer->GetOrCreateTextureIdFor(m_gameViewTarget->GetTexture());
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::Image(textureId, displaySize, ImVec2(0, 0), ImVec2(1, 1));
            if (m_showStats)
            {
                drawStatsOverlay(imagePos, displaySize);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
void GameViewPanel::drawToolbar()
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    static const char* kAspectLabels[] = {"自由", "16:9", "9:16", "4:3", "1:1", "设计分辨率"};
    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("##GameViewAspect", &m_aspectModeIndex, kAspectLabels, IM_ARRAYSIZE(kAspectLabels));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("显示宽高比（非自由模式下居中显示）");
    }
    ImGui::SameLine();
    ImGui::Checkbox("统计", &m_showStats);
    ImGui::Separator();
}
void GameViewPanel::drawStatsOverlay(const ImVec2& imageMin, const ImVec2& imageSize) const
{
    // RenderSystem 未暴露 DrawCall 计数，退而显示 FPS/UPS/对象数
    uint32_t objectCount = m_context->activeScene ? m_context->activeScene->GetGameObjectCount() : 0;
    char statsText[128];
    std::snprintf(statsText, sizeof(statsText), "FPS: %.1f\nUPS: %.1f\n对象数: %u",
                  m_context->lastFps, m_context->lastUps, objectCount);
    const float pad = 6.0f;
    ImVec2 textSize = ImGui::CalcTextSize(statsText);
    ImVec2 boxMin(imageMin.x + imageSize.x - textSize.x - pad * 3.0f, imageMin.y + pad);
    ImVec2 boxMax(imageMin.x + imageSize.x - pad, boxMin.y + textSize.y + pad * 2.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 160), 4.0f);
    drawList->AddText(ImVec2(boxMin.x + pad, boxMin.y + pad), IM_COL32(255, 255, 255, 220), statsText);
}
void GameViewPanel::Shutdown()
{
    m_gameViewTarget.reset();
    if (m_particleRenderer)
    {
        m_particleRenderer->Shutdown();
        m_particleRenderer.reset();
    }
}
void GameViewPanel::renderParticlesGPU()
{
    if (!m_context || !m_context->activeScene || !m_gameViewTarget)
        return;
    auto nutContext = m_context->graphicsBackend->GetNutContext();
    if (!nutContext)
        return;
    if (!m_particleRendererInitialized && m_particleRenderer)
    {
        m_particleRenderer->Initialize(nutContext);
        m_particleRendererInitialized = true;
    }
    if (!m_particleRendererInitialized)
        return;
    auto& registry = m_context->activeScene->GetRegistry();
    m_particleRenderer->PrepareRender(registry);
    if (m_particleRenderer->GetTotalParticleCount() == 0)
        return;
    EngineData engineData{};
    CameraManager::GetInstance().GetActiveCamera().FillEngineData(engineData);
    engineData.CameraScaleY *= -1.0f;
    auto targetTexture = Nut::TextureA::CreateTextureA(
        m_gameViewTarget->GetTexture(), nutContext);
    auto attachmentBuilder = Nut::ColorAttachmentBuilder();
    attachmentBuilder.SetTexture(targetTexture)
                     .SetLoadOnOpen(Nut::LoadOnOpen::Load)
                     .SetStoreOnOpen(Nut::StoreOnOpen::Store);
    auto renderPass = nutContext->BeginRenderFrame()
                                .AddColorAttachment(attachmentBuilder.Build())
                                .Build();
    m_particleRenderer->Render(renderPass, engineData);
    nutContext->Submit({nutContext->EndRenderFrame(renderPass)});
}
