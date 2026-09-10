#!/usr/bin/env python3
"""Drives `tilt lsp` over stdio and checks initialize / diagnostics / completion
/ hover / definition / signatureHelp / formatting."""
import json
import subprocess
import sys


def frame(obj):
    b = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b


def parse_frames(data: bytes):
    out, i = [], 0
    while True:
        j = data.find(b"\r\n\r\n", i)
        if j < 0:
            break
        header = data[i:j]
        try:
            n = int(header.split(b"Content-Length:")[1].split(b"\r\n")[0].strip())
        except (IndexError, ValueError):
            break
        out.append(json.loads(data[j + 4 : j + 4 + n]))
        i = j + 4 + n
    return out


def run_lsp(binary, doc, requests):
    """`requests` is a list of request dicts; they are framed and sent after didOpen."""
    last_id = 1
    for r in requests:
        if "id" in r:
            last_id = r["id"]
    msgs = [
        frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}),
        frame(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {"textDocument": {"uri": "file:///t.tilt", "text": doc}},
            }
        ),
    ]
    msgs += [frame(r) for r in requests]
    msgs.append(frame({"jsonrpc": "2.0", "id": last_id + 1, "method": "shutdown", "params": {}}))
    msgs.append(frame({"jsonrpc": "2.0", "method": "exit"}))
    proc = subprocess.run([binary, "lsp"], input=b"".join(msgs), capture_output=True, timeout=15)
    if proc.returncode != 0:
        print("lsp exit code", proc.returncode)
        return None
    return parse_frames(proc.stdout)


