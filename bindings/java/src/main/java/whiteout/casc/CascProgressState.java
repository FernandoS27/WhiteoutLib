// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). Mirrors whiteout::storages::casc::ProgressState.

package whiteout.casc;

/** Position of a progress event within its step's lifetime. */
public enum CascProgressState {
    /** The step is starting. Always paired with an {@link #End}. */
    Begin(0),
    /** Counters advanced. Throttled — samples may be dropped. */
    Update(1),
    /** The step finished; the counters hold the final tally. */
    End(2);

    /** The C++ enumerator value. */
    public final int value;

    CascProgressState(int value) {
        this.value = value;
    }

    /** Map a native value to its constant; unknown values map to {@link #End}. */
    public static CascProgressState fromValue(int value) {
        for (CascProgressState s : values()) {
            if (s.value == value) return s;
        }
        return End;
    }
}
