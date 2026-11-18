package dev.tessera;

import java.util.Objects;

/**
 * What a context should be like. Build one with {@link #builder()}; every field has the native library's "don't care"
 * default (zero), except that {@link #builder()} starts with desktop OpenGL.
 *
 * <p>Sizes and the version are <em>minimums</em>. With {@link Fallback#STRICT} (the default) the call fails unless every
 * requested attribute is met; with {@link Fallback#BEST_EFFORT} soft attributes (samples, depth, flags the driver cannot
 * provide) are relaxed and the real result is reported by {@link Context#info()}.
 */
public final class ContextConfig {

    public enum Api {
        OPENGL(1), OPENGL_ES(2);
        final int nativeValue;

        Api(int v) {
            nativeValue = v;
        }
    }

    public enum Profile {
        ANY(0), CORE(1), COMPATIBILITY(2);
        final int nativeValue;

        Profile(int v) {
            nativeValue = v;
        }

        static Profile of(int v) {
            for (Profile p : values()) {
                if (p.nativeValue == v) {
                    return p;
                }
            }
            return ANY;
        }
    }

    public enum Surface {
        DEFAULT(0), HIDDEN_WINDOW(1), PBUFFER(2), NONE(3),
        /** Reported for contexts made by {@link Context#createForWindow}; not a value to request. */
        WINDOW(4);
        final int nativeValue;

        Surface(int v) {
            nativeValue = v;
        }

        static Surface of(int v) {
            for (Surface s : values()) {
                if (s.nativeValue == v) {
                    return s;
                }
            }
            return DEFAULT;
        }
    }

    public enum Fallback {
        STRICT(0), BEST_EFFORT(1);
        final int nativeValue;

        Fallback(int v) {
            nativeValue = v;
        }
    }

    /** Bit values of {@link #flags()} (and of {@link ContextInfo#flags()}). */
    public static final class Flags {
        public static final int DEBUG = 0x001;
        public static final int FORWARD_COMPATIBLE = 0x002;
        public static final int ROBUST_ACCESS = 0x004;
        public static final int RESET_NOTIFICATION = 0x008;
        public static final int NO_ERROR = 0x010;
        public static final int SRGB = 0x020;
        public static final int FLOAT_COLOR = 0x040;
        public static final int DOUBLE_BUFFER = 0x080;
        public static final int STEREO = 0x100;
        public static final int RELEASE_FLUSH = 0x200;

        private Flags() {
        }
    }

    private final Api api;
    private final int versionMajor;
    private final int versionMinor;
    private final Profile profile;
    private final int flags;
    private final Surface surface;
    private final Fallback fallback;
    private final int red;
    private final int green;
    private final int blue;
    private final int alpha;
    private final int depth;
    private final int stencil;
    private final int samples;
    private final Context shareWith;

    private ContextConfig(Builder b) {
        api = b.api;
        versionMajor = b.versionMajor;
        versionMinor = b.versionMinor;
        profile = b.profile;
        flags = b.flags;
        surface = b.surface;
        fallback = b.fallback;
        red = b.red;
        green = b.green;
        blue = b.blue;
        alpha = b.alpha;
        depth = b.depth;
        stencil = b.stencil;
        samples = b.samples;
        shareWith = b.shareWith;
    }

    public static Builder builder() {
        return new Builder();
    }

    public Api api() {
        return api;
    }

    public int versionMajor() {
        return versionMajor;
    }

    public int versionMinor() {
        return versionMinor;
    }

    public Profile profile() {
        return profile;
    }

    public int flags() {
        return flags;
    }

    public Surface surface() {
        return surface;
    }

    public Fallback fallback() {
        return fallback;
    }

    public int samples() {
        return samples;
    }

    public Context shareWith() {
        return shareWith;
    }

    /** Layout shared with the native side: api, major, minor, profile, flags, surface, fallback, r, g, b, a, depth, stencil, samples. */
    int[] toNative() {
        return new int[] {api.nativeValue, versionMajor, versionMinor, profile.nativeValue, flags, surface.nativeValue,
                fallback.nativeValue, red, green, blue, alpha, depth, stencil, samples};
    }

    @Override
    public String toString() {
        return "ContextConfig{" + api + " " + versionMajor + "." + versionMinor + " " + profile + ", flags=0x"
                + Integer.toHexString(flags) + ", samples=" + samples + ", depth=" + depth + ", stencil=" + stencil + ", "
                + surface + ", " + fallback + "}";
    }

    public static final class Builder {
        private Api api = Api.OPENGL;
        private int versionMajor;
        private int versionMinor;
        private Profile profile = Profile.ANY;
        private int flags;
        private Surface surface = Surface.DEFAULT;
        private Fallback fallback = Fallback.STRICT;
        private int red;
        private int green;
        private int blue;
        private int alpha;
        private int depth;
        private int stencil;
        private int samples;
        private Context shareWith;

        private Builder() {
        }

        public Builder api(Api api) {
            this.api = Objects.requireNonNull(api, "api");
            return this;
        }

        /** Minimum version; {@code 0, 0} (the default) means any. */
        public Builder version(int major, int minor) {
            if (major < 0 || minor < 0) {
                throw new IllegalArgumentException("version must not be negative");
            }
            versionMajor = major;
            versionMinor = minor;
            return this;
        }

        public Builder profile(Profile profile) {
            this.profile = Objects.requireNonNull(profile, "profile");
            return this;
        }

        /** ORs in {@link Flags} bits. */
        public Builder flags(int flags) {
            this.flags |= flags;
            return this;
        }

        public Builder surface(Surface surface) {
            this.surface = Objects.requireNonNull(surface, "surface");
            return this;
        }

        public Builder fallback(Fallback fallback) {
            this.fallback = Objects.requireNonNull(fallback, "fallback");
            return this;
        }

        public Builder colorBits(int red, int green, int blue, int alpha) {
            this.red = red;
            this.green = green;
            this.blue = blue;
            this.alpha = alpha;
            return this;
        }

        public Builder depthBits(int bits) {
            depth = bits;
            return this;
        }

        public Builder stencilBits(int bits) {
            stencil = bits;
            return this;
        }

        public Builder samples(int samples) {
            this.samples = samples;
            return this;
        }

        /** The new context shares textures, buffers and shaders with {@code context}. */
        public Builder shareWith(Context context) {
            shareWith = context;
            return this;
        }

        public ContextConfig build() {
            return new ContextConfig(this);
        }
    }
}
