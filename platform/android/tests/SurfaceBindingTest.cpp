#include "../player/surface_binding.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>

struct ANativeWindow {
    std::atomic<int> releases{0};
};

namespace {
using emberfall::android::SurfaceBinding;
using namespace std::chrono_literals;

void check(bool value,const char* problem) {
    if (value) return;
    std::fprintf(stderr,"%s\n",problem);
    std::abort();
}

auto binding_for(ANativeWindow& window) {
    return std::make_shared<SurfaceBinding>(&window,[](ANativeWindow* value) { ++value->releases; });
}

void waits_for_an_active_submission() {
    ANativeWindow window;
    auto binding=binding_for(window);
    std::promise<void> entered,finish;
    auto entered_future=entered.get_future(),finish_future=finish.get_future();
    auto worker=std::async(std::launch::async,[&] {
        binding->submit([&](ANativeWindow* output) {
            check(output==&window,"Wrong window was submitted");
            entered.set_value();
            finish_future.wait();
            check(window.releases==0,"Window was released during a submission");
        });
    });
    check(entered_future.wait_for(5s)==std::future_status::ready,"Render worker did not enter submission");
    auto deactivate=std::async(std::launch::async,[&] { binding->deactivate(); });
    check(deactivate.wait_for(5s)==std::future_status::ready,"Deactivation waited for an active submission");
    deactivate.get();
    check(!binding->available() && window.releases==0,"Deactivation must stop new draws without releasing the active window");
    auto destroy=std::async(std::launch::async,[&] { binding->invalidate(); });
    auto deadline=std::chrono::steady_clock::now()+5s;
    while (binding->available() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    check(!binding->available(),"Destruction did not disable future submissions");
    check(destroy.wait_for(0s)==std::future_status::timeout,"Destruction returned while a frame was in flight");
    finish.set_value();
    check(worker.wait_for(5s)==std::future_status::ready,"Frame did not finish");
    check(destroy.wait_for(5s)==std::future_status::ready,"Destruction did not finish after the frame");
    worker.get();
    destroy.get();
    binding.reset();
    check(window.releases==1,"Window reference was not released exactly once");
}

void stale_frame_preparation_cannot_reuse_a_destroyed_window() {
    ANativeWindow old_window,new_window;
    auto current=binding_for(old_window);
    std::promise<void> preparing,finish;
    auto preparing_future=preparing.get_future(),finish_future=finish.get_future();
    auto worker=std::async(std::launch::async,[&,old=current] {
        preparing.set_value();
        finish_future.wait();
        old->submit([](ANativeWindow*) { check(false,"Stale render touched a destroyed window"); });
    });
    check(preparing_future.wait_for(5s)==std::future_status::ready,"Frame preparation did not begin");
    auto destroy=std::async(std::launch::async,[old=current] { old->invalidate(); });
    check(destroy.wait_for(5s)==std::future_status::ready,"Destruction waited for frame preparation");
    destroy.get();
    check(old_window.releases==1,"Old window was not released before returning");
    current=binding_for(new_window);
    bool drew=false;
    current->submit([&](ANativeWindow* output) { drew=output==&new_window; });
    check(drew,"Replacement window could not be submitted");
    finish.set_value();
    check(worker.wait_for(5s)==std::future_status::ready,"Stale render did not finish");
    worker.get();
    check(old_window.releases==1,"Stale render released the old window again");
}
}

int main() {
    waits_for_an_active_submission();
    stale_frame_preparation_cannot_reuse_a_destroyed_window();
    std::puts("Surface binding lifecycle checks passed");
}
