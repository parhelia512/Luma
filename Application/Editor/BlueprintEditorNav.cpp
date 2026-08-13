#define IMGUI_DEFINE_MATH_OPERATORS
#include "BlueprintEditorNav.h"
#include <imgui_node_editor_internal.h>

ImVec4 BlueprintEditorNav::GetViewRect(ax::NodeEditor::EditorContext* context)
{
    if (!context)
    {
        return ImVec4(0, 0, 0, 0);
    }
    // 与库内 API 层相同的转换方式：公开的 EditorContext 即 Detail::EditorContext
    auto* editor = reinterpret_cast<ax::NodeEditor::Detail::EditorContext*>(context);
    const ImRect& rect = editor->GetViewRect();
    return ImVec4(rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y);
}

void BlueprintEditorNav::NavigateToRect(ax::NodeEditor::EditorContext* context, const ImVec4& rect, float duration)
{
    if (!context)
    {
        return;
    }
    auto* editor = reinterpret_cast<ax::NodeEditor::Detail::EditorContext*>(context);
    ImRect target(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w));
    // NavigateTo(zoomIn=true) 会把目标矩形按最长边外扩 5%（c_NavigationZoomMargin/2）后再适配，
    // 这里先做等量内缩补偿，使最终可见矩形与传入矩形一致
    const float shrink = ImMax(target.GetWidth(), target.GetHeight()) * 0.05f / 1.1f;
    if (target.GetWidth() > shrink * 2.0f && target.GetHeight() > shrink * 2.0f)
    {
        target.Expand(-shrink);
        editor->NavigateTo(target, true, duration);
    }
    else
    {
        // 矩形过于狭长时补偿会翻转边界，退化为只平移不改缩放
        editor->NavigateTo(target, false, duration);
    }
}
