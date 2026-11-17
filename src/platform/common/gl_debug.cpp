#include "gl_debug.hpp"

#include <tessera/tessera.h>

#include "core/log.hpp"
#include "gl_query.hpp"

namespace tessera {
namespace {

constexpr unsigned kDebugOutput = 0x92E0;
constexpr unsigned kDebugOutputSynchronous = 0x8242;

const char* sourceName(unsigned source) {
    switch (source) {
    case 0x8246: return "api";
    case 0x8247: return "window-system";
    case 0x8248: return "shader-compiler";
    case 0x8249: return "third-party";
    case 0x824A: return "application";
    default: return "other";
    }
}

const char* typeName(unsigned type) {
    switch (type) {
    case 0x824C: return "error";
    case 0x824D: return "deprecated";
    case 0x824E: return "undefined-behavior";
    case 0x824F: return "portability";
    case 0x8250: return "performance";
    case 0x8268: return "marker";
    case 0x8269: return "push-group";
    case 0x826A: return "pop-group";
    default: return "other";
    }
}

int levelFor(unsigned severity) {
    switch (severity) {
    case 0x9146: return TESSERA_LOG_ERROR;  // GL_DEBUG_SEVERITY_HIGH
    case 0x9147: return TESSERA_LOG_WARN;   // MEDIUM
    case 0x9148: return TESSERA_LOG_INFO;   // LOW
    case 0x826B: return TESSERA_LOG_DEBUG;  // NOTIFICATION
    default: return TESSERA_LOG_INFO;
    }
}

// Called by the driver, possibly deep inside a GL call. Must not throw, must not call back into GL.
void TESSERA_GLCALL debugTrampoline(unsigned source, unsigned type, unsigned id, unsigned severity, int length,
                                    const char* message, const void* /*userParam*/) {
    if (!message) return;
    // `length` excludes the terminator when non-negative; a negative length means NUL-terminated. Clamp either way.
    int n = 0;
    if (length >= 0) {
        n = length > 380 ? 380 : length;
    } else {
        while (n < 380 && message[n] != '\0') ++n;
    }
    logf(levelFor(severity), "GL debug [%s/%s #%u]: %.*s", sourceName(source), typeName(type), id, n, message);
}

} // namespace

bool installGlDebugForwarder(const std::function<void*(const char*)>& getProc) noexcept {
    try {
        using CallbackFn = void(TESSERA_GLCALL*)(void (TESSERA_GLCALL*)(unsigned, unsigned, unsigned, unsigned, int, const char*, const void*), const void*);
        using EnableFn = void(TESSERA_GLCALL*)(unsigned);
        using ErrorFn = unsigned(TESSERA_GLCALL*)();

        // Core name first, then the extension spellings.
        CallbackFn setCallback = reinterpret_cast<CallbackFn>(getProc("glDebugMessageCallback"));
        if (!setCallback) setCallback = reinterpret_cast<CallbackFn>(getProc("glDebugMessageCallbackKHR"));
        if (!setCallback) setCallback = reinterpret_cast<CallbackFn>(getProc("glDebugMessageCallbackARB"));
        const EnableFn enable = reinterpret_cast<EnableFn>(getProc("glEnable"));
        const ErrorFn getError = reinterpret_cast<ErrorFn>(getProc("glGetError"));
        if (!setCallback || !enable || !getError) return false;

        enable(kDebugOutput);
        enable(kDebugOutputSynchronous);
        setCallback(&debugTrampoline, nullptr);
        while (getError() != 0) {} // never leave an error behind in the application's queue
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace tessera
