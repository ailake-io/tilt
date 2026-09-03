#!/usr/bin/env python3
"""Drives `tilt lsp` over stdio and checks initialize / diagnostics / completion."""
import json
import subprocess
import sys


def frame(obj):
    b = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b


def parse_frames(data: bytes):
    out, i = [], 0
    text = data.decode("utf-8", "replace")
    while True:
        j = text.find("\r\n\r\n", i)
        if j < 0:
            break
        header = text[i:j]
        try:
            n = int(header.split("Content-Length:")[1].split("\r\n")[0].strip())
        except (IndexError, ValueError):
            break
        out.append(json.loads(text[j + 4 : j + 4 + n]))
        i = j + 4 + n
    return out


def main() -> int:
    binary = sys.argv[1]
    bad = "tipo M:\n  x: tensor[f33, 4]\n\n"
    msgs = b"".join(
        [
            frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}),
            frame(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {"uri": "file:///t.tilt", "text": bad}},
                }
            ),
            frame(
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": "file:///t.tilt"},
                        "position": {"line": 2, "character": 0},
                    },
                }
            ),
            frame({"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}}),
            frame({"jsonrpc": "2.0", "method": "exit"}),
        ]
    )
    proc = subprocess.run([binary, "lsp"], input=msgs, capture_output=True, timeout=15)
    if proc.returncode != 0:
        print("lsp exit code", proc.returncode)
        return 1

    frames = parse_frames(proc.stdout)
    ok = True

    init = next((f for f in frames if f.get("id") == 1), None)
    if not init or "completionProvider" not in init.get("result", {}).get("capabilities", {}):
        print("initialize sem completionProvider")
        ok = False

    diags = next(
        (f for f in frames if f.get("method") == "textDocument/publishDiagnostics"), None
    )
    codes = [d["code"] for d in diags["params"]["diagnostics"]] if diags else []
    if "T034" not in codes:
        print("esperava diagnostico T034, veio", codes)
        ok = False

    comp = next((f for f in frames if f.get("id") == 2), None)
    labels = [i["label"] for i in comp["result"]["items"]] if comp else []
    if "modelo" not in labels:
        print("completion sem 'modelo':", labels[:10])
        ok = False

    print("lsp_test ok" if ok else "lsp_test FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
