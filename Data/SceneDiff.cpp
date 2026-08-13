#include "SceneDiff.h"

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    /**
     * @brief 展平表中单个实体的记录。
     */
    struct FlatEntry
    {
        Guid guid;                              ///< 实体的 localGuid。
        Guid parentGuid;                        ///< 父实体 GUID，根实体为无效 GUID。
        size_t siblingIndex = 0;                ///< 在父节点 children（或根实体列表）内的序号。
        const Data::PrefabNode* node = nullptr; ///< 指向原树中节点的非拥有指针。
    };

    /**
     * @brief 实体树按 localGuid 建立的展平索引，entries 保持先序遍历顺序。
     */
    struct FlatTable
    {
        std::vector<FlatEntry> entries;               ///< 先序排列的实体记录。
        std::unordered_map<Guid, size_t> indexByGuid; ///< localGuid 到 entries 下标的映射。
    };

    /**
     * @brief 先序遍历实体子树并写入展平表。
     * @param nodes 同级节点列表。
     * @param parentGuid 这些节点的父实体 GUID（根层为无效 GUID）。
     * @param table 输出展平表。
     * @return 遇到重复或无效 localGuid 时返回 false，表示无法安全按 GUID 索引。
     */
    bool flattenInto(const std::vector<Data::PrefabNode>& nodes, const Guid& parentGuid, FlatTable& table)
    {
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const Data::PrefabNode& child = nodes[i];
            if (!child.localGuid.Valid())
            {
                return false;
            }
            if (!table.indexByGuid.emplace(child.localGuid, table.entries.size()).second)
            {
                return false;
            }
            FlatEntry entry;
            entry.guid = child.localGuid;
            entry.parentGuid = parentGuid;
            entry.siblingIndex = i;
            entry.node = &child;
            table.entries.push_back(entry);
            if (!flattenInto(child.children, child.localGuid, table))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 生成单个实体节点不含 children 的规范化 YAML 文本。
     *
     * 键的写入顺序固定为 localGuid、name、prefabSource（仅有效时）、
     * components，其中 components 按键名字典序排序，保证同一逻辑状态总是
     * 产生相同文本。该文本经 YAML::Load 与 convert<PrefabNode>::decode 还原
     * 后再次规范化序列化仍得到相同文本（yaml-cpp 标量在节点内以字符串原样
     * 保存），据此保证增量往返的一致性。
     */
    std::string dumpNodeCanonical(const Data::PrefabNode& node)
    {
        YAML::Node result(YAML::NodeType::Map);
        result["localGuid"] = node.localGuid;
        result["name"] = node.name;
        if (node.prefabSource.Valid())
        {
            result["prefabSource"] = node.prefabSource;
        }
        std::vector<const std::string*> keys;
        keys.reserve(node.components.size());
        for (const auto& pair : node.components)
        {
            keys.push_back(&pair.first);
        }
        std::sort(keys.begin(), keys.end(),
                  [](const std::string* lhs, const std::string* rhs) { return *lhs < *rhs; });
        YAML::Node components(YAML::NodeType::Map);
        for (const std::string* key : keys)
        {
            components[*key] = node.components.at(*key);
        }
        result["components"] = components;
        return YAML::Dump(result);
    }

    /**
     * @brief 生成场景头（名称与相机属性）的规范化 YAML 文本。
     *
     * 键顺序与 convert<SceneData>::encode 保持一致（name/camp/uiCamp），
     * 浮点标量由 yaml-cpp 以可无损往返的精度输出，同值必得同文本。
     */
    std::string dumpHeader(const Data::SceneData& scene)
    {
        YAML::Node header(YAML::NodeType::Map);
        header["name"] = scene.name;
        header["camp"] = scene.cameraProperties;
        header["uiCamp"] = scene.uiCameraProperties;
        return YAML::Dump(header);
    }

    /**
     * @brief 解析增量条目中的单实体 YAML 文本。
     * @param yaml 由 dumpNodeCanonical 生成的文本。
     * @param out 输出的实体节点（children 为空）。
     * @return 解析失败返回 false（正常流程不应发生，仅作防御）。
     */
    bool parseNodeYaml(const std::string& yaml, Data::PrefabNode& out)
    {
        try
        {
            const YAML::Node node = YAML::Load(yaml);
            return YAML::convert<Data::PrefabNode>::decode(node, out);
        }
        catch (const YAML::Exception&)
        {
            return false;
        }
    }

    /**
     * @brief 拷贝节点数据但不拷贝 children（children 由重建流程重新组装）。
     *
     * components 中的 YAML::Node 为引用语义浅拷贝；撤销系统中的场景数据
     * 全程只读（仅供对比与 LoadFromData 读取），共享底层节点是安全的，
     * 现有撤销栈拷贝整份 SceneData 时行为亦相同。
     */
    Data::PrefabNode copyNodeWithoutChildren(const Data::PrefabNode& source)
    {
        Data::PrefabNode node;
        node.localGuid = source.localGuid;
        node.name = source.name;
        node.prefabSource = source.prefabSource;
        node.components = source.components;
        return node;
    }

    /**
     * @brief 重建期使用的可变实体记录。
     */
    struct RebuildRecord
    {
        Data::PrefabNode data;   ///< 节点数据（children 为空，稍后组装）。
        Guid parentGuid;         ///< 目标状态中的父实体 GUID。
        size_t siblingIndex = 0; ///< 目标状态中的同级序号。
    };

    /**
     * @brief 自顶向下递归组装子树，节点数据自 records 中移出。
     * @param guid 子树根实体的 GUID（必须存在于 records 中）。
     * @param records 重建记录表。
     * @param childrenOf 父 GUID 到已排序子 GUID 列表的映射。
     * @return 组装完成的实体节点。
     */
    Data::PrefabNode buildSubtree(const Guid& guid,
                                  std::unordered_map<Guid, RebuildRecord>& records,
                                  const std::unordered_map<Guid, std::vector<Guid>>& childrenOf)
    {
        Data::PrefabNode node = std::move(records.at(guid).data);
        node.children.clear();
        const auto it = childrenOf.find(guid);
        if (it != childrenOf.end())
        {
            node.children.reserve(it->second.size());
            for (const Guid& childGuid : it->second)
            {
                node.children.push_back(buildSubtree(childGuid, records, childrenOf));
            }
        }
        return node;
    }
}

