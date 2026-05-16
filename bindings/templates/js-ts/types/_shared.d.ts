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
 *  read-modify-write pattern in the README.
 *
 *  Supports `for (const x of vec) ...` — the iterator yields by-value
 *  copies (Embind semantics), so mutating the yielded element does NOT
 *  write back to the C++ vector. Use `vec.set(i, ...)` for that. */
export interface EmbindVector<T> extends EmbindObject {
    size(): number;
    get(index: number): T;
    set(index: number, value: T): void;
    push_back(value: T): void;
    resize(n: number, value: T): void;
    [Symbol.iterator](): IterableIterator<T>;
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


// Texture, PixelFormat, TextureType, and every texture-format Parser /
// Writer class are exported by the auto-generated `./textures.d.ts`.
// Import them from there to get the full, codegen-derived surface.
