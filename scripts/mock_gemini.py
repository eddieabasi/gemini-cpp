#!/usr/bin/env python3
"""A tiny stand-in for the Gemini REST API, for offline end-to-end tests of gemini-cpp.

It implements just enough of the API surface used by the client:

  POST /v1beta/models/{model}:generateContent
  POST /v1beta/models/{model}:streamGenerateContent?alt=sse
  POST /v1beta/models/{model}:countTokens
  GET  /v1beta/models

Behaviour:
  * Requests without an `x-goog-api-key` header get HTTP 401.
  * The very first generate call returns HTTP 503 to exercise client retries.
  * When the prompt matches a bundled eval task, the reply contains that task's
    reference.hpp. For --buggy-task ID, the first attempt is deliberately broken so the
    harness's repair loop is exercised; the repair round then gets the reference.
  * Otherwise the reply echoes the prompt, streamed in a few chunks.

Usage:
  python3 scripts/mock_gemini.py --port 8765 --tasks tasks --buggy-task lru-cache
  GEMINI_API_KEY=test GEMINI_BASE_URL=http://127.0.0.1:8765/v1beta gemini-cpp eval --repair 1
"""

import argparse
import json
import pathlib
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"calls": 0, "lock": threading.Lock()}


def load_tasks(root):
    tasks = []
    for d in sorted(pathlib.Path(root).iterdir()):
        manifest, ref = d / "task.json", d / "reference.hpp"
        if manifest.exists() and ref.exists():
            meta = json.loads(manifest.read_text())
            prompt = meta["prompt"]
            if isinstance(prompt, list):
                prompt = "\n".join(prompt)
            tasks.append({"id": meta["id"], "first_line": prompt.splitlines()[0], "reference": ref.read_text()})
    return tasks


def response(text, finish="STOP"):
    return {
        "candidates": [{"content": {"role": "model", "parts": [{"text": text}]}, "finishReason": finish, "index": 0}],
        "usageMetadata": {"promptTokenCount": 12, "candidatesTokenCount": max(1, len(text) // 4),
                          "totalTokenCount": 12 + max(1, len(text) // 4)},
        "modelVersion": "mock-gemini",
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "mock-gemini/1.0"

    def log_message(self, fmt, *args):  # quiet
        pass

    def _json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorised(self):
        if not self.headers.get("x-goog-api-key"):
            self._json(401, {"error": {"code": 401, "message": "API key missing", "status": "UNAUTHENTICATED"}})
            return False
        return True

    def do_GET(self):
        if not self._authorised():
            return
        if self.path.startswith("/v1beta/models"):
            self._json(200, {"models": [{"name": "models/mock-gemini", "displayName": "Mock Gemini",
                                         "inputTokenLimit": 1048576, "outputTokenLimit": 65536,
                                         "supportedGenerationMethods": ["generateContent", "countTokens"]}]})
        else:
            self._json(404, {"error": {"code": 404, "message": "not found", "status": "NOT_FOUND"}})

    def do_POST(self):
        if not self._authorised():
            return
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length) or b"{}")

        if ":countTokens" in self.path:
            inner = request.get("generateContentRequest", request)
            words = sum(len(p.get("text", "").split()) for c in inner.get("contents", []) for p in c.get("parts", []))
            self._json(200, {"totalTokens": words})
            return

        with STATE["lock"]:
            STATE["calls"] += 1
            first_call = STATE["calls"] == 1
        if first_call:
            self._json(503, {"error": {"code": 503, "message": "The model is overloaded.", "status": "UNAVAILABLE"}})
            return

        text = self.server.reply_for(request)
        if ":streamGenerateContent" in self.path:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            pieces = [text[i:i + 40] for i in range(0, len(text), 40)] or [""]
            for i, piece in enumerate(pieces):
                chunk = response(piece, "STOP" if i == len(pieces) - 1 else "")
                if i != len(pieces) - 1:
                    del chunk["usageMetadata"]
                self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\r\n\r\n")
                self.wfile.flush()
        elif ":generateContent" in self.path:
            self._json(200, response(text))
        else:
            self._json(404, {"error": {"code": 404, "message": "unknown method", "status": "NOT_FOUND"}})


class MockServer(ThreadingHTTPServer):
    def __init__(self, addr, tasks, buggy):
        super().__init__(addr, Handler)
        self.tasks = tasks
        self.buggy = set(buggy)

    def reply_for(self, request):
        contents = request.get("contents", [])
        user_texts = [p.get("text", "") for c in contents if c.get("role") == "user" for p in c.get("parts", [])]
        first = user_texts[0] if user_texts else ""
        is_repair = len(user_texts) > 1 and user_texts[-1].startswith("Your solution")
        for task in self.tasks:
            if first.startswith(task["first_line"]):
                code = task["reference"]
                if task["id"] in self.buggy and not is_repair:
                    code += "\n#error intentionally broken first attempt\n"
                return "Here is my solution:\n\n```cpp\n" + code + "```\n"
        return "mock reply to: " + (user_texts[-1] if user_texts else "")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--tasks", default="tasks")
    parser.add_argument("--buggy-task", action="append", default=[])
    args = parser.parse_args()
    server = MockServer(("127.0.0.1", args.port), load_tasks(args.tasks), args.buggy_task)
    print(f"mock Gemini listening on http://127.0.0.1:{args.port}/v1beta", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
