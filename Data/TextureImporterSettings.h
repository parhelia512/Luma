#ifndef LUMAENGINE_TEXTUREIMPORTERSETTINGS_H
#define LUMAENGINE_TEXTUREIMPORTERSETTINGS_H

#include "IData.h"

/**
 * @brief 纹理子精灵切片定义。
 *
 * 非破坏性元数据：仅记录源纹理上的矩形区域，不复制像素。
 * rect 使用源图像素坐标（左上原点），pivot 为归一化坐标（0-1，左上原点，默认中心）。
 */
struct TextureSliceEntry
{
    std::string name; ///< 子精灵名称。
    ECS::RectF rect = {0.0f, 0.0f, 0.0f, 0.0f}; ///< 源矩形区域（像素）。
    ECS::Vector2f pivot = {0.5f, 0.5f}; ///< 归一化轴心。
};

/**
 * @brief 纹理导入器设置。
 *
 * 包含用于配置纹理导入过程的各种参数。
 */
struct TextureImporterSettings : Data::IData<TextureImporterSettings>
{
    ECS::FilterQuality filterQuality = ECS::FilterQuality::Bilinear; ///< 纹理过滤质量。
    ECS::WrapMode wrapMode = ECS::WrapMode::Clamp; ///< 纹理环绕模式。
    YAML::Binary rawData; ///< 原始二进制数据。
    int pixelPerUnit = 100; ///< 每单位像素数。
    std::vector<TextureSliceEntry> slices; ///< 子精灵切片列表（由纹理切片编辑器维护）。
};


namespace YAML
{
    /**
     * @brief 为TextureSliceEntry结构体提供YAML序列化和反序列化转换。
     */
    template <>
    struct convert<TextureSliceEntry>
    {
        /**
         * @brief 将TextureSliceEntry对象编码为YAML节点。
         * @param rhs 要编码的TextureSliceEntry对象。
         * @return 表示TextureSliceEntry的YAML节点。
         */
        static Node encode(const TextureSliceEntry& rhs)
        {
            Node node;
            node["name"] = rhs.name;
            node["rect"] = rhs.rect;
            node["pivot"] = rhs.pivot;
            return node;
        }

        /**
         * @brief 从YAML节点解码TextureSliceEntry对象。
         * @param node 包含TextureSliceEntry数据的YAML节点。
         * @param rhs 要填充的TextureSliceEntry对象。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, TextureSliceEntry& rhs)
        {
            if (!node.IsMap() || !node["rect"]) return false;
            rhs.name = node["name"].as<std::string>("");
            rhs.rect = node["rect"].as<ECS::RectF>();
            if (node["pivot"])
            {
                rhs.pivot = node["pivot"].as<ECS::Vector2f>();
            }
            else
            {
                rhs.pivot = ECS::Vector2f(0.5f, 0.5f);
            }
            return true;
        }
    };

    /**
     * @brief 为TextureImporterSettings结构体提供YAML序列化和反序列化转换。
     */
    template <>
    struct convert<TextureImporterSettings>
    {
        /**
         * @brief 将TextureImporterSettings对象编码为YAML节点。
         * @param rhs 要编码的TextureImporterSettings对象。
         * @return 表示TextureImporterSettings的YAML节点。
         */
        static Node encode(const TextureImporterSettings& rhs)
        {
            Node node;
            node["filterQuality"] = static_cast<int>(rhs.filterQuality);
            node["wrapMode"] = static_cast<int>(rhs.wrapMode);
            node["rawData"] = rhs.rawData;
            node["pixelPerUnit"] = rhs.pixelPerUnit;
            // 无切片时不写该键，保持旧 .meta 文件形态不变
            if (!rhs.slices.empty())
            {
                node["slices"] = rhs.slices;
            }
            return node;
        }

        /**
         * @brief 从YAML节点解码TextureImporterSettings对象。
         * @param node 包含TextureImporterSettings数据的YAML节点。
         * @param rhs 要填充的TextureImporterSettings对象。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, TextureImporterSettings& rhs)
        {
            if (!node.IsMap()) return false;
            rhs.filterQuality = (ECS::FilterQuality)node["filterQuality"].as<int>(1);
            rhs.wrapMode = (ECS::WrapMode)node["wrapMode"].as<int>(0);
            if (node["rawData"])
            {
                rhs.rawData = node["rawData"].as<YAML::Binary>();
            }

            rhs.pixelPerUnit = node["pixelPerUnit"].as<int>(100);
            rhs.slices.clear();
            if (node["slices"] && node["slices"].IsSequence())
            {
                for (const auto& sliceNode : node["slices"])
                {
                    TextureSliceEntry entry;
                    if (convert<TextureSliceEntry>::decode(sliceNode, entry))
                    {
                        rhs.slices.push_back(std::move(entry));
                    }
                }
            }
            return true;
        }
    };
}

namespace CustomDrawing
{
    /**
     * @brief 为ECS::FilterQuality枚举类型提供UI小部件绘制功能。
     */
    template <>
    struct WidgetDrawer<ECS::FilterQuality>
    {
        /**
         * @brief 绘制ECS::FilterQuality的UI小部件。
         * @param label 小部件的标签。
         * @param value 要编辑的ECS::FilterQuality值。
         * @param callbacks UI绘制回调数据。
         * @return 如果值被修改则返回true，否则返回false。
         */
        static bool Draw(const std::string& label, ECS::FilterQuality& value, const UIDrawData& callbacks)
        {
            const char* items[] = {"Nearest", "Bilinear", "Mipmap"};
            int currentItem = static_cast<int>(value);
            if (ImGui::Combo(label.c_str(), &currentItem, items, IM_ARRAYSIZE(items)))
            {
                LogInfo("FilterQuality changed to {}", currentItem);
                value = static_cast<ECS::FilterQuality>(currentItem);
                return true;
            }
            return false;
        }
    };

    /**
     * @brief 为ECS::WrapMode枚举类型提供UI小部件绘制功能。
     */
    template <>
    struct WidgetDrawer<ECS::WrapMode>
    {
        /**
         * @brief 绘制ECS::WrapMode的UI小部件。
         * @param label 小部件的标签。
         * @param value 要编辑的ECS::WrapMode值。
         * @param callbacks UI绘制回调数据。
         * @return 如果值被修改则返回true，否则返回false。
         */
        static bool Draw(const std::string& label, ECS::WrapMode& value, const UIDrawData& callbacks)
        {
            const char* items[] = {"Clamp", "Repeat", "Mirror"};
            int currentItem = static_cast<int>(value);
            if (ImGui::Combo(label.c_str(), &currentItem, items, IM_ARRAYSIZE(items)))
            {
                value = static_cast<ECS::WrapMode>(currentItem);
                return true;
            }
            return false;
        }
    };
}

// REGISTRY 块通常用于注册系统或组件，不直接作为Doxygen注释的目标。
// 内部的属性注册已经指向了TextureImporterSettings的成员，这些成员已经有注释。
REGISTRY
{
    AssetRegistry_<TextureImporterSettings>(AssetType::Texture)
        .property("filterQuality", &TextureImporterSettings::filterQuality)
        .property("wrapMode", &TextureImporterSettings::wrapMode)
        .property("pixelPerUnit", &TextureImporterSettings::pixelPerUnit, true)
        .property("rawData", &TextureImporterSettings::rawData, false);
}


#endif