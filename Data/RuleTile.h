#ifndef LUMAENGINE_RULETILE_H
#define LUMAENGINE_RULETILE_H


#pragma once

#include "Tile.h"
#include <array>
#include <vector>
#include <cstdint>

/**
 * @brief 定义了邻居瓦片的规则类型。
 */
enum class NeighborRule {
    DontCare,       ///< 不关心邻居瓦片的状态。
    MustBeThis,     ///< 邻居瓦片必须是指定的类型。
    MustNotBeThis   ///< 邻居瓦片不能是指定的类型。
};

/**
 * @brief 规则匹配后的一个候选结果瓦片及其随机权重。
 */
struct RuleTileOutput
{
    AssetHandle tileHandle; ///< 候选结果瓦片的资源句柄。
    float weight = 1.0f;    ///< 加权随机取样的权重，非正值不参与取样。
};

/**
 * @brief 定义了一个瓦片规则，包含满足规则时的候选结果列表和周围邻居的规则。
 */
struct Rule
{
    std::vector<RuleTileOutput> outputs;    ///< 满足规则时的候选结果列表（旧版单一 resultTileHandle 反序列化为单元素权重 1）。
    std::array<NeighborRule, 8> neighbors;  ///< 定义了周围8个邻居瓦片的规则。
};

/**
 * @brief 按格子坐标从候选列表中确定性加权取样，保证同一格子每次求值结果一致。
 */
inline AssetHandle PickRuleTileOutput(const std::vector<RuleTileOutput>& outputs, int x, int y)
{
    if (outputs.empty()) return AssetHandle();
    if (outputs.size() == 1) return outputs[0].tileHandle;
    float totalWeight = 0.0f;
    for (const auto& output : outputs)
    {
        if (output.weight > 0.0f) totalWeight += output.weight;
    }
    if (totalWeight <= 0.0f) return outputs[0].tileHandle;
    // 坐标哈希后再做一次雪崩混合，避免相邻格子的取样结果出现线性条纹
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    float sample = (static_cast<float>(h & 0xFFFFFFu) / 16777216.0f) * totalWeight;
    for (const auto& output : outputs)
    {
        if (output.weight <= 0.0f) continue;
        sample -= output.weight;
        if (sample < 0.0f) return output.tileHandle;
    }
    return outputs.back().tileHandle;
}

/**
 * @brief 规则瓦片资产的数据结构，用于存储默认瓦片和一系列规则。
 */
struct RuleTileAssetData : Data::IData<RuleTileAssetData>
{
    AssetHandle defaultTileHandle;  ///< 默认瓦片的资源句柄。
    std::vector<Rule> rules;        ///< 瓦片规则的集合。
};


namespace YAML
{
    /**
     * @brief YAML::Node 到 NeighborRule 枚举类型的转换特化。
     */
    template <>
    struct convert<NeighborRule>
    {
        /**
         * @brief 将 NeighborRule 枚举编码为 YAML::Node。
         * @param rule 要编码的 NeighborRule 枚举值。
         * @return 编码后的 YAML::Node。
         */
        static Node encode(const NeighborRule& rule)
        {
            Node node;
            node = static_cast<int>(rule);
            return node;
        }

        /**
         * @brief 将 YAML::Node 解码为 NeighborRule 枚举。
         * @param node 要解码的 YAML::Node。
         * @param rule 解码后的 NeighborRule 枚举值。
         * @return 如果解码成功则返回 true，否则返回 false。
         */
        static bool decode(const Node& node, NeighborRule& rule)
        {
            if (!node.IsScalar())
                return false;
            int value = node.as<int>();
            if (value < 0 || value > 2)
                return false;
            rule = static_cast<NeighborRule>(value);
            return true;
        }
    };

    /**
     * @brief YAML::Node 到 RuleTileOutput 结构体的转换特化。
     */
    template <>
    struct convert<RuleTileOutput>
    {
        /**
         * @brief 将 RuleTileOutput 结构体编码为 YAML::Node。
         * @param output 要编码的 RuleTileOutput 结构体。
         * @return 编码后的 YAML::Node。
         */
        static Node encode(const RuleTileOutput& output)
        {
            Node node;
            node["tileHandle"] = output.tileHandle;
            node["weight"] = output.weight;
            return node;
        }

        /**
         * @brief 将 YAML::Node 解码为 RuleTileOutput 结构体。
         * @param node 要解码的 YAML::Node。
         * @param output 解码后的 RuleTileOutput 结构体。
         * @return 如果解码成功则返回 true，否则返回 false。
         */
        static bool decode(const Node& node, RuleTileOutput& output)
        {
            if (!node.IsMap() || !node["tileHandle"])
                return false;
            output.tileHandle = node["tileHandle"].as<AssetHandle>();
            output.weight = node["weight"] ? node["weight"].as<float>() : 1.0f;
            return true;
        }
    };

    /**
     * @brief YAML::Node 到 Rule 结构体的转换特化。
     */
    template <>
    struct convert<Rule>
    {
        /**
         * @brief 将 Rule 结构体编码为 YAML::Node。
         * @param rule 要编码的 Rule 结构体。
         * @return 编码后的 YAML::Node。
         */
        static Node encode(const Rule& rule)
        {
            Node node;
            node["outputs"] = rule.outputs;
            node["neighbors"] = rule.neighbors;
            return node;
        }

        /**
         * @brief 将 YAML::Node 解码为 Rule 结构体。
         * @param node 要解码的 YAML::Node。
         * @param rule 解码后的 Rule 结构体。
         * @return 如果解码成功则返回 true，否则返回 false。
         */
        static bool decode(const Node& node, Rule& rule)
        {
            if (!node.IsMap())
                return false;

            if (!node["neighbors"])
                return false;

            rule.outputs.clear();
            if (node["outputs"])
            {
                rule.outputs = node["outputs"].as<std::vector<RuleTileOutput>>();
            }
            else if (node["resultTileHandle"])
            {
                // 兼容旧格式：单一结果句柄读入为单元素、权重 1 的列表
                RuleTileOutput output;
                output.tileHandle = node["resultTileHandle"].as<AssetHandle>();
                output.weight = 1.0f;
                rule.outputs.push_back(output);
            }
            else
            {
                return false;
            }
            rule.neighbors = node["neighbors"].as<std::array<NeighborRule, 8>>();
            return true;
        }
    };

    /**
     * @brief YAML::Node 到 RuleTileAssetData 结构体的转换特化。
     */
    template <>
    struct convert<RuleTileAssetData>
    {
        /**
         * @brief 将 RuleTileAssetData 结构体编码为 YAML::Node。
         * @param rhs 要编码的 RuleTileAssetData 结构体。
         * @return 编码后的 YAML::Node。
         */
        static Node encode(const RuleTileAssetData& rhs)
        {
            Node node;
            node["defaultTileHandle"] = rhs.defaultTileHandle;
            node["rules"] = rhs.rules;
            return node;
        }

        /**
         * @brief 将 YAML::Node 解码为 RuleTileAssetData 结构体。
         * @param node 要解码的 YAML::Node。
         * @param rhs 解码后的 RuleTileAssetData 结构体。
         * @return 如果解码成功则返回 true，否则返回 false。
         */
        static bool decode(const Node& node, RuleTileAssetData& rhs)
        {
            if (!node.IsMap())
                return false;

            if (node["defaultTileHandle"])
                rhs.defaultTileHandle = node["defaultTileHandle"].as<AssetHandle>();
            if (node["rules"])
                rhs.rules = node["rules"].as<std::vector<Rule>>();
            return true;
        }
    };
}
#endif