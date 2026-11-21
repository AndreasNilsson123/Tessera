// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** The share group of a context, from {@link Context#shareGroup()}: what survives a loss, without keeping your own bookkeeping. */
public final class ShareGroup {
    private final long id;
    private final int members;
    private final int alive;

    ShareGroup(long id, int members, int alive) {
        this.id = id;
        this.members = members;
        this.alive = alive;
    }

    /** Opaque; equal for contexts of one group, never reused within a process. */
    public long id() {
        return id;
    }

    /** Live handles in the group, lost ones included (they still have to be closed). */
    public int members() {
        return members;
    }

    /** Of those, not lost: what can be shared with. */
    public int alive() {
        return alive;
    }

    @Override
    public String toString() {
        return "ShareGroup{id=" + id + ", members=" + members + ", alive=" + alive + "}";
    }
}
