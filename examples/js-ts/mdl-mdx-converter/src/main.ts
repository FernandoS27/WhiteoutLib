// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Browser MDX ↔ MDL converter.
//
// One factory call (`Whiteout()`) brings the WASM module up; every
// conversion is then a synchronous Parser → Writer round-trip. The
// example uses the lower-level `mdx.Parser` / `mdx.Writer` classes
// directly (rather than the `mdx.parse` / `mdx.writeMdl` facade) so it
// can surface the parser's issue list — a nice illustration of how
// lenient parsing reports recoverable problems without throwing.
//
// Memory note: every Embind handle (Parser, Writer, Model, and the
// std::vector<u8> returned by Writer.write_*) owns a native allocation;
// each one is released in a try/finally block.

import Whiteout from 'whiteout-wasm';

const whiteoutReady = Whiteout();

// ── DOM handles ────────────────────────────────────────────────────────

const fileInput     = byId<HTMLInputElement>('file-input');
const dropZone      = byId<HTMLElement>('drop-zone');
const optionsRow    = byId<HTMLElement>('options-row');
const dialectSelect = byId<HTMLSelectElement>('dialect-select');
const convertBtn    = byId<HTMLButtonElement>('convert-btn');
const statusEl      = byId<HTMLElement>('status');
const issuesEl      = byId<HTMLElement>('issues');
const downloadLink  = byId<HTMLAnchorElement>('download-link');

function byId<T extends HTMLElement>(id: string): T {
    const el = document.getElementById(id);
    if (!el) throw new Error(`Missing #${id} in DOM`);
    return el as T;
}

// ── State ───────────────────────────────────────────────────────────────

type SrcFormat = 'mdx' | 'mdl';
let currentFile: File | null = null;
let lastDownloadUrl: string | null = null;

// ── UI helpers ──────────────────────────────────────────────────────────

type StatusKind = 'info' | 'busy' | 'ok' | 'error';
function setStatus(text: string, kind: StatusKind = 'info'): void {
    statusEl.textContent = text;
    statusEl.dataset.kind = kind;
}

function showIssues(issues: string[]): void {
    if (issues.length === 0) {
        issuesEl.hidden = true;
        issuesEl.replaceChildren();
        return;
    }
    const h3 = document.createElement('h3');
    h3.textContent = `Parser issues (${issues.length})`;
    const ul = document.createElement('ul');
    for (const msg of issues) {
        const li = document.createElement('li');
        li.textContent = msg;
        ul.appendChild(li);
    }
    issuesEl.replaceChildren(h3, ul);
    issuesEl.hidden = false;
}

function detectFormat(name: string): SrcFormat | null {
    const lower = name.toLowerCase();
    if (lower.endsWith('.mdx')) return 'mdx';
    if (lower.endsWith('.mdl')) return 'mdl';
    return null;
}

function swapExtension(name: string, src: SrcFormat): string {
    const stem = name.replace(/\.(mdx|mdl)$/i, '');
    return src === 'mdx' ? `${stem}.mdl` : `${stem}.mdx`;
}

function formatBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

function refreshOptionsVisibility(): void {
    // The dialect picker only affects MDL output — hide it when the
    // input is .mdl (i.e. when we're writing binary MDX).
    const src = currentFile ? detectFormat(currentFile.name) : null;
    optionsRow.hidden = src !== 'mdx';
}

/** Minimal shape every Embind `std::vector<T>` proxy exposes. Typed
 *  loosely on purpose — the live binding handles both u8 and string. */
interface EmbindVec<T> {
    size(): number;
    get(i: number): T;
    delete(): void;
}

/** Copy a `std::vector<u8>` proxy out of the WASM heap into a fresh
 *  Uint8Array backed by a plain ArrayBuffer (no SharedArrayBuffer), then
 *  release the vector. The plain-ArrayBuffer guarantee matters for
 *  passing the result straight into a Blob without TypeScript narrowing
 *  complaints. */
function vecToBytes(vec: EmbindVec<number>): Uint8Array {
    const n = vec.size();
    const buf = new ArrayBuffer(n);
    const out = new Uint8Array(buf);
    for (let i = 0; i < n; i++) out[i] = vec.get(i);
    vec.delete();
    return out;
}

/** Drain a `std::vector<string>` proxy into a plain JS string[]. */
function vecToStrings(vec: EmbindVec<string>): string[] {
    const n = vec.size();
    const out: string[] = new Array(n);
    for (let i = 0; i < n; i++) out[i] = vec.get(i);
    vec.delete();
    return out;
}

