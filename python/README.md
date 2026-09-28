# tilt-lang (cliente Python da linguagem Tilt)

Chama funcoes e pipelines de arquivos `.tilt` a partir do Python e do PySpark.
Sem dependencias: fala com `tilt rpc` por JSON-lines.
Para tabelas grandes, `pip install './python[columnar]'` habilita
`vendas.chamar_colunar("funcao", tabela_arrow)`: os dados cruzam o processo
em Parquet temporario e uma `tabela` retornada vira `pyarrow.Table`.

```python
import tilt

vendas = tilt.carregar("vendas.tilt")            # sobe `tilt rpc vendas.tilt`
vendas.classificar(120)                          # 'alto'
vendas.total_por_regiao([{"regiao": "sul", "valor": 30}])
vendas.chamar_lote("classificar", [[10], [60]])  # varias chamadas, uma ida e volta
```

```python
from tilt.spark import transformar, por_grupo, coluna   # PySpark

transformar(df, "vendas.tilt", "enriquecer", "regiao string, valor long, faixa string")
```

Requer o executavel `tilt` no `PATH` (ou `TILT_BIN`). Guia completo:
`docs/guia-17-interoperabilidade.md`.
