#ifndef LUMAENGINE_BLUEPRINTEDITORNAV_H
#define LUMAENGINE_BLUEPRINTEDITORNAV_H
#include <imgui.h>

namespace ax::NodeEditor
{
    struct EditorContext;
}

/**
 * @brief 桥接 imgui-node-editor 的内部视图导航能力。
 *
 * 公开 API 只能对内容/选区自适应缩放，无法精确设置画布可见矩形（书签恢复需要）。
 * 内部头 imgui_node_editor_internal.h 在全局作用域定义 `namespace ed = ax::NodeEditor::Detail`，
 * 与 BlueprintPanel.h 的 `ed = ax::NodeEditor` 别名冲突，因此隔离在独立编译单元中封装。
 */
namespace BlueprintEditorNav
{
    /// 获取当前画布可见矩形（画布坐标；返回值 xy = Min，zw = Max）。
    ImVec4 GetViewRect(ax::NodeEditor::EditorContext* context);

    /// 把画布视角导航到指定矩形（画布坐标；参数 xy = Min，zw = Max），同时恢复位置与缩放。
    void NavigateToRect(ax::NodeEditor::EditorContext* context, const ImVec4& rect, float duration);
}
#endif
