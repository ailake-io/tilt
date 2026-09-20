#include "lsp/completion.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "semantic/checker.hpp"
#include "semantic/type.hpp"

namespace tilt::lsp {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

const std::array<std::string_view, 22> kDeclKeywords = {
    "tipo",       "funcao",      "seja",      "constante", "importar", "de",
    "fonte",      "pipeline",    "verificar", "modelo",    "treino",   "busca",
    "tarefa",     "experimento", "avaliacao", "llm",       "indice",   "fluxo",
    "ferramenta", "agente",      "equipe",    "servico"};

const std::array<std::string_view, 7> kStmtKeywords = {
    "se", "senao", "para cada", "enquanto", "tentar", "capturar", "retornar"};

const std::array<std::string_view, 65> kBuiltins = {"imprimir",
                                                    "registrar",
                                                    "env",
                                                    "tamanho",
                                                    "contar",
                                                    "somar",
                                                    "media",
                                                    "min",
                                                    "max",
                                                    "intervalo",
                                                    "dividir",
                                                    "ler_csv",
                                                    "escrever_csv",
                                                    "ler_json",
                                                    "escrever_json",
                                                    "ler",
                                                    "carregador",
                                                    "perguntar",
                                                    "incorporar",
                                                    "dividir_texto",
                                                    "responder",
                                                    "tensor",
                                                    "zeros",
                                                    "checar_tilt",
                                                    "executar_sql",
                                                    "consultar_sql",
                                                    "transacao",
                                                    "spark_sql",
                                                    "spark_executar",
                                                    "ler_parquet",
                                                    "escrever_parquet",
                                                    "ler_delta",
                                                    "escrever_delta",
                                                    "anexar_delta",
                                                    "ler_iceberg",
                                                    "escrever_iceberg",
                                                    "anexar_iceberg",
                                                    "apagar_iceberg",
                                                    "ler_redis",
                                                    "escrever_redis",
                                                    "redis_executar",
                                                    "redis_lote",
                                                    "ler_kafka",
                                                    "escrever_kafka",
                                                    "transacao_kafka",
                                                    "mongo_inserir",
                                                    "mongo_buscar",
                                                    "mongo_atualizar",
                                                    "mongo_deletar",
                                                    "mongo_criar_indice",
                                                    "mongo_agregar",
                                                    "ler_s3",
                                                    "escrever_s3",
                                                    "listar_s3",
                                                    "apagar_s3",
                                                    "copiar_s3",
                                                    "cabecalho_s3",
                                                    "s3_iniciar_upload",
                                                    "s3_enviar_parte",
                                                    "s3_concluir_upload",
                                                    "s3_abortar_upload",
                                                    "http_get_json",
                                                    "http_post_json",
                                                    "es_buscar",
                                                    "es_executar"};

const std::array<std::string_view, 11> kTableMethods = {
    "filtrar", "derivar",   "mapear",   "agrupar_por", "selecionar", "ordenar_por",
    "limite",  "primeiros", "distinto", "tamanho",     "inserir"};

const std::array<std::string_view, 17> kTensorMethods = {
    "forma", "dados",    "matmul", "transposta", "reformar", "conv2d", "norma_lote", "relu", "gelu",
    "silu",  "sigmoide", "tanh",   "softmax",    "soma",     "media",  "argmax",     "item"};

struct FieldSet {
  std::string_view decl;
  std::vector<std::string_view> fields;
};

const std::vector<FieldSet>& field_sets() {
  static const std::vector<FieldSet> sets = {
      {"tipo", {}},
      {"llm", {"provedor", "modelo", "temperatura", "max_tokens", "chave", "base_url"}},
      {"modelo", {"camadas", "dispositivo", "pesos", "entrada", "arquitetura"}},
      {"treino",
       {"dados", "perda", "otimizador", "epocas", "taxa", "taxa_aprendizado", "lote", "semente",
        "validacao", "parar_cedo", "agendador", "checkpoint", "a_cada", "retomar", "verboso"}},
      {"busca",
       {"modelo", "dados", "perda", "otimizador", "epocas", "taxa", "lote", "semente", "validacao",
        "parar_cedo", "agendador", "grade", "criterio", "verboso"}},
      {"tarefa", {"entrada", "executar"}},
      {"indice", {"embeddings", "armazenamento", "dimensao", "metrica"}},
      {"fonte",
       {"tipo", "caminho", "arquivo", "url", "consulta", "formato", "brokers", "topico", "lingua",
        "conf"}},
      {"pipeline", {"passos", "agenda", "ao_falhar"}},
      {"fluxo", {"entrada", "passos"}},
      {"ferramenta", {"descricao", "entrada", "executar"}},
      {"agente", {"llm", "papel", "ferramentas", "memoria", "max_passos"}},
      {"equipe", {"agentes", "estrategia", "supervisor", "objetivo"}},
      {"servico", {"porta", "dispositivo", "meio", "rota"}},
      {"verificar", {"nao_nulo", "unico", "intervalo", "ao_violar"}},
      {"avaliacao",
       {"dados", "executar", "metricas", "limiar", "tolerancia", "ao_reprovar", "verboso",
        "amostra", "semente", "juiz", "registrar_em"}},
  };
  return sets;
}

void push(std::vector<CompletionItem>& out, std::string_view prefix, std::string_view label,
          const char* kind, std::string detail) {
  if (!prefix.empty() && !starts_with_ci(label, prefix)) return;
  for (const auto& e : out) {
    if (e.label == label) return;
  }
  out.push_back({std::string(label), kind, std::move(detail)});
}

// Walk tokens up to the cursor, tracking the stack of enclosing declaration
// keywords via INDENT/DEDENT.
std::string enclosing_decl(const std::vector<Token>& toks, std::uint32_t line, std::uint32_t col) {
  std::vector<std::string> stack;
  std::string line_decl;     // decl keyword at the start of the line being scanned
  std::string pending_open;  // decl of the line that just ended; consumed by the next INDENT
  bool at_line_start = true;

  const auto is_decl = [](std::string_view w) {
    return std::find(kDeclKeywords.begin(), kDeclKeywords.end(), w) != kDeclKeywords.end();
  };

  for (const Token& t : toks) {
    if (t.span.line > line || (t.span.line == line && t.span.column > col)) break;
    switch (t.kind) {
      case TokenKind::Newline:
        pending_open = line_decl;
        line_decl.clear();
        at_line_start = true;
        break;
      case TokenKind::Indent:
        stack.push_back(!pending_open.empty() ? pending_open
                                              : (stack.empty() ? std::string() : stack.back()));
        pending_open.clear();
        at_line_start = true;
        break;
      case TokenKind::Dedent:
        if (!stack.empty()) stack.pop_back();
        at_line_start = true;
        break;
      case TokenKind::Identifier:
        if (at_line_start) {
          line_decl = is_decl(t.lexeme) ? std::string(t.lexeme) : std::string();
          at_line_start = false;
        }
        break;
      default:
        at_line_start = false;
        break;
    }
  }
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    if (!it->empty()) return *it;
  }
  return {};
}

