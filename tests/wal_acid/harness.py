"""
Shared harness for tests/wal_acid/ — branch-independent.

Provides a Server class that launches the ndd binary against a temp data dir
and a small HTTP client. The tests below run unmodified on both `master` and
`single_txn`; each branch's behaviour is what differs, not the test code.
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]


def _find_binary() -> Path:
    candidates = [
        REPO_ROOT / "build-acid" / "ndd-avx2",
        REPO_ROOT / "build" / "ndd-avx2",
        REPO_ROOT / "build-acid" / "ndd",
        REPO_ROOT / "build" / "ndd",
    ]
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    raise RuntimeError(
        "No ndd binary found. Build the server first (e.g. "
        "`cmake --build build-acid --target ndd-avx2`) or set NDD_BINARY."
    )


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


_ROOT_TOKEN = "wal-acid-test-root-token"
_TEST_USER = "endee"


class Server:
    """One ndd process bound to a temp data dir.

    Works with both OSS (no auth) and serverless (auth required) builds.
    Detects the build's auth mode automatically by trying an unauthenticated
    health call after start; if the server rejects it, switches to impersonation
    via NDD_AUTH_TOKEN + the "root/<user>:<token>" header format.
    """

    def __init__(self, data_dir: Path | None = None, binary: Path | None = None) -> None:
        self.data_dir = data_dir or Path(tempfile.mkdtemp(prefix="ndd_wal_acid_"))
        self.data_dir.mkdir(parents=True, exist_ok=True)
        env_binary = os.environ.get("NDD_BINARY")
        self.binary = binary or (Path(env_binary) if env_binary else _find_binary())
        self.port = _free_port()
        self.proc: subprocess.Popen | None = None
        self.log_path = self.data_dir / "server.log"
        # Auth mode is determined at first start. After the first start we
        # know whether the binary is serverless (requires auth) or OSS.
        self._auth_root_token: str | None = None
        self._user_provisioned = False

    # ── lifecycle ──────────────────────────────────────────────────────────

    def start(self, timeout_s: float = 20.0) -> None:
        env = os.environ.copy()
        env["NDD_DATA_DIR"] = str(self.data_dir)
        env["NDD_SERVER_PORT"] = str(self.port)
        env["NDD_MIN_DRAM_MB"] = "256"
        # Serverless builds REQUIRE NDD_AUTH_TOKEN and exit at startup
        # without it. OSS builds ignore the value (auth disabled when
        # AUTH_TOKEN is empty, but a non-empty value just enables a token
        # check we satisfy by sending it). So always set it: it's a no-op
        # on OSS until we send the matching header, and required on
        # serverless. We assume serverless mode if NDD_AUTH_TOKEN is set,
        # which is now always; we then use the impersonation flow.
        env["NDD_AUTH_TOKEN"] = _ROOT_TOKEN
        self._auth_root_token = _ROOT_TOKEN

        log_fd = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [str(self.binary)],
            env=env,
            stdout=log_fd,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self._wait_ready(timeout_s)

        # On first start of this Server object, provision the test user.
        # On subsequent restarts (against the same data dir), the user
        # already exists.
        if not self._user_provisioned:
            self._provision_test_user()
            self._user_provisioned = True

    def stop(self, sig: int = signal.SIGTERM, timeout_s: float = 10.0) -> None:
        if self.proc is None:
            return
        try:
            self.proc.send_signal(sig)
            self.proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.proc = None

    def kill(self) -> None:
        """SIGKILL — no graceful shutdown, simulates a crash."""
        self.stop(sig=signal.SIGKILL, timeout_s=5.0)

    def cleanup(self) -> None:
        shutil.rmtree(self.data_dir, ignore_errors=True)

    def _wait_ready(self, timeout_s: float) -> None:
        deadline = time.monotonic() + timeout_s
        last_err: Exception | None = None
        while time.monotonic() < deadline:
            try:
                # Send root token; OSS will ignore mismatch (auth disabled
                # when its own NDD_AUTH_TOKEN is empty -- we set it but OSS
                # treats this as enabling auth, so the same header works in
                # both modes). Serverless validates against getRootToken().
                code, _ = self.get("/api/v1/health", timeout=0.5)
                if code in (200, 401):
                    # 200 = OSS open mode or serverless allowing root health
                    # 401 = auth required (serverless rejected unauth'd)
                    return
            except Exception as e:
                last_err = e
                time.sleep(0.1)
        raise TimeoutError(f"server on :{self.port} not ready after {timeout_s}s — last={last_err}")

    def _provision_test_user(self) -> None:
        """Create the test user via the admin route. On OSS builds the admin
        routes don't exist (the `serverless/` folder isn't compiled in); the
        404 is treated as "OSS, no user needed" and we proceed unauth'd."""
        code, body = self.post_json(
            "/api/v1/admin/users",
            {"username": _TEST_USER, "user_type": "Admin"},
            auth_header=self._auth_root_token,
        )
        if code == 404:
            # OSS build — admin routes don't exist. Auth is also irrelevant.
            self._auth_root_token = None
            return
        if code == 400 and b"already exist" in body.lower():
            return
        if code == 201:
            return
        raise RuntimeError(f"failed to provision test user: HTTP {code} {body!r}")

    # ── tiny HTTP client ───────────────────────────────────────────────────

    def _url(self, path: str) -> str:
        return f"http://127.0.0.1:{self.port}{path}"

    def _data_auth(self) -> str | None:
        """Auth header for data-plane routes (/api/v1/index/*). Uses
        root impersonation in serverless mode, no header in OSS mode."""
        if self._auth_root_token is None:
            return None
        return f"root/{_TEST_USER}:{self._auth_root_token}"

    def _headers(self, content_type: str | None, auth_header: str | None) -> dict[str, str]:
        h: dict[str, str] = {}
        if content_type is not None:
            h["Content-Type"] = content_type
        # Data-plane impersonation token by default; callers can override
        # with an explicit auth_header for admin routes.
        effective_auth = auth_header if auth_header is not None else self._data_auth()
        if effective_auth is not None:
            h["Authorization"] = effective_auth
        return h

    def get(self, path: str, timeout: float = 5.0) -> tuple[int, bytes]:
        req = urllib.request.Request(
            self._url(path),
            method="GET",
            headers=self._headers(None, self._auth_root_token),  # health uses root token
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def post_json(
        self,
        path: str,
        body: Any,
        timeout: float = 10.0,
        auth_header: str | None = None,
    ) -> tuple[int, bytes]:
        data = json.dumps(body).encode()
        req = urllib.request.Request(
            self._url(path),
            data=data,
            method="POST",
            headers=self._headers("application/json", auth_header),
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def delete(self, path: str, timeout: float = 10.0) -> tuple[int, bytes]:
        req = urllib.request.Request(
            self._url(path),
            method="DELETE",
            headers=self._headers(None, None),
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    # ── helpers used by tests ──────────────────────────────────────────────

    def create_index(self, name: str, dim: int = 2) -> None:
        code, body = self.post_json(
            "/api/v1/index/create",
            {"index_name": name, "dim": dim, "space_type": "cosine"},
        )
        assert code == 200, f"create_index({name}) failed: {code} {body!r}"

    def insert(self, index: str, vector_id: str, vector: list[float]) -> None:
        code, body = self.post_json(
            f"/api/v1/index/{index}/vector/insert",
            [{"id": vector_id, "vector": vector}],
        )
        assert code == 200, f"insert({vector_id}) failed: {code} {body!r}"

    def insert_batch(self, index: str, items: list[tuple[str, list[float]]]) -> None:
        payload = [{"id": vid, "vector": v} for vid, v in items]
        code, body = self.post_json(f"/api/v1/index/{index}/vector/insert", payload)
        assert code == 200, f"insert_batch failed: {code} {body!r}"

    def delete_vector(self, index: str, vector_id: str) -> None:
        code, body = self.delete(f"/api/v1/index/{index}/vector/{vector_id}/delete")
        assert code == 200, f"delete({vector_id}) failed: {code} {body!r}"

    def get_vector_bytes(self, index: str, vector_id: str) -> bytes | None:
        """Returns the raw vector payload (msgpack bytes). None if 404."""
        code, body = self.post_json(
            f"/api/v1/index/{index}/vector/get", {"id": vector_id}
        )
        if code == 404:
            return None
        assert code == 200, f"get({vector_id}) failed: {code} {body!r}"
        return body


# ── reporting helpers ─────────────────────────────────────────────────────


class TestReport:
    def __init__(self, name: str) -> None:
        self.name = name
        self.failures: list[str] = []

    def expect(self, cond: bool, msg: str) -> None:
        if not cond:
            self.failures.append(msg)

    def finalize(self) -> int:
        print(f"\n=== {self.name} ===")
        if self.failures:
            print(f"FAIL — {len(self.failures)} failure(s):")
            for f in self.failures:
                print(f"  - {f}")
            return 1
        print("PASS")
        return 0
