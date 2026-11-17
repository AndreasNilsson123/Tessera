package dev.tessera;

/**
 * Receives the native library's log messages (it never writes to stdout/stderr itself). May be called on any thread,
 * including ones the JVM did not start; it must not throw and should return quickly.
 */
@FunctionalInterface
public interface Logger {
    enum Level {
        ERROR, WARN, INFO, DEBUG;

        static Level of(int nativeLevel) {
            switch (nativeLevel) {
                case 1:
                    return ERROR;
                case 2:
                    return WARN;
                case 4:
                    return DEBUG;
                default:
                    return INFO;
            }
        }
    }

    void log(Level level, String message);
}
