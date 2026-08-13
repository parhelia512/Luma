#ifndef COMPONENTCATALOG_H
#define COMPONENTCATALOG_H

#include <string>
#include <vector>

/**
 * @brief 编辑器组件目录：为"添加组件"菜单提供分类与显示名。
 *
 * 组件注册表（ComponentRegistry）本身是平铺的，这里维护一份
 * 组件名 → 分类/中文显示名 的元数据，用于把添加菜单组织成
 * Unity 风格的分类树。未登记的组件自动归入"其他"分类。
 */
class ComponentCatalog
{
public:
    /**
     * @brief 目录条目。
     */
    struct Entry
    {
        std::string componentName; ///< 注册表中的组件名（如 "SpriteComponent"）。
        std::string displayName;   ///< 菜单显示名（中文 + 原名后缀便于搜索）。
        std::string category;      ///< 所属分类。
    };

    /**
     * @brief 获取组件的目录条目（未登记时返回归入"其他"的条目）。
     * @param componentName 注册表组件名。
     */
    static Entry Get(const std::string& componentName);

    /**
     * @brief 分类的推荐显示顺序（添加菜单按此排序，未列出的分类排在末尾）。
     */
    static const std::vector<std::string>& CategoryOrder();
};

#endif