struct BuiltinDoc {
  const char* name;
  const char* signature;  // "ler_csv(caminho)"
  const char* params;     // comma-separated parameter names (for signatureHelp)
  const char* doc;        // one-liner
  const char* example;    // may be nullptr
};

// One-liner + example per builtin, mirroring src/semantic/checker.cpp's
// BuiltinSig table (kept local so the LSP does not depend on sema types).
const std::vector<BuiltinDoc>& builtin_docs() {
  static const std::vector<BuiltinDoc> docs = {
      {"imprimir", "imprimir(valor, ...)", "valor", "Imprime valores na saida padrao.",
       "imprimir \"ola\", 42"},
      {"registrar", "registrar(mensagem, ...)", "mensagem", "Registra uma mensagem de log.",
       "registrar \"iniciando etapa\""},
      {"env", "env(nome)", "nome", "Le o valor de uma variavel de ambiente.", "env \"HOME\""},
      {"tamanho", "tamanho(colecao)", "colecao", "Tamanho de lista, texto, mapa ou tabela.",
       nullptr},
      {"contar", "contar(colecao)", "colecao", "Conta elementos de uma tabela ou lista.", nullptr},
      {"somar", "somar(lista)", "lista", "Soma os elementos de uma lista numerica.",
       "somar [1, 2, 3]"},
      {"media", "media(lista)", "lista", "Media aritmetica dos elementos de uma lista.",
       "media [1, 2, 3]"},
      {"min", "min(lista)", "lista", "Menor elemento de uma lista.", nullptr},
      {"max", "max(lista)", "lista", "Maior elemento de uma lista.", nullptr},
      {"intervalo", "intervalo(inicio, fim, passo?)", "inicio,fim,passo",
       "Lista de numeros de inicio ate fim (passo opcional).", "intervalo 0, 10"},
      {"dividir", "dividir(texto, separador)", "texto,separador",
       "Divide um texto em uma lista, pelo separador.", "dividir \"a,b,c\", \",\""},
      {"ler_csv", "ler_csv(caminho)", "caminho", "Le um arquivo CSV como tabela.",
       "ler_csv \"dados.csv\""},
      {"escrever_csv", "escrever_csv(tabela, caminho)", "tabela,caminho",
       "Grava uma tabela em arquivo CSV.", "escrever_csv clientes, \"saida.csv\""},
      {"ler_json", "ler_json(caminho)", "caminho", "Le um arquivo JSON.", nullptr},
      {"escrever_json", "escrever_json(valor, caminho)", "valor,caminho",
       "Grava um valor em arquivo JSON.", nullptr},
      {"ler", "ler(prompt?)", "prompt", "Le uma linha da entrada padrao.", nullptr},
      {"carregador", "carregador(caminho, alvo: ...)", "caminho,alvo",
       "Carrega dados (CSV/JSON/Parquet) para treino de modelo.",
       "carregador \"dados.csv\", alvo: \"classe\""},
      {"perguntar", "perguntar(texto)", "texto", "Envia um prompt a um LLM e retorna a resposta.",
       nullptr},
      {"incorporar", "incorporar(modelo, texto)", "modelo,texto",
       "Gera um embedding (tensor) para um texto.", nullptr},
      {"dividir_texto", "dividir_texto(texto, tamanho: n)", "texto,tamanho",
       "Divide um texto em pedacos de tamanho fixo.", "dividir_texto texto, tamanho: 4"},
      {"responder", "responder(valor)", "valor",
       "Define a resposta de uma ferramenta/agente dentro de um fluxo.", nullptr},
      {"tensor", "tensor([[...], [...]])", "dados", "Cria um tensor a partir de listas aninhadas.",
       "tensor [[1, 2], [3, 4]]"},
      {"zeros", "zeros([dim, ...])", "dims", "Tensor preenchido com zeros.", nullptr},
      {"checar_tilt", "checar_tilt(caminho)", "caminho", "Valida a sintaxe de um arquivo .tilt.",
       nullptr},
      {"executar_sql", "executar_sql(conexao, consulta)", "conexao,consulta",
       "Executa uma consulta SQL em uma conexao.", nullptr},
      {"consultar_sql", "consultar_sql(conexao, consulta, [params])", "conexao,consulta,params",
       "Executa um SELECT com placeholders '?' e devolve tabela.", nullptr},
      {"transacao", "transacao(conexao, passos)", "conexao,passos",
       "Executa passos SQL atomicamente (BEGIN/COMMIT; ROLLBACK em falha).", nullptr},
      {"es_buscar", "es_buscar(url, dsl)", "url,dsl",
       "Busca no Elasticsearch/OpenSearch (DSL JSON) e devolve {total, hits}.", nullptr},
      {"es_executar", "es_executar(url, metodo, caminho, [corpo])", "url,metodo,caminho,corpo",
       "Chama qualquer endpoint REST do Elasticsearch/OpenSearch.", nullptr},
      {"ler_parquet", "ler_parquet(caminho)", "caminho", "Le um arquivo Parquet como tabela.",
       nullptr},
      {"escrever_parquet", "escrever_parquet(tabela, caminho)", "tabela,caminho",
       "Grava uma tabela em arquivo Parquet.", nullptr},
      {"ler_delta", "ler_delta(caminho)", "caminho", "Le uma tabela Delta Lake.", nullptr},
      {"escrever_delta", "escrever_delta(tabela, caminho)", "tabela,caminho",
       "Grava uma tabela em Delta Lake.", nullptr},
      {"anexar_delta", "anexar_delta(tabela, caminho)", "tabela,caminho",
       "Anexa linhas a uma tabela Delta Lake.", nullptr},
      {"ler_iceberg", "ler_iceberg(caminho)", "caminho", "Le uma tabela Iceberg.", nullptr},
      {"escrever_iceberg", "escrever_iceberg(tabela, caminho)", "tabela,caminho",
       "Grava uma tabela em Iceberg.", nullptr},
      {"anexar_iceberg", "anexar_iceberg(tabela, caminho)", "tabela,caminho",
       "Anexa linhas a uma tabela Iceberg.", nullptr},
      {"apagar_iceberg", "apagar_iceberg(caminho, onde: {...})", "caminho",
       "Apaga linhas de uma tabela Iceberg (position/equality deletes).", nullptr},
      {"ler_redis", "ler_redis(conexao, chave)", "conexao,chave", "Le um valor do Redis.", nullptr},
      {"escrever_redis", "escrever_redis(conexao, chave, valor)", "conexao,chave,valor",
       "Grava um valor no Redis.", nullptr},
      {"redis_executar", "redis_executar(conexao, comando)", "conexao,comando",
       "Executa um comando Redis.", nullptr},
      {"redis_lote", "redis_lote(conexao, operacoes)", "conexao,operacoes",
       "Executa operacoes Redis em lote.", nullptr},
      {"ler_kafka", "ler_kafka(topico, ...)", "topico", "Consome mensagens de um topico Kafka.",
       nullptr},
      {"escrever_kafka", "escrever_kafka(topico, mensagem)", "topico,mensagem",
       "Publica uma mensagem em um topico Kafka.", nullptr},
      {"transacao_kafka", "transacao_kafka(id, registros, opcoes?)", "id,registros,opcoes",
       "Publica registros em varias particoes com commit atomico Kafka.", nullptr},
      {"mongo_inserir", "mongo_inserir(colecao, documento)", "colecao,documento",
       "Insere um documento no MongoDB.", nullptr},
      {"mongo_buscar", "mongo_buscar(colecao, filtro?)", "colecao,filtro",
       "Busca documentos no MongoDB.", nullptr},
      {"mongo_atualizar", "mongo_atualizar(colecao, filtro, atualizacao)",
       "colecao,filtro,atualizacao", "Atualiza documentos no MongoDB.", nullptr},
      {"mongo_deletar", "mongo_deletar(colecao, filtro)", "colecao,filtro",
       "Remove documentos do MongoDB.", nullptr},
      {"mongo_criar_indice", "mongo_criar_indice(colecao, chaves)", "colecao,chaves",
       "Cria um indice no MongoDB.", nullptr},
      {"mongo_agregar", "mongo_agregar(colecao, pipeline)", "colecao,pipeline",
       "Executa um pipeline de agregacao no MongoDB.", nullptr},
      {"ler_s3", "ler_s3(uri)", "uri", "Le um objeto do S3.", nullptr},
      {"escrever_s3", "escrever_s3(uri, dados)", "uri,dados", "Grava um objeto no S3.", nullptr},
      {"listar_s3", "listar_s3(uri)", "uri", "Lista objetos de um prefixo no S3.", nullptr},
      {"apagar_s3", "apagar_s3(uri)", "uri", "Apaga um objeto do S3.", nullptr},
      {"copiar_s3", "copiar_s3(origem, destino)", "origem,destino", "Copia um objeto no S3.",
       nullptr},
      {"cabecalho_s3", "cabecalho_s3(uri)", "uri", "Retorna os metadados de um objeto S3.",
       nullptr},
      {"s3_iniciar_upload", "s3_iniciar_upload(uri)", "uri", "Inicia um upload multipart no S3.",
       nullptr},
      {"s3_enviar_parte", "s3_enviar_parte(id, parte, dados, ...)", "id,parte,dados",
       "Envia uma parte de um upload multipart S3.", nullptr},
      {"s3_concluir_upload", "s3_concluir_upload(id, partes, ...)", "id,partes",
       "Conclui um upload multipart S3.", nullptr},
      {"s3_abortar_upload", "s3_abortar_upload(id, ...)", "id", "Aborta um upload multipart S3.",
       nullptr},
      {"http_get_json", "http_get_json(url, cabecalhos?)", "url,cabecalhos",
       "Faz um GET HTTP e retorna o JSON da resposta.", "http_get_json \"https://api/status\""},
      {"http_post_json", "http_post_json(url, corpo, cabecalhos?)", "url,corpo,cabecalhos",
       "Envia um valor como JSON (POST) e retorna o JSON da resposta.",
       "http_post_json url, {nome: \"tilt\"}"},
      {"spark_sql", "spark_sql(url, sql, lingua?, conf?)", "url,sql,lingua,conf",
       "Executa Spark SQL via Apache Livy e retorna a tabela de linhas.",
       "spark_sql \"http://localhost:8998\", \"select * from vendas\""},
      {"spark_executar", "spark_executar(url, codigo, lingua?, conf?)", "url,codigo,lingua,conf",
       "Executa codigo (Scala/PySpark) via Apache Livy e retorna o texto do resultado.",
       "spark_executar \"http://localhost:8998\", \"spark.range(10).count()\""},
  };
  return docs;
}

