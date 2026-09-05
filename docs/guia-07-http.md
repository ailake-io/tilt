# 07 — Serviços HTTP

## `servico` / `rota`

```tilt
tipo NovoPedido:
  cliente: texto
  valor: decimal

servico Loja:
  porta: 8080
  rota post "/pedidos":
    entrada: NovoPedido
    passos:
      - imposto = entrada.valor * 0.1
      - responder:
          status: 201
          dados:
            cliente: entrada.cliente
            total: entrada.valor + imposto
  rota get "/saude":
    passos:
      - responder:
          dados:
            status: "ok"
```

- `rota <metodo> "/caminho":` — casa método (`get`/`post`/...) e caminho exatos.
- O corpo JSON da requisição vira a variável `entrada` no escopo dos `passos:`.
- Se a rota declara `entrada: <Tipo>`, campos ausentes → `400 { "erro": "campo 'x' ausente" }`.
- `- responder: status:, dados:` monta a resposta. `dados:` pode ser um bloco
  (vira mapa) ou um valor. Sem `status:`, é `200`.
- Rota não encontrada → `404`. Exceção no handler → `500 { "erro": "..." }`.
- `meio:` (middleware): blocos de passos que rodam antes de cada rota
  casada, no mesmo escopo dela — variáveis atribuídas no `meio:` são
  visíveis nos `passos:` da rota, e um `responder:` no `meio:` aborta a
  rota (a resposta do middleware vence; caso de uso: autenticação):

```tilt
servico Api:
  meio:
    - prefixo = "v1"
    - se entrada?.chave != "segredo":
        responder:
          status: 401
          dados:
            erro: "nao autorizado"
  rota get "/saude":
    passos:
      - responder:
          dados:
            ok: verdadeiro
            versao: prefixo
```

## Subir o serviço

```bash
tilt servir servico.tilt --porta 8080
tilt servir servico.tilt --porta 8080 --requisicoes 3   # atende 3 e sai (testes)
tilt servir servico.tilt --threads 8                    # pool com 8 workers
tilt servir servico.tilt --threads 1                    # rotas em série
```

No Linux, o servidor usa epoll com sockets não-bloqueantes: várias conexões
simultâneas (uma cliente lenta não trava as outras), HTTP/1.1 com keep-alive
(`Connection: close` honrado), escrita não-bloqueante, timeout de ociosidade
de 30 s e uma `TiltArena` de scratch por requisição — resetada assim que a
resposta é despachada. O parsing de headers aloca nessa arena. Em outros
sistemas, um fallback bloqueante atende uma conexão por vez.

### Execução paralela de rotas

Por padrão (`--threads` omitido) o tratamento das rotas roda num pool de
`min(4, núcleos)` workers — rotas de CPU-bound (inferência, ETL, loops)
atendem em paralelo, uma requisição lenta não bloqueia as outras. O event
loop só faz rede: parseia o request, o despacha numerado para a fila do pool
e recebe as respostas de volta via `eventfd`. Cada conexão numera seus
requests por sequência, então respostas de requests pipelined na **mesma**
conexão saem sempre na ordem enviada, mesmo completando fora de ordem.

O interpretador é reentrante nesse modo: a resposta corrente (`responder:`),
o estado de `se`/`senao:` e o dispositivo ativo (`cpu`/`cuda`) são
`thread_local`, e os caches compartilhados (chunks da VM, modelos, índice em
memória, memória de conversa dos agentes) são protegidos por mutex.
`--threads 1` volta ao modo serial (mesmo comportamento de antes).

Log por requisição (uma linha por resposta, ordem de conclusão):

```
servico Loja: escutando 127.0.0.1:8080
POST /pedidos -> 201
GET /saude -> 200
```

## Cliente

```bash
curl -s -XPOST localhost:8080/pedidos -d '{"cliente":"ana","valor":200}'
# { "cliente": "ana", "total": 220 }

curl -s localhost:8080/saude
# { "status": "ok" }
```

Exemplo: [`../exemplos/servico.tilt`](../exemplos/servico.tilt).
