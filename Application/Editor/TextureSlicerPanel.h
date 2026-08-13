#ifndef TEXTURESLICERPANEL_H
#define TEXTURESLICERPANEL_H
#include "IEditorPanel.h"
#include <vector>
#include <string>
#include "Utils/Guid.h"
#include <webgpu/webgpu_cpp.h>
struct SimpleRect
{
    float x, y, w, h;
    SimpleRect() : x(0), y(0), w(0), h(0)
    {
    }
    SimpleRect(float _x, float _y, float _w, float _h) : x(_x), y(_y), w(_w), h(_h)
    {
    }
    float left() const { return x; }
    float top() const { return y; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float width() const { return w; }
    float height() const { return h; }
};
struct SliceRect
{
    SimpleRect rect; 
    std::string name; 
    float pivotX = 0.5f; ///< 归一化轴心 X（0-1，左上原点）
    float pivotY = 0.5f; ///< 归一化轴心 Y（0-1，左上原点）
    bool selected = false; 
};
enum class SliceMode
{
    Grid, 
    Manual, 
    Auto ///< 按透明度连通域自动切片
};
enum class InteractionMode
{
    Create, 
    Edit 
};
class TextureSlicerPanel : public IEditorPanel
{
public:
    TextureSlicerPanel() = default;
    ~TextureSlicerPanel() override = default;
    void Initialize(EditorContext* context) override;
    void Update(float deltaTime) override;
    void Draw() override;
    void Shutdown() override;
    const char* GetPanelName() const override { return "纹理切片编辑器"; }
    void OpenTexture(const Guid& textureGuid);
    void Close();
private:
    void drawToolbar();
    void drawTexturePreview();
    void drawSettingsPanel();
    void drawPivotEditor(SliceRect& slice);
    void drawSliceList();
    void applySlices();
    void generateTileAssets();
    void exportSlicesAsImages();
    void loadSlicesFromMetadata();
    void generateGridSlices();
    void generatePixelSizeSlices(int sliceW, int sliceH);
    void generateAutoSlices();
    void handleManualSlicing(ImVec2 imagePos, ImVec2 imageSize, float scale);
    void handlePreviewEditing(ImVec2 imagePos, ImVec2 imageSize, float scale);
    void saveSlice(const SliceRect& slice, int index);
    void loadTexture();
private:
    bool m_isOpen = false; 
    Guid m_currentTextureGuid; 
    unsigned char* m_textureData = nullptr; 
    int m_textureWidth = 0; 
    int m_textureHeight = 0; 
    int m_textureChannels = 0; 
    ImTextureID m_textureID = -1; 
    wgpu::Texture m_gpuTexture = nullptr; 
    std::string m_texturePath; 
    SliceMode m_sliceMode = SliceMode::Grid; 
    std::vector<SliceRect> m_slices; 
    int m_gridRows = 1; 
    int m_gridColumns = 1; 
    bool m_usePixelGrid = false; 
    int m_cellWidth = 32; 
    int m_cellHeight = 32; 
    int m_gridOffsetX = 0; ///< 网格起始偏移 X（px）
    int m_gridOffsetY = 0; ///< 网格起始偏移 Y（px）
    int m_gridSpacingX = 0; ///< 网格单元水平间距（px）
    int m_gridSpacingY = 0; ///< 网格单元垂直间距（px）
    int m_autoMinSize = 2; ///< 自动切片的最小包围盒边长（px），过滤噪点
    bool m_generateTileAssets = true; ///< 应用切片时是否同步生成 .tile 子精灵资产
    bool m_isDragging = false; 
    float m_dragStartX = 0, m_dragStartY = 0; 
    float m_dragEndX = 0, m_dragEndY = 0; 
    int m_selectedSliceIndex = -1; 
    InteractionMode m_interactionMode = InteractionMode::Create; 
    bool m_isMovingSlice = false; 
    bool m_isResizingSlice = false; 
    int m_resizeCorner = -1; ///< 0-3 为四角（左上/右上/右下/左下），4-7 为四边中点（上/右/下/左）
    float m_moveStartMouseX = 0, m_moveStartMouseY = 0; 
    SimpleRect m_moveStartRect; 
    float m_zoom = 1.0f; 
    float m_panX = 0.0f, m_panY = 0.0f; 
    bool m_showSlicePreviews = true; 
    char m_namePrefix[256] = "sprite"; 
    int m_defaultSliceWidthPx = 64;
    int m_defaultSliceHeightPx = 64;
};
#endif 