namespace Data
{
    std::optional<SceneDiff> ComputeSceneDiff(const SceneData& before, const SceneData& after)
    {
        FlatTable oldTable;
        FlatTable newTable;
        if (!flattenInto(before.entities, Guid(), oldTable) ||
            !flattenInto(after.entities, Guid(), newTable))
        {
            return std::nullopt;
        }

        try
        {
            SceneDiff diff;
            std::string afterHeader = dumpHeader(after);
            if (dumpHeader(before) != afterHeader)
            {
                diff.headerChanged = true;
                diff.headerYaml = std::move(afterHeader);
            }

            // 新增与修改：按 after 的先序遍历产出，条目顺序确定。
            // 同级顺序变化（siblingIndex 改变）也记为 Modified，因此应用增量后
            // 每个实体的（父 GUID，同级序号）都与 after 严格一致；同一父节点下
            // 的序号在 after 中天然连续且互不重复，按序号排序即可精确还原顺序
            // 与层级（往返一致性据此成立，另见 SceneDiff 的类型注释）。
            for (const FlatEntry& newEntry : newTable.entries)
            {
                const auto oldIt = oldTable.indexByGuid.find(newEntry.guid);
                if (oldIt == oldTable.indexByGuid.end())
                {
                    EntityDelta delta;
                    delta.kind = EntityDeltaKind::Added;
                    delta.guid = newEntry.guid;
                    delta.parentGuid = newEntry.parentGuid;
                    delta.siblingIndex = newEntry.siblingIndex;
                    delta.nodeYaml = dumpNodeCanonical(*newEntry.node);
                    diff.entityDeltas.push_back(std::move(delta));
                    continue;
                }
                const FlatEntry& oldEntry = oldTable.entries[oldIt->second];
                const bool placementChanged = oldEntry.parentGuid != newEntry.parentGuid ||
                    oldEntry.siblingIndex != newEntry.siblingIndex;
                std::string newYaml = dumpNodeCanonical(*newEntry.node);
                if (placementChanged || dumpNodeCanonical(*oldEntry.node) != newYaml)
                {
                    EntityDelta delta;
                    delta.kind = EntityDeltaKind::Modified;
                    delta.guid = newEntry.guid;
                    delta.parentGuid = newEntry.parentGuid;
                    delta.siblingIndex = newEntry.siblingIndex;
                    delta.nodeYaml = std::move(newYaml);
                    diff.entityDeltas.push_back(std::move(delta));
                }
            }

            // 删除：按 before 的先序遍历产出；整棵子树被删除时，其每个后代
            // 实体都会各自产生一条 Removed 记录（展平表覆盖全部节点）。
            for (const FlatEntry& oldEntry : oldTable.entries)
            {
                if (newTable.indexByGuid.find(oldEntry.guid) == newTable.indexByGuid.end())
                {
                    EntityDelta delta;
                    delta.kind = EntityDeltaKind::Removed;
                    delta.guid = oldEntry.guid;
                    diff.entityDeltas.push_back(std::move(delta));
                }
            }
            return diff;
        }
        catch (const std::exception&)
        {
            // 序列化异常（理论上不发生）时放弃增量，调用方回退为全量快照。
            return std::nullopt;
        }
    }