// Biblioteca padrao pura (src/runtime/stdlib.cpp): documentacao para hover,
// signatureHelp e autocomplete.
const std::vector<BuiltinDoc>& stdlib_docs() {
  static const std::vector<BuiltinDoc> docs = {
      {"raiz", "raiz(numero)", "numero", "Raiz quadrada.", "raiz(16)"},
      {"abs", "abs(numero)", "numero", "Valor absoluto.", nullptr},
      {"exp", "exp(numero)", "numero", "Exponencial (e elevado ao numero).", nullptr},
      {"logaritmo", "logaritmo(numero, base?)", "numero,base",
       "Logaritmo natural, ou na base dada.", "logaritmo(100, 10)"},
      {"potencia", "potencia(base, expoente)", "base,expoente", "Base elevada ao expoente.",
       "potencia(2, 10)"},
      {"piso", "piso(numero)", "numero", "Maior inteiro menor ou igual ao numero.", nullptr},
      {"teto", "teto(numero)", "numero", "Menor inteiro maior ou igual ao numero.", nullptr},
      {"arredondar", "arredondar(numero, casas?)", "numero,casas",
       "Arredonda para inteiro, ou para N casas decimais.", "arredondar(3.14159, 2)"},
      {"seno", "seno(radianos)", "radianos", "Seno.", nullptr},
      {"cosseno", "cosseno(radianos)", "radianos", "Cosseno.", nullptr},
      {"tangente", "tangente(radianos)", "radianos", "Tangente.", nullptr},
      {"pi", "pi()", "", "Constante pi.", nullptr},
      {"inteiro", "inteiro(valor)", "valor", "Converte texto, decimal ou logico em inteiro.",
       "inteiro(\"42\")"},
      {"decimal", "decimal(valor)", "valor", "Converte texto, inteiro ou logico em decimal.",
       "decimal(\"2.5\")"},
      {"texto", "texto(valor)", "valor", "Converte qualquer valor em texto.", "texto(3)"},
      {"logico", "logico(valor)", "valor", "Converte um valor em logico (verdadeiro/falso).",
       nullptr},
      {"tipo_de", "tipo_de(valor)", "valor", "Nome do tipo do valor (inteiro, texto, lista...).",
       nullptr},
      {"maiusculas", "maiusculas(texto)", "texto", "Texto em maiusculas (ASCII).", nullptr},
      {"minusculas", "minusculas(texto)", "texto", "Texto em minusculas (ASCII).", nullptr},
      {"aparar", "aparar(texto)", "texto", "Remove espacos das pontas do texto.", nullptr},
      {"substituir", "substituir(texto, de, para)", "texto,de,para",
       "Substitui todas as ocorrencias de um trecho.", "substituir(\"a-b\", \"-\", \"+\")"},
      {"comeca_com", "comeca_com(texto, prefixo)", "texto,prefixo",
       "Verdadeiro se o texto comeca com o prefixo.", nullptr},
      {"termina_com", "termina_com(texto, sufixo)", "texto,sufixo",
       "Verdadeiro se o texto termina com o sufixo.", nullptr},
      {"juntar", "juntar(lista, separador?)", "lista,separador",
       "Junta os elementos de uma lista em um texto.", "juntar([1, 2, 3], \", \")"},
      {"regex_casa", "regex_casa(texto, padrao)", "texto,padrao",
       "Verdadeiro se o texto contem uma ocorrencia da expressao regular.", nullptr},
      {"regex_extrair", "regex_extrair(texto, padrao)", "texto,padrao",
       "Lista de todas as ocorrencias (grupo 1, se houver).", nullptr},
      {"regex_substituir", "regex_substituir(texto, padrao, para)", "texto,padrao,para",
       "Substitui as ocorrencias da expressao regular ($1 para grupos).", nullptr},
      {"ordenar", "ordenar(lista, ordem?)", "lista,ordem",
       "Lista ordenada (ordem: \"crescente\" ou \"decrescente\").", "ordenar([3, 1, 2])"},
      {"unicos", "unicos(lista)", "lista", "Remove repetidos, mantendo a ordem da 1a ocorrencia.",
       nullptr},
      {"reverso", "reverso(lista_ou_texto)", "valor", "Inverte uma lista ou um texto.", nullptr},
      {"zip", "zip(lista, lista)", "a,b", "Pares [a_i, b_i] ate o fim da menor lista.", nullptr},
      {"enumerar", "enumerar(lista)", "lista", "Lista de { indice, valor }.", nullptr},
      {"chaves", "chaves(mapa)", "mapa", "Lista das chaves de um mapa.", nullptr},
      {"valores", "valores(mapa)", "mapa", "Lista dos valores de um mapa.", nullptr},
      {"agora", "agora()", "", "Data e hora atuais em UTC (ISO 8601).", nullptr},
      {"timestamp", "timestamp()", "", "Segundos desde 1970-01-01 (UTC).", nullptr},
      {"formatar_data", "formatar_data(data, formato?)", "data,formato",
       "Formata data ISO ou timestamp (UTC) com strftime.", "formatar_data(agora(), \"%d/%m/%Y\")"},
      {"dormir", "dormir(segundos)", "segundos", "Pausa a execucao.", nullptr},
      {"ler_texto", "ler_texto(caminho)", "caminho", "Le um arquivo inteiro como texto.", nullptr},
      {"escrever_texto", "escrever_texto(caminho, texto)", "caminho,texto",
       "Grava (sobrescreve) um arquivo de texto.", nullptr},
      {"anexar_texto", "anexar_texto(caminho, texto)", "caminho,texto",
       "Acrescenta texto ao fim de um arquivo.", nullptr},
      {"listar_arquivos", "listar_arquivos(diretorio)", "diretorio",
       "Nomes das entradas de um diretorio, ordenados.", nullptr},
      {"remover_arquivo", "remover_arquivo(caminho)", "caminho",
       "Remove um arquivo; devolve se conseguiu.", nullptr},
      {"sha256", "sha256(texto)", "texto", "Hash SHA-256 em hexadecimal.", nullptr},
      {"base64_codificar", "base64_codificar(texto)", "texto", "Codifica em Base64.", nullptr},
      {"base64_decodificar", "base64_decodificar(texto)", "texto", "Decodifica Base64.", nullptr},
      {"json_texto", "json_texto(valor)", "valor", "Converte um valor em texto JSON.", nullptr},
      {"json_ler", "json_ler(texto)", "texto", "Interpreta um texto JSON como valor.", nullptr},
  };
  return docs;
}

