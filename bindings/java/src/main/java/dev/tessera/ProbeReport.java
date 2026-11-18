package dev.tessera;

/**
 * The answer to "would this request work here, and what would I get?" (see {@link Tessera#probe(ContextConfig)}). A probe
 * really creates the context, reads it back and destroys it again, so the answer is what {@link Context#create} would do.
 */
public final class ProbeReport {
    private final Result result;
    private final ContextInfo info;
    private final String backend;
    private final String message;

    ProbeReport(Result result, ContextInfo info, String backend, String message) {
        this.result = result;
        this.info = info;
        this.backend = backend;
        this.message = message;
    }

    /** What {@link Context#create} would fail with, or {@link Result#SUCCESS}. */
    public Result result() {
        return result;
    }

    public boolean succeeded() {
        return result == Result.SUCCESS;
    }

    /** What the driver would give; {@code null} unless {@link #succeeded()}. */
    public ContextInfo info() {
        return info;
    }

    /** The backend that answered ({@code "wgl"}, {@code "glx"}, {@code "egl"}, {@code "cgl"}) or the last one that was asked. */
    public String backend() {
        return backend;
    }

    /** Why it failed; empty on success. */
    public String message() {
        return message;
    }

    @Override
    public String toString() {
        return succeeded() ? "ProbeReport{" + backend + ": " + info + "}" : "ProbeReport{" + result + " via " + backend + ": " + message + "}";
    }
}
