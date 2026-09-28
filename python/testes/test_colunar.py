import os
import tempfile
import unittest

import tilt

try:
    import pyarrow as pa
except ImportError:
    pa = None


@unittest.skipUnless(pa is not None, "pyarrow opcional nao instalado")
class PonteColunar(unittest.TestCase):
    def test_tabela_e_escalar(self):
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "ponte.tilt")
            with open(source, "w", encoding="utf-8") as output:
                output.write(
                    "funcao eco linhas:\n"
                    "  retornar linhas\n"
                    "funcao contar linhas:\n"
                    "  retornar tamanho(linhas)\n"
                )
            rows = [
                {"id": 1, "nome": "ana", "valor": 2.5},
                {"id": 2, "nome": None, "valor": 3.5},
                {"id": 3, "nome": "bia", "valor": None},
            ]
            table = pa.Table.from_pylist(rows)
            with tilt.carregar(source) as program:
                result = program.chamar_colunar("eco", table)
                self.assertIsInstance(result, pa.Table)
                self.assertEqual(result.to_pylist(), rows)
                self.assertEqual(program.chamar_colunar("contar", table), 3)
                self.assertEqual(program.chamar("contar", rows), 3)

                nested = pa.Table.from_pylist([
                    {"id": 1, "tags": [1, 2], "meta": {"score": 3.5}},
                    {"id": 2, "tags": [], "meta": None},
                ])
                self.assertEqual(
                    program.chamar_colunar("eco", nested).to_pylist(), nested.to_pylist()
                )


if __name__ == "__main__":
    unittest.main()
