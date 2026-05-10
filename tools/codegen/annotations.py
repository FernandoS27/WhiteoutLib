# SPDX-License-Identifier: BSD-3-Clause
"""Parser for `@bind` annotations in C++ doc comments.

Annotation grammar:

    @bind                      -> include this declaration
    @bind value_object         -> include + bind as value_object
    @bind rename=isHd          -> include + rename in JS
    @bind skip                 -> exclude this field
    @bind value_object, expose -> multiple modifiers per line

Annotations may appear in any of these comment forms:

    /// @bind                                           (leading `///`)
    /**
     * @bind
     */                                                 (leading doxygen)
    int x; ///< @bind rename=foo — descriptive text     (trailing `///<`)

The annotation runs from `@bind` up to either end-of-line, the next `@`
directive, or one of the punctuation markers ` — ` / ` -- ` (commonly used
to start the human-readable description that follows the annotation).
"""

from __future__ import annotations

import re
from typing import Any, Optional


# Capture everything from `@bind` up to (but not including) the next
# newline, the next @-tag, an em-dash, or a `--` separator. The latter
# two are conventional separators between the annotation and a human
# description on the same line.
_ANNOTATION_RE = re.compile(r'@bind\b\s*:?\s*(?P<rest>[^\n@—]*?)(?=\n|@|—|--|\Z)', re.MULTILINE)


def _strip_glyphs(line: str) -> str:
    """Drop leading + trailing comment glyphs and surrounding whitespace.

    Handles both line-style (`///`, `//`) and single-line block-style
    (`/** ... */`, `/* ... */`) comments without leaving a stray `*/`
    that would close a generated JSDoc/docstring prematurely.
    """
    s = line.strip()
    for glyph in ('///<', '///', '/**', '/*', '*/', '*', '//'):
        if s.startswith(glyph):
            s = s[len(glyph):].lstrip()
            break
    if s.endswith('*/'):
        s = s[:-2].rstrip()
    return s.rstrip()


def parse(raw_comment: Optional[str]) -> dict[str, Any]:
    """Return a dict of @bind directives. {} means "no @bind found"."""
    if not raw_comment:
        return {}
    out: dict[str, Any] = {}

    # Normalise comment to plain text by stripping glyphs line by line.
    text_lines = [_strip_glyphs(line) for line in raw_comment.splitlines()]
    text = '\n'.join(text_lines)

    for m in _ANNOTATION_RE.finditer(text):
        out['_present'] = True
        rest = m.group('rest').strip()
        if not rest:
            continue
        for item in rest.split(','):
            item = item.strip()
            if not item:
                continue
            if '=' in item:
                k, v = item.split('=', 1)
                out[k.strip()] = v.strip()
            else:
                out[item] = True
    return out


def is_bound(annotations: dict[str, Any]) -> bool:
    return bool(annotations.get('_present'))


# Leading doxygen tags to scrub. `@brief` we drop entirely (the description
# IS the brief); `@note` / `@warning` we keep so the prose flows naturally.
_DOXY_DROP_RE = re.compile(r'^\s*@brief\s+', re.IGNORECASE)


def extract_doc(raw_comment: Optional[str]) -> str:
    """Return the human-readable description from a doc comment.

    - Strips C++ comment glyphs (`///`, `///<`, `/**`, `* `, etc.).
    - Removes any `@bind ...` annotation on the same line (everything
      before the `—` / `--` separator, or the whole `@bind ...` clause
      if no separator).
    - Drops a leading `@brief ` tag.
    - Joins consecutive non-empty lines with spaces; preserves blank
      lines between paragraphs.

    Returns '' for empty / annotation-only comments.
    """
    if not raw_comment:
        return ''
    paragraphs: list[list[str]] = [[]]
    for line in raw_comment.splitlines():
        s = _strip_glyphs(line)
        if '@bind' in s:
            bind_idx = s.find('@bind')
            sep_idx = -1
            sep_len = 0
            for sep in ('—', '--'):
                idx = s.find(sep)
                if idx > bind_idx:
                    sep_idx = idx
                    sep_len = len(sep)
                    break
            if sep_idx >= 0:
                s = s[sep_idx + sep_len:].strip()
            else:
                s = s[:bind_idx].strip()
        if not s:
            if paragraphs[-1]:
                paragraphs.append([])
            continue
        paragraphs[-1].append(s)

    out_paras = [' '.join(p).strip() for p in paragraphs if p]
    out_paras = [_DOXY_DROP_RE.sub('', p) for p in out_paras]
    return '\n\n'.join(p for p in out_paras if p)
