#!/usr/bin/env python3
"""Drives `tilt lsp` over stdio and checks initialize / diagnostics / completion
/ hover / definition / references / signatureHelp / formatting."""
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
        "referencesProvider",
        "renameProvider",
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

    # ------------------------------------------------- hover com tipos (S3.4)
    typedoc = (
        "tipo Pedido:\n"
        "  nome: texto\n"
        "  qtd: inteiro = 1\n"
        "\n"
        "funcao soma a: inteiro, b: inteiro -> inteiro:\n"
        "  retornar a + b\n"
        "\n"
        "pipeline p:\n"
        "  passos:\n"
        "    - total = soma(2, 3)\n"
        "    - imprimir total\n"
    )
    tframes = run_lsp(
        binary,
        typedoc,
        [
            hover_req(30, 4, 8),  # soma (assinatura com tipos)
            hover_req(31, 0, 6),  # Pedido (campos do tipo)
            hover_req(32, 4, 12),  # a (parametro tipado)
            hover_req(33, 9, 8),  # total (variavel sem anotacao: sem tipo)
        ],
    )
    if tframes is None:
        return 1
    tr = by_id(tframes)

    h = (tr.get(30, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "funcao soma(a: inteiro, b: inteiro) -> inteiro" not in h:
        problems.append(f"hover assinatura soma: {h[:160]!r}")
    h = (tr.get(31, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "nome: texto" not in h or "qtd: inteiro" not in h:
        problems.append(f"hover campos Pedido: {h[:160]!r}")
    h = (tr.get(32, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "a: inteiro" not in h:
        problems.append(f"hover parametro a: {h[:160]!r}")
    h = (tr.get(33, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "variavel total" not in h or "->" in h.split("```")[0]:
        problems.append(f"hover variavel sem tipo vazou tipo: {h[:160]!r}")

    # ------------------------------------------- hover de expressoes (S3.4-D2)
    exprdoc = (
        "pipeline p:\n"
        "  passos:\n"
        "    - x = 2 + 3\n"
        "    - nomes = [\"a\", \"b\"]\n"
        "    - primeiro = nomes[0]\n"
        "    - imprimir x, primeiro\n"
    )
    eframes = run_lsp(
        binary,
        exprdoc,
        [
            hover_req(40, 2, 12),  # 2 + 3 (binaria => inteiro)
            hover_req(41, 5, 15),  # x em `imprimir` (uso => valor: inteiro)
            hover_req(42, 5, 18),  # primeiro em `imprimir` (uso => valor: texto)
        ],
    )
    if eframes is None:
        return 1
    er = by_id(eframes)

    h = (er.get(40, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "`inteiro`" not in h:
        problems.append(f"hover expr 2+3: {h[:160]!r}")
    h = (er.get(41, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "valor: inteiro" not in h:
        problems.append(f"hover uso de x: {h[:160]!r}")
    h = (er.get(42, {}).get("result") or {}).get("contents", {}).get("value", "")
    if "valor: texto" not in h:
        problems.append(f"hover uso de primeiro: {h[:160]!r}")

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

    def refs_req(i, line, ch, include_declaration):
        return {
            "jsonrpc": "2.0",
            "id": i,
            "method": "textDocument/references",
            "params": {
                "textDocument": {"uri": "file:///t.tilt"},
                "position": {"line": line, "character": ch},
                "context": {"includeDeclaration": include_declaration},
            },
        }

    def rename_req(i, line, ch, new_name):
        return {
            "jsonrpc": "2.0",
            "id": i,
            "method": "textDocument/rename",
            "params": {
                "textDocument": {"uri": "file:///t.tilt"},
                "position": {"line": line, "character": ch},
                "newName": new_name,
            },
        }

    frames = run_lsp(
        binary,
        doc,
        [
            def_req(20, 11, 9),  # y -> `seja y` (linha 10, char 5)
            def_req(21, 2, 12),  # etl -> `pipeline etl:` (linha 2, char 9)
            def_req(22, 8, 11),  # x -> parametro de dobro (linha 7, char 13)
            refs_req(23, 8, 11, True),  # x: parametro + uso no retornar
            refs_req(24, 11, 9, False),  # y: somente uso
            rename_req(25, 11, 9, "resultado"),  # y -> resultado em dois locais
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

    refs = r.get(23, {}).get("result") or []
    starts = [ref.get("range", {}).get("start") for ref in refs]
    if starts != [{"line": 7, "character": 13}, {"line": 8, "character": 11}]:
        problems.append(f"references de x: {refs}")
    refs = r.get(24, {}).get("result") or []
    starts = [ref.get("range", {}).get("start") for ref in refs]
    if starts != [{"line": 11, "character": 9}]:
        problems.append(f"references de y sem declaracao: {refs}")
    rename = r.get(25, {}).get("result") or {}
    edits = rename.get("changes", {}).get("file:///t.tilt", [])
    if len(edits) != 2 or any(edit.get("newText") != "resultado" for edit in edits):
        problems.append(f"rename de y: {rename}")

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

    # ------------------------------------------- formatting preserva estilo tab
    # Arquivo no estilo tab (1ª linha indentada com tab): formatação deve
    # normalizar para tabs (1 por nível), nunca converter para espaços.
    tabbed = (
        "pipeline etl:\t\n"
        "\tpassos:\n"
        "\t\t- x = 1  \n"
        "\t\t- se x == 1:\n"
        "\t\t\timprimir \"um\"\n"
    )
    esperado_tab = (
        "pipeline etl:\n"
        "\tpassos:\n"
        "\t\t- x = 1\n"
        "\t\t- se x == 1:\n"
        "\t\t\timprimir \"um\"\n"
    )

    frames = run_lsp(binary, tabbed, [fmt_req(50)])
    if frames is None:
        return 1
    r = by_id(frames)
    edits_tab = r.get(50, {}).get("result") or []
    if not edits_tab:
        problems.append("formatting (tab) nao retornou edicoes")
        formatted_tab = tabbed
    else:
        formatted_tab = edits_tab[0]["newText"]

    if formatted_tab != esperado_tab:
        problems.append(f"formatting converteu tab para espacos: {formatted_tab[:60]!r}")
    if any(line.endswith((" ", "\t")) for line in formatted_tab.split("\n")):
        problems.append("formatting (tab) deixou espaco trailing")

    frames = run_lsp(binary, formatted_tab, [fmt_req(51)])
    if frames is None:
        return 1
    r = by_id(frames)
    edits_tab2 = r.get(51, {}).get("result")
    if edits_tab2 not in (None, []):
        again_tab = edits_tab2[0]["newText"]
        if again_tab != formatted_tab:
            problems.append("formatting (tab) nao e idempotente")

    for p in problems:
        print(p)
    print("lsp_test ok" if not problems else "lsp_test FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
