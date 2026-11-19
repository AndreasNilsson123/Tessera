// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** What a context actually is, as reported by the driver (not what was requested). */
public final class ContextInfo {
    private final ContextConfig.Api api;
    private final int versionMajor;
    private final int versionMinor;
    private final ContextConfig.Profile profile;
    private final int flags;
    private final ContextConfig.Surface surface;
    private final int samples;
    private final int red;
    private final int green;
    private final int blue;
    private final int alpha;
    private final int depth;
    private final int stencil;
    private final String vendor;
    private final String renderer;
    private final String version;

    ContextInfo(int[] v, String[] s) {
        api = v[0] == 2 ? ContextConfig.Api.OPENGL_ES : ContextConfig.Api.OPENGL;
        versionMajor = v[1];
        versionMinor = v[2];
        profile = ContextConfig.Profile.of(v[3]);
        flags = v[4];
        surface = ContextConfig.Surface.of(v[5]);
        samples = v[6];
        red = v[7];
        green = v[8];
        blue = v[9];
        alpha = v[10];
        depth = v[11];
        stencil = v[12];
        vendor = s[0];
        renderer = s[1];
        version = s[2];
    }

    public ContextConfig.Api api() {
        return api;
    }

    public int versionMajor() {
        return versionMajor;
    }

    public int versionMinor() {
        return versionMinor;
    }

    public ContextConfig.Profile profile() {
        return profile;
    }

    /** {@link ContextConfig.Flags} bits that are actually in effect. */
    public int flags() {
        return flags;
    }

    public boolean hasFlag(int flag) {
        return (flags & flag) == flag;
    }

    public ContextConfig.Surface surface() {
        return surface;
    }

    public int samples() {
        return samples;
    }

    public int redBits() {
        return red;
    }

    public int greenBits() {
        return green;
    }

    public int blueBits() {
        return blue;
    }

    public int alphaBits() {
        return alpha;
    }

    public int depthBits() {
        return depth;
    }

    public int stencilBits() {
        return stencil;
    }

    /** {@code GL_VENDOR}. Non-ASCII characters are replaced by {@code ?}. */
    public String vendor() {
        return vendor;
    }

    public String renderer() {
        return renderer;
    }

    /** {@code GL_VERSION}. */
    public String version() {
        return version;
    }

    @Override
    public String toString() {
        return "ContextInfo{" + api + " " + versionMajor + "." + versionMinor + " " + profile + ", " + renderer + ", flags=0x"
                + Integer.toHexString(flags) + ", rgba=" + red + green + blue + alpha + ", depth=" + depth + ", stencil="
                + stencil + ", samples=" + samples + ", " + surface + "}";
    }
}
