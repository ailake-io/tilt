# Interoperabilidade: Python, PySpark e Kof

O arquivo `portugues.tilt` reúne três pontes:

- `chamar_python` inicia uma função Python em subprocesso e troca dados por
  JSON. O exemplo usa somente `math`, portanto não exige NumPy.
- `spark_sql` fala com Livy e aceita `lingua: "pyspark"`. É necessário um
  servidor Livy/Spark em `http://localhost:8998`.
- `http_post_json` consome um serviço Kof por HTTP. O cliente Kof também pode
  chamar o Tilt por `tilt rpc`.

As mesmas ideias estão em `english.tilt`. A diretiva `# language: en` força os
aliases ingleses quando um arquivo mistura nomes de domínio com palavras
reservadas.

## Python local

```bash
python -m pip install -e python
python treinamentos/13_interoperabilidade/python_cliente.py
```

## PySpark

```bash
PYTHONPATH=python spark-submit treinamentos/13_interoperabilidade/pyspark_job.py
```

O adaptador sobe um processo `tilt rpc` por partição e devolve uma tabela para
o DataFrame. Em cluster, distribua o executável `tilt`, o pacote `python/` e
este diretório com `--py-files`/`--files`.

## Kof e RPC HTTP

Em um terminal:

```bash
build/release/bin/tilt rpc treinamentos/13_interoperabilidade/portugues.tilt --porta 8091
```

Em outro, execute o cliente com o toolchain Kof:

```bash
kof run treinamentos/13_interoperabilidade/kof_cliente.kf
```

Também é possível iniciar o serviço Kof existente em
`exemplos/interop/kof/servico/servico.kf` e executar o pipeline `kof_http`.

Todos os `.tilt` desta pasta passam por `tilt checar`. As pontes externas são
testadas apenas quando Python, Spark/Livy ou Kof estão instalados e disponíveis.
