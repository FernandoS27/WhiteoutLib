// SPDX-License-Identifier: BSD-3-Clause
// Shared TypeScript declarations consumed by every per-format types/*.d.ts.
// Hand-written; not generated.

// ── Embind base types ─────────────────────────────────────────────────────

/** Base class for every Embind-bound C++ object. Must be released with
 *  `delete()` to free WASM heap memory. */
export class EmbindObject {
    delete(): void;
    deleteLater(): EmbindObject;
    isDeleted(): boolean;
    isAliasOf(other: EmbindObject): boolean;
    clone(): this;
}

/** Embind-bound `std::vector<T>`. List-like; mutate via `push_back`/
 *  `set`, read via `get`. Vector property access copies — see the
 *  read-modify-write pattern in the README. */
export interface EmbindVector<T> extends EmbindObject {
    size(): number;
    get(index: number): T;
    set(index: number, value: T): void;
    push_back(value: T): void;
    resize(n: number, value: T): void;
}

/** Buffer-friendly vector: layout-compatible with a flat scalar array
 *  (primitives, Vector2f/3f/4f, Quaternion, ColorBGRA). `view()` returns
 *  a zero-copy JS TypedArray aliased to the WASM heap.
 *
 *  Elements with multiple components (e.g. Vector3f) come back as a flat
 *  TypedArray of length `size() * components`; callers index by stride
 *  (`view[i*3 + 0]`) or wrap in their own ndarray library if they want
 *  a 2D shape.
 *
 *  **Caveat:** any size-changing call (`push_back`, `resize`, heap
 *  growth from another WASM call) may invalidate the view. Re-acquire
 *  via `view()` after mutating. */
export interface EmbindBufferVector<T, TView> extends EmbindVector<T> {
    /** Zero-copy TypedArray aliased to WASM linear memory. */
    view(): TView;
}

/** A single value of an Embind enum: just `{ value: number }`. */
export interface EnumValue {
    readonly value: number;
}

/** Type-alias used for class fields whose type is a specific enum. The
 *  `TEnum` parameter brands the type so `node.flags = SequenceFlag.None`
 *  raises a type error at usage sites. */
export type EnumMember<TEnum> = TEnum[keyof TEnum];

// ── Shared math types ─────────────────────────────────────────────────────
// These are bound at the WASM module's root (whiteout.Vector3f etc.) and
// passed by value as plain JS objects. No `delete()` needed.

export interface Vector2f { x: number; y: number; }
export interface Vector3f { x: number; y: number; z: number; }
export interface Vector4f { x: number; y: number; z: number; w: number; }
export interface Quaternion { x: number; y: number; z: number; w: number; }


// ── Texture (hand-written; the C++ class has overloaded methods and a
//    custom data() accessor that the codegen can't auto-generate) ─────────

/** Format-agnostic GPU texture. Returned by every texture parser; consumed
 *  by every texture writer. */
export class Texture extends EmbindObject {
    constructor();
    type(): EnumValue;
    format(): EnumValue;
    width(): number;
    height(): number;
    depth(): number;
    mipCount(): number;
    layerCount(): number;
    arraySize(): number;
    dataSize(): number;
    /** Zero-copy memoryview onto the WASM heap. Copy out before any other
     *  WASM call — heap growth invalidates the view. */
    data(): Uint8Array;
    convertTo(format: EnumValue): void;
    /** Returns "" on success, an error message otherwise. */
    generateMipmaps(): string;
}

