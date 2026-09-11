#include "lsp/completion.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"

namespace tilt::lsp {

namespace {

bool ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

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

const std::array<std::string_view, 20> kDeclKeywords = {
    "tipo",   "funcao",   "seja",   "constante", "importar", "de",     "fonte",
    "pipeline", "verificar", "modelo", "treino",  "tarefa",   "experimento", "llm",
    "indice", "fluxo",    "ferramenta", "agente", "equipe",  "servico"};

const std::array<std::string_view, 7> kStmtKeywords = {
    "se", "senao", "para cada", "enquanto", "tentar", "capturar", "retornar"};

const std::array<std::string_view, 59> kBuiltins = {
    "imprimir",  "registrar",  "env",        "tamanho",     "contar",  "somar",
    "media",     "min",        "max",        "intervalo",   "dividir", "ler_csv",
    "escrever_csv", "ler_json", "escrever_json", "ler",      "carregador", "perguntar",
    "incorporar", "dividir_texto", "responder", "tensor",   "zeros",   "checar_tilt",
    "executar_sql",
    "ler_parquet", "escrever_parquet", "ler_delta", "escrever_delta", "anexar_delta",
    "ler_iceberg", "escrever_iceberg", "anexar_iceberg",
    "ler_redis", "escrever_redis", "redis_executar", "redis_lote", "ler_kafka",
    "escrever_kafka", "mongo_inserir",
    "mongo_buscar", "mongo_atualizar", "mongo_deletar", "mongo_criar_indice",
    "mongo_agregar",
    "ler_s3", "escrever_s3", "listar_s3", "apagar_s3",
    "copiar_s3", "cabecalho_s3", "s3_iniciar_upload", "s3_enviar_parte",
    "s3_concluir_upload", "s3_abortar_upload", "http_get_json", "http_post_json",
    "es_buscar", "es_executar"};

const std::array<std::string_view, 11> kTableMethods = {
    "filtrar",  "derivar",   "mapear",   "agrupar_por", "selecionar", "ordenar_por",
    "limite",   "primeiros", "distinto", "tamanho",     "inserir"};

const std::array<std::string_view, 17> kTensorMethods = {
    "forma",      "dados", "matmul", "transposta", "reformar", "conv2d", "norma_lote",
    "relu",       "gelu",  "silu",   "sigmoide",   "tanh",     "softmax",
    "soma",       "media", "argmax", "item"};

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
       {"dados", "perda", "otimizador", "epocas", "taxa", "taxa_aprendizado", "lote", "verboso"}},
      {"tarefa", {"entrada", "executar"}},
      {"indice", {"embeddings", "armazenamento", "dimensao", "metrica"}},
      {"fonte", {"tipo", "caminho", "arquivo", "url", "consulta", "formato", "brokers", "topico"}},
      {"pipeline", {"passos", "agenda", "ao_falhar"}},
      {"fluxo", {"entrada", "passos"}},
      {"ferramenta", {"descricao", "entrada", "executar"}},
      {"agente", {"llm", "papel", "ferramentas", "memoria", "max_passos"}},
      {"equipe", {"agentes", "estrategia", "supervisor", "objetivo"}},
      {"servico", {"porta", "dispositivo", "meio", "rota"}},
      {"verificar", {"nao_nulo", "unico", "intervalo", "ao_violar"}},
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
      {"env", "env(nome)", "nome", "Le o valor de uma variavel de ambiente.",
       "env \"HOME\""},
      {"tamanho", "tamanho(colecao)", "colecao", "Tamanho de lista, texto, mapa ou tabela.",
       nullptr},
      {"contar", "contar(colecao)", "colecao", "Conta elementos de uma tabela ou lista.",
       nullptr},
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
      {"tensor", "tensor([[...], [...]])", "dados",
       "Cria um tensor a partir de listas aninhadas.", "tensor [[1, 2], [3, 4]]"},
      {"zeros", "zeros([dim, ...])", "dims", "Tensor preenchido com zeros.", nullptr},
      {"checar_tilt", "checar_tilt(caminho)", "caminho",
       "Valida a sintaxe de um arquivo .tilt.", nullptr},
      {"executar_sql", "executar_sql(conexao, consulta)", "conexao,consulta",
       "Executa uma consulta SQL em uma conexao.", nullptr},
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
      {"ler_redis", "ler_redis(conexao, chave)", "conexao,chave", "Le um valor do Redis.",
       nullptr},
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
      {"copiar_s3", "copiar_s3(origem, destino)", "origem,destino",
       "Copia um objeto no S3.", nullptr},
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
  };
  return docs;
}

