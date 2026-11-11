#pragma once

// Bounded multi-producer / multi-consumer FIFO built on a mutex and condition variable
// (spec §10.4: start with mutex/condvar queues; lock-free structures only with evidence).
// Producers never block: try_push() fails when the queue is full or closed. Consumers block in
// pop()/pop_batch() until an item arrives or the queue is closed and drained.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace enginex {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Appends unless full or closed. Never blocks.
    bool try_push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || items_.size() >= capacity_) {
                return false;
            }
            items_.push_back(std::move(value));
        }
        ready_.notify_one();
        return true;
    }

    // Appends even when full (one reserved control message, e.g. a final resync notice).
    // Fails only when closed.
    bool push_reserved(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            items_.push_back(std::move(value));
        }
        ready_.notify_one();
        return true;
    }

    // Blocks until an item is available; returns nullopt once closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

    // Waits up to `timeout` for at least one item, then moves up to `max` items into `out`
    // (appending). Returns the number of items moved (0 on timeout or closed-and-empty).
    std::size_t pop_batch(std::vector<T>& out, std::size_t max, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
        std::size_t moved = 0;
        while (moved < max && !items_.empty()) {
            out.push_back(std::move(items_.front()));
            items_.pop_front();
            ++moved;
        }
        return moved;
    }

    // Rejects future pushes and wakes all waiters. Items already queued can still be popped.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    // Drops queued items (used when a connection is torn down).
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }
    [[nodiscard]] bool closed_and_empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && items_.empty();
    }
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace enginex
