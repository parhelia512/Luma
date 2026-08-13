#include "TransformSystem.h"
#include "../Resources/RuntimeAsset/RuntimeScene.h"
#include "../Components/Transform.h"
#include "../Components/ActivityComponent.h"
#include "../Components/RelationshipComponent.h"
#include "../Utils/Logger.h"

#include <cmath>

namespace Systems
{
    void TransformSystem::OnCreate(RuntimeScene* scene, EngineContext& context)
    {
    }

    void TransformSystem::OnUpdate(RuntimeScene* scene, float deltaTime, EngineContext& context)
    {
        auto& registry = scene->GetRegistry();


        auto rootView = registry.view<ECS::TransformComponent>(entt::exclude<ECS::ParentComponent>);


        for (auto entity : rootView)
        {
            // 直接查组件而不是构造 RuntimeGameObject（旧实现每实体每帧一次包装对象 + 组件查找，
            // 且对缺少 ActivityComponent 的实体是未定义行为）
            if (const auto* activity = registry.try_get<ECS::ActivityComponent>(entity);
                activity && !activity->isActive)
            {
                continue;
            }
            UpdateWorldTransform(entity, registry);
        }
    }

    namespace
    {
        /**
         * 2D 变换直接合成，替代旧实现的 glm::mat4 相乘 + glm::decompose + 四元数转欧拉角
         * （对 2D TRS 而言六次乘加即可，矩阵分解慢一个数量级且引入数值噪声）。
         *
         * 同时修复旧实现的正确性问题：子物体世界坐标此前用 localPosition + parentPosition
         * 直接相加，忽略了父级旋转与缩放（子物体不会绕旋转的父级公转），而旋转缩放又取自
         * 矩阵分解，两者互相矛盾。现在位置、旋转、缩放全部按同一套层级变换规则合成：
         *   world.rotation = parent.rotation + local.rotation
         *   world.scale    = parent.scale ⊙ local.scale
         *   world.position = parent.position + rotate(parent.rotation, parent.scale ⊙ local.position)
         */
        static void UpdateWorldTransformImpl(entt::entity entity, entt::registry& registry, int depth)
        {
            if (depth > 1024)
            {
                LogError("TransformSystem: recursion depth exceeded at entity {}. Possible cyclic hierarchy.",
                         static_cast<uint32_t>(entity));
                return;
            }
            if (!registry.valid(entity)) return;
            auto* transform = registry.try_get<ECS::TransformComponent>(entity);
            if (!transform) return;

            if (const auto* parentComponent = registry.try_get<ECS::ParentComponent>(entity))
            {
                const entt::entity parent = parentComponent->parent;
                if (parent == entity)
                {
                    LogWarn("TransformSystem: entity {} has itself as parent. Ignoring parent.",
                            static_cast<uint32_t>(entity));
                }
                else if (registry.valid(parent))
                {
                    if (const auto* parentTransform = registry.try_get<ECS::TransformComponent>(parent))
                    {
                        const float scaledLocalX = transform->localPosition.x * parentTransform->scale.x;
                        const float scaledLocalY = transform->localPosition.y * parentTransform->scale.y;
                        const float sinR = std::sin(parentTransform->rotation);
                        const float cosR = std::cos(parentTransform->rotation);
                        transform->position = {
                            parentTransform->position.x + scaledLocalX * cosR - scaledLocalY * sinR,
                            parentTransform->position.y + scaledLocalX * sinR + scaledLocalY * cosR
                        };
                        transform->rotation = parentTransform->rotation + transform->localRotation;
                        transform->scale = {
                            parentTransform->scale.x * transform->localScale.x,
                            parentTransform->scale.y * transform->localScale.y
                        };
                    }
                }
            }

            if (const auto* childrenComponent = registry.try_get<ECS::ChildrenComponent>(entity))
            {
                for (auto child : childrenComponent->children)
                {
                    if (child == entity)
                    {
                        LogWarn("TransformSystem: child equals parent for entity {}.",
                                static_cast<uint32_t>(entity));
                        continue;
                    }
                    if (!registry.valid(child)) continue;
                    UpdateWorldTransformImpl(child, registry, depth + 1);
                }
            }
        }
    }

    void TransformSystem::UpdateWorldTransform(entt::entity entity, entt::registry& registry)
    {
        UpdateWorldTransformImpl(entity, registry, 0);
    }
}
