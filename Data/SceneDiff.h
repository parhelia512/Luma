#ifndef SCENEDIFF_H
#define SCENEDIFF_H
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SceneData.h"

namespace Data
{
    /**
     * @brief 实体增量条目的类别。
     */
    enum class EntityDeltaKind : uint8_t
    {
        Added,   ///< 目标状态中新增的实体。
        Removed, ///< 目标状态中被删除的实体。
        Modified ///< 内容、父节点或同级顺序发生变化的实体。
    };

    /**
     * @brief 单个实体节点相对于前一状态的增量记录。
     *
     * 实体树按 localGuid 展平为逐节点表后对比得到。nodeYaml 保存目标状态下
     * 该节点不含 children 的规范化 YAML 文本（components 按键名排序，消除
     * std::unordered_map 迭代顺序的不确定性）。增量条目是纯值语义的字符串，
     * 不与任何场景快照共享 YAML::Node 底层内存。
     */
    struct EntityDelta
    {
        EntityDeltaKind kind = EntityDeltaKind::Modified; ///< 增量类别。
        Guid guid;               ///< 目标实体的 localGuid。
        Guid parentGuid;         ///< 目标状态中的父实体 GUID，根实体为无效 GUID；Removed 条目不使用。
        size_t siblingIndex = 0; ///< 目标状态中位于父节点 children（或根实体列表）内的序号；Removed 条目不使用。
        std::string nodeYaml;    ///< 目标状态下的单实体规范化 YAML（不含 children）；Removed 条目为空。
    };

    /**
     * @brief 相邻两个撤销状态之间的场景增量。
     *
     * 往返一致性约定：设 d = ComputeSceneDiff(A, B)，则 ApplySceneDiff(A, *d)
     * 与 B 在规范化序列化口径下逐字节等价（对每个实体取"components 按键名
     * 排序、不含 children"的 YAML 文本，对场景头取 name/camp/uiCamp 的 YAML
     * 文本进行对比）。由于 PrefabNode::components 是 std::unordered_map，其
     * 物理迭代顺序不属于场景语义，等价性因此以规范化口径衡量；实体集合、
     * 父子层级、同级顺序与全部组件数据在该口径下均被精确还原。
     */
    struct SceneDiff
    {
        bool headerChanged = false;            ///< 场景名或相机属性是否发生变化。
        std::string headerYaml;                ///< headerChanged 为 true 时保存目标状态的场景头 YAML（name/camp/uiCamp）。
        std::vector<EntityDelta> entityDeltas; ///< 实体级增量列表。
    };

    /**
     * @brief 计算从 before 到 after 的场景增量。
     *
     * 将两棵实体树分别按 localGuid 展平（记录父 GUID 与同级序号），逐节点
     * 对比规范化 YAML 文本与挂载位置：仅存在于 after 的实体记为 Added，仅
     * 存在于 before 的记为 Removed，内容或挂载位置变化的记为 Modified。
     * 同级顺序变化的实体同样产生 Modified 条目，因此应用增量后每个实体的
     * （父 GUID，同级序号）与 after 严格一致，可精确重建层级与渲染排序。
     *
     * @param before 前一状态的场景数据。
     * @param after 目标状态的场景数据。
     * @return 成功时返回增量；当任一状态的实体树存在重复或无效 localGuid
     *         等无法安全按 GUID 索引的情况时返回 std::nullopt，调用方应
     *         回退为存储全量快照。
     */
    std::optional<SceneDiff> ComputeSceneDiff(const SceneData& before, const SceneData& after);

    /**
     * @brief 将增量正向应用到基准状态，重建目标状态的场景数据。
     *
     * 基准实体树展平后按增量执行插入、删除与覆盖，再按（父 GUID，同级
     * 序号）重新组装实体树；未变更实体的节点数据直接复用基准中的数据。
     * 本函数是纯函数，不修改 base。
     *
     * @param base 基准状态的场景数据。
     * @param diff 由 ComputeSceneDiff 计算出的增量。
     * @return 重建出的目标状态场景数据。
     */
    SceneData ApplySceneDiff(const SceneData& base, const SceneDiff& diff);
}

#endif
