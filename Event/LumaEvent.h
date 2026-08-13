#ifndef LUMAEVENT_H
#define LUMAEVENT_H

#include <functional>
#include <vector>
#include <cstdint>
#include <map>
#include <mutex>

/**
 * @brief 监听器句柄，用于唯一标识一个事件监听器。
 */
struct ListenerHandle
{
    uint64_t id = 0; ///< 监听器的唯一标识符。

    /**
     * @brief 检查句柄是否有效。
     * @return 如果句柄的ID不为0，则返回true；否则返回false。
     */
    bool IsValid() const { return id != 0; }
};

/**
 * @brief 一个通用的事件分发器，支持添加、移除和触发监听器。
 *
 * 线程安全：内部使用互斥锁保护监听器表；Invoke 时先在锁内拷贝监听器快照，
 * 再在锁外调用，允许监听器在回调中安全地增删监听器（对本次分发不生效）。
 *
 * @tparam Args 事件参数的类型列表。
 */
template <typename... Args>
class LumaEvent
{
public:
    /// 事件监听器的函数类型。
    using Listener = std::function<void(Args...)>;

    LumaEvent() = default;

    LumaEvent(const LumaEvent& other)
    {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        m_listeners = other.m_listeners;
        m_nextListenerId = other.m_nextListenerId;
    }

    LumaEvent& operator=(const LumaEvent& other)
    {
        if (this == &other) return *this;
        // 按固定顺序锁定双方，避免死锁
        std::scoped_lock lock(m_mutex, other.m_mutex);
        m_listeners = other.m_listeners;
        m_nextListenerId = other.m_nextListenerId;
        return *this;
    }

    LumaEvent(LumaEvent&& other) noexcept
    {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        m_listeners = std::move(other.m_listeners);
        m_nextListenerId = other.m_nextListenerId;
    }

    LumaEvent& operator=(LumaEvent&& other) noexcept
    {
        if (this == &other) return *this;
        std::scoped_lock lock(m_mutex, other.m_mutex);
        m_listeners = std::move(other.m_listeners);
        m_nextListenerId = other.m_nextListenerId;
        return *this;
    }

    /**
     * @brief 添加一个事件监听器。
     * @param listener 要添加的监听器函数。
     * @return 一个ListenerHandle，用于标识新添加的监听器。
     */
    ListenerHandle AddListener(Listener&& listener)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint64_t id = m_nextListenerId++;
        m_listeners[id] = std::move(listener);
        return ListenerHandle{id};
    }

    /**
     * @brief 移除一个事件监听器。
     * @param handle 要移除的监听器的句柄。
     * @return 如果成功移除监听器，则返回true；否则返回false。
     */
    bool RemoveListener(ListenerHandle handle)
    {
        if (handle.IsValid())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_listeners.erase(handle.id) > 0;
        }
        return false;
    }

    /**
     * @brief 触发所有已注册的监听器。
     *
     * 先拷贝监听器快照再在锁外调用，避免回调中增删监听器或跨线程分发导致的数据竞争。
     * @param args 传递给监听器函数的参数。
     */
    void Invoke(Args... args) const
    {
        std::vector<Listener> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot.reserve(m_listeners.size());
            for (const auto& pair : m_listeners)
            {
                if (pair.second)
                {
                    snapshot.push_back(pair.second);
                }
            }
        }
        for (const auto& listener : snapshot)
        {
            listener(args...);
        }
    }

    /**
     * @brief 重载函数调用运算符，用于触发所有已注册的监听器。
     * @param args 传递给监听器函数的参数。
     */
    void operator()(Args... args) const
    {
        Invoke(args...);
    }

    /**
     * @brief 清除所有已注册的监听器。
     */
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_listeners.clear();
    }

    /**
     * @brief 检查事件是否没有任何监听器。
     * @return 如果没有监听器，则返回true；否则返回false。
     */
    bool IsEmpty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_listeners.empty();
    }

    /**
     * @brief 重载逻辑非运算符，检查事件是否为空。
     * @return 如果事件为空（没有监听器），则返回true。
     */
    bool operator!() const
    {
        return IsEmpty();
    }

    /**
     * @brief 重载布尔类型转换运算符，检查事件是否非空。
     * @return 如果事件非空（有监听器），则返回true。
     */
    explicit operator bool() const
    {
        return !IsEmpty();
    }

    /**
     * @brief 重载+=运算符，用于添加监听器。
     * @param listener 要添加的监听器函数。
     * @return 一个ListenerHandle，用于标识新添加的监听器。
     */
    ListenerHandle operator+=(Listener&& listener)
    {
        return AddListener(std::move(listener));
    }

    /**
     * @brief 重载+=运算符，用于添加监听器（左值引用版本）。
     * @param listener 要添加的监听器函数。
     * @return 一个ListenerHandle，用于标识新添加的监听器。
     */
    ListenerHandle operator+=(const Listener& listener)
    {
        Listener copy = listener;
        return AddListener(std::move(copy));
    }

    /**
     * @brief 重载-=运算符，用于移除监听器。
     * @param handle 要移除的监听器的句柄。
     * @return 对当前事件对象的引用。
     */
    LumaEvent& operator-=(ListenerHandle handle)
    {
        RemoveListener(handle);
        return *this;
    }

private:
    mutable std::mutex m_mutex; ///< 保护监听器表的互斥锁。
    std::map<uint64_t, Listener> m_listeners; ///< 存储所有监听器的映射，键为监听器ID，值为监听器函数。
    uint64_t m_nextListenerId = 1; ///< 下一个可用的监听器ID。
};

#endif
