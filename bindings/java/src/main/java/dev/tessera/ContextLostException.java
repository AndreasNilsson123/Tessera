package dev.tessera;

/**
 * The GPU context was lost (a reset, or the device went away). The context stays lost: create a replacement with
 * {@link Context#recreate()}.
 */
public class ContextLostException extends TesseraException {
    private static final long serialVersionUID = 1L;

    public ContextLostException(String message) {
        super(Result.CONTEXT_LOST, message);
    }
}
