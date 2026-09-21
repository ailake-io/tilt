import os
import unittest

try:
    from pyspark.sql import SparkSession
except ImportError:  # pragma: no cover - PySpark e opcional
    SparkSession = None

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VENDAS = os.path.join(RAIZ, "exemplos", "interop", "vendas.tilt")


@unittest.skipIf(SparkSession is None, "pyspark nao instalado")
class TiltNoSpark(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Os workers Python do Spark precisam achar o pacote e o executavel.
        pacote = os.path.join(RAIZ, "python")
        os.environ["PYTHONPATH"] = pacote + os.pathsep + os.environ.get("PYTHONPATH", "")
        os.environ.pop("SPARK_HOME", None)
        import sys

        os.environ.setdefault("PYSPARK_PYTHON", sys.executable)
        cls.spark = (
            SparkSession.builder.master("local[2]")
            .config("spark.ui.enabled", "false")
            .config("spark.sql.shuffle.partitions", "2")
            .getOrCreate()
        )
        cls.spark.sparkContext.setLogLevel("ERROR")
        cls.df = cls.spark.createDataFrame(
            [("sul", 30), ("norte", 120), ("sul", 5), ("leste", 60)],
            "regiao string, valor long",
        )

    @classmethod
    def tearDownClass(cls):
        cls.spark.stop()

    def test_transformar(self):
        from tilt.spark import transformar

        out = transformar(self.df, VENDAS, "enriquecer", "regiao string, valor long, faixa string")
        faixas = {(r.regiao, r.valor): r.faixa for r in out.collect()}
        self.assertEqual(faixas[("norte", 120)], "alto")
        self.assertEqual(faixas[("leste", 60)], "medio")
        self.assertEqual(faixas[("sul", 5)], "baixo")

    def test_por_grupo(self):
        from tilt.spark import por_grupo

        out = por_grupo(
            self.df, "regiao", VENDAS, "total_por_regiao",
            "regiao string, total long, pedidos long",
        )
        totais = {r.regiao: (r.total, r.pedidos) for r in out.collect()}
        self.assertEqual(totais, {"sul": (35, 2), "norte": (120, 1), "leste": (60, 1)})

    def test_coluna(self):
        from tilt.spark import coluna

        out = self.df.withColumn("faixa", coluna(VENDAS, "classificar", "string")("valor"))
        self.assertEqual(
            sorted((r.valor, r.faixa) for r in out.collect()),
            [(5, "baixo"), (30, "baixo"), (60, "medio"), (120, "alto")],
        )


if __name__ == "__main__":
    unittest.main()
