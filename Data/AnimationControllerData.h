#ifndef ANIMATIONCONTROLLER_H
#define ANIMATIONCONTROLLER_H
#include <string>
#include <variant>
#include <vector> // Added for std::vector in Transition and AnimationState
#include <unordered_map> // Added for std::unordered_map in AnimationControllerData

#include "Guid.h"
#include "IData.h"

// Forward declarations for YAML-CPP types if not included, assuming they are in the original context.
// For this task, I will assume YAML::Node is available.
namespace YAML {
    class Node;
}


/**
 * @brief 定义动画变量的类型。
 */
enum class VariableType
{
    VariableType_Float,  ///< 浮点型变量。
    VariableType_Bool,   ///< 布尔型变量。
    VariableType_Int,    ///< 整型变量。
    VariableType_Trigger ///< 触发器变量。
};

/**
 * @brief 表示一个动画控制器中的变量。
 */
struct AnimationVariable
{
    std::string Name; ///< 变量的名称。
    VariableType Type = VariableType::VariableType_Float; ///< 变量的类型。
    std::variant<float, bool, int> Value; ///< 变量的值，可以是浮点、布尔或整数。
};

/**
 * @brief 表示一个浮点型条件。
 */
struct FloatCondition
{
    /**
     * @brief 浮点型变量的比较操作符。
     */
    enum Comparison
    {
        GreaterThan, ///< 大于。
        LessThan,    ///< 小于。
    } op; ///< 比较操作符。

    std::string VarName; ///< 要比较的变量名称。
    float Value;         ///< 用于比较的浮点值。
};

/**
 * @brief 表示一个布尔型条件。
 */
struct BoolCondition
{
    /**
     * @brief 布尔型变量的比较操作符。
     */
    enum Comparison
    {
        IsTrue,  ///< 为真。
        IsFalse, ///< 为假。
    } op; ///< 比较操作符。

    std::string VarName; ///< 要比较的变量名称。
};

/**
 * @brief 表示一个整型条件。
 */
struct IntCondition
{
    /**
     * @brief 整型变量的比较操作符。
     */
    enum Comparison
    {
        GreaterThan, ///< 大于。
        LessThan,    ///< 小于。
        Equal,       ///< 等于。
        NotEqual,    ///< 不等于。
    } op; ///< 比较操作符。

    std::string VarName; ///< 要比较的变量名称。
    int Value;           ///< 用于比较的整数值。
};


/**
 * @brief 表示一个触发器条件。
 */
struct TriggerCondition
{
    std::string VarName; ///< 要检查的触发器变量名称。
};


/**
 * @brief 条件的变体类型，可以是浮点、布尔、整数或触发器条件。
 */
using Condition = std::variant<FloatCondition, BoolCondition, IntCondition, TriggerCondition>;

/**
 * @brief 表示动画状态之间的过渡。
 */
struct Transition
{
    Guid ToGuid; ///< 目标状态的全局唯一标识符。
    std::string TransitionName; ///< 过渡的名称。
    float TransitionDuration = 0.3f; ///< 过渡的持续时间。
    std::vector<Condition> Conditions; ///< 触发此过渡的条件列表。
    int priority = 0; ///< 过渡的优先级。
    bool hasExitTime = true; ///< 是否有退出时间。
    float exitTime = 1.0f; ///< 归一化退出时间（0-1），hasExitTime 开启时动画播放进度达到该比例才允许过渡。
};


/**
 * @brief 动画状态的类型。
 */
enum class AnimationStateType
{
    Clip = 0,      ///< 绑定单个动画剪辑，状态GUID即剪辑GUID。
    BlendTree = 1, ///< 一维混合树，按 float 参数在多个子项剪辑间混合。
};

/**
 * @brief 一维混合树数据：绑定一个 float 参数，按阈值组织子项剪辑。
 */
struct BlendTreeData
{
    /**
     * @brief 混合树子项：一个剪辑及其对应的参数阈值。
     */
    struct Child
    {
        Guid clipGuid;          ///< 子项动画剪辑的全局唯一标识符。
        float threshold = 0.0f; ///< 参数阈值。
    };

    std::string parameterName;   ///< 绑定的 float 变量名。
    std::vector<Child> children; ///< 子项列表，编辑器维护为按阈值升序。
};

