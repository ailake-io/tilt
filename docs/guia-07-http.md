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

Servidor TCP bloqueante, uma requisição por vez, `Connection: close`. epoll +
keep-alive + arena por requisição chegam numa próxima passada. Log
determinístico:

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
