#pragma once

#include <atomic>
#include <mutex>
#include <utility>

struct ANativeWindow;

namespace emberfall::android {
// The UI and game session own separate references. Only window operations
// share this lock, so destroying a Surface need not wait for the VM.
class SurfaceBinding {
public:
    using Release=void (*)(ANativeWindow*);

    SurfaceBinding(ANativeWindow* window,Release release) : window_(window),release_(release),live_(window!=nullptr) {}
    ~SurfaceBinding() { if (window_) release_(window_); }

    bool available() const { return live_.load(std::memory_order_acquire); }
    void deactivate() noexcept { live_.store(false,std::memory_order_release); }

    template<class Draw>
    void submit(Draw&& draw) {
        std::lock_guard lock(mutex_);
        if (available()) draw(window_);
    }

    void invalidate() {
        deactivate();
        ANativeWindow* old;
        {
            std::lock_guard lock(mutex_);
            old=std::exchange(window_,nullptr);
        }
        if (old) release_(old);
    }

private:
    std::mutex mutex_;
    ANativeWindow* window_;
    Release release_;
    std::atomic_bool live_;
};
}
