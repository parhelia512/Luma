#ifndef EVENTBUS_H
#define EVENTBUS_H

#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>

#include "LazySingleton.h"
#include "LumaEvent.h"

/**
 * @brief 事件总线，用于发布和订阅不同类型的事件。
 *
 * 这是一个单例模式的事件总线，允许组件之间通过事件进行解耦通信。
 * 任何类型的事件都可以被发布，并且可以有多个监听器订阅特定类型的事件。
 *
 * 线程安全：内部表由互斥锁保护，事件对象以 shared_ptr 稳定存储（哈希表扩容不
 * 影响正在分发的事件对象）；实际分发在锁外进行，由 LumaEvent 自身的锁保证安全。
 */
class LUMA_API EventBus : public LazySingleton<EventBus>
{
public:
    friend class LazySingleton<EventBus>;

    /**
     * @brief 订阅一个特定类型的事件。
     *
     * 当指定类型的事件被发布时，提供的监听器将被调用。
     *
     * @tparam TEvent 事件类型。
     * @param listener 事件监听器，一个可调用对象，接受 `const TEvent&` 作为参数。
     * @return ListenerHandle 一个句柄，用于后续取消订阅。如果订阅失败，返回一个无效句柄。
     */
    template <typename TEvent>
    ListenerHandle Subscribe(typename LumaEvent<const TEvent&>::Listener&& listener);

    /**
     * @brief 取消订阅一个事件。
     *
     * 使用之前 `Subscribe` 方法返回的句柄来取消订阅。
     *
     * @param handle 要取消订阅的监听器句柄。
     */
    void Unsubscribe(ListenerHandle handle);

    /**
     * @brief 发布一个特定类型的事件。
     *
     * 所有订阅了该类型事件的监听器都将被调用。监听器在调用方线程上同步执行。
     *
     * @tparam TEvent 事件类型。
     * @param event 要发布的事件对象。
     */
    template <typename TEvent>
    void Publish(const TEvent& event);

    /**
     * @brief 清除所有已注册的事件和监听器。
     *
     * 调用此方法后，所有事件订阅都将失效。
     */
    void Clear();

private:
    EventBus() = default;
    ~EventBus() override = default;

    template <typename TEvent>
    std::shared_ptr<LumaEvent<const TEvent&>> getEventPtr();

    mutable std::mutex m_mutex; ///< 保护下方两个表的互斥锁。
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_events; ///< 各事件类型对应的事件对象（地址稳定）。
    std::unordered_map<uint64_t, std::function<void()>> m_unsubscribers; ///< 取消订阅回调，按全局句柄ID索引。
    uint64_t m_nextHandleId = 1; ///< 下一个可用的全局监听器句柄ID。
};


template <typename TEvent>
std::shared_ptr<LumaEvent<const TEvent&>> EventBus::getEventPtr()
{
    const auto typeId = std::type_index(typeid(TEvent));
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_events.find(typeId);
    if (it == m_events.end())
    {
        auto created = std::make_shared<LumaEvent<const TEvent&>>();
        m_events[typeId] = created;
        return created;
    }
    return std::static_pointer_cast<LumaEvent<const TEvent&>>(it->second);
}

template <typename TEvent>
ListenerHandle EventBus::Subscribe(typename LumaEvent<const TEvent&>::Listener&& listener)
{
    auto eventPtr = getEventPtr<TEvent>();

    const ListenerHandle localHandle = eventPtr->AddListener(std::move(listener));
    if (!localHandle.IsValid())
    {
        return {};
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t globalId = m_nextHandleId++;

    // 捕获事件对象的弱引用，Clear() 后取消订阅调用可安全地成为空操作
    std::weak_ptr<LumaEvent<const TEvent&>> weakEvent = eventPtr;
    m_unsubscribers[globalId] = [weakEvent, localHandle]
    {
        if (auto strongEvent = weakEvent.lock())
        {
            strongEvent->RemoveListener(localHandle);
        }
    };

    return ListenerHandle{globalId};
}


inline void EventBus::Unsubscribe(ListenerHandle handle)
{
    if (!handle.IsValid())
    {
        return;
    }

    std::function<void()> unsubscriber;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_unsubscribers.find(handle.id);
        if (it == m_unsubscribers.end())
        {
            return;
        }
        unsubscriber = std::move(it->second);
        m_unsubscribers.erase(it);
    }
    // 在锁外执行，避免与订阅/分发路径互锁
    unsubscriber();
}

template <typename TEvent>
void EventBus::Publish(const TEvent& event)
{
    std::shared_ptr<LumaEvent<const TEvent&>> eventPtr;
    {
        const auto typeId = std::type_index(typeid(TEvent));
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_events.find(typeId);
        if (it == m_events.end())
        {
            return;
        }
        eventPtr = std::static_pointer_cast<LumaEvent<const TEvent&>>(it->second);
    }
    // 锁外分发；shared_ptr 保证 Clear() 并发时事件对象仍然存活
    eventPtr->Invoke(event);
}

inline void EventBus::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.clear();
    m_unsubscribers.clear();
    m_nextHandleId = 1;
}

#endif