/**
 * @brief 表示一个动画状态。
 */
struct AnimationState
{
    std::vector<Transition> Transitions; ///< 从此状态出发的过渡列表。
    std::string stateName; ///< 状态显示名，与剪辑名解耦；旧数据缺省为空，编辑器加载时以剪辑名初始化。
    float speed = 1.0f; ///< 状态播放速度倍率，运行时播放该状态动画时乘上。
    float editorPosX = 0.0f; ///< 编辑器中节点画布位置X。
    float editorPosY = 0.0f; ///< 编辑器中节点画布位置Y。
    bool hasEditorPosition = false; ///< 旧数据无位置字段时为 false，编辑器回退到网格布局并写回。
    AnimationStateType stateType = AnimationStateType::Clip; ///< 状态类型；旧数据无此字段时按单剪辑处理。
    BlendTreeData blendTree; ///< 混合树数据，仅 stateType 为 BlendTree 时有效。
};


/**
 * @brief 包含特殊动画状态的全局唯一标识符。
 */
struct SpecialStateGuids
{
    /**
     * @brief 获取入口状态的全局唯一标识符。
     * @return 入口状态的Guid。
     */
    static Guid Entry() { return Guid::FromString("00000000-0000-0000-0000-000000000001"); }
    /**
     * @brief 获取任意状态的全局唯一标识符。
     * @return 任意状态的Guid。
     */
    static Guid AnyState() { return Guid::FromString("00000000-0000-0000-0000-000000000002"); }
};


/**
 * @brief 动画控制器的数据结构，继承自IData。
 */
struct AnimationControllerData : Data::IData<AnimationControllerData>
{
    std::unordered_map<std::string, Guid> Clips; ///< 动画片段的名称到Guid的映射。
    std::unordered_map<Guid, AnimationState> States; ///< 动画状态的Guid到AnimationState的映射。
    std::vector<AnimationVariable> Variables; ///< 动画控制器中使用的变量列表。
};

namespace YAML
{
    /**
     * @brief YAML转换器，用于VariableType枚举的序列化和反序列化。
     */
    template <>
    struct convert<VariableType>
    {
        /**
         * @brief 将VariableType编码为YAML节点。
         * @param type 要编码的VariableType值。
         * @return 表示VariableType的YAML节点。
         */
        static Node encode(const VariableType& type)
        {
            switch (type)
            {
            case VariableType::VariableType_Float: return Node("Float");
            case VariableType::VariableType_Bool: return Node("Bool");
            case VariableType::VariableType_Int: return Node("Int");
            case VariableType::VariableType_Trigger: return Node("Trigger");
            default: return Node("Float");
            }
        }

        /**
         * @brief 将YAML节点解码为VariableType。
         * @param node 要解码的YAML节点。
         * @param type 存储解码结果的VariableType引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, VariableType& type)
        {
            if (!node.IsScalar()) return false;
            std::string s = node.as<std::string>();
            if (s == "Float") type = VariableType::VariableType_Float;
            else if (s == "Bool") type = VariableType::VariableType_Bool;
            else if (s == "Int") type = VariableType::VariableType_Int;
            else if (s == "Trigger") type = VariableType::VariableType_Trigger;
            else return false;
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于FloatCondition::Comparison枚举的序列化和反序列化。
     */
    template <>
    struct convert<FloatCondition::Comparison>
    {
        /**
         * @brief 将FloatCondition::Comparison编码为YAML节点。
         * @param op 要编码的比较操作符。
         * @return 表示比较操作符的YAML节点。
         */
        static Node encode(const FloatCondition::Comparison& op) { return Node(static_cast<int>(op)); }

        /**
         * @brief 将YAML节点解码为FloatCondition::Comparison。
         * @param node 要解码的YAML节点。
         * @param op 存储解码结果的FloatCondition::Comparison引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, FloatCondition::Comparison& op)
        {
            if (!node.IsScalar()) return false;
            op = static_cast<FloatCondition::Comparison>(node.as<int>());
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于BoolCondition::Comparison枚举的序列化和反序列化。
     */
    template <>
    struct convert<BoolCondition::Comparison>
    {
        /**
         * @brief 将BoolCondition::Comparison编码为YAML节点。
         * @param op 要编码的比较操作符。
         * @return 表示比较操作符的YAML节点。
         */
        static Node encode(const BoolCondition::Comparison& op) { return Node(static_cast<int>(op)); }