    SceneData ApplySceneDiff(const SceneData& base, const SceneDiff& diff)
    {
        SceneData result;
        result.name = base.name;
        result.cameraProperties = base.cameraProperties;
        result.uiCameraProperties = base.uiCameraProperties;
        if (diff.headerChanged)
        {
            try
            {
                const YAML::Node header = YAML::Load(diff.headerYaml);
                result.name = header["name"].as<std::string>("");
                if (header["camp"])
                {
                    result.cameraProperties = header["camp"].as<Camera::CamProperties>();
                }
                if (header["uiCamp"])
                {
                    result.uiCameraProperties = header["uiCamp"].as<Camera::CamProperties>();
                }
            }
            catch (const YAML::Exception&)
            {
                // 场景头增量损坏时保留基准值，避免撤销流程中断。
                assert(false && "ApplySceneDiff: 场景头 YAML 解析失败");
            }
        }

        FlatTable baseTable;
        if (!flattenInto(base.entities, Guid(), baseTable))
        {
            // 基准状态来自可信的快照或重放链，展平失败意味着数据损坏；
            // 保底原样返回基准实体，避免撤销操作丢失整个场景。
            assert(false && "ApplySceneDiff: 基准状态展平失败");
            result.entities = base.entities;
            return result;
        }

        // 第一步：以基准展平表初始化重建记录，order 记录确定性的遍历顺序
        // （基准先序在前，新增实体按增量顺序在后）。
        std::unordered_map<Guid, RebuildRecord> records;
        std::vector<Guid> order;
        records.reserve(baseTable.entries.size() + diff.entityDeltas.size());
        order.reserve(baseTable.entries.size() + diff.entityDeltas.size());
        for (const FlatEntry& entry : baseTable.entries)
        {
            RebuildRecord record;
            record.data = copyNodeWithoutChildren(*entry.node);
            record.parentGuid = entry.parentGuid;
            record.siblingIndex = entry.siblingIndex;
            records.emplace(entry.guid, std::move(record));
            order.push_back(entry.guid);
        }

        // 第二步：按顺序应用增量条目（删除、插入、覆盖）。
        for (const EntityDelta& delta : diff.entityDeltas)
        {
            if (delta.kind == EntityDeltaKind::Removed)
            {
                records.erase(delta.guid);
                continue;
            }
            PrefabNode node;
            if (!parseNodeYaml(delta.nodeYaml, node))
            {
                // 增量条目损坏时跳过该实体，保留基准中的数据。
                assert(false && "ApplySceneDiff: 增量条目 YAML 解析失败");
                continue;
            }
            const auto emplaced = records.try_emplace(delta.guid);
            RebuildRecord& record = emplaced.first->second;
            record.data = std::move(node);
            record.parentGuid = delta.parentGuid;
            record.siblingIndex = delta.siblingIndex;
            if (emplaced.second)
            {
                order.push_back(delta.guid);
            }
        }

        // 第三步：按父 GUID 分组。遍历使用 order 的确定性顺序，即便数据
        // 异常也能得到可复现的组装结果；grouped 防御同一 GUID 重复挂载。
        std::unordered_map<Guid, std::vector<Guid>> childrenOf;
        std::vector<Guid> rootGuids;
        std::unordered_set<Guid> grouped;
        for (const Guid& guid : order)
        {
            const auto recordIt = records.find(guid);
            if (recordIt == records.end())
            {
                continue;
            }
            if (!grouped.insert(guid).second)
            {
                continue;
            }
            const Guid& parent = recordIt->second.parentGuid;
            if (!parent.Valid())
            {
                rootGuids.push_back(guid);
            }
            else if (records.find(parent) != records.end())
            {
                childrenOf[parent].push_back(guid);
            }
            else
            {
                // 正确的增量不会引用不存在的父实体，此处保底挂到根列表末尾。
                assert(false && "ApplySceneDiff: 增量引用了不存在的父实体");
                rootGuids.push_back(guid);
            }
        }

        // 第四步：组内按目标状态的同级序号排序。正常数据下序号连续且唯一，
        // 排序结果与目标状态完全一致；stable_sort 仅为异常数据保证确定性。
        const auto bySiblingIndex = [&records](const Guid& lhs, const Guid& rhs)
        {
            return records.at(lhs).siblingIndex < records.at(rhs).siblingIndex;
        };
        std::stable_sort(rootGuids.begin(), rootGuids.end(), bySiblingIndex);
        for (auto& group : childrenOf)
        {
            std::stable_sort(group.second.begin(), group.second.end(), bySiblingIndex);
        }

        // 第五步：自根开始递归组装实体树。
        result.entities.reserve(rootGuids.size());
        for (const Guid& rootGuid : rootGuids)
        {
            result.entities.push_back(buildSubtree(rootGuid, records, childrenOf));
        }
        return result;
    }
}
