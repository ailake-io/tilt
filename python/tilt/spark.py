"""Tilt dentro do PySpark.

Aplica funcoes ``.tilt`` a DataFrames do Spark. Cada particao sobe um processo
``tilt rpc`` e manda as linhas em lotes; a funcao Tilt recebe uma ``tabela``
(lista de objetos) e devolve outra.

    from pyspark.sql import SparkSession
    from tilt.spark import transformar, coluna

    spark = SparkSession.builder.getOrCreate()
    df = spark.createDataFrame([("sul", 30), ("norte", 120)], "regiao string, valor long")

    # tabela -> tabela, lote a lote (mapInPandas)
    ricos = transformar(df, "vendas.tilt", "enriquecer",
                        "regiao string, valor long, faixa string")

    # grupo inteiro -> tabela (groupBy.applyInPandas): agregacoes
    por_grupo(df, "regiao", "vendas.tilt", "total_por_regiao",
              "regiao string, total long, pedidos long")

    # coluna -> coluna (pandas_udf)
    df.withColumn("faixa", coluna("vendas.tilt", "classificar", "string")("valor"))

Os executores precisam do executavel ``tilt`` (``TILT_BIN`` ou no ``PATH``) e do
arquivo ``.tilt`` no mesmo caminho (``spark.sparkContext.addFile`` +
``SparkFiles.get`` em clusters). Ver docs/guia-17-interoperabilidade.md.
"""

from typing import Any, Callable, Dict, Iterator, Optional

from . import Tilt, _para_json

__all__ = ["transformar", "por_grupo", "coluna", "funcao_de_particao"]


def funcao_de_particao(
    arquivo: str,
    funcao: str,
    binario: Optional[str] = None,
    nomeados: Optional[Dict[str, Any]] = None,
) -> Callable[[Iterator[Any]], Iterator[Any]]:
    """Funcao para ``DataFrame.mapInPandas``: um processo Tilt por particao."""
    nomeados = dict(nomeados or {})

    def por_particao(lotes: Iterator[Any]) -> Iterator[Any]:
        import pandas as pd

        with Tilt(arquivo, binario) as prog:
            for pdf in lotes:
                linhas = pdf.to_dict("records")
                resultado = prog.chamar(funcao, linhas, **nomeados)
                yield pd.DataFrame(resultado)

    return por_particao


def transformar(
    df: Any,
    arquivo: str,
    funcao: str,
    schema: Any,
    binario: Optional[str] = None,
    **nomeados: Any,
) -> Any:
    """Aplica ``funcao`` (tabela -> tabela) a cada lote do DataFrame ``df``.

    ``schema`` descreve as colunas do resultado (string DDL ou ``StructType``).
    """
    return df.mapInPandas(funcao_de_particao(arquivo, funcao, binario, nomeados), schema)


def por_grupo(
    df: Any,
    chaves: Any,
    arquivo: str,
    funcao: str,
    schema: Any,
    binario: Optional[str] = None,
    **nomeados: Any,
) -> Any:
    """``df.groupBy(*chaves)``: cada grupo inteiro vira uma tabela para ``funcao``.

    Diferente de :func:`transformar` (que ve so o lote da particao), aqui a
    funcao enxerga todas as linhas do grupo: serve para agregacoes.
    """
    import pandas as pd

    def por_linhas(pdf: Any) -> Any:
        with Tilt(arquivo, binario) as prog:
            return pd.DataFrame(prog.chamar(funcao, pdf.to_dict("records"), **nomeados))

    if isinstance(chaves, str):
        chaves = [chaves]
    return df.groupBy(*chaves).applyInPandas(por_linhas, schema)


def coluna(
    arquivo: str,
    funcao: str,
    tipo_retorno: Any,
    aridade: int = 1,
    binario: Optional[str] = None,
) -> Any:
    """UDF vetorizado: aplica ``funcao`` a cada linha da(s) coluna(s) passada(s).

    ``coluna(a, f, "string", aridade=2)("x", "y")`` chama ``f(x, y)`` por linha,
    em lotes (uma ida e volta ao processo Tilt por lote do Spark).
    """
    import pandas as pd
    from pyspark.sql.functions import pandas_udf

    def rodar(*series: "pd.Series") -> "pd.Series":
        with Tilt(arquivo, binario) as prog:
            lote = list(zip(*[[_para_json(v) for v in s] for s in series]))
            return pd.Series(prog.chamar_lote(funcao, lote), index=series[0].index)

    # O Spark le as anotacoes da assinatura: uma funcao por aridade.
    if aridade == 1:

        def udf1(a: pd.Series) -> pd.Series:
            return rodar(a)

        return pandas_udf(udf1, returnType=tipo_retorno)
    if aridade == 2:

        def udf2(a: pd.Series, b: pd.Series) -> pd.Series:
            return rodar(a, b)

        return pandas_udf(udf2, returnType=tipo_retorno)
    if aridade == 3:

        def udf3(a: pd.Series, b: pd.Series, c: pd.Series) -> pd.Series:
            return rodar(a, b, c)

        return pandas_udf(udf3, returnType=tipo_retorno)
    raise ValueError("aridade suportada: 1, 2 ou 3")
