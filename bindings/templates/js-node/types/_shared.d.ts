// SPDX-License-Identifier: BSD-3-Clause
// Shared TypeScript declarations consumed by every per-format types/*.d.ts.
// Hand-written; not generated.
//
// This is the Node-flavoured variant: identical Embind base types as the
// web build's _shared.d.ts, plus the OsFileSystem / SimpleThreadPool /
// HttpHandler interfaces that only the Node build registers.

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

/** Buffer-friendly vector: layout-compatible with a flat scalar array.
 *  `view()` returns a zero-copy JS TypedArray aliased to the WASM heap.
 *  Any size-changing call invalidates the view — re-acquire after mutating. */
export interface EmbindBufferVector<T, TView> extends EmbindVector<T> {
    /** Zero-copy TypedArray aliased to WASM linear memory. */
    view(): TView;
}

/** A single value of an Embind enum: just `{ value: number }`. */
export interface EnumValue {
    readonly value: number;
}

/** Type-alias used for class fields whose type is a specific enum. */
export type EnumMember<TEnum> = TEnum[keyof TEnum];

// ── Shared math types (passed by value as plain JS objects) ───────────────

export interface Vector2f { x: number; y: number; }
export interface Vector3f { x: number; y: number; z: number; }
export interface Vector4f { x: number; y: number; z: number; w: number; }
export interface Quaternion { x: number; y: number; z: number; w: number; }


// ── Node-only types: file I/O, threading, HTTP ───────────────────────────

/** Path-based VirtualPathFileSystem backed by the host disk via
 *  NODERAWFS. Pass to format parsers that take a VirtualPathFileSystem
 *  (e.g. `M2Parser::parse(fs, path)`). */
export class OsFileSystem extends EmbindObject {
    constructor(rootPath: string);
    /** Read a file at `rootPath / path`. Returns a TypedArray view aliased
     *  to WASM memory — copy out (`new Uint8Array(view)`) before any
     *  subsequent WASM allocation. */
    readFile(path: string): Uint8Array;
    writeFile(path: string, data: Uint8Array): boolean;
    fileExists(path: string): boolean;
}

/** std::thread-backed WorkerPool (mapped onto Emscripten pthreads /
 *  Node worker_threads). Pass into format conversion / mipmap calls
 *  that accept a pool for parallel work. */
export class SimpleThreadPool extends EmbindObject {
    constructor(nThreads: number);
    /** Block until every submitted task has completed. */
    waitIdle(): void;
    /** Number of worker threads in this pool. */
    threadCount(): number;
}

/** Response object produced by user-supplied HttpHandler implementations.
 *  Passed back to the C++ side via the `complete` callback. */
export interface HttpResponse {
    statusCode: number;
    body?: Uint8Array | null;
    error?: string;
}

/** User-supplied HTTP handler shape (object). Pass to
 *  `whiteout.makeHttpHandler(impl)`. `complete(response)` MUST be called
 *  exactly once per request. */
export interface HttpHandlerImpl {
    capabilities?(): number;
    getAsync(url: string,
             complete: (response: HttpResponse) => void): void | Promise<void>;
    getRangeAsync?(url: string, start: number, end: number,
                   complete: (response: HttpResponse) => void): void | Promise<void>;
}

/** Opaque C++-side HttpHandler instance produced by makeHttpHandler.
 *  Pass to library calls that take an HttpHandler (online CASC, …). */
export class HttpHandler extends EmbindObject {
    /** Reports the JS impl's capability flags (defaults to 0). */
    capabilities(): number;
}
