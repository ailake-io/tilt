# Correção da materialização em lote — 26/09/2026

A materialização passa a alocar os mapas de linhas com std::make_shared.
O pool permanece nas fábricas gerais de mapas, listas e tabelas.

| Caso/threads | Pool original (ms) | Após correção (ms) | Variação |
|---|---:|---:|---:|
| materialize/1 | 25.690 | 11.165 | -56.5% |
| materialize/2 | 33.173 | 14.746 | -55.5% |
| materialize/4 | 47.579 | 27.409 | -42.4% |
| join/1 | 72.805 | 78.798 | +8.2% |
| join/2 | 100.582 | 96.093 | -4.5% |
| join/4 | 139.180 | 139.789 | +0.4% |
| lists/1 | 9.093 | 9.171 | +0.9% |
| lists/2 | 18.609 | 18.611 | +0.0% |
| lists/4 | 37.437 | 38.163 | +1.9% |

Mesma configuração de 100 mil linhas por thread, 4 rodadas, 7 amostras e 2 aquecimentos.
Medições antes/depois ocorreram em execuções separadas; variações pequenas podem ser ruído.
O caso de listas não usa a função alterada e funciona como controle de variação entre execuções.
Tempos são medianas por worker da soma criação+destruição, com validação entre as fases.
Ver value-pool-lifecycle-2026-09-26.md para todas as limitações.

## Decisão e próximos passos

- Corrigir apenas a materialização em lote, onde a regressão foi consistente nas três contagens de threads.
- Manter o pool geral por enquanto: os joins se beneficiam e as listas apresentam resultado misto.
- A variante sem sincronização também regrediu na materialização; portanto os dados não sustentam atribuir toda a regressão a locks. Perfil de alocações/cache ainda é necessário para estabelecer a causa.
- Antes de mudar a política de listas, medir listas maiores, durações distintas e criação/destruição em threads diferentes.
