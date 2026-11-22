// A reentrant lock with a condition, as Python's threading.RLock and threading.Condition(rlock) behave: a wait
// gives the lock up entirely, however often its holder took it, and takes it back as often.
#ifndef KERFKITE_SERVER_RLOCK_HPP
#define KERFKITE_SERVER_RLOCK_HPP

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kerfkite {
namespace server {

class RLock {
public:
    void lock() {
        std::unique_lock<std::mutex> guard(mutex_);
        const std::thread::id me = std::this_thread::get_id();
        if (count_ > 0 && owner_ == me) {
            ++count_;
            return;
        }
        free_.wait(guard, [&] { return count_ == 0; });
        owner_ = me;
        count_ = 1;
    }

    void unlock() {
        std::unique_lock<std::mutex> guard(mutex_);
        if (--count_ == 0) {
            owner_ = std::thread::id();
            free_.notify_one();
        }
    }

    // Condition.notify_all on the lock's condition; the caller holds the lock.
    void notify_all() { changed_.notify_all(); }

    // Condition.wait_for(predicate, timeout): the lock is given up while waiting; true when predicate holds.
    // The caller holds the lock.
    template <class Predicate>
    bool wait_for(Predicate predicate, double seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        std::unique_lock<std::mutex> guard(mutex_);
        const int held = count_;
        for (;;) {
            // predicate runs with the lock held, as in Python.
            guard.unlock();
            const bool done = predicate();
            guard.lock();
            if (done) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            count_ = 0;
            owner_ = std::thread::id();
            free_.notify_one();
            changed_.wait_until(guard, deadline);
            free_.wait(guard, [&] { return count_ == 0; });
            owner_ = std::this_thread::get_id();
            count_ = held;
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable free_, changed_;
    std::thread::id owner_;
    int count_ = 0;
};

using Hold = std::lock_guard<RLock>;

}  // namespace server
}  // namespace kerfkite

#endif
