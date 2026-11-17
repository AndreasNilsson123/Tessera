# ADR 0002: Java binding strategy
Status: proposed - blocked on the minimum JDK/LWJGL decision (ROADMAP open questions)

Primary: FFM/Panama (JDK 22+) with jextract-generated bindings checked in. Fallback: a thin JNI shim over the same header. Both bind the C ABI only. LWJGL integration goes through an exposed `getProcAddress` (Phase 7). `bindings/java` is a placeholder until this is accepted.
