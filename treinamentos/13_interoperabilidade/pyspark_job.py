"""Exemplo de adaptador PySpark para funções Tilt.

Execute em um ambiente que já tenha pyspark e o pacote local `python/`.
"""

from pyspark.sql import SparkSession

from tilt.spark import transformar


def main() -> None:
    spark = SparkSession.builder.appName("tilt-treinamento").getOrCreate()
    dados = spark.createDataFrame(
        [("sul", 120), ("norte", 40)], ["regiao", "valor"]
    )
    saida = transformar(
        dados,
        "treinamentos/13_interoperabilidade/portugues.tilt",
        "enriquecer",
        "regiao string, valor long, faixa string",
    )
    saida.show()
    spark.stop()


if __name__ == "__main__":
    main()
