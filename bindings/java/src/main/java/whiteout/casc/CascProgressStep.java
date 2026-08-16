// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). Mirrors whiteout::storages::casc::ProgressStep.

package whiteout.casc;

/** Stage of the open sequence a progress event belongs to. */
public enum CascProgressStep {
    /** Online: versions/cdns endpoint lookup. */
    ResolvingVersion(0),
    /** Build config (fetch or disk read + parse). */
    LoadingBuildConfig(1),
    /** CDN config (fetch or disk read + parse). */
    LoadingCdnConfig(2),
    /** Local {@code .idx} bucket files. */
    LoadingIndexFiles(3),
    /** Local {@code data.NNN} archives being memory-mapped. */
    MappingArchives(4),
    /** Online: per-archive {@code .index} fetches. */
    LoadingArchiveIndexes(5),
    /** ENCODING manifest decode + parse. */
    LoadingEncodingTable(6),
    /** TVFS sub-manifests (~870 on WoW retail). */
    LoadingVfsManifests(7),
    /** ROOT manifest decode + parse. */
    LoadingRootManifest(8),
    /** Storage is usable. Always the final event. */
    Ready(9);

    /** The C++ enumerator value. */
    public final int value;

    CascProgressStep(int value) {
        this.value = value;
    }

    /** Map a native value to its constant; unknown values map to {@link #Ready}. */
    public static CascProgressStep fromValue(int value) {
        for (CascProgressStep s : values()) {
            if (s.value == value) return s;
        }
        return Ready;
    }

    /** English label for this step, as the library spells it. */
    public String label() {
        return CascShims.progressStepName(this);
    }
}