const BuiltinDoc* find_builtin_doc(std::string_view name) {
  for (const auto& d : builtin_docs()) {
    if (name == d.name) return &d;
  }
  for (const auto& d : stdlib_docs()) {
    if (name == d.name) return &d;
  }
  return nullptr;
}

struct KeywordDoc {
  const char* word;
  const char* doc;
};

const std::vector<KeywordDoc>& keyword_docs() {
  static const std::vector<KeywordDoc> docs = {
      {"tipo", "Declaracao de tipo/estrutura de dados."},
      {"funcao", "Declara uma funcao: `funcao nome param: tipo -> retorno:`."},
      {"seja", "Atribuicao local: `seja nome = valor`."},
      {"constante", "Declara uma constante: `constante nome = valor`."},
      {"importar", "Importa um modulo: `importar nome` / `de nome importar a, b`."},
      {"de", "Acompanha `importar`: `de stdlib.math importar media`."},
      {"fonte", "Declara uma fonte de dados (arquivo, URL, query, broker)."},
      {"pipeline", "Declara um pipeline de dados (bloco com `passos:`, `agenda:`, ...)."},
      {"verificar", "Declara regras de verificacao de dados."},
      {"modelo", "Declara um modelo (bloco com `camadas:`, `entrada:`, ...)."},
      {"treino", "Bloco de treino de um modelo (`dados:`, `perda:`, `epocas:`, ...)."},
      {"busca", "Busca em grade de hiperparametros (`modelo:`, `grade:`, `criterio:`)."},
      {"tarefa", "Declara uma tarefa de um experimento."},
      {"experimento", "Declara um experimento de ML."},
      {"avaliacao",
       "Declara uma avaliacao (evals: `dados:`, `executar:`, `metricas:`, `limiar:`)."},
      {"llm", "Declara uma configuracao de LLM (`provedor:`, `modelo:`, `temperatura:`, ...)."},
      {"indice", "Declara um indice de embeddings para RAG."},
      {"fluxo", "Declara um fluxo de agente (`entrada:`, `passos:`)."},
      {"ferramenta", "Declara uma ferramenta que um agente pode usar."},
      {"agente", "Declara um agente (`llm:`, `papel:`, `ferramentas:`, ...)."},
      {"equipe", "Declara uma equipe de agentes."},
      {"servico", "Declara um servico HTTP (`rota`, `porta:`, ...)."},
      {"se", "Condicional: `se condicao:` com bloco indentado."},
      {"senao", "Ramificacao alternativa de `se`."},
      {"para", "Laco: `para cada item em lista:`."},
      {"cada", "Laco: `para cada item em lista:`."},
      {"enquanto", "Laco condicional: `enquanto condicao:`."},
      {"tentar", "Tratamento de erro: `tentar:` ... `capturar erro:`."},
      {"capturar", "Bloco de captura de erro de `tentar`."},
      {"retornar", "Retorna um valor de uma funcao."},
  };
  return docs;
}

const char* find_keyword_doc(std::string_view word) {
  for (const auto& d : keyword_docs()) {
    if (word == d.word) return d.doc;
  }
  return nullptr;
}

