#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

/**
 * @brief Thread-safe FIFO queue for inter-thread audio buffer communication.
 *
 * Used between:
 *   Capture Thread  →  CaptureQueue  →  DSP Thread
 *   DSP Thread      →  OutputQueue   →  Output Thread
 */
template<typename T>
class ThreadSafeQueue
{
public:
    explicit ThreadSafeQueue(size_t maxSize = 32)
        : m_maxSize(maxSize)
    {}

    // Push an item. Blocks if the queue is full (back-pressure).
    void Push(T item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_queue.size() < m_maxSize || m_shutdown; });
        if (m_shutdown) return;
        m_queue.push(std::move(item));
        m_notEmpty.notify_one();
    }

    // Try to push without blocking. Returns false if full.
    bool TryPush(T item)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.size() >= m_maxSize) return false;
        m_queue.push(std::move(item));
        m_notEmpty.notify_one();
        return true;
    }

    // Pop an item. Blocks until an item is available or shutdown.
    std::optional<T> Pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return !m_queue.empty() || m_shutdown; });
        if (m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());
        m_queue.pop();
        m_notFull.notify_one();
        return item;
    }

    // Pop with timeout. Returns nullopt on timeout or shutdown.
    std::optional<T> PopTimeout(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_notEmpty.wait_for(lock, timeout, [this] { return !m_queue.empty() || m_shutdown; }))
            return std::nullopt;
        if (m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());
        m_queue.pop();
        m_notFull.notify_one();
        return item;
    }

    // Signal all waiting threads to exit.
    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = true;
        }
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) m_queue.pop();
        m_shutdown = false;
    }

private:
    mutable std::mutex          m_mutex;
    std::condition_variable     m_notEmpty;
    std::condition_variable     m_notFull;
    std::queue<T>               m_queue;
    size_t                      m_maxSize;
    bool                        m_shutdown = false;
};
