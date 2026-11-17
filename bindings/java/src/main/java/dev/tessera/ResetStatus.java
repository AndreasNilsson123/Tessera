package dev.tessera;

/** Outcome of polling a robust context ({@code glGetGraphicsResetStatus}). Anything but {@link #NONE} means the context is lost. */
public enum ResetStatus {
    NONE,
    /** This context caused the reset. */
    GUILTY,
    /** Another context caused it, but this one was affected. */
    INNOCENT,
    UNKNOWN;

    static ResetStatus of(int nativeValue) {
        switch (nativeValue) {
            case 0:
                return NONE;
            case 1:
                return GUILTY;
            case 2:
                return INNOCENT;
            default:
                return UNKNOWN;
        }
    }
}