def by_id(frames):
    return {f.get("id"): f for f in frames if "id" in f}


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
    problems = []

    init = next((f for f in frames if f.get("id") == 1), None)
    caps = init.get("result", {}).get("capabilities", {}) if init else {}
    for key in (
        "completionProvider",
        "hoverProvider",
        "definitionProvider",
        "documentFormattingProvider",
        "signatureHelpProvider",
    ):
        if key not in caps:
            problems.append(f"initialize sem {key}")

    diags = next(
        (f for f in frames if f.get("method") == "textDocument/publishDiagnostics"), None
    )
    codes = [d["code"] for d in diags["params"]["diagnostics"]] if diags else []
    if "T034" not in codes:
        problems.append(f"esperava diagnostico T034, veio {codes}")

    comp = next((f for f in frames if f.get("id") == 2), None)
    labels = [i["label"] for i in comp["result"]["items"]] if comp else []
    if "modelo" not in labels:
        problems.append(f"completion sem 'modelo': {labels[:10]}")

    # ---------------------------------------------------------------- hover
    doc = (
        'seja dados = ler_csv "x.csv"\n'
        "\n"
        "pipeline etl:\n"
        "  passos:\n"
        "    - base = ler_csv \"a.csv\"\n"
        "    - resultado = base.filtrar linha -> verdadeiro\n"
        "\n"
        "funcao dobro x:\n"
        "  retornar x * 2\n"
        "\n"
        "seja y = dobro 4\n"
        "imprimir y\n"
    )

    def hover_req(i, line, ch):
        return {
            "jsonrpc": "2.0",
            "id": i,
            "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": "file:///t.tilt"},
                "position": {"line": line, "character": ch},
            },
        }

    frames = run_lsp(
        binary,
        doc,
        [
            hover_req(10, 0, 15),  # ler_csv (builtin)
            hover_req(11, 2, 2),  # pipeline (keyword)
            hover_req(12, 5, 28),  # filtrar (method)
            hover_req(13, 11, 9),  # y (nome declarado)
        ],
    )
    if frames is None:
        return 1
    r = by_id(frames)

    h = (r.get(10, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "ler_csv" not in h or "tabela" not in h:
        problems.append(f"hover builtin sem doc: {h[:80]!r}")
    h = (r.get(11, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "pipeline" not in h:
        problems.append(f"hover keyword vazio: {h[:80]!r}")
    h = (r.get(12, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "filtrar" not in h or "tabela" not in h:
        problems.append(f"hover metodo vazio: {h[:80]!r}")
    h = (r.get(13, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "seja y" not in h:
        problems.append(f"hover nome declarado vazio: {h[:80]!r}")

    # ----------------------------------------------------------- definition
    def def_req(i, line, ch):
        return {
            "jsonrpc": "2.0",
            "id": i,
            "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": "file:///t.tilt"},
                "position": {"line": line, "character": ch},
            },
        }

    frames = run_lsp(
        binary,
        doc,
        [
            def_req(20, 11, 9),  # y -> `seja y` (linha 10, char 5)
            def_req(21, 2, 12),  # etl -> `pipeline etl:` (linha 2, char 9)
            def_req(22, 8, 11),  # x -> parametro de dobro (linha 7, char 13)
        ],
    )
    if frames is None:
        return 1
    r = by_id(frames)

    d = r.get(20, {}).get("result") or {}
    if d.get("range", {}).get("start") != {"line": 10, "character": 5}:
        problems.append(f"definition de y: {d}")
    d = r.get(21, {}).get("result") or {}
    if d.get("range", {}).get("start") != {"line": 2, "character": 9}:
        problems.append(f"definition de pipeline etl: {d}")
    d = r.get(22, {}).get("result") or {}
    if d.get("range", {}).get("start") != {"line": 7, "character": 13}:
        problems.append(f"definition de parametro x: {d}")

    # -------------------------------------------------------- signatureHelp
    sig_doc = 'seja t = dividir "a,b,c", ","\n'
    frames = run_lsp(
        binary,
        sig_doc,
        [
            {
                "jsonrpc": "2.0",
                "id": 30,
                "method": "textDocument/signatureHelp",
                "params": {
                    "textDocument": {"uri": "file:///t.tilt"},
                    "position": {"line": 0, "character": 30},
                },
            },
        ],
    )
    if frames is None:
        return 1
    r = by_id(frames)
    sig = r.get(30, {}).get("result") or {}
    labels = [s.get("label") for s in sig.get("signatures", [])]
    if "dividir(texto, separador)" not in labels:
        problems.append(f"signatureHelp sem assinatura de dividir: {labels}")
    elif sig.get("activeParameter") != 1:
        problems.append(f"signatureHelp activeParameter != 1: {sig.get('activeParameter')}")

    # ------------------------------------------------------------ formatting
    broken = (
        "pipeline etl:\n"
        "      passos:\n"
        "        - dados = ler_csv \"a.csv\"\n"
        "        - resultado = dados.filtrar linha -> verdadeiro\n"
        "    imprimir \"fim\"  \n"
        "\n"
        "funcao dobro x:\n"
        "        retornar x * 2\n"
    )

    def fmt_req(i):
        return {
            "jsonrpc": "2.0",
            "id": i,
            "method": "textDocument/formatting",
            "params": {
                "textDocument": {"uri": "file:///t.tilt"},
                "options": {"tabSize": 2, "insertSpaces": True},
            },
        }

    frames = run_lsp(binary, broken, [fmt_req(40)])
    if frames is None:
        return 1
    r = by_id(frames)
    edits = r.get(40, {}).get("result") or []
    if not edits:
        problems.append("formatting nao retornou edicoes")
        formatted = broken
    else:
        formatted = edits[0]["newText"]

    if "  passos:" not in formatted:
        problems.append(f"formatting nao normalizou indentacao: {formatted[:60]!r}")
    if any(line.endswith((" ", "\t")) for line in formatted.split("\n")):
        problems.append("formatting deixou espaco trailing")
    if not formatted.endswith("\n") or formatted.endswith("\n\n"):
        problems.append("formatting sem newline final unico")

    frames = run_lsp(binary, formatted, [fmt_req(41)])
    if frames is None:
        return 1
    r = by_id(frames)
    edits2 = r.get(41, {}).get("result")
    if edits2 not in (None, []):
        again = edits2[0]["newText"]
        if again != formatted:
            problems.append("formatting nao e idempotente")

    for p in problems:
        print(p)
    print("lsp_test ok" if not problems else "lsp_test FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