const char* find_method_doc(std::string_view name) {
  static const std::vector<KeywordDoc> docs = {
      // tabela / lista
      {"filtrar", "Metodo de tabela: filtra linhas por predicado."},
      {"derivar", "Metodo de tabela: adiciona coluna derivada de outras."},
      {"mapear", "Metodo de tabela: transforma valores de uma coluna."},
      {"agrupar_por", "Metodo de tabela: agrupa linhas pelas colunas dadas."},
      {"selecionar", "Metodo de tabela: seleciona/renomeia colunas."},
      {"ordenar_por", "Metodo de tabela: ordena pelas colunas dadas."},
      {"limite", "Metodo de tabela: limita a N linhas."},
      {"primeiros", "Metodo de tabela: pega as N primeiras linhas."},
      {"distinto", "Metodo de tabela: remove linhas duplicadas."},
      {"inserir", "Metodo de tabela: insere uma linha."},
      // tensor
      {"forma", "Metodo de tensor: retorna as dimensoes."},
      {"dados", "Metodo de tensor: retorna os dados brutos."},
      {"matmul", "Metodo de tensor: multiplicacao de matrizes."},
      {"transposta", "Metodo de tensor: transposicao."},
      {"reformar", "Metodo de tensor: muda a forma (reshape)."},
      {"conv2d", "Metodo de tensor: convolucao 2D."},
      {"norma_lote", "Metodo de tensor: normalizacao em lote (batch norm)."},
      {"relu", "Metodo de tensor: ativacao ReLU."},
      {"gelu", "Metodo de tensor: ativacao GELU."},
      {"silu", "Metodo de tensor: ativacao SiLU."},
      {"sigmoide", "Metodo de tensor: ativacao sigmoide."},
      {"tanh", "Metodo de tensor: ativacao tangente hiperbolica."},
      {"softmax", "Metodo de tensor: softmax ao longo de um eixo."},
      {"soma", "Metodo de tensor: soma dos elementos."},
      {"media", "Metodo de tensor: media dos elementos."},
      {"argmax", "Metodo de tensor: indice do maior valor."},
      {"item", "Metodo de tensor: extrai o valor escalar."},
      // outros receivers
      {"executar", "Metodo de modelo/ferramenta: executa com as entradas dadas."},
      {"responder", "Metodo de agente: envia a resposta no fluxo."},
      {"buscar", "Metodo de indice: busca os vizinhos mais proximos."},
      {"texto", "Metodo de resposta de LLM: retorna o texto da resposta."},
  };
  for (const auto& d : docs) {
    if (name == d.word) return d.doc;
  }
  return nullptr;
}

// Identifier word touching the 1-based (line, column) cursor, or empty.
// When the word follows a '.', `as_member` is set.
std::string word_at(const SourceFile& src, std::uint32_t line, std::uint32_t column,
                    bool* as_member) {
  const std::string_view text = src.line_text(line);
  std::uint32_t c = column > 0 ? column - 1 : 0;
  if (c > text.size()) c = static_cast<std::uint32_t>(text.size());
  std::uint32_t start = c;
  while (start > 0 && ident_char(text[start - 1])) --start;
  std::uint32_t end = c;
  while (end < text.size() && ident_char(text[end])) ++end;
  if (as_member) *as_member = start > 0 && text[start - 1] == '.';
  return std::string(text.substr(start, end - start));
}

bool span_ends_before(const Span& s, std::uint32_t line, std::uint32_t column) {
  if (s.line < line) return true;
  if (s.line > line) return false;
  return s.column + (s.length > 0 ? s.length : 1) <= column;
}

bool span_is_at(const Span& s, std::uint32_t line, std::uint32_t column) {
  return s.line == line && s.column == column;
}

struct NameDecl {
  std::string name;
  Span span;
  std::string kind;    // declaring keyword, or "variavel" / "parametro"
  std::string parent;  // funcao dona do parametro (vazio nos demais)
};

// First Identifier token named `name` at/after `from_offset` (used to recover
// spans the AST does not store, e.g. `funcao` parameters and `para cada` vars).
Span token_span_after(const std::vector<Token>& toks, std::string_view name,
                      std::uint32_t from_offset) {
  for (const Token& t : toks) {
    if (t.kind == TokenKind::Identifier && t.lexeme == name && t.span.offset >= from_offset) {
      return t.span;
    }
  }
  return Span{from_offset, 0, 0, 0};
}

void collect_item(const ast::Item& it, const std::vector<Token>& toks, std::vector<NameDecl>& out);

void collect_stmt(const ast::Stmt& s, const std::vector<Token>& toks, std::vector<NameDecl>& out) {
  if (s.kind == ast::StmtKind::Assign && s.a && s.a->kind == ast::ExprKind::Name) {
    out.push_back({s.a->text, s.a->span, "variavel", {}});
  }
  if (s.kind == ast::StmtKind::ForEach && !s.name.empty()) {
    out.push_back({s.name, token_span_after(toks, s.name, s.span.offset), "variavel", {}});
  }
  if (s.kind == ast::StmtKind::Try && !s.name.empty()) {
    out.push_back({s.name, token_span_after(toks, s.name, s.span.offset), "variavel", {}});
  }
  auto walk_block = [&](const ast::Block& b) {
    for (const auto& i : b.items) {
      if (i) collect_item(*i, toks, out);
    }
  };
  walk_block(s.body);
  for (const auto& ei : s.elifs) walk_block(ei.body);
  if (s.else_body) walk_block(*s.else_body);
  if (s.catch_body) walk_block(*s.catch_body);
}

void collect_item(const ast::Item& it, const std::vector<Token>& toks, std::vector<NameDecl>& out) {
  if (it.kind == ast::ItemKind::Decl) {
    std::string decl_name;
    if (!it.header.empty() && it.header[0] && it.header[0]->kind == ast::ExprKind::Name) {
      decl_name = it.header[0]->text;
      out.push_back({decl_name, it.header[0]->span, it.key, {}});
    }
    if (it.key == "funcao") {
      std::uint32_t from = it.span.offset;
      for (const auto& p : it.params) {
        if (p.name.empty()) continue;
        const Span sp = token_span_after(toks, p.name, from);
        out.push_back({p.name, sp, "parametro", decl_name});
        from = sp.offset + 1;
      }
    }
  }
  if (it.stmt) collect_stmt(*it.stmt, toks, out);
  if (it.child) collect_item(*it.child, toks, out);
  if (it.block) {
    for (const auto& i : it.block->items) {
      if (i) collect_item(*i, toks, out);
    }
  }
}

std::vector<NameDecl> collect_decls(const ast::Program& prog, const std::vector<Token>& toks) {
  std::vector<NameDecl> out;
  for (const auto& it : prog.items) {
    if (it) collect_item(*it, toks, out);
  }
  return out;
}

// Best-known declaration for `name` at the cursor: the declaration on the
// cursor itself, else the nearest one ending before the cursor, else the
// first one after it. Returns nullptr when the name is not declared.
const NameDecl* resolve_decl(const std::vector<NameDecl>& decls, std::string_view name,
                             std::uint32_t line, std::uint32_t column) {
  const NameDecl* before = nullptr;
  const NameDecl* after = nullptr;
  for (const auto& d : decls) {
    if (d.name != name) continue;
    if (span_is_at(d.span, line, column)) return &d;
    if (span_ends_before(d.span, line, column)) {
      if (!before || d.span.line > before->span.line ||
          (d.span.line == before->span.line && d.span.column > before->span.column)) {
        before = &d;
      }
    } else if (!after) {
      after = &d;
    }
  }
  return before ? before : after;
}

std::string trim_right(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return std::string(s);
}

// Filhos que carregam valor (nomes de campo/chave nao sao expressoes).
template <typename F>
void each_child_expr(const ast::Expr& e, F&& fn) {
  if (e.lhs) fn(*e.lhs);
  if (e.rhs) fn(*e.rhs);
  if (e.extra) fn(*e.extra);
  for (const auto& a : e.args) {
    if (a.value) fn(*a.value);
  }
  for (const auto& el : e.elems) {
    if (el) fn(*el);
  }
  for (const auto& en : e.entries) {
    if (en.value) fn(*en.value);
  }
  if (e.block) {
    for (const auto& it : e.block->items) {
      if (it && it->kind == ast::ItemKind::Field && it->value) fn(*it->value);
    }
  }
}

