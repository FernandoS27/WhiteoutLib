# SPDX-License-Identifier: BSD-3-Clause
"""Smoke tests for the host-extra bindings: OsFileSystem, SimpleThreadPool,
HttpHandler subclassable trampoline, and SimpleHttpHandler.

Run from the repo root:

    python -m pytest tests/python/test_host.py -q

Pre-requisite: build the extension with `scripts\\build-python.ps1`.
"""

from __future__ import annotations

import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'packages' / 'python'))

import whiteout as w


# ── OsFileSystem ──────────────────────────────────────────────────────────

def test_os_file_system_round_trip(tmp_path: Path) -> None:
    payload = b"\x01\x02\x03\x04\x05"
    (tmp_path / "hello.bin").write_bytes(payload)

    fs = w.OsFileSystem(str(tmp_path))
    assert fs.file_exists("hello.bin")
    assert not fs.file_exists("missing.bin")
    assert fs.read_file("hello.bin") == payload


def test_os_file_system_write(tmp_path: Path) -> None:
    fs = w.OsFileSystem(str(tmp_path))
    payload = b"hello, world"
    # write_file is codegen-produced — its `data` arg takes the opaque
    # VectorU8 (PYBIND11_MAKE_OPAQUE precludes auto py::bytes conversion).
    # Construct one from the buffer-protocol-friendly bytes object.
    vec = w.VectorU8()
    vec.extend(payload)
    assert fs.write_file("out.bin", vec)
    assert (tmp_path / "out.bin").read_bytes() == payload


# ── SimpleThreadPool ──────────────────────────────────────────────────────

def test_simple_thread_pool_reports_thread_count() -> None:
    pool = w.SimpleThreadPool(4)
    assert pool.thread_count() == 4
    pool.wait_idle()


def test_simple_thread_pool_subclass_chain() -> None:
    # Just constructing a pool and passing it through the WorkerPool base
    # is enough to validate the class hierarchy is right (we can't yet pass
    # the pool to any pybind11-bound parser — the Texture/Storage call
    # sites still take the plain default — but the binding hierarchy
    # itself must be correct for that future wiring).
    pool = w.SimpleThreadPool(2)
    assert isinstance(pool, w.WorkerPool)
    pool.wait_idle()


# ── HttpHandler trampoline ────────────────────────────────────────────────

def test_http_handler_subclass_round_trips_response() -> None:
    """A Python subclass receives `(url, complete)` and invokes complete
    with an HttpResponse. We capture the response on the C++ side by
    invoking get_async ourselves (the only available `dispatch` path
    without an actual CDN consumer in this build)."""

    captured: list[w.HttpResponse] = []

    class FakeHandler(w.HttpHandler):
        def capabilities(self) -> int:
            return w.HTTP_CAPABILITY_HTTP2_MULTIPLEXING

        def get_async(self, url, complete):
            complete(w.HttpResponse(
                status_code=200,
                body=b"payload-for-" + url.encode(),
            ))

        def get_range_async(self, url, start, end, complete):
            complete(w.HttpResponse(
                status_code=206,
                body=f"{start}-{end}".encode(),
            ))

    h = FakeHandler()
    assert h.capabilities() == w.HTTP_CAPABILITY_HTTP2_MULTIPLEXING

    h.get_async("https://example/foo", lambda r: captured.append(r))
    assert len(captured) == 1
    assert captured[0].status_code == 200
    assert captured[0].body == b"payload-for-https://example/foo"

    h.get_range_async("https://example/foo", 0, 511,
                      lambda r: captured.append(r))
    assert len(captured) == 2
    assert captured[1].status_code == 206
    assert captured[1].body == b"0-511"


def test_http_handler_default_capabilities() -> None:
    class MinimalHandler(w.HttpHandler):
        def get_async(self, url, complete):
            complete(w.HttpResponse(status_code=204))

        def get_range_async(self, url, start, end, complete):
            complete(w.HttpResponse(status_code=204))

    # No `capabilities` override — default is HTTP_CAPABILITY_NONE.
    h = MinimalHandler()
    assert h.capabilities() == w.HTTP_CAPABILITY_NONE


# ── SimpleHttpHandler ────────────────────────────────────────────────────

def test_simple_http_handler_constructs() -> None:
    # We don't fire a real request (would hit the network and be flaky in
    # CI). Just verify the binding exists and constructs cleanly — and
    # that its capabilities() resolves through the C++ vtable, not the
    # Python trampoline.
    h = w.SimpleHttpHandler(n_threads=2)
    caps = h.capabilities()
    # Should be either NONE or HTTP2_MULTIPLEXING depending on backend.
    assert caps in (w.HTTP_CAPABILITY_NONE,
                    w.HTTP_CAPABILITY_HTTP2_MULTIPLEXING)


# ── HttpResponse value type ──────────────────────────────────────────────

def test_http_response_defaults() -> None:
    r = w.HttpResponse()
    assert r.status_code == 0
    assert r.body == b""
    assert r.error == ""


def test_http_response_field_round_trip() -> None:
    r = w.HttpResponse(status_code=404, body=b"not found", error="missing")
    assert r.status_code == 404
    assert r.body == b"not found"
    assert r.error == "missing"
    r.status_code = 200
    r.body = b"ok"
    r.error = ""
    assert r.status_code == 200
    assert r.body == b"ok"
    assert r.error == ""
