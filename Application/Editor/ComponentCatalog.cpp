#include "ComponentCatalog.h"

#include <unordered_map>

namespace
{
    struct CatalogMeta
    {
        const char* displayName;
        const char* category;
    };

    const std::unordered_map<std::string, CatalogMeta>& catalogTable()
    {
        static const std::unordered_map<std::string, CatalogMeta> table =
        {
            // 渲染
            {"SpriteComponent", {"精灵 (Sprite)", "渲染"}},
            {"TextComponent", {"文本 (Text)", "渲染"}},
            {"TilemapComponent", {"瓦片地图 (Tilemap)", "渲染"}},
            {"TilemapRendererComponent", {"瓦片地图渲染器 (Tilemap Renderer)", "渲染"}},

            // 光照
            {"PointLightComponent", {"点光源 (Point Light)", "光照"}},
            {"SpotLightComponent", {"聚光灯 (Spot Light)", "光照"}},
            {"DirectionalLightComponent", {"方向光 (Directional Light)", "光照"}},
            {"AreaLightComponent", {"面光源 (Area Light)", "光照"}},
            {"AmbientZoneComponent", {"环境光区域 (Ambient Zone)", "光照"}},
            {"LightProbeComponent", {"光照探针 (Light Probe)", "光照"}},
            {"ShadowCasterComponent", {"阴影投射体 (Shadow Caster)", "光照"}},
            {"LightingSettingsComponent", {"光照设置 (Lighting Settings)", "光照"}},

            // 物理
            {"RigidBodyComponent", {"刚体 (Rigid Body)", "物理"}},
            {"BoxColliderComponent", {"盒型碰撞体 (Box Collider)", "物理"}},
            {"CircleColliderComponent", {"圆形碰撞体 (Circle Collider)", "物理"}},
            {"CapsuleColliderComponent", {"胶囊碰撞体 (Capsule Collider)", "物理"}},
            {"PolygonColliderComponent", {"多边形碰撞体 (Polygon Collider)", "物理"}},
            {"EdgeColliderComponent", {"边缘碰撞体 (Edge Collider)", "物理"}},
            {"TilemapColliderComponent", {"瓦片地图碰撞体 (Tilemap Collider)", "物理"}},

            // UI
            {"ButtonComponent", {"按钮 (Button)", "UI"}},
            {"InputTextComponent", {"输入框 (Input Text)", "UI"}},
            {"ToggleButtonComponent", {"开关按钮 (Toggle Button)", "UI"}},
            {"CheckBoxComponent", {"复选框 (Check Box)", "UI"}},
            {"RadioButtonComponent", {"单选按钮 (Radio Button)", "UI"}},
            {"SliderComponent", {"滑动条 (Slider)", "UI"}},
            {"ComboBoxComponent", {"下拉框 (Combo Box)", "UI"}},
            {"ListBoxComponent", {"列表框 (List Box)", "UI"}},
            {"ExpanderComponent", {"折叠面板 (Expander)", "UI"}},
            {"ProgressBarComponent", {"进度条 (Progress Bar)", "UI"}},
            {"TabControlComponent", {"选项卡 (Tab Control)", "UI"}},
            {"UILayoutComponent", {"UI 布局 (UI Layout)", "UI"}},

            // 音频
            {"AudioComponent", {"音频源 (Audio Source)", "音频"}},

            // 动画 / 特效
            {"AnimationControllerComponent", {"动画控制器 (Animation Controller)", "动画与特效"}},
            {"ParticleSystemComponent", {"粒子系统 (Particle System)", "动画与特效"}},

            // 脚本
            {"ScriptsComponent", {"C# 脚本 (Scripts)", "脚本"}},
            {"ScriptComponent", {"C# 脚本（单个） (Script)", "脚本"}},
            {"SerializableEventTarget", {"事件目标 (Event Target)", "脚本"}},

            // 画质与后处理
            {"PostProcessSettingsComponent", {"后处理设置 (Post Process)", "画质与后处理"}},
            {"QualitySettingsComponent", {"画质设置 (Quality Settings)", "画质与后处理"}},

            // 基础
            {"TransformComponent", {"变换 (Transform)", "基础"}},
            {"TagComponent", {"标签 (Tag)", "基础"}},
            {"LayerComponent", {"层级 (Layer)", "基础"}},
            {"ActivityComponent", {"激活状态 (Activity)", "基础"}},
        };
        return table;
    }
}

ComponentCatalog::Entry ComponentCatalog::Get(const std::string& componentName)
{
    Entry entry;
    entry.componentName = componentName;
    const auto& table = catalogTable();
    auto it = table.find(componentName);
    if (it != table.end())
    {
        entry.displayName = it->second.displayName;
        entry.category = it->second.category;
    }
    else
    {
        entry.displayName = componentName;
        entry.category = "其他";
    }
    return entry;
}

const std::vector<std::string>& ComponentCatalog::CategoryOrder()
{
    static const std::vector<std::string> order =
    {
        "基础", "渲染", "光照", "物理", "UI", "音频", "动画与特效", "脚本", "画质与后处理", "其他",
    };
    return order;
}
