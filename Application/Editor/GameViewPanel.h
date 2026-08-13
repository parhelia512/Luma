#ifndef GAMEVIEWPANEL_H
#define GAMEVIEWPANEL_H
#include "IEditorPanel.h"
#include "../Renderer/RenderTarget.h"
#include "../Particles/ParticleRenderer.h"
#include <memory>
class GameViewPanel : public IEditorPanel
{
public:
    GameViewPanel() = default;
    ~GameViewPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "游戏视图"; }
private:
    void renderParticlesGPU();
    void drawToolbar();
    void drawStatsOverlay(const ImVec2& imageMin, const ImVec2& imageSize) const;
    std::shared_ptr<RenderTarget> m_gameViewTarget; 
    std::unique_ptr<Particles::ParticleRenderer> m_particleRenderer; 
    bool m_particleRendererInitialized = false; 
    int m_aspectModeIndex = 0; ///< 显示宽高比：0=自由，其余见 drawToolbar 的选项表
    bool m_showStats = false; 
};
#endif
