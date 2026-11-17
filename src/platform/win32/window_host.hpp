#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tessera::win32 {

/*
 * One private thread that owns every hidden window Tessera creates, and pumps their messages.
 *
 * Why: Win32 only lets a window be destroyed by the thread that created it, and windows whose thread
 * never pumps messages can be flagged as unresponsive. Giving all windows to one dedicated, pumping
 * thread means contexts can be created and destroyed from ANY caller thread (including JVM threads that
 * come and go), and we never depend on, or interfere with, the host application's own message loop.
 *
 * The window class is private to this host: a unique name per module instance, registered when the host
 * starts and unregistered when it stops.
 */
class WindowHost {
  public:
    struct Window {
        HWND hwnd = nullptr;
        HDC dc = nullptr; // CS_OWNDC: valid for the lifetime of the window, usable from any thread
    };

    WindowHost() = default;
    ~WindowHost() { stop(); }
    WindowHost(const WindowHost&) = delete;
    WindowHost& operator=(const WindowHost&) = delete;

    bool start(std::string& error);
    void stop() noexcept;

    /* Creates a hidden, never-shown, never-activated 1x1 popup window on the host thread. */
    bool createWindow(Window& out, std::string& error);
    /* Safe to call with an empty Window, and safe after stop(). */
    void destroyWindow(Window& window) noexcept;

    /* Runs `fn` on the host thread and waits. False if the host is not running. Rethrows fn's exception. */
    bool invoke(const std::function<void()>& fn);

  private:
    struct Task {
        std::function<void()> fn;
        bool done = false;
        bool ran = false;
        std::exception_ptr error;
    };

    void threadMain();
    void drain();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Task>> queue_;
    std::thread thread_;
    HANDLE wakeEvent_ = nullptr;
    bool running_ = false;
    bool ready_ = false;
    bool stopRequested_ = false;

    std::wstring className_;
    ATOM atom_ = 0;
    HINSTANCE hinstance_ = nullptr;
};

} // namespace tessera::win32
