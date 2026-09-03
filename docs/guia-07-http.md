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
- `meio:` (middleware) é reconhecido com nota; a implementação chega adiante.

## Subir o serviço

```bash
tilt servir servico.tilt --porta 8080
tilt servir servico.tilt --porta 8080 --requisicoes 3   # atende 3 e sai (testes)
```

No Linux, o servidor usa epoll com sockets não-bloqueantes: várias conexões
simultâneas (uma cliente lenta não trava as outras), HTTP/1.1 com keep-alive
(`Connection: close` honrado), escrita não-bloqueante, timeout de ociosidade
de 30 s e uma `TiltArena` de scratch por requisição — resetada assim que a
resposta é despachada. O parsing de headers aloca nessa arena. Em outros
sistemas, um fallback bloqueante atende uma conexão por vez.

As rotas ainda executam em série, na thread do event loop (o interpretador
não é reentrante). Log determinístico:

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