        /**
         * @brief 将YAML节点解码为BoolCondition::Comparison。
         * @param node 要解码的YAML节点。
         * @param op 存储解码结果的BoolCondition::Comparison引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, BoolCondition::Comparison& op)
        {
            if (!node.IsScalar()) return false;
            op = static_cast<BoolCondition::Comparison>(node.as<int>());
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于IntCondition::Comparison枚举的序列化和反序列化。
     */
    template <>
    struct convert<IntCondition::Comparison>
    {
        /**
         * @brief 将IntCondition::Comparison编码为YAML节点。
         * @param op 要编码的比较操作符。
         * @return 表示比较操作符的YAML节点。
         */
        static Node encode(const IntCondition::Comparison& op) { return Node(static_cast<int>(op)); }

        /**
         * @brief 将YAML节点解码为IntCondition::Comparison。
         * @param node 要解码的YAML节点。
         * @param op 存储解码结果的IntCondition::Comparison引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, IntCondition::Comparison& op)
        {
            if (!node.IsScalar()) return false;
            op = static_cast<IntCondition::Comparison>(node.as<int>());
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于Condition变体类型的序列化和反序列化。
     */
    template <>
    struct convert<Condition>
    {
        /**
         * @brief 将Condition编码为YAML节点。
         * @param condition 要编码的Condition值。
         * @return 表示Condition的YAML节点。
         */
        static Node encode(const Condition& condition)
        {
            Node node;
            if (std::holds_alternative<FloatCondition>(condition))
            {
                const auto& cond = std::get<FloatCondition>(condition);
                node["Type"] = "Float";
                node["VarName"] = cond.VarName;
                node["Comparison"] = cond.op;
                node["Value"] = cond.Value;
            }
            else if (std::holds_alternative<BoolCondition>(condition))
            {
                const auto& cond = std::get<BoolCondition>(condition);
                node["Type"] = "Bool";
                node["Comparison"] = cond.op;
                node["VarName"] = cond.VarName;
            }
            else if (std::holds_alternative<IntCondition>(condition))
            {
                const auto& cond = std::get<IntCondition>(condition);
                node["Type"] = "Int";
                node["Comparison"] = cond.op;
                node["VarName"] = cond.VarName;
                node["Value"] = cond.Value;
            }

            else if (std::holds_alternative<TriggerCondition>(condition))
            {
                const auto& cond = std::get<TriggerCondition>(condition);
                node["Type"] = "Trigger";
                node["VarName"] = cond.VarName;
            }
            else
            {
                return Node();
            }
            return node;
        }

        /**
         * @brief 将YAML节点解码为Condition。
         * @param node 要解码的YAML节点。
         * @param condition 存储解码结果的Condition引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, Condition& condition)
        {
            if (!node.IsMap() || !node["Type"]) return false;
            std::string type = node["Type"].as<std::string>();
            if (type == "Float")
            {
                FloatCondition cond;
                cond.op = node["Comparison"].as<FloatCondition::Comparison>();
                cond.Value = node["Value"].as<float>();
                cond.VarName = node["VarName"].as<std::string>();
                condition = cond;
            }
            else if (type == "Bool")
            {
                BoolCondition cond;
                cond.op = node["Comparison"].as<BoolCondition::Comparison>();
                cond.VarName = node["VarName"].as<std::string>();
                condition = cond;
            }
            else if (type == "Int")
            {
                IntCondition cond;
                cond.op = node["Comparison"].as<IntCondition::Comparison>();
                cond.VarName = node["VarName"].as<std::string>();
                cond.Value = node["Value"].as<int>();
                condition = cond;
            }

            else if (type == "Trigger")
            {
                TriggerCondition cond;
                cond.VarName = node["VarName"].as<std::string>();
                condition = cond;
            }
            else
            {
                return false;
            }
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于AnimationVariable结构的序列化和反序列化。
     */
    template <>
    struct convert<AnimationVariable>
    {
        /**
         * @brief 将AnimationVariable编码为YAML节点。
         * @param variable 要编码的AnimationVariable值。
         * @return 表示AnimationVariable的YAML节点。
         */
        static Node encode(const AnimationVariable& variable)
        {
            Node node;
            node["Name"] = variable.Name;

            node["Type"] = variable.Type;


            if (variable.Type == VariableType::VariableType_Trigger)
            {
            }
            else if (std::holds_alternative<float>(variable.Value))
            {
                node["Value"] = std::get<float>(variable.Value);
            }
            else if (std::holds_alternative<bool>(variable.Value))
            {
                node["Value"] = std::get<bool>(variable.Value);
            }
            else if (std::holds_alternative<int>(variable.Value))
            {
                node["Value"] = std::get<int>(variable.Value);
            }
            return node;
        }