// Fim efetivo de uma expressao: o parser nao estende o span dos nos compostos
// (Binary pega o span do lhs), entao o fim e o maximo entre o proprio span e
// o dos filhos. Sem isso, hover no operador/2o operando nao acha nada.
std::uint32_t expr_end(const ast::Expr& e) {
  std::uint32_t end = e.span.offset + e.span.length;
  each_child_expr(e, [&](const ast::Expr& c) {
    const std::uint32_t ce = expr_end(c);
    if (ce > end) end = ce;
  });
  return end;
}

// Expressao mais interna contendo o offset (para hover de tipos).
// `<=` no fim porque spans de nos compostos nao cobrem o fechamento
// (`]`/`)`): pairar o fechamento mostra o tipo do no. Efeito colateral
// benigno: espaco colado apos a expressao mostra o tipo dela.
const ast::Expr* inner_expr(const ast::Expr& e, std::uint32_t off) {
  if (!(e.span.offset <= off && off <= expr_end(e))) return nullptr;
  const ast::Expr* best = &e;
  each_child_expr(e, [&](const ast::Expr& c) {
    if (const ast::Expr* f = inner_expr(c, off)) best = f;
  });
  return best;
}

const ast::Expr* inner_stmt_expr(const ast::Stmt& s, std::uint32_t off) {
  const ast::Expr* best = nullptr;
  auto descend = [&](const ast::Expr* c) {
    if (c) {
      if (const ast::Expr* f = inner_expr(*c, off)) best = f;
    }
  };
  descend(s.a.get());
  descend(s.b.get());
  auto walk_block = [&](const ast::Block& b, auto&& self) -> void {
    for (const auto& it : b.items) {
      if (!it) continue;
      if (it->stmt) {
        if (const ast::Expr* f = inner_stmt_expr(*it->stmt, off)) best = f;
      }
      if (it->child && it->child->stmt) {
        if (const ast::Expr* f = inner_stmt_expr(*it->child->stmt, off)) best = f;
      }
      if (it->value) descend(it->value.get());
      if (it->default_value) descend(it->default_value.get());
      for (const auto& h : it->header) descend(h.get());
      if (it->block) self(*it->block, self);
    }
  };
  walk_block(s.body, walk_block);
  for (const auto& ei : s.elifs) {
    descend(ei.cond.get());
    walk_block(ei.body, walk_block);
  }
  if (s.else_body) walk_block(*s.else_body, walk_block);
  if (s.catch_body) walk_block(*s.catch_body, walk_block);
  return best;
}

void inner_prog_item(const ast::Item& it, std::uint32_t off, const ast::Expr*& best) {
  if (it.stmt) {
    if (const ast::Expr* f = inner_stmt_expr(*it.stmt, off)) best = f;
  }
  if (it.child && it.child->stmt) {
    if (const ast::Expr* f = inner_stmt_expr(*it.child->stmt, off)) best = f;
  }
  if (it.value) {
    if (const ast::Expr* f = inner_expr(*it.value, off)) best = f;
  }
  if (it.default_value) {
    if (const ast::Expr* f = inner_expr(*it.default_value, off)) best = f;
  }
  for (const auto& h : it.header) {
    if (h) {
      if (const ast::Expr* f = inner_expr(*h, off)) best = f;
    }
  }
  // Params/retorno de funcao tambem sao expressoes pairaveis.
  if (it.kind == ast::ItemKind::Decl && it.key == "funcao") {
    for (const auto& pm : it.params) {
      if (pm.value) {
        if (const ast::Expr* f = inner_expr(*pm.value, off)) best = f;
      }
    }
  }
  if (it.block) {
    for (const auto& sub : it.block->items) {
      if (sub) inner_prog_item(*sub, off, best);
    }
  }
}

const ast::Expr* inner_prog_expr(const ast::Program& prog, std::uint32_t off) {
  const ast::Expr* best = nullptr;
  for (const auto& p : prog.items) {
    if (p) inner_prog_item(*p, off, best);
  }
  return best;
}

// RHS da atribuicao cujo alvo tem o span dado (para tipo de uso de variavel).
const ast::Expr* assign_rhs_at(const ast::Block& b, std::uint32_t target_off) {
  const ast::Expr* found = nullptr;
  std::function<void(const ast::Block&)> walk = [&](const ast::Block& blk) {
    for (const auto& it : blk.items) {
      if (!it || found) return;
      const ast::Item* node = it.get();
      if (node->kind == ast::ItemKind::ListEntry && node->child) node = node->child.get();
      if (node->kind == ast::ItemKind::Stmt && node->stmt &&
          node->stmt->kind == ast::StmtKind::Assign && node->stmt->a &&
          node->stmt->a->kind == ast::ExprKind::Name && node->stmt->a->span.offset == target_off &&
          node->stmt->b) {
        found = node->stmt->b.get();
        return;
      }
      if (node->block) walk(*node->block);
    }
  };
  walk(b);
  return found;
}

std::string render_hover_type(const SemanticChecker& sema, const ast::Expr* e) {
  if (!e) return {};
  const sema::TypeKind* k = sema.hover_type(e);
  if (!k) return {};
  if (*k == sema::TypeKind::Tensor) {
    sema::Type t;
    t.kind = sema::TypeKind::Tensor;
    if (const auto* sh = sema.hover_shape(e)) t.dims = *sh;
    return sema::type_to_string(t);
  }
  sema::Type t = sema::Type::scalar(*k);
  return sema::type_to_string(t);
}

// Sufixo de tipos para o hover de uma declaracao resolvida ("" = desconhecido,
// mantem a mensagem textual atual). Usa os tipos ja resolvidos pelo checker
// (passada 2 + tabela de simbolos), sem inferencia nova.
std::string hover_type_suffix(const ast::Program& prog, const SemanticChecker& sema,
                              const NameDecl& d) {
  const auto& globals = sema.globals();
  if (d.kind == "funcao") {
    const ast::Item* decl = nullptr;
    for (const auto& it : prog.items) {
      if (it && it->kind == ast::ItemKind::Decl && it->key == "funcao" && !it->header.empty() &&
          it->header[0] && it->header[0]->text == d.name) {
        decl = it.get();
        break;
      }
    }
    if (!decl) return {};
    std::string md = "\n\n`funcao " + d.name + "(";
    bool first = true;
    for (const auto& p : decl->params) {
      if (p.name.empty()) continue;
      if (!first) md += ", ";
      first = false;
      std::string pname = p.name;
      // `nome[]` = parametro opcional (sufixo do parser).
      bool opcional = false;
      if (pname.size() > 2 && pname.compare(pname.size() - 2, 2, "[]") == 0) {
        pname.erase(pname.size() - 2);
        opcional = true;
      } else if (!p.optional_annotation.empty()) {
        opcional = true;
      }
      md += pname + ": ";
      if (const sema::Type* t = sema.annotation_of(p.value.get())) {
        md += sema::type_to_string(*t);
      } else {
        md += "?";
      }
      if (opcional) md += " (opcional)";
    }
    md += ")";
    std::string ret = "?";
    if (auto it = globals.find(d.name); it != globals.end() && it->second.type.ret &&
                                        it->second.type.ret->kind != sema::TypeKind::Unknown) {
      ret = sema::type_to_string(*it->second.type.ret);
    } else if (const sema::Type* t = sema.annotation_of(decl->value.get())) {
      if (t->kind != sema::TypeKind::Unknown) ret = sema::type_to_string(*t);
    }
    md += " -> " + ret + "`";
    return md;
  }
  if (d.kind == "tipo") {
    auto it = globals.find(d.name);
    if (it == globals.end() || it->second.type.fields.empty()) return {};
    std::string md = "\n\nCampos de `" + d.name + "`:";
    for (const auto& [fname, ftype] : it->second.type.fields) {
      md += "\n- " + fname + ": " + (ftype ? sema::type_to_string(*ftype) : "?");
    }
    return md;
  }
  if (d.kind == "parametro" && !d.parent.empty()) {
    for (const auto& it : prog.items) {
      if (!it || it->kind != ast::ItemKind::Decl || it->key != "funcao" || it->header.empty() ||
          !it->header[0] || it->header[0]->text != d.parent) {
        continue;
      }
      for (const auto& p : it->params) {
        std::string pname = p.name;
        if (pname.size() > 2 && pname.compare(pname.size() - 2, 2, "[]") == 0)
          pname.erase(pname.size() - 2);
        if (pname != d.name) continue;
        if (const sema::Type* t = sema.annotation_of(p.value.get())) {
          if (t->kind != sema::TypeKind::Unknown)
            return "\n\n`" + d.name + ": " + sema::type_to_string(*t) + "`";
        }
        return {};
      }
    }
  }
  return {};
}

}  // namespace

