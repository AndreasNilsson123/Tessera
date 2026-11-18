#include "window_host.hpp"

#include <atomic>
#include <cstdio>

#include "wgl_api.hpp"

namespace tessera::win32 {
namespace {
// Any address inside this module works to find our HMODULE.
void moduleAnchor() {}
} // namespace

bool WindowHost::start(std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (running_)
        return true;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) {
        error = describeWin32Error("GetModuleHandleEx failed", GetLastError());
        return false;
    }
    hinstance_ = module;

    static std::atomic<unsigned> counter{0};
    wchar_t name[96];
    std::swprintf(name, sizeof name / sizeof name[0], L"TesseraHost_%p_%u", static_cast<void*>(module),
                  counter.fetch_add(1));
    className_ = name;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hinstance_;
    wc.lpszClassName = className_.c_str();
    atom_ = RegisterClassExW(&wc);
    if (!atom_) {
        error = describeWin32Error("RegisterClassEx failed", GetLastError());
        return false;
    }

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) {
        error = describeWin32Error("CreateEvent failed", GetLastError());
        UnregisterClassW(className_.c_str(), hinstance_);
        atom_ = 0;
        return false;
    }

    stopRequested_ = false;
    ready_ = false;
    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::exception& e) {
        error = std::string("could not start window host thread: ") + e.what();
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
        UnregisterClassW(className_.c_str(), hinstance_);
        atom_ = 0;
        return false;
    }
    cv_.wait(lock, [this] { return ready_; });
    running_ = true;
    return true;
}

void WindowHost::threadMain() {
    // Force creation of this thread's message queue before declaring ourselves ready.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
    }
    cv_.notify_all();

    for (;;) {
        drain();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_ && queue_.empty())
                break;
        }
        MsgWaitForMultipleObjects(1, &wakeEvent_, FALSE, INFINITE, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Reject anything that raced in after the last drain.
    std::deque<std::shared_ptr<Task>> leftover;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        leftover.swap(queue_);
        for (auto& t : leftover)
            t->done = true;
    }
    cv_.notify_all();
}

void WindowHost::drain() {
    for (;;) {
        std::shared_ptr<Task> task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty())
                return;
            task = queue_.front();
            queue_.pop_front();
        }
        try {
            task->fn();
        } catch (...) {
            task->error = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task->ran = true;
            task->done = true;
        }
        cv_.notify_all();
    }
}

bool WindowHost::invoke(const std::function<void()>& fn) {
    auto task = std::make_shared<Task>();
    task->fn = fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopRequested_)
            return false;
        queue_.push_back(task);
    }
    SetEvent(wakeEvent_);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return task->done; });
    if (task->error)
        std::rethrow_exception(task->error);
    return task->ran;
}

void WindowHost::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable())
            return;
        stopRequested_ = true;
        worker = std::move(thread_);
    }
    if (wakeEvent_)
        SetEvent(wakeEvent_);
    worker.join();
    if (wakeEvent_)
        CloseHandle(wakeEvent_);
    wakeEvent_ = nullptr;
    if (atom_)
        UnregisterClassW(className_.c_str(), hinstance_);
    atom_ = 0;
}

bool WindowHost::createWindow(Window& out, std::string& error) {
    out = Window{};
    Window result;
    std::string failure;
    const bool ran = invoke([&] {
        result.hwnd =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className_.c_str(), L"Tessera",
                            WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, hinstance_, nullptr);
        if (!result.hwnd) {
            failure = describeWin32Error("CreateWindowEx failed", GetLastError());
            return;
        }
        result.dc = GetDC(result.hwnd);
        if (!result.dc) {
            failure = describeWin32Error("GetDC failed", GetLastError());
            DestroyWindow(result.hwnd);
            result.hwnd = nullptr;
        }
    });
    if (!ran) {
        error = "window host thread is not running";
        return false;
    }
    if (!result.hwnd) {
        error = failure;
        return false;
    }
    out = result;
    return true;
}

void WindowHost::destroyWindow(Window& window) noexcept {
    if (!window.hwnd)
        return;
    const Window w = window;
    window = Window{};
    try {
        invoke([w] {
            if (w.dc)
                ReleaseDC(w.hwnd, w.dc);
            DestroyWindow(w.hwnd);
        });
    } catch (...) {
    }
    // If the host is already stopped its thread is gone; the OS destroys the window with its thread.
}

} // namespace tessera::win32