        /**
         * @brief 将YAML节点解码为AnimationVariable。
         * @param node 要解码的YAML节点。
         * @param variable 存储解码结果的AnimationVariable引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, AnimationVariable& variable)
        {
            if (!node.IsMap() || !node["Name"]) return false;

            variable.Name = node["Name"].as<std::string>();

            variable.Type = node["Type"] ? node["Type"].as<VariableType>() : VariableType::VariableType_Float;

            if (variable.Type == VariableType::VariableType_Trigger)
            {
                variable.Value = false;
                return true;
            }

            if (node["Value"])
            {
                switch (variable.Type)
                {
                case VariableType::VariableType_Float: variable.Value = node["Value"].as<float>();
                    break;
                case VariableType::VariableType_Bool: variable.Value = node["Value"].as<bool>();
                    break;
                case VariableType::VariableType_Int: variable.Value = node["Value"].as<int>();
                    break;
                case VariableType::VariableType_Trigger: break;
                }
            }
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于Transition结构的序列化和反序列化。
     */
    template <>
    struct convert<Transition>
    {
        /**
         * @brief 将Transition编码为YAML节点。
         * @param transition 要编码的Transition值。
         * @return 表示Transition的YAML节点。
         */
        static Node encode(const Transition& transition)
        {
            Node node;
            node["ToGuid"] = transition.ToGuid.ToString();
            node["TransitionDuration"] = transition.TransitionDuration;
            node["TransitionName"] = transition.TransitionName;
            node["Conditions"] = transition.Conditions;
            node["priority"] = transition.priority;
            node["hasExitTime"] = transition.hasExitTime;
            node["exitTime"] = transition.exitTime;
            return node;
        }

        /**
         * @brief 将YAML节点解码为Transition。
         * @param node 要解码的YAML节点。
         * @param transition 存储解码结果的Transition引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, Transition& transition)
        {
            if (!node.IsMap()) return false;
            transition.ToGuid = Guid::FromString(node["ToGuid"].as<std::string>());
            transition.TransitionDuration = node["TransitionDuration"].as<float>();
            transition.TransitionName = node["TransitionName"].as<std::string>();
            if (node["Conditions"])
                transition.Conditions = node["Conditions"].as<std::vector<Condition>>();
            transition.priority = node["priority"] ? node["priority"].as<int>() : 0;
            transition.hasExitTime = node["hasExitTime"] ? node["hasExitTime"].as<bool>() : true;
            transition.exitTime = node["exitTime"] ? node["exitTime"].as<float>() : 1.0f;
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于BlendTreeData::Child结构的序列化和反序列化。
     */
    template <>
    struct convert<BlendTreeData::Child>
    {
        /**
         * @brief 将BlendTreeData::Child编码为YAML节点。
         * @param child 要编码的子项。
         * @return 表示子项的YAML节点。
         */
        static Node encode(const BlendTreeData::Child& child)
        {
            Node node;
            node["ClipGuid"] = child.clipGuid.ToString();
            node["Threshold"] = child.threshold;
            return node;
        }

        /**
         * @brief 将YAML节点解码为BlendTreeData::Child。
         * @param node 要解码的YAML节点。
         * @param child 存储解码结果的子项引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, BlendTreeData::Child& child)
        {
            if (!node.IsMap()) return false;
            if (node["ClipGuid"])
                child.clipGuid = Guid::FromString(node["ClipGuid"].as<std::string>());
            child.threshold = node["Threshold"] ? node["Threshold"].as<float>() : 0.0f;
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于BlendTreeData结构的序列化和反序列化。
     */
    template <>
    struct convert<BlendTreeData>
    {
        /**
         * @brief 将BlendTreeData编码为YAML节点。
         * @param tree 要编码的混合树数据。
         * @return 表示混合树数据的YAML节点。
         */
        static Node encode(const BlendTreeData& tree)
        {
            Node node;
            node["ParameterName"] = tree.parameterName;
            node["Children"] = tree.children;
            return node;
        }

