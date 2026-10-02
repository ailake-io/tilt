"""Cliente Python para uma funcao Tilt via RPC.

Requer `tilt` no PATH ou TILT_BIN apontando para o executavel.
"""

import os
import tilt


ARQUIVO = os.path.join("treinamentos", "13_interoperabilidade", "portugues.tilt")


def main() -> None:
    with tilt.carregar(ARQUIVO) as programa:
        print(programa.classificar(120))
        print(programa.chamar_lote("classificar", [[10], [120], [200]]))


if __name__ == "__main__":
    main()