const BuiltinDoc* find_builtin_doc(std::string_view name) {
  for (const auto& d : builtin_docs()) {
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
      {"tarefa", "Declara uma tarefa de um experimento."},
      {"experimento", "Declara um experimento de ML."},
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
  std::string kind;  // declaring keyword, or "variavel" / "parametro"
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
    out.push_back({s.a->text, s.a->span, "variavel"});
  }
  if (s.kind == ast::StmtKind::ForEach && !s.name.empty()) {
    out.push_back({s.name, token_span_after(toks, s.name, s.span.offset), "variavel"});
  }
  if (s.kind == ast::StmtKind::Try && !s.name.empty()) {
    out.push_back({s.name, token_span_after(toks, s.name, s.span.offset), "variavel"});
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
    if (!it.header.empty() && it.header[0] && it.header[0]->kind == ast::ExprKind::Name) {
      out.push_back({it.header[0]->text, it.header[0]->span, it.key});
    }
    if (it.key == "funcao") {
      std::uint32_t from = it.span.offset;
      for (const auto& p : it.params) {
        if (p.name.empty()) continue;
        const Span sp = token_span_after(toks, p.name, from);
        out.push_back({p.name, sp, "parametro"});
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

}  // namespace

std::vector<CompletionItem> complete(const SourceFile& src, std::uint32_t line, std::uint32_t column) {
  std::vector<CompletionItem> out;

  const std::string_view text = src.line_text(line);
  std::uint32_t c = column > 0 ? column - 1 : 0;
  if (c > text.size()) c = static_cast<std::uint32_t>(text.size());

  std::uint32_t start = c;
  while (start > 0 && ident_char(text[start - 1])) --start;
  const std::string_view prefix = text.substr(start, c - start);
  const bool dot = start > 0 && text[start - 1] == '.';
  const bool at_line_head = text.substr(0, start).find_first_not_of(" \t") == std::string_view::npos;

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
  for (auto k : kStmtKeywords) push(out, prefix, k, "keyword", "instrucao");

  // Names declared in this file.
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  for (const auto& it : prog.items) {
    if (!it || it->kind != ast::ItemKind::Decl) continue;
    if (it->header.empty() || !it->header[0] || it->header[0]->kind != ast::ExprKind::Name) continue;
    push(out, prefix, it->header[0]->text, "name", "declarado em '" + it->key + "'");
  }

  std::sort(out.begin(), out.end(),
            [](const CompletionItem& a, const CompletionItem& b) { return a.label < b.label; });
  return out;
}

std::string hover(const SourceFile& src, std::uint32_t line, std::uint32_t column) {
  bool as_member = false;
  const std::string word = word_at(src, line, column, &as_member);
  if (word.empty()) return {};

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
  const auto decls = collect_decls(prog, toks);
  if (const NameDecl* d = resolve_decl(decls, word, line, column)) {
    std::string md = std::string("`") + d->kind + " " + word + "` — declarado na linha " +
                     std::to_string(d->span.line) + ".";
    const std::string_view decl_line = src.line_text(d->span.line);
    if (!decl_line.empty()) {
      md += std::string("\n\n```tilt\n") + std::string(trim_right(decl_line)) + "\n```";
    }
    return md;
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
        case '"': in_text = true; break;
        case '(':
        case '[':
        case '{': ++depth; break;
        case ')':
        case ']':
        case '}': if (depth > 0) --depth; break;
        case ',': if (depth == count_at && commas) ++*commas; break;
        default: break;
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
      case TokenKind::Indent: ++level; continue;
      case TokenKind::Dedent: if (level > 0) --level; continue;
      case TokenKind::Newline:
      case TokenKind::EndOfFile: continue;
      default: break;
    }
    if (t.span.line < line_level.size() && line_level[t.span.line] < 0) {
      line_level[t.span.line] = bracket_depth == 0 ? level : -1;
    }
    if (t.kind == TokenKind::LBracket || t.kind == TokenKind::LBrace || t.kind == TokenKind::LParen) {
      ++bracket_depth;
    } else if (t.kind == TokenKind::RBracket || t.kind == TokenKind::RBrace ||
               t.kind == TokenKind::RParen) {
      if (bracket_depth > 0) --bracket_depth;
    }
  }

  const std::uint32_t n = src.line_count();
  std::string out;
  for (std::uint32_t l = 1; l <= n; ++l) {
    const std::string_view raw = src.line_text(l);
    if (in_string[l]) {
      out.append(raw.data(), raw.size());  // verbatim, even trailing spaces
    } else {
      const std::string trimmed = trim_right(raw);
      if (l < line_level.size() && line_level[l] >= 0) {
        out.append(static_cast<std::size_t>(line_level[l]) * 2, ' ');
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