        /**
         * @brief 将YAML节点解码为BlendTreeData。
         * @param node 要解码的YAML节点。
         * @param tree 存储解码结果的混合树数据引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, BlendTreeData& tree)
        {
            if (!node.IsMap()) return false;
            if (node["ParameterName"])
                tree.parameterName = node["ParameterName"].as<std::string>();
            if (node["Children"])
                tree.children = node["Children"].as<std::vector<BlendTreeData::Child>>();
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于AnimationState结构的序列化和反序列化。
     */
    template <>
    struct convert<AnimationState>
    {
        /**
         * @brief 将AnimationState编码为YAML节点。
         * @param state 要编码的AnimationState值。
         * @return 表示AnimationState的YAML节点。
         */
        static Node encode(const AnimationState& state)
        {
            Node node;
            node["Transitions"] = state.Transitions;
            if (!state.stateName.empty())
                node["stateName"] = state.stateName;
            node["speed"] = state.speed;
            if (state.hasEditorPosition)
            {
                node["editorPosX"] = state.editorPosX;
                node["editorPosY"] = state.editorPosY;
            }
            // 仅混合树状态写入类型与混合树字段，单剪辑状态保持旧格式不变
            if (state.stateType == AnimationStateType::BlendTree)
            {
                node["stateType"] = static_cast<int>(state.stateType);
                node["blendTree"] = state.blendTree;
            }
            return node;
        }

        /**
         * @brief 将YAML节点解码为AnimationState。
         * @param node 要解码的YAML节点。
         * @param state 存储解码结果的AnimationState引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, AnimationState& state)
        {
            if (!node.IsMap()) return false;
            if (node["Transitions"])
                state.Transitions = node["Transitions"].as<std::vector<Transition>>();
            if (node["stateName"])
                state.stateName = node["stateName"].as<std::string>();
            state.speed = node["speed"] ? node["speed"].as<float>() : 1.0f;
            if (node["editorPosX"] && node["editorPosY"])
            {
                state.editorPosX = node["editorPosX"].as<float>();
                state.editorPosY = node["editorPosY"].as<float>();
                state.hasEditorPosition = true;
            }
            state.stateType = node["stateType"]
                                  ? static_cast<AnimationStateType>(node["stateType"].as<int>())
                                  : AnimationStateType::Clip;
            if (node["blendTree"])
                state.blendTree = node["blendTree"].as<BlendTreeData>();
            return true;
        }
    };

    /**
     * @brief YAML转换器，用于AnimationControllerData结构的序列化和反序列化。
     */
    template <>
    struct convert<AnimationControllerData>
    {
        /**
         * @brief 将AnimationControllerData编码为YAML节点。
         * @param controller 要编码的AnimationControllerData值。
         * @return 表示AnimationControllerData的YAML节点。
         */
        static Node encode(const AnimationControllerData& controller)
        {
            Node node;
            for (const auto& [name, clip] : controller.Clips)
            {
                node["Clips"][name] = clip;
            }
            for (const auto& [guid, state] : controller.States)
            {
                node["States"][guid.ToString()] = state;
            }
            node["Variables"] = controller.Variables;
            return node;
        }

        /**
         * @brief 将YAML节点解码为AnimationControllerData。
         * @param node 要解码的YAML节点。
         * @param controller 存储解码结果的AnimationControllerData引用。
         * @return 如果解码成功则返回true，否则返回false。
         */
        static bool decode(const Node& node, AnimationControllerData& controller)
        {
            if (!node.IsMap()) return false;
            if (node["Clips"])
            {
                for (const auto& it : node["Clips"])
                {
                    controller.Clips[it.first.as<std::string>()] = it.second.as<Guid>();
                }
            }
            if (node["States"])
            {
                for (const auto& it : node["States"])
                {
                    controller.States[Guid::FromString(it.first.as<std::string>())] = it.second.as<AnimationState>();
                }
            }
            if (node["Variables"])
            {
                controller.Variables = node["Variables"].as<std::vector<AnimationVariable>>();
            }
            return true;
        }
    };
}
#endif