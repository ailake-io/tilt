import os
import unittest

import tilt

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VENDAS = os.path.join(RAIZ, "exemplos", "interop", "vendas.tilt")

LINHAS = [
    {"regiao": "sul", "valor": 30},
    {"regiao": "norte", "valor": 120},
    {"regiao": "sul", "valor": 5},
]


class ClienteTilt(unittest.TestCase):
    def setUp(self):
        self.prog = tilt.carregar(VENDAS)

    def tearDown(self):
        self.prog.fechar()

    def test_banner(self):
        self.assertIn("classificar", self.prog.funcoes)
        self.assertEqual(self.prog.funcoes["so_altos"], ["linhas", "minimo"])
        self.assertEqual(self.prog.pipelines, ["demo"])

    def test_chamada_simples_e_atributo(self):
        self.assertEqual(self.prog.classificar(120), "alto")
        self.assertEqual(self.prog.chamar("classificar", 60), "medio")

    def test_tabela_ida_e_volta(self):
        self.assertEqual(
            self.prog.total_por_regiao(LINHAS),
            [
                {"regiao": "sul", "total": 35, "pedidos": 2},
                {"regiao": "norte", "total": 120, "pedidos": 1},
            ],
        )

    def test_nomeados(self):
        self.assertEqual(self.prog.so_altos(LINHAS, minimo=20)[0]["valor"], 30)
        self.assertEqual(len(self.prog.so_altos(LINHAS)), 1)  # padrao 50

    def test_decimal_continua_decimal(self):
        res = self.prog.enriquecer([{"valor": 100.5}, {"valor": 1}])
        self.assertEqual([r["faixa"] for r in res], ["alto", "baixo"])
        self.assertIsInstance(res[0]["valor"], float)
        self.assertIsInstance(res[1]["valor"], int)

    def test_lote(self):
        self.assertEqual(
            self.prog.chamar_lote("classificar", [[10], [60], [200]]),
            ["baixo", "medio", "alto"],
        )

    def test_saida_impressa(self):
        res, saida = self.prog.chamar_com_saida("saudar", "Ana")
        self.assertEqual(res, "ola, Ana")
        self.assertEqual(saida, "saudando Ana\n")

    def test_erro_nao_derruba_o_processo(self):
        with self.assertRaises(tilt.TiltErro) as ctx:
            self.prog.falhar()
        self.assertIn("sempre falha", str(ctx.exception))
        self.assertEqual(self.prog.classificar(1), "baixo")  # continua vivo

    def test_funcao_inexistente(self):
        with self.assertRaises(AttributeError):
            self.prog.nao_existe
        with self.assertRaises(tilt.TiltErro):
            self.prog.chamar("nao_existe")

    def test_pipeline(self):
        self.assertIn("pipeline demo rodou", self.prog.executar_pipeline("demo"))

    def test_tipos_estranhos(self):
        with self.assertRaises(TypeError):
            self.prog.classificar(object())
        # NaN vira nulo (JSON nao tem NaN)
        self.assertEqual(self.prog.classificar(float("nan")), "baixo")

    def test_uma_chamada(self):
        self.assertEqual(tilt.chamar(VENDAS, "classificar", 60), "medio")

    def test_arquivo_com_erro(self):
        with self.assertRaises(tilt.TiltErro):
            tilt.carregar(os.path.join(RAIZ, "nao_existe.tilt"))

    def test_contexto(self):
        with tilt.carregar(VENDAS) as p:
            self.assertTrue(p.ping())
        with self.assertRaises(tilt.TiltErro):
            p.ping()


if __name__ == "__main__":
    unittest.main()