std::vector<CompletionItem> complete(const SourceFile& src, std::uint32_t line,
                                     std::uint32_t column) {
  std::vector<CompletionItem> out;

  const std::string_view text = src.line_text(line);
  std::uint32_t c = column > 0 ? column - 1 : 0;
  if (c > text.size()) c = static_cast<std::uint32_t>(text.size());

  std::uint32_t start = c;
  while (start > 0 && ident_char(text[start - 1])) --start;
  const std::string_view prefix = text.substr(start, c - start);
  const bool dot = start > 0 && text[start - 1] == '.';
  const bool at_line_head =
      text.substr(0, start).find_first_not_of(" \t") == std::string_view::npos;

  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  // A line that begins at column 1 can only hold a top-level declaration,
  // regardless of DEDENT tokens the lexer defers to end-of-file.
  const bool top_level = at_line_head && start == 0;
  const std::string decl = top_level ? std::string() : enclosing_decl(toks, line, column);

  if (dot) {
    for (auto m : kTableMethods) push(out, prefix, m, "method", "metodo de tabela/lista");
    for (auto m : kTensorMethods) push(out, prefix, m, "method", "metodo de tensor");
    push(out, prefix, "executar", "method", "modelo/ferramenta");
    push(out, prefix, "responder", "method", "agente");
    push(out, prefix, "buscar", "method", "indice");
    push(out, prefix, "texto", "method", "resposta de LLM");
    return out;
  }

  if (top_level || (at_line_head && decl.empty())) {
    for (auto k : kDeclKeywords) push(out, prefix, k, "keyword", "declaracao de topo");
    return out;
  }

  if (at_line_head && !decl.empty()) {
    for (const auto& fs : field_sets()) {
      if (fs.decl == decl) {
        for (auto f : fs.fields) push(out, prefix, f, "field", "campo de '" + decl + "'");
      }
    }
    for (auto k : kStmtKeywords) push(out, prefix, k, "keyword", "instrucao");
  }

  for (auto b : kBuiltins) push(out, prefix, b, "builtin", "funcao embutida");
  for (const auto& d : stdlib_docs()) push(out, prefix, d.name, "builtin", d.doc);
  for (auto k : kStmtKeywords) push(out, prefix, k, "keyword", "instrucao");

  // Names declared in this file.
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  for (const auto& it : prog.items) {
    if (!it || it->kind != ast::ItemKind::Decl) continue;
    if (it->header.empty() || !it->header[0] || it->header[0]->kind != ast::ExprKind::Name)
      continue;
    push(out, prefix, it->header[0]->text, "name", "declarado em '" + it->key + "'");
  }

  std::sort(out.begin(), out.end(),
            [](const CompletionItem& a, const CompletionItem& b) { return a.label < b.label; });
  return out;
}

std::string hover(const SourceFile& src, std::uint32_t line, std::uint32_t column) {
  bool as_member = false;
  const std::string word = word_at(src, line, column, &as_member);
  // Sem palavra (operador, pontuacao): so o tipo da expressao pode responder.
  if (word.empty()) {
    DiagnosticEngine d2(&src);
    Lexer l2(src, d2);
    const std::vector<Token> t2 = l2.tokenize();
    Parser p2(t2, d2);
    const ast::Program pr = p2.parse_program();
    SemanticChecker s2(pr, d2);
    s2.run();
    if (const ast::Expr* e = inner_prog_expr(pr, src.offset_of(line, column))) {
      if (std::string t = render_hover_type(s2, e); !t.empty()) return "`" + t + "`";
    }
    return {};
  }

  if (as_member) {
    if (const char* doc = find_method_doc(word)) {
      return std::string("**") + word + "**\n\n" + doc;
    }
    return {};
  }

  if (const BuiltinDoc* b = find_builtin_doc(word)) {
    std::string md = std::string("**") + word + "** `" + b->signature + "`\n\n" + b->doc;
    if (b->example) md += std::string("\n\nExemplo: `") + b->example + "`";
    return md;
  }
  if (const char* doc = find_keyword_doc(word)) {
    return std::string("**") + word + "**\n\n" + doc;
  }

  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  SemanticChecker sema(prog, diag);
  sema.run();
  const auto decls = collect_decls(prog, toks);
  if (const NameDecl* d = resolve_decl(decls, word, line, column)) {
    std::string md = std::string("`") + d->kind + " " + word + "` — declarado na linha " +
                     std::to_string(d->span.line) + ".";
    md += hover_type_suffix(prog, sema, *d);
    // Uso de variavel: tipo do RHS da atribuicao que a declarou.
    if (d->kind == "variavel") {
      for (const auto& it : prog.items) {
        if (!it || !it->block) continue;
        if (const ast::Expr* rhs = assign_rhs_at(*it->block, d->span.offset)) {
          if (std::string t = render_hover_type(sema, rhs); !t.empty()) {
            md += "\n\n`valor: " + t + "`";
            break;
          }
        }
      }
    }
    const std::string_view decl_line = src.line_text(d->span.line);
    if (!decl_line.empty()) {
      md += std::string("\n\n```tilt\n") + std::string(trim_right(decl_line)) + "\n```";
    }
    return md;
  }
  // Sem declaracao: tipo da expressao mais interna sob o cursor.
  // (lsp_server ja converte para 1-based antes de chamar hover().)
  const std::uint32_t off = src.offset_of(line, column);
  if (const ast::Expr* e = inner_prog_expr(prog, off)) {
    if (std::string t = render_hover_type(sema, e); !t.empty()) {
      std::string md = "`" + t + "`";
      const std::string_view text = src.text();
      if (e->span.length > 0 && e->span.length <= 60 &&
          e->span.offset + e->span.length <= text.size()) {
        std::string slice(text.substr(e->span.offset, e->span.length));
        md += " — `" + slice + "`";
      }
      return md;
    }
  }
  return {};
}

