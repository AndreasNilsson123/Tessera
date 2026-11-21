// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

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

    /**
     * Which GPU to use. Windows (WGL) only; other backends refuse a non-default value with {@link Result#UNSUPPORTED}. A process
     * cannot make Windows load a different GPU's OpenGL driver, so this does not switch GPUs: Tessera resolves the adapter through
     * DXGI and <em>checks</em> that the OpenGL driver it got belongs to it (by PCI vendor), failing with
     * {@link Result#NO_MATCHING_CONFIG} instead of silently handing out another GPU. For a preference the check is a failure in
     * strict mode and a logged warning in best-effort mode; for {@link Builder#adapterLuid(long)} it is always a failure.
     */
    public enum GpuPreference {
        DEFAULT(0), HIGH_PERFORMANCE(1), LOW_POWER(2);
        final int nativeValue;

        GpuPreference(int v) {
            nativeValue = v;
        }
    }

    /**
     * A hint for how the GPU schedules this context's work against other contexts. Available through GLX
     * ({@code GLX_EXT_context_priority}) and EGL ({@code EGL_IMG_context_priority}); WGL has no such extension. With
     * {@link Fallback#STRICT} a request that cannot be honoured, including a driver that grants a lower level than asked, fails
     * with {@link Result#NO_MATCHING_CONFIG}; best-effort logs and continues. See {@link ContextInfo#priority()}.
     */
    public enum Priority {
        DEFAULT(0), LOW(1), MEDIUM(2), HIGH(3);
        final int nativeValue;

        Priority(int v) {
            nativeValue = v;
        }

        static Priority of(int v) {
            for (Priority p : values()) {
                if (p.nativeValue == v) {
                    return p;
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
        /**
         * Ask the driver for exactly {@link Builder#version}, with no search for a newer one (ABI 0.13.0). Without it the
         * context is the newest the driver offers that is at least the requested version. A driver may still answer with a newer
         * compatible version (Mesa does for core profiles); {@link ContextInfo} says what was granted.
         */
        public static final int EXACT_VERSION = 0x400;
        /**
         * Do not check the reset status inside {@link Context#makeCurrent()} and {@link Context#swapBuffers()} (ABI 0.15.0).
         * By default a context created with {@link #RESET_NOTIFICATION} is checked there (one cheap query), so the lost
         * listener fires without the application polling. No effect without reset notification.
         */
        public static final int NO_RESET_POLL = 0x800;

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
    private final GpuPreference gpuPreference;
    private final long adapterLuid;
    private final String adapter;
    private final Priority priority;

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
        gpuPreference = b.gpuPreference;
        adapterLuid = b.adapterLuid;
        adapter = b.adapter;
        priority = b.priority;
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

    public GpuPreference gpuPreference() {
        return gpuPreference;
    }

    public Priority priority() {
        return priority;
    }

    /** The requested DXGI adapter LUID, or 0. */
    public long adapterLuid() {
        return adapterLuid;
    }

    /** The adapter id asked for ({@link Tessera#enumerateAdapters()}), or "" for no request. */
    public String adapter() {
        return adapter;
    }

    /**
     * Layout shared with the native side: api, major, minor, profile, flags, surface, fallback, r, g, b, a, depth, stencil,
     * samples, gpuPreference, adapterLuid (low 32 bits, high 32 bits), priority, then the 48 bytes of the adapter id in 12 ints
     * (four to an int, least significant byte first).
     */
    int[] toNative() {
        final int[] v = new int[30];
        final int[] head = {api.nativeValue, versionMajor, versionMinor, profile.nativeValue, flags, surface.nativeValue,
                fallback.nativeValue, red, green, blue, alpha, depth, stencil, samples, gpuPreference.nativeValue,
                (int) adapterLuid, (int) (adapterLuid >>> 32), priority.nativeValue};
        System.arraycopy(head, 0, v, 0, head.length);
        for (int i = 0; i < adapter.length(); i++) {
            v[18 + i / 4] |= (adapter.charAt(i) & 0xFF) << (8 * (i % 4));
        }
        return v;
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
        private GpuPreference gpuPreference = GpuPreference.DEFAULT;
        private long adapterLuid;
        private String adapter = "";
        private Priority priority = Priority.DEFAULT;
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

        /**
         * Minimum version; {@code 0, 0} (the default) means any. The context is the newest version the driver offers that is at
         * least this (OpenGL ES with no version tries 3.2, 3.1, 3.0, 2.0); add {@link Flags#EXACT_VERSION} to get exactly it.
         */
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

        /** See {@link GpuPreference}. Not combinable with {@link #adapterLuid(long)}. */
        public Builder gpuPreference(GpuPreference preference) {
            this.gpuPreference = Objects.requireNonNull(preference, "preference");
            return this;
        }

        /** A DXGI adapter LUID ({@code (HighPart << 32) | LowPart}); Windows only, see {@link GpuPreference}. 0 = none. */
        public Builder adapterLuid(long luid) {
            this.adapterLuid = luid;
            return this;
        }

        /**
         * Which GPU: an id from {@link Tessera#enumerateAdapters()} (text such as {@code luid:00000000-0000F7E7},
         * {@code uuid:...}, {@code node:/dev/dri/renderD128}, {@code cgl:0x00024600}); "" = no request. At most 47 printable ASCII
         * characters; not combinable with {@link #adapterLuid(long)} or {@link #gpuPreference}. A backend that cannot honour it
         * refuses ({@code UNSUPPORTED}); it is never silently replaced by another GPU.
         */
        public Builder adapter(String id) {
            Objects.requireNonNull(id, "id");
            if (id.length() > 47) {
                throw new IllegalArgumentException("an adapter id has at most 47 characters");
            }
            for (int i = 0; i < id.length(); i++) {
                if (id.charAt(i) < 0x20 || id.charAt(i) > 0x7E) {
                    throw new IllegalArgumentException("an adapter id is printable ASCII");
                }
            }
            this.adapter = id;
            return this;
        }

        /** See {@link Priority}. */
        public Builder priority(Priority priority) {
            this.priority = Objects.requireNonNull(priority, "priority");
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
