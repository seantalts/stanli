"""Long-lived JSON-lines transport for reference shard processes."""

from __future__ import annotations

import collections
import json
import pathlib
import queue
import subprocess
import threading
from typing import Deque, Dict, Mapping, Optional, Sequence


class ProtocolError(RuntimeError):
    pass


_EOF = object()


class JsonLinesClient:
    """One request at a time over a process which serves many cases.

    Reader threads keep timeout handling portable across Linux and macOS;
    `select()` on text wrappers is subtly platform-specific.  The protocol is
    intentionally serial because one reverse-mode autodiff stack serves one
    reference process.  Concurrency happens across shard processes.
    """

    def __init__(self, command: Sequence[str], cwd: Optional[pathlib.Path] = None,
                 timeout: float = 30.0, stderr_lines: int = 80,
                 env: Optional[Mapping[str, str]] = None,
                 label: str = "process"):
        if timeout <= 0:
            raise ValueError("protocol timeout must be positive")
        self.command = tuple(str(x) for x in command)
        self.timeout = timeout
        self.label = label
        try:
            self.process = subprocess.Popen(
                self.command, cwd=str(cwd) if cwd is not None else None,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, bufsize=1,
                env=dict(env) if env is not None else None)
        except OSError as exc:
            raise ProtocolError(f"could not start {self.command[0]}: {exc}") \
                from exc
        self._responses: "queue.Queue[object]" = queue.Queue()
        self._stderr: Deque[str] = collections.deque(maxlen=stderr_lines)
        self._lock = threading.Lock()
        self._next_id = 1
        self._closed = False
        self._stdout_thread = threading.Thread(target=self._read_stdout,
                                               daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr,
                                               daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        try:
            for line in self.process.stdout:
                self._responses.put(line)
        finally:
            self._responses.put(_EOF)

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for line in self.process.stderr:
            self._stderr.append(line.rstrip())

    def _context(self) -> str:
        detail = "\n".join(self._stderr)
        return f"\nstderr:\n{detail}" if detail else ""

    def request(self, payload: Mapping[str, object]) -> Dict[str, object]:
        with self._lock:
            if self._closed:
                raise ProtocolError(f"request on a closed {self.label} process")
            if "request_id" in payload:
                raise ValueError("request_id is owned by JsonLinesClient")
            request_id = self._next_id
            self._next_id += 1
            message = dict(payload)
            message["request_id"] = request_id
            assert self.process.stdin is not None
            try:
                self.process.stdin.write(json.dumps(message, separators=(",", ":"))
                                         + "\n")
                self.process.stdin.flush()
            except (BrokenPipeError, OSError) as exc:
                raise ProtocolError(
                    f"{self.label} process closed its input{self._context()}") \
                    from exc
            try:
                raw = self._responses.get(timeout=self.timeout)
            except queue.Empty as exc:
                raise ProtocolError(
                    f"{self.label} protocol timed out after {self.timeout:g}s"
                    f"{self._context()}") from exc
            if raw is _EOF:
                code = self.process.poll()
                raise ProtocolError(
                    f"{self.label} process exited before replying (exit {code})"
                    f"{self._context()}")
            try:
                response = json.loads(str(raw))
            except json.JSONDecodeError as exc:
                raise ProtocolError(
                    f"{self.label} returned malformed JSON: {str(raw).rstrip()!r}"
                    f"{self._context()}") from exc
            if not isinstance(response, dict):
                raise ProtocolError(
                    f"{self.label} response is not a JSON object")
            if response.get("request_id") != request_id:
                raise ProtocolError(
                    f"{self.label} response ID mismatch: "
                    f"wanted {request_id}, got {response.get('request_id')!r}")
            return response

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self.process.stdin is not None:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        try:
            self.process.wait(timeout=min(self.timeout, 5.0))
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self._stdout_thread.join(timeout=1.0)
        self._stderr_thread.join(timeout=1.0)
        for stream in (self.process.stdout, self.process.stderr):
            if stream is not None:
                stream.close()

    def __enter__(self) -> "JsonLinesClient":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()