Span definition(const SourceFile& src, std::uint32_t line, std::uint32_t column) {
  const std::string word = word_at(src, line, column, nullptr);
  if (word.empty()) return Span{0, 0, 0, 0};

  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  const auto decls = collect_decls(prog, toks);
  if (const NameDecl* d = resolve_decl(decls, word, line, column)) return d->span;
  return Span{0, 0, 0, 0};
}

std::vector<Span> references(const SourceFile& src, std::uint32_t line, std::uint32_t column,
                             bool include_declaration) {
  std::vector<Span> out;
  const std::string word = word_at(src, line, column, nullptr);
  if (word.empty()) return out;

  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  const auto decls = collect_decls(prog, toks);
  const NameDecl* target = resolve_decl(decls, word, line, column);
  if (!target) return out;

  const std::string_view text = src.text();
  for (const Token& tok : toks) {
    if (tok.kind != TokenKind::Identifier || tok.lexeme != word) continue;
    if (tok.span.offset > 0 && tok.span.offset <= text.size() &&
        text[tok.span.offset - 1] == '.') {
      continue;  // membro de mapa/tensor, nao uma referencia ao nome declarado.
    }
    const bool is_target_declaration = tok.span.offset == target->span.offset;
    if (is_target_declaration && !include_declaration) continue;
    out.push_back(tok.span);
  }
  return out;
}

SigHelp signature_help(const SourceFile& src, std::uint32_t line, std::uint32_t column) {
  SigHelp out;
  const std::string_view text = src.line_text(line);
  std::uint32_t c = column > 0 ? column - 1 : 0;
  if (c > text.size()) c = static_cast<std::uint32_t>(text.size());

  // Innermost call on the line: the last builtin identifier before the cursor
  // that is not a member access (`a.b`). Only same-line arguments count.
  const BuiltinDoc* b = nullptr;
  std::uint32_t name_end = 0;
  for (std::uint32_t i = 0; i < c;) {
    if (!ident_char(text[i]) || (i > 0 && ident_char(text[i - 1]))) {
      ++i;
      continue;
    }
    std::uint32_t e = i;
    while (e < text.size() && ident_char(text[e])) ++e;
    if (e <= c && (i == 0 || text[i - 1] != '.')) {
      if (const BuiltinDoc* cand = find_builtin_doc(std::string(text.substr(i, e - i)))) {
        b = cand;
        name_end = e;
      }
    }
    i = e;
  }
  if (!b || c <= name_end) return out;

  // Bracket depth at the end of the callee name; the active parameter is the
  // number of commas seen at that same depth before the cursor.
  auto scan = [&](std::uint32_t from, std::uint32_t to, int depth, int count_at,
                  std::uint32_t* commas) {
    bool in_text = false;
    for (std::uint32_t i = from; i < to; ++i) {
      const char ch = text[i];
      if (in_text) {
        if (ch == '"') in_text = false;
        continue;
      }
      switch (ch) {
        case '"':
          in_text = true;
          break;
        case '(':
        case '[':
        case '{':
          ++depth;
          break;
        case ')':
        case ']':
        case '}':
          if (depth > 0) --depth;
          break;
        case ',':
          if (depth == count_at && commas) ++*commas;
          break;
        default:
          break;
      }
    }
    return depth;
  };
  const int target_depth = scan(0, name_end, 0, -1, nullptr);
  std::uint32_t commas = 0;
  scan(name_end, c, target_depth, target_depth, &commas);

  out.found = true;
  out.label = b->signature;
  std::string_view params = b->params;
  while (!params.empty()) {
    const std::size_t comma = params.find(',');
    out.params.push_back(std::string(params.substr(0, comma)));
    params = comma == std::string_view::npos ? std::string_view() : params.substr(comma + 1);
  }
  out.active_parameter = commas < out.params.size() ? commas : out.params.size() - 1;
  return out;
}

std::string format_document(const std::string& text) {
  SourceFile src("<format>", text);

  // Lines fully inside a multiline (triple-quoted) string are never touched:
  // their whitespace is part of the string content.
  std::vector<bool> in_string(src.line_count() + 2, false);
  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  for (const Token& t : toks) {
    if (t.kind != TokenKind::Text) continue;
    std::uint32_t end_line = t.span.line;
    for (const char ch : t.lexeme) {
      if (ch == '\n') ++end_line;
    }
    for (std::uint32_t l = t.span.line + 1; l <= end_line && l < in_string.size(); ++l) {
      in_string[l] = true;
    }
  }

  // Target indentation per line, from the lexer's own Indent/Dedent stream:
  // the block level at the line's first content token. Lines whose first
  // token sits inside brackets (continuation lines) are left alone.
  std::vector<int> line_level(src.line_count() + 2, -1);
  int level = 0;
  int bracket_depth = 0;
  for (const Token& t : toks) {
    switch (t.kind) {
      case TokenKind::Indent:
        ++level;
        continue;
      case TokenKind::Dedent:
        if (level > 0) --level;
        continue;
      case TokenKind::Newline:
      case TokenKind::EndOfFile:
        continue;
      default:
        break;
    }
    if (t.span.line < line_level.size() && line_level[t.span.line] < 0) {
      line_level[t.span.line] = bracket_depth == 0 ? level : -1;
    }
    if (t.kind == TokenKind::LBracket || t.kind == TokenKind::LBrace ||
        t.kind == TokenKind::LParen) {
      ++bracket_depth;
    } else if (t.kind == TokenKind::RBracket || t.kind == TokenKind::RBrace ||
               t.kind == TokenKind::RParen) {
      if (bracket_depth > 0) --bracket_depth;
    }
  }

  const std::uint32_t n = src.line_count();
  std::string out;
  // Normalize to the file's own indentation style: 1 tab per level in tab
  // files, 2 spaces per level in space files (default when no line is indented).
  const bool tab_style = lexer.indent_style() == IndentStyle::Tabs;
  for (std::uint32_t l = 1; l <= n; ++l) {
    const std::string_view raw = src.line_text(l);
    if (in_string[l]) {
      out.append(raw.data(), raw.size());  // verbatim, even trailing spaces
    } else {
      const std::string trimmed = trim_right(raw);
      if (l < line_level.size() && line_level[l] >= 0) {
        if (tab_style) {
          out.append(static_cast<std::size_t>(line_level[l]), '\t');
        } else {
          out.append(static_cast<std::size_t>(line_level[l]) * 2, ' ');
        }
        std::size_t lead = 0;
        while (lead < trimmed.size() && (trimmed[lead] == ' ' || trimmed[lead] == '\t')) ++lead;
        out.append(trimmed.data() + lead, trimmed.size() - lead);
      } else {
        out.append(trimmed);  // blank / comment / continuation line
      }
    }
    out.push_back('\n');
  }
  return out;
}

}  // namespace tilt::lsp
