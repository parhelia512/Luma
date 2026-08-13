#ifndef LUMAEVENT_H
#define LUMAEVENT_H

#include <functional>
#include <memory>
#include <vector>
#include <cstdint>
#include <mutex>
#include <utility>

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
 * 线程安全（写时复制）：监听器表以 `shared_ptr<const vector>` 不可变快照存储，
 * 增删监听器时在锁内复制新表原子替换；Invoke 只在锁内取一次快照引用（无表拷贝、
 * 无堆分配），随后在锁外调用，允许回调中安全地增删监听器（对本次分发不生效）。
 * 高频路径（每帧多次 Invoke）零分配。
 *
 * @tparam Args 事件参数的类型列表。
 */
template <typename... Args>
class LumaEvent
{
public:
    /// 事件监听器的函数类型。
    using Listener = std::function<void(Args...)>;

private:
    /// 监听器表条目：ID + 回调。
    using ListenerEntry = std::pair<uint64_t, Listener>;
    /// 不可变监听器表快照类型。
    using ListenerList = std::vector<ListenerEntry>;

public:
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
        auto newList = m_listeners
                           ? std::make_shared<ListenerList>(*m_listeners)
                           : std::make_shared<ListenerList>();
        newList->emplace_back(id, std::move(listener));
        m_listeners = std::move(newList);
        return ListenerHandle{id};
    }

    /**
     * @brief 移除一个事件监听器。
     * @param handle 要移除的监听器的句柄。
     * @return 如果成功移除监听器，则返回true；否则返回false。
     */
    bool RemoveListener(ListenerHandle handle)
    {
        if (!handle.IsValid())
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_listeners)
        {
            return false;
        }
        auto newList = std::make_shared<ListenerList>();
        newList->reserve(m_listeners->size());
        bool removed = false;
        for (const auto& entry : *m_listeners)
        {
            if (entry.first == handle.id)
            {
                removed = true;
                continue;
            }
            newList->push_back(entry);
        }
        if (removed)
        {
            m_listeners = std::move(newList);
        }
        return removed;
    }

    /**
     * @brief 触发所有已注册的监听器。
     *
     * 仅在锁内取一次不可变快照引用（零拷贝零分配），锁外依序调用。
     * @param args 传递给监听器函数的参数。
     */
    void Invoke(Args... args) const
    {
        std::shared_ptr<const ListenerList> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_listeners;
        }
        if (!snapshot)
        {
            return;
        }
        for (const auto& entry : *snapshot)
        {
            if (entry.second)
            {
                entry.second(args...);
            }
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
        m_listeners.reset();
    }

    /**
     * @brief 检查事件是否没有任何监听器。
     * @return 如果没有监听器，则返回true；否则返回false。
     */
    bool IsEmpty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_listeners || m_listeners->empty();
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
    mutable std::mutex m_mutex; ///< 保护快照指针与ID计数器的互斥锁。
    std::shared_ptr<const ListenerList> m_listeners; ///< 不可变监听器表快照（写时复制）。
    uint64_t m_nextListenerId = 1; ///< 下一个可用的监听器ID。
};

#endif
