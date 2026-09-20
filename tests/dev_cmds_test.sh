#!/usr/bin/env sh
# `tilt novo`, `tilt testar` e `tilt formatar` de ponta a ponta, em um diretorio
# temporario: projeto novo passa nos proprios testes; testes que falham dao
# exit 1 com a linha e a saida capturada; --filtro seleciona; formatar
# normaliza espacos, respeita texto """...""" e e idempotente.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
fail=0
ck() { # ck <descricao> <comando...>; falha se o comando falhar
  desc=$1; shift
  "$@" >/dev/null 2>&1 || { echo "FALHA: $desc"; fail=1; }
}

# --- novo
"$BIN" novo app >/dev/null
for f in principal.tilt testes.tilt README.md .gitignore; do
  [ -f "app/$f" ] || { echo "FALHA: novo nao criou $f"; fail=1; }
done
"$BIN" novo app >/dev/null 2>&1 && { echo "FALHA: novo sobrescreveu projeto existente"; fail=1; }
"$BIN" novo "nome invalido" >/dev/null 2>&1 && { echo "FALHA: novo aceitou nome invalido"; fail=1; }
out=$(cd app && "$BIN" executar principal.tilt)
echo "$out" | grep -q "Ola, mundo!" || { echo "FALHA: projeto novo nao roda: $out"; fail=1; }

# --- testar: projeto novo passa
out=$(cd app && "$BIN" testar) || { echo "FALHA: testes do projeto novo falharam: $out"; fail=1; }
echo "$out" | grep -q "2 teste(s): 2 ok, 0 falhou" || { echo "FALHA: resumo: $out"; fail=1; }

# --- testar: falhas
cat > app/quebrado_teste.tilt <<'EOF2'
funcao dobro x:
  retornar x * 2

teste passa:
  passos:
    - afirmar dobro(2) == 4

teste falha_igual:
  passos:
    - imprimir "debug antes"
    - afirmar_igual(dobro(2), 5)

teste erro_execucao:
  passos:
    - x = raiz(-1)
EOF2
if out=$(cd app && "$BIN" testar quebrado_teste.tilt); then
  echo "FALHA: testar deveria sair com codigo 1"; fail=1
fi
echo "$out" | grep -q "ok      quebrado_teste.tilt::passa" || { echo "FALHA: sem o ok: $out"; fail=1; }
echo "$out" | grep -q "linha 11: afirmar_igual: afirmacao falhou: esperado 5, obtido 4" || {
  echo "FALHA: mensagem de afirmar_igual: $out"; fail=1; }
echo "$out" | grep -q "| debug antes" || { echo "FALHA: saida capturada do teste que falhou: $out"; fail=1; }
echo "$out" | grep -q "linha 15: raiz: raiz de numero negativo" || { echo "FALHA: erro de execucao: $out"; fail=1; }
echo "$out" | grep -q "3 teste(s): 1 ok, 2 falhou(ram)" || { echo "FALHA: resumo de falhas: $out"; fail=1; }
out=$(cd app && "$BIN" testar quebrado_teste.tilt --filtro passa) || { echo "FALHA: --filtro"; fail=1; }
echo "$out" | grep -q "1 teste(s): 1 ok" || { echo "FALHA: resumo do filtro: $out"; fail=1; }

# --- formatar
printf '\n\nfuncao f x:   \n  retornar x  \r\n\n\n\n\npipeline p:\n  passos:\n    - t = """a   \n  b   \n"""   \n    - imprimir t   \n\n\n' > sujo.tilt
"$BIN" formatar --verificar sujo.tilt >/dev/null && { echo "FALHA: --verificar deveria acusar"; fail=1; }
ck "formatar" "$BIN" formatar sujo.tilt
ck "idempotente" "$BIN" formatar --verificar sujo.tilt
grep -q '^  retornar x$' sujo.tilt || { echo "FALHA: espaco no fim nao removido"; fail=1; }
grep -q '^"""$' sujo.tilt || { echo "FALHA: fechamento do texto nao limpo"; fail=1; }
grep -q '^  b   $' sujo.tilt || { echo "FALHA: conteudo do texto triplo nao foi preservado"; fail=1; }
[ "$(head -c1 sujo.tilt)" = "f" ] || { echo "FALHA: linhas em branco no inicio"; fail=1; }
ck "formatado ainda executa" "$BIN" executar sujo.tilt

# --- repl: estado entre linhas, expressao solta imprime, funcao/bloco/lambda, erro
# nao encerra a sessao, :carregar registra as declaracoes de um arquivo
printf 'funcao triplo n:\n  retornar n * 3\n' > util_repl.tilt
out=$(printf '%s\n' 'x = 3' 'x * 2' 'funcao dobro n:' '  retornar n * 2' 'dobro(x + 1)' \
  'se x > 1:' '  imprimir "maior"' 'senao:' '  imprimir "menor"' \
  'f = funcao a: a + x' 'f(10)' 'y_indefinido' 'x + 100' \
  ':carregar util_repl.tilt' 'triplo(4)' ':sair' 'imprimir "depois do sair"' | "$BIN" repl 2>&1) || {
  echo "FALHA: repl saiu com erro: $out"; fail=1; }
printf '%s\n' "$out" > repl.out
[ "$(sed -n 1p repl.out)" = "6" ] || { echo "FALHA: repl x * 2: $out"; fail=1; }
grep -qx '8' repl.out || { echo "FALHA: repl funcao dobro(x + 1): $out"; fail=1; }
grep -qx 'maior' repl.out || { echo "FALHA: repl bloco se/senao: $out"; fail=1; }
grep -qx '13' repl.out || { echo "FALHA: repl lambda capturando x: $out"; fail=1; }
grep -q "nome 'y_indefinido' nao definido" repl.out || { echo "FALHA: repl erro de execucao: $out"; fail=1; }
grep -qx '103' repl.out || { echo "FALHA: repl nao seguiu apos o erro: $out"; fail=1; }
grep -qx '12' repl.out || { echo "FALHA: repl :carregar + triplo(4): $out"; fail=1; }
grep -q 'depois do sair' repl.out && { echo "FALHA: repl executou apos :sair"; fail=1; }

[ "$fail" = 0 ] && echo "dev_cmds_test ok"
exit "$fail"
