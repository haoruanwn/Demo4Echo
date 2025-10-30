// 线程安全队列的实现
// 提供：push, pop (阻塞), try_pop (非阻塞返回 optional), size, empty
// 以及用于与 poll/select 集成的 eventfd （get_event_fd）

#pragma once

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <sys/eventfd.h>
#include <unistd.h>

// 线程安全的队列，头文件实现（模板）
template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() : m_eventFd(-1), m_closed(false) {
        // 创建非阻塞的 eventfd，用于在 push 时通知 poll
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd >= 0) {
            m_eventFd = efd;
        }
    }

    ~ThreadSafeQueue() {
        // 标记关闭并唤醒等待者
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_closed = true;
        }
        m_cv.notify_all();
        if (m_eventFd >= 0) {
            close(m_eventFd);
            m_eventFd = -1;
        }
    }

    // 禁止拷贝
    ThreadSafeQueue(const ThreadSafeQueue &) = delete;
    ThreadSafeQueue &operator=(const ThreadSafeQueue &) = delete;

    // push (拷贝)
    void push(const T &value) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_queue.push_back(value);
        }
        notify();
        m_cv.notify_one();
    }

    // push (移动)
    void push(T &&value) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_queue.push_back(std::move(value));
        }
        notify();
        m_cv.notify_one();
    }

    // 阻塞 pop，直到有元素或队列被销毁（返回 optional：如果被销毁返回 nullopt）
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk, [this]() { return !m_queue.empty() || m_closed; });
        if (m_queue.empty()) {
            return std::nullopt;
        }
        T val = std::move(m_queue.front());
        m_queue.pop_front();
        return val;
    }

    // 非阻塞 try_pop，成功返回 value，否则返回 nullopt
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_queue.empty())
            return std::nullopt;
        T val = std::move(m_queue.front());
        m_queue.pop_front();
        return val;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_queue.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_queue.size();
    }

    // 返回 eventfd，用于在 worker thread 中将其与 poll/select 一起监听。
    // 如果创建失败，返回 -1。
    int get_event_fd() const { return m_eventFd; }

private:
    void notify() {
        if (m_eventFd < 0)
            return;
        uint64_t one = 1;
        // eventfd 使用非阻塞模式；如果缓冲区已满（理论上不太会出现），忽略错误
        ssize_t w = write(m_eventFd, &one, sizeof(one));
        (void) w; // 忽略写入结果；如果出错，poll 仍会触发（或在下一次写入）
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<T> m_queue;
    int m_eventFd;
    bool m_closed;
};
