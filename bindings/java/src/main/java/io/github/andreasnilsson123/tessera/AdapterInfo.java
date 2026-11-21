// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** A GPU a backend can use, from {@link Tessera#enumerateAdapters()}. */
public final class AdapterInfo {
    /** Runs on the CPU (WARP, llvmpipe, Apple Software Renderer). */
    public static final int SOFTWARE = 0x1;
    /** Shares memory with the CPU (known where the platform says so). */
    public static final int INTEGRATED = 0x2;
    /** Has its own memory (known where the platform says so). */
    public static final int DISCRETE = 0x4;

    private final int flags;
    private final int vendorId;
    private final int deviceId;
    private final long luid;
    private final String backend;
    private final String id;
    private final String name;

    AdapterInfo(int[] v, String[] s) {
        flags = v[0];
        vendorId = v[1];
        deviceId = v[2];
        luid = (v[3] & 0xFFFFFFFFL) | ((long) v[4] << 32);
        backend = s[0];
        id = s[1];
        name = s[2];
    }

    /** {@link #SOFTWARE}, {@link #INTEGRATED}, {@link #DISCRETE}. */
    public int flags() {
        return flags;
    }

    public boolean isSoftware() {
        return (flags & SOFTWARE) != 0;
    }

    /** PCI vendor id, 0 = not known. */
    public int vendorId() {
        return vendorId;
    }

    /** PCI device id, 0 = not known. */
    public int deviceId() {
        return deviceId;
    }

    /** Windows: the adapter LUID as {@link ContextConfig.Builder#adapterLuid(long)} takes it; otherwise 0. */
    public long luid() {
        return luid;
    }

    /** The backend that can use this adapter: {@code wgl}, {@code egl}, {@code cgl}. */
    public String backend() {
        return backend;
    }

    /** What {@link ContextConfig.Builder#adapter(String)} takes. */
    public String id() {
        return id;
    }

    /** For people. */
    public String name() {
        return name;
    }

    @Override
    public String toString() {
        return "AdapterInfo{" + backend + " " + id + " \"" + name + "\" flags=0x" + Integer.toHexString(flags) + "}";
    }
}
