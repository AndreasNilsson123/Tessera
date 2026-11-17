# ADR 0002: Java binding strategy
Status: superseded by ADR 0009 (JNI baseline for Java 11+, FFM on 22+, hand-written instead of jextract).

Primary: FFM/Panama (JDK 22+) with jextract-generated bindings checked in. Fallback: a thin JNI shim over the same header. Both bind the C ABI only. LWJGL integration goes through an exposed `getProcAddress` (Phase 7). `bindings/java` is a placeholder until this is accepted.