// ── Core conversion ───────────────────────────────────────────────────

async function convert(file: File): Promise<void> {
    const src = detectFormat(file.name);
    if (src === null) {
        setStatus(
            `Unsupported file "${file.name}" — expected a .mdx or .mdl extension.`,
            'error',
        );
        return;
    }

    setStatus(`Loading WASM…`, 'busy');
    const whiteout = await whiteoutReady;

    setStatus(`Reading ${file.name} (${formatBytes(file.size)})…`, 'busy');
    const bytes = new Uint8Array(await file.arrayBuffer());

    setStatus(`Parsing ${src.toUpperCase()}…`, 'busy');

    const parser = new whiteout.mdx.Parser();
    try {
        const inputFormat = src === 'mdx'
            ? whiteout.mdx.MDLXFormat.MDX
            : whiteout.mdx.MDLXFormat.MDL;

        // The library is exception-free; parse returns a Model proxy
        // regardless of input quality (issues are surfaced via getIssues).
        const model = parser.parse_buffer_format(bytes, inputFormat);
        try {
            showIssues(vecToStrings(parser.getIssues() as unknown as EmbindVec<string>));

            setStatus(`Writing ${src === 'mdx' ? 'MDL' : 'MDX'}…`, 'busy');
            const writer = new whiteout.mdx.Writer();
            try {
                const dialect = dialectSelect.value === 'hiveworkshop'
                    ? whiteout.mdx.MdlFormat.Hiveworkshop
                    : whiteout.mdx.MdlFormat.WarcraftIII;
                const outputFormat = src === 'mdx'
                    ? whiteout.mdx.MDLXFormat.MDL
                    : whiteout.mdx.MDLXFormat.MDX;

                const outBytes = vecToBytes(
                    writer.write_mdx_format_mdlFormat(
                        model, outputFormat, dialect,
                    ) as unknown as EmbindVec<number>,
                );

                const outName = swapExtension(file.name, src);
                if (lastDownloadUrl) URL.revokeObjectURL(lastDownloadUrl);
                const mime = src === 'mdx' ? 'text/plain' : 'application/octet-stream';
                // Cast to BlobPart: vecToBytes guarantees a plain ArrayBuffer-backed
                // Uint8Array, but TS narrows to `Uint8Array<ArrayBufferLike>` which
                // its newer lib.dom.d.ts doesn't accept directly.
                lastDownloadUrl = URL.createObjectURL(
                    new Blob([outBytes as BlobPart], { type: mime }),
                );
                downloadLink.href = lastDownloadUrl;
                downloadLink.download = outName;
                downloadLink.textContent =
                    `Download ${outName} (${formatBytes(outBytes.length)})`;
                downloadLink.hidden = false;

                setStatus(
                    `Converted ${file.name} → ${outName} ` +
                        `(${formatBytes(bytes.length)} → ${formatBytes(outBytes.length)}).`,
                    'ok',
                );
            } finally {
                writer.delete();
            }
        } finally {
            model.delete();
        }
    } catch (e) {
        setStatus(`Conversion failed: ${(e as Error).message ?? String(e)}`, 'error');
    } finally {
        parser.delete();
    }
}

// ── Event wiring ──────────────────────────────────────────────────────

fileInput.addEventListener('change', () => {
    const file = fileInput.files?.[0] ?? null;
    if (!file) return;
    currentFile = file;
    refreshOptionsVisibility();
    void convert(file);
});

convertBtn.addEventListener('click', () => {
    if (currentFile) void convert(currentFile);
});

dialectSelect.addEventListener('change', () => {
    if (currentFile && detectFormat(currentFile.name) === 'mdx') {
        void convert(currentFile);
    }
});

dropZone.addEventListener('dragover', e => {
    e.preventDefault();
    dropZone.classList.add('dragover');
});
dropZone.addEventListener('dragleave', () => {
    dropZone.classList.remove('dragover');
});
dropZone.addEventListener('drop', e => {
    e.preventDefault();
    dropZone.classList.remove('dragover');
    const file = e.dataTransfer?.files?.[0] ?? null;
    if (!file) return;
    currentFile = file;
    refreshOptionsVisibility();
    void convert(file);
});

setStatus('Drop a .mdx or .mdl file, or click "Browse for a file".', 'info');
