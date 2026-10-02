# Spill de agregação — 30/09/2026

Dataset CSV sintético com 2.000.000 de linhas e 500.000 grupos (`id`,
`valor`). O benchmark executou três repetições no binário Release, com
`TILT_ANALYTIC_ENGINE=nativo`; o spill foi ativado somente com
`TILT_AGG_SPILL=1`.

| caminho | mediana (ms) | mediana RSS (KiB) |
| --- | ---: | ---: |
| in-memory | 2.843,8 | 1.675.064 |
| spill externo | 1.903,0 | 409.496 |

Neste caso o spill reduziu o pico de RSS em aproximadamente 75,6% e o tempo em
aproximadamente 33,1%. O resultado inclui leitura CSV e agregação; o ganho deve
ser repetido em outras cardinalidades antes de tornar o modo automático.
