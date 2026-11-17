# ADR 0004: One private window-host thread (Windows)
Status: accepted (Phase 2)

Win32 allows a window to be destroyed only by the thread that created it, and a window whose thread never pumps messages can be treated as unresponsive. Creating windows on the caller's thread would force contexts to be destroyed on their creating thread, which does not fit JVM-managed threads that come and go, and would tie us to the host application's message loop.

Decision: Tessera runs a single internal thread that creates, pumps and destroys every hidden window (`src/platform/win32/window_host.*`). Windows are 1x1, never shown, never activated, with a private `CS_OWNDC` class whose name is unique per module instance. Callers on any thread marshal window work to it and wait. The GL context itself is not bound to that thread: it is made current on whichever thread the caller chooses, using the window's DC (valid process-wide because of `CS_OWNDC`).

Consequences: a thread is started on first context creation and joined (class unregistered) when the last context is gone and `tesseraTerminate` releases the backend. A caller that never terminates keeps one idle thread. Context creation pays a cross-thread round trip.
