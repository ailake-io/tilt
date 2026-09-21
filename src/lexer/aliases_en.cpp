#include "lexer/aliases_en.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tilt {

namespace {

// {ingles, portugues, papeis}. Ver aliases_en.hpp para o significado dos papeis.
// Uma palavra pode aparecer em mais de uma linha (papeis ou traducoes diferentes).
const std::vector<AliasEn> kTabela = {
    // ---- controle de fluxo, literais e operadores (em qualquer posicao) ----
    {"if", "se", "C"},
    {"else", "senao", "C"},
    {"for", "para", "C"},
    {"each", "cada", "C"},
    {"in", "em", "C"},
    {"while", "enquanto", "C"},
    {"try", "tentar", "C"},
    {"catch", "capturar", "C"},
    {"return", "retornar", "C"},
    {"break", "parar", "C"},
    {"continue", "continuar", "C"},
    {"and", "e", "C"},
    {"or", "ou", "C"},
    {"not", "nao", "C"},
    {"contains", "contem", "C"},
    {"true", "verdadeiro", "C"},
    {"false", "falso", "C"},
    {"null", "nulo", "C"},
    {"let", "seja", "C"},
    {"const", "constante", "C"},
    {"function", "funcao", "C"},
    {"import", "importar", "C"},
    {"from", "de", "C"},
    {"as", "como", "C"},
    {"device", "dispositivo", "CK"},  // `on device cuda:0` (par com `on`) e `device: auto`

    // ---- declaracoes e chaves de bloco (inicio de linha, antes de ':') ----
    {"type", "tipo", "K"},
    {"llm", "llm", "K"},
    {"source", "fonte", "K"},
    {"pipeline", "pipeline", "K"},
    {"verify", "verificar", "K"},
    {"not_null", "nao_nulo", "K"},
    {"unique", "unico", "K"},
    {"range", "intervalo", "KN"},
    {"on_violation", "ao_violar", "K"},
    {"one_hot", "um_de_n", "KV"},
    {"standardize", "padronizar", "KV"},
    {"impute", "imputar", "KV"},
    {"model", "modelo", "KN"},
    {"train", "treino", "K"},
    {"search", "busca", "K"},
    {"task", "tarefa", "K"},
    {"experiment", "experimento", "KN"},
    {"evaluation", "avaliacao", "K"},
    {"index", "indice", "K"},
    {"flow", "fluxo", "K"},
    {"tool", "ferramenta", "K"},
    {"agent", "agente", "K"},
    {"team", "equipe", "K"},
    {"service", "servico", "K"},
    {"test", "teste", "K"},
    {"steps", "passos", "K"},
    {"route", "rota", "K"},
    {"schedule", "agenda", "K"},
    {"on_failure", "ao_falhar", "K"},
    {"window", "janela", "K"},
    {"provider", "provedor", "K"},
    {"temperature", "temperatura", "K"},
    {"key", "chave", "K"},
    {"time_limit", "tempo_limite", "K"},
    {"attempts", "tentativas", "K"},
    {"token_cap", "teto_tokens", "K"},
    {"fallback", "reserva", "K"},
    {"storage", "armazenamento", "K"},
    {"dimension", "dimensao", "K"},
    {"metric", "metrica", "K"},
    {"description", "descricao", "K"},
    {"input", "entrada", "KN"},
    {"row", "linha", "N"},
    {"rows", "linhas", "N"},
    {"epoch", "epoca", "N"},
    {"step", "passo", "N"},
    {"result", "resultado", "N"},
    {"output", "saida", "K"},
    {"execute", "executar", "K"},
    {"role", "papel", "K"},
    {"tools", "ferramentas", "K"},
    {"memory", "memoria", "K"},
    {"max_steps", "max_passos", "K"},
    {"agents", "agentes", "K"},
    {"strategy", "estrategia", "K"},
    {"goal", "objetivo", "K"},
    {"port", "porta", "K"},
    {"middleware", "meio", "K"},
    {"layers", "camadas", "K"},
    {"dense", "densa", "K"},
    {"dropout", "abandono", "K"},
    {"activation", "ativacao", "K"},
    {"loss", "perda", "K"},
    {"optimizer", "otimizador", "K"},
    {"epochs", "epocas", "K"},
    {"batch", "lote", "K"},
    {"seed", "semente", "K"},
    {"scheduler", "agendador", "K"},
    {"early_stop", "parar_cedo", "K"},
    {"validation", "validacao", "K"},
    {"cross_validation", "validacao_cruzada", "K"},
    {"resume", "retomar", "K"},
    {"every", "a_cada", "K"},
    {"weights", "pesos", "K"},
    {"data", "dados", "K"},
    {"target", "alvo", "K"},
    {"features", "atributos", "K"},
    {"preprocess", "pre_processar", "K"},
    {"split", "dividir", "KN"},
    {"metrics", "metricas", "KN"},
    {"path", "caminho", "K"},
    {"query", "consulta", "K"},
    {"topic", "topico", "K"},
    {"format", "formato", "KA"},
    {"rate", "taxa", "K"},
    {"learning_rate", "taxa_aprendizado", "K"},
    {"criterion", "criterio", "K"},
    {"grid", "grade", "K"},
    {"threshold", "limiar", "K"},
    {"tolerance", "tolerancia", "K"},
    {"sample", "amostra", "K"},
    {"shuffle", "embaralhar", "K"},
    {"verbose", "verboso", "K"},
    {"quarantine", "quarentena", "K"},
    {"partition_by", "particionar_por", "KA"},
    {"on_epoch", "ao_epoca", "K"},
    {"system", "sistema", "KA"},
    {"user", "usuario", "KA"},
    {"depends_on", "depende_de", "K"},
    {"respond", "responder", "KN"},
    {"respond_stream", "responder_em_fluxo", "KN"},
    {"pretrained", "pesos", "K"},
    {"checkpoint", "checkpoint", "K"},

    // ---- funcoes embutidas e da biblioteca padrao (nomes) ----
    {"print", "imprimir", "N"},
    {"length", "tamanho", "N"},
    {"size", "tamanho", "N"},
    {"count", "contar", "N"},
    {"sum", "somar", "N"},
    {"mean", "media", "N"},
    {"ones", "uns", "N"},
    {"random", "aleatorio", "N"},
    {"register", "registrar", "N"},
    {"abort", "abortar", "N"},
    {"warn", "avisar", "N"},
    {"ask", "perguntar", "N"},
    {"ask_stream", "perguntar_em_fluxo", "N"},
    {"embed", "incorporar", "N"},
    {"split_text", "dividir_texto", "N"},
    {"loader", "carregador", "N"},
    {"read", "ler", "N"},
    {"read_csv", "ler_csv", "N"},
    {"read_json", "ler_json", "N"},
    {"read_parquet", "ler_parquet", "N"},
    {"read_delta", "ler_delta", "N"},
    {"read_iceberg", "ler_iceberg", "N"},
    {"read_kafka", "ler_kafka", "N"},
    {"read_redis", "ler_redis", "N"},
    {"read_s3", "ler_s3", "N"},
    {"read_text", "ler_texto", "N"},
    {"write_csv", "escrever_csv", "N"},
    {"write_json", "escrever_json", "N"},
    {"write_parquet", "escrever_parquet", "N"},
    {"write_delta", "escrever_delta", "N"},
    {"write_iceberg", "escrever_iceberg", "N"},
    {"write_kafka", "escrever_kafka", "N"},
    {"write_redis", "escrever_redis", "N"},
    {"write_s3", "escrever_s3", "N"},
    {"write_text", "escrever_texto", "N"},
    {"append_text", "anexar_texto", "N"},
    {"append_delta", "anexar_delta", "N"},
    {"append_iceberg", "anexar_iceberg", "N"},
    {"list_s3", "listar_s3", "N"},
    {"delete_s3", "apagar_s3", "N"},
    {"copy_s3", "copiar_s3", "N"},
    {"list_files", "listar_arquivos", "N"},
    {"remove_file", "remover_arquivo", "N"},
    {"run_sql", "executar_sql", "N"},
    {"query_sql", "consultar_sql", "N"},
    {"transaction", "transacao", "N"},
    {"call_python", "chamar_python", "N"},
    {"map", "mapear", "N"},
    {"filter", "filtrar", "N"},
    {"reduce", "reduzir", "N"},
    {"any", "qualquer", "N"},
    {"all", "todos", "N"},
    {"assert", "afirmar", "N"},
    {"assert_equal", "afirmar_igual", "N"},
    {"now", "agora", "N"},
    {"sleep", "dormir", "N"},
    {"type_of", "tipo_de", "N"},
    {"integer", "inteiro", "NT"},
    {"boolean", "logico", "NT"},
    {"text", "texto", "NT"},
    {"float", "decimal", "NT"},
    {"list", "lista", "NT"},
    {"optional", "opcional", "NT"},
    {"table", "tabela", "NT"},
    {"stream", "fluxo", "NT"},
    {"trace", "rastro", "N"},
    {"probability", "probabilidade", "N"},
    {"rate_limit", "limite_taxa", "K"},
    {"request_logging", "registro_requisicoes", "N"},
    {"train", "treino", "B"},
    {"test", "teste", "B"},
    {"per_minute", "por_minuto", "B"},
    {"validation", "validacao", "B"},
    {"floor", "piso", "N"},
    {"ceil", "teto", "N"},
    {"round", "arredondar", "N"},
    {"power", "potencia", "N"},
    {"sqrt", "raiz", "N"},
    {"sin", "seno", "N"},
    {"cos", "cosseno", "N"},
    {"tan", "tangente", "N"},
    {"ln", "logaritmo", "N"},
    {"keys", "chaves", "N"},
    {"values", "valores", "N"},
    {"upper", "maiusculas", "N"},
    {"lower", "minusculas", "N"},
    {"trim", "aparar", "N"},
    {"replace", "substituir", "N"},
    {"join", "juntar", "N"},
    {"starts_with", "comeca_com", "N"},
    {"ends_with", "termina_com", "N"},
    {"sort", "ordenar", "N"},
    {"reverse", "reverso", "N"},
    {"unique", "unicos", "N"},
    {"enumerate", "enumerar", "N"},
    {"format_date", "formatar_data", "N"},
    {"regex_match", "regex_casa", "N"},
    {"regex_extract", "regex_extrair", "N"},
    {"regex_replace", "regex_substituir", "N"},
    {"json_read", "json_ler", "N"},
    {"json_text", "json_texto", "N"},
    {"base64_encode", "base64_codificar", "N"},
    {"base64_decode", "base64_decodificar", "N"},
    {"retry", "repetir", "N"},
    {"wait", "espera", "A"},
    {"optimize_delta", "otimizar_delta", "N"},
    {"optimize_iceberg", "otimizar_iceberg", "N"},
    {"rerank", "reranquear", "N"},

    // ---- valores de vocabulario (nomes usados como valor de uma chave) ----
    {"cross_entropy", "entropia_cruzada", "V"},
    {"quadratic", "quadratica", "V"},
    {"linear_regression", "regressao_linear", "V"},
    {"logistic_regression", "regressao_logistica", "V"},
    {"random_forest", "floresta_aleatoria", "V"},
    {"gradient_boosting", "gradiente_impulsionado", "V"},
    {"accuracy", "acuracia", "V"},
    {"sequential", "sequencial", "V"},
    {"parallel", "paralelo", "V"},
    {"conversation", "conversa", "V"},
    {"vector", "vetorial", "V"},
    {"cosine", "cosseno", "V"},

    // ---- metodos (depois de '.', com argumentos) ----
    {"filter", "filtrar", "M"},
    {"derive", "derivar", "M"},
    {"group_by", "agrupar_por", "M"},
    {"select", "selecionar", "M"},
    {"order_by", "ordenar_por", "M"},
    {"map", "mapear", "M"},
    {"head", "primeiros", "M"},
    {"limit", "limite", "M"},
    {"insert", "inserir", "M"},
    {"search", "buscar", "M"},
    {"save_weights", "salvar_pesos", "M"},
    {"load_weights", "carregar_pesos", "M"},
    {"export_onnx", "exportar_onnx", "M"},
    {"export_gguf", "exportar_gguf", "M"},
    {"run", "executar", "M"},
    {"forward", "para_frente", "M"},
    {"predict", "prever", "M"},
    {"reshape", "reformar", "M"},
    {"respond", "responder", "M"},
    {"ask", "perguntar", "M"},
    {"sum", "soma", "M"},

    // ---- metodos sem argumentos (depois de '.'; ver `receptor_de_dados`) ----
    {"shape", "forma", "Z"},
    {"transpose", "transposta", "Z"},
    {"distinct", "distinto", "Z"},
    {"upper", "maiusculas", "Z"},
    {"lower", "minusculas", "Z"},
    {"mean", "media", "Z"},

    // ---- chaves de mapa do vocabulario (`split: { train: .. }`), papel B ----
    // (ver as linhas com papel "B" acima)

    // ---- argumentos nomeados (`chave: valor` fora de `{ }`) ----
    {"top_k", "top_k", "A"},
    {"versions", "versoes", "A"},
};

struct Indice {
  // palavra -> (papel -> portugues)
  std::unordered_map<std::string_view, std::vector<std::pair<char, std::string_view>>> por_palavra;
  Indice() {
    for (const AliasEn& a : kTabela) {
      for (const char* p = a.papeis; *p != '\0'; ++p) {
        por_palavra[a.en].emplace_back(*p, a.pt);
      }
    }
  }
  // `w` (ja em portugues) e uma chave de bloco do vocabulario?
  std::string_view buscar_pt_chave(std::string_view w) const {
    for (const AliasEn& a : kTabela) {
      if (std::string_view(a.pt) == w &&
          std::string_view(a.papeis).find('K') != std::string_view::npos) {
        return w;
      }
    }
    return {};
  }
  std::string_view buscar(std::string_view en, char papel) const {
    const auto it = por_palavra.find(en);
    if (it == por_palavra.end()) return {};
    for (const auto& [p, pt] : it->second) {
      if (p == papel) return pt;
    }
    return {};
  }
};

const Indice& indice() {
  static const Indice* const kIndice = new Indice();
  return *kIndice;
}

// Chaves cujo bloco filho traz nomes do usuario (campos, parametros, colunas):
// dentro dele as chaves nao sao traduzidas.
bool dono_de_dados(std::string_view pt) {
  return pt == "tipo" || pt == "entrada" || pt == "saida" || pt == "dados";
}

bool inicio_de_linha(const std::vector<Token>& t, std::size_t i) {
  if (i == 0) return true;
  const TokenKind p = t[i - 1].kind;
  if (p == TokenKind::Newline || p == TokenKind::Indent || p == TokenKind::Dedent) return true;
  // `- palavra`: o traco abre item de lista no inicio da linha.
  return p == TokenKind::Dash && (i < 2 || inicio_de_linha(t, i - 1));
}

// Indice do Newline que fecha a linha de `i` (ou do fim do vetor).
std::size_t fim_da_linha(const std::vector<Token>& t, std::size_t i) {
  while (i < t.size() && t[i].kind != TokenKind::Newline && t[i].kind != TokenKind::EndOfFile) {
    ++i;
  }
  return i;
}

bool linha_tem_dois_pontos(const std::vector<Token>& t, std::size_t i) {
  const std::size_t fim = fim_da_linha(t, i);
  for (; i < fim; ++i) {
    if (t[i].kind == TokenKind::Colon) return true;
  }
  return false;
}

// A linha termina em ':' e a proxima abre um bloco indentado.
bool abre_bloco(const std::vector<Token>& t, std::size_t i) {
  const std::size_t fim = fim_da_linha(t, i);
  return fim > i && fim + 1 < t.size() && t[fim - 1].kind == TokenKind::Colon &&
         t[fim + 1].kind == TokenKind::Indent;
}

// Traducao de `w` como chave de bloco (portugues canonico), ou "" se nao e chave.
std::string_view idx_traducao_chave(std::string_view w) {
  if (const std::string_view k = indice().buscar(w, 'K'); !k.empty()) return k;
  return indice().buscar_pt_chave(w);
}

// Nomes que o arquivo define: nunca traduzidos como funcao/valor.
std::unordered_set<std::string> nomes_do_usuario(const std::vector<Token>& t, std::size_t de_i,
                                                 std::size_t ate_i) {
  std::unordered_set<std::string> u;
  const auto ident = [&](std::size_t i) {
    return i < t.size() && t[i].kind == TokenKind::Identifier;
  };
  const auto ate_fim_da_linha = [&](std::size_t de) {
    const std::size_t fim = fim_da_linha(t, de);
    for (std::size_t j = de; j < fim; ++j) {
      if (t[j].kind == TokenKind::Identifier) u.emplace(t[j].lexeme);
    }
  };
  for (std::size_t i = de_i; i < ate_i; ++i) {
    if (t[i].kind != TokenKind::Identifier) continue;
    const std::string_view w = t[i].lexeme;
    if (i > 0 && (t[i - 1].kind == TokenKind::Dot || t[i - 1].kind == TokenKind::QuestionDot)) {
      continue;
    }
    // alvo de atribuicao: `x = ...`
    if (i + 1 < t.size() && t[i + 1].kind == TokenKind::Equal) u.emplace(w);
    // `let x` / `const x` / `seja x` / `constante x`
    if ((w == "let" || w == "const" || w == "seja" || w == "constante") && ident(i + 1)) {
      u.emplace(t[i + 1].lexeme);
    }
    // `function f a, b:` e lambdas `function x: ...`
    if (w == "function" || w == "funcao") {
      const bool decl = inicio_de_linha(t, i);
      for (std::size_t j = i + 1; j < t.size(); ++j) {
        if (t[j].kind == TokenKind::Newline || t[j].kind == TokenKind::EndOfFile) break;
        if (t[j].kind == TokenKind::Colon &&
            (!decl || (j + 1 < t.size() && t[j + 1].kind == TokenKind::Newline))) {
          break;
        }
        if (t[j].kind == TokenKind::Identifier) u.emplace(t[j].lexeme);
      }
    }
    // `for each x in ...` / `para cada x em ...` e `catch e:` / `capturar erro:`
    if ((w == "each" || w == "cada" || w == "catch" || w == "capturar") && ident(i + 1)) {
      u.emplace(t[i + 1].lexeme);
    }
    // `import a as b` / `from m import a, b`
    if ((w == "import" || w == "importar" || w == "from" || w == "de") && inicio_de_linha(t, i) &&
        !(i + 1 < t.size() && t[i + 1].kind == TokenKind::Colon)) {
      ate_fim_da_linha(i + 1);
    }
    // nome de declaracao: `agent Nome:`, `model Nome:`, `type Nome:` ...
    if (inicio_de_linha(t, i) && ident(i + 1) && abre_bloco(t, i)) {
      const std::string_view pt = idx_traducao_chave(w);
      if (!pt.empty() && pt != "passos") u.emplace(t[i + 1].lexeme);
    }
  }
  // Parametros de ferramenta: `tool nome: input:` + campos (viram variaveis do corpo).
  {
    int prof = 0;
    int tool_ate = -1;
    int input_ate = -1;
    for (std::size_t i = de_i; i < ate_i; ++i) {
      if (t[i].kind == TokenKind::Indent) ++prof;
      if (t[i].kind == TokenKind::Dedent) {
        --prof;
        if (tool_ate >= 0 && prof <= tool_ate) tool_ate = -1;
        if (input_ate >= 0 && prof <= input_ate) input_ate = -1;
      }
      if (t[i].kind != TokenKind::Identifier || !inicio_de_linha(t, i)) continue;
      const std::string_view w = t[i].lexeme;
      if ((w == "tool" || w == "ferramenta") && abre_bloco(t, i))
        tool_ate = prof;
      else if (tool_ate >= 0 && (w == "input" || w == "entrada") && abre_bloco(t, i))
        input_ate = prof;
      else if (input_ate >= 0 && i + 1 < t.size() && t[i + 1].kind == TokenKind::Colon) {
        u.emplace(w);
      }
    }
  }
  // Nomes implicitos do runtime (`linha`, `entrada`...): renomeados de forma
  // consistente em todo o arquivo (o `for each row` e o `filter row.x` combinam).
  for (const char* magico : {"row", "rows", "epoch", "step", "result", "input", "metrics"}) {
    u.erase(magico);
  }
  return u;
}

// Nomes definidos no nivel de topo (valem no arquivo todo): funcoes, `let`/`const`,
// importacoes e nomes de declaracoes.
std::unordered_set<std::string> nomes_globais(const std::vector<Token>& t) {
  std::unordered_set<std::string> g;
  int prof = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i].kind == TokenKind::Indent) ++prof;
    if (t[i].kind == TokenKind::Dedent) --prof;
    if (prof != 0 || t[i].kind != TokenKind::Identifier || !inicio_de_linha(t, i)) continue;
    const std::string_view w = t[i].lexeme;
    const bool tem_nome = i + 1 < t.size() && t[i + 1].kind == TokenKind::Identifier;
    if ((w == "function" || w == "funcao" || w == "let" || w == "const" || w == "seja" ||
         w == "constante") &&
        tem_nome) {
      g.emplace(t[i + 1].lexeme);
    } else if (w == "import" || w == "importar" || w == "from" || w == "de") {
      const std::size_t fim = fim_da_linha(t, i);
      for (std::size_t j = i + 1; j < fim; ++j) {
        if (t[j].kind == TokenKind::Identifier) g.emplace(t[j].lexeme);
      }
    } else if (tem_nome && abre_bloco(t, i) && !idx_traducao_chave(w).empty() && w != "steps" &&
               w != "passos") {
      g.emplace(t[i + 1].lexeme);
    }
  }
  return g;
}

// Inicios (indices de token) de cada declaracao de nivel de topo.
std::vector<std::size_t> inicios_de_topo(const std::vector<Token>& t) {
  std::vector<std::size_t> v;
  int prof = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i].kind == TokenKind::Indent) ++prof;
    if (t[i].kind == TokenKind::Dedent) --prof;
    if (prof == 0 && t[i].kind == TokenKind::Identifier && inicio_de_linha(t, i)) v.push_back(i);
  }
  return v;
}

// Um token que pode abrir um argumento de chamada sem parenteses.
bool comeca_argumento(const Token& t) {
  switch (t.kind) {
    case TokenKind::Identifier:
      return t.lexeme != "and" && t.lexeme != "or" && t.lexeme != "in" && t.lexeme != "contains" &&
             t.lexeme != "if" && t.lexeme != "else" && t.lexeme != "e" && t.lexeme != "ou" &&
             t.lexeme != "em" && t.lexeme != "contem" && t.lexeme != "se" && t.lexeme != "senao";
    case TokenKind::Integer:
    case TokenKind::Decimal:
    case TokenKind::Text:
    case TokenKind::LParen:
    case TokenKind::LBrace:
    case TokenKind::LBracket:
      return true;
    default:
      return false;
  }
}

// `linha.count` / `row.shape`: coluna de dados, nao metodo.
bool receptor_de_dados(const std::vector<Token>& t, std::size_t ponto) {
  if (ponto == 0 || t[ponto - 1].kind != TokenKind::Identifier) return false;
  const std::string_view r = t[ponto - 1].lexeme;
  return r == "linha" || r == "row" || r == "entrada" || r == "input" || r == "passo" ||
         r == "step" || r == "resultado" || r == "result";
}

}  // namespace

const std::vector<AliasEn>& tabela_aliases_en() { return kTabela; }

std::string_view alias_en_para_pt(std::string_view en) {
  static const char kPapeis[] = {'C', 'K', 'N', 'V', 'M', 'Z', 'A'};
  for (const char p : kPapeis) {
    const std::string_view pt = indice().buscar(en, p);
    if (!pt.empty()) return pt;
  }
  return {};
}

Idioma idioma_do_fonte(std::string_view fonte) {
  std::size_t pos = 0;
  for (int linhas = 0; linhas < 5 && pos < fonte.size(); ++linhas) {
    std::size_t fim = fonte.find('\n', pos);
    if (fim == std::string_view::npos) fim = fonte.size();
    std::string_view l = fonte.substr(pos, fim - pos);
    pos = fim + 1;
    while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
    if (l.empty()) continue;
    if (l.front() != '#') return Idioma::Auto;
    l.remove_prefix(1);
    while (!l.empty() && l.front() == ' ') l.remove_prefix(1);
    for (const std::string_view chave : {"idioma:", "language:", "lang:"}) {
      if (l.substr(0, chave.size()) != chave) continue;
      l.remove_prefix(chave.size());
      while (!l.empty() && l.front() == ' ') l.remove_prefix(1);
      if (l.substr(0, 2) == "en") return Idioma::Ingles;
      if (l.substr(0, 2) == "pt") return Idioma::Portugues;
    }
  }
  return Idioma::Auto;
}

namespace {

// Palavras estruturais (chave de bloco / controle) no inicio das linhas: ingles vs
// portugues. So conta as que diferem entre os idiomas.
bool predomina_ingles(const std::vector<Token>& t) {
  static const std::unordered_set<std::string_view>* const kEn = [] {
    auto* s = new std::unordered_set<std::string_view>();
    for (const AliasEn& a : kTabela) {
      const std::string_view papeis = a.papeis;
      if ((papeis.find('K') != std::string_view::npos ||
           papeis.find('C') != std::string_view::npos) &&
          std::string_view(a.en) != a.pt) {
        s->insert(a.en);
      }
    }
    return s;
  }();
  static const std::unordered_set<std::string_view>* const kPt = [] {
    auto* s = new std::unordered_set<std::string_view>();
    for (const AliasEn& a : kTabela) {
      const std::string_view papeis = a.papeis;
      if ((papeis.find('K') != std::string_view::npos ||
           papeis.find('C') != std::string_view::npos) &&
          std::string_view(a.en) != a.pt) {
        s->insert(a.pt);
      }
    }
    return s;
  }();
  int en = 0;
  int pt = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i].kind != TokenKind::Identifier || !inicio_de_linha(t, i)) continue;
    if (i + 1 < t.size() &&
        (t[i + 1].kind == TokenKind::Equal || t[i + 1].kind == TokenKind::Dot)) {
      continue;  // atribuicao / acesso: nome do usuario
    }
    if (kEn->count(t[i].lexeme) != 0) ++en;
    if (kPt->count(t[i].lexeme) != 0) ++pt;
  }
  return en > pt;
}

}  // namespace

void aplicar_aliases_en(std::vector<Token>& t, Idioma idioma) {
  if (idioma == Idioma::Portugues) return;
  if (idioma == Idioma::Auto && !predomina_ingles(t)) return;
  const Indice& idx = indice();
  // Nomes do usuario: os de topo valem no arquivo todo; variaveis, parametros e
  // atribuicoes valem so dentro da declaracao onde aparecem.
  const std::unordered_set<std::string> globais = nomes_globais(t);
  const std::vector<std::size_t> topo = inicios_de_topo(t);
  std::vector<std::unordered_set<std::string>> locais;
  locais.reserve(topo.size());
  for (std::size_t k = 0; k < topo.size(); ++k) {
    locais.push_back(nomes_do_usuario(t, topo[k], k + 1 < topo.size() ? topo[k + 1] : t.size()));
  }
  std::size_t seg = 0;
  const auto do_usuario = [&](std::string_view w) {
    const std::string chave(w);
    return globais.count(chave) != 0 || (seg < locais.size() && locais[seg].count(chave) != 0);
  };

  int profundidade = 0;  // niveis de indentacao
  int dados_ate = -1;    // >= 0: dentro de um bloco de nomes do usuario (abaixo deste nivel)
  int chaves = 0;        // `{ }` abertas: chaves de mapa sao dados
  int colchetes = 0;     // `[ ]` abertas
  std::string_view chave_linha;  // chave do vocabulario que abre a linha atual (ja em portugues)

  for (std::size_t i = 0; i < t.size(); ++i) {
    Token& tok = t[i];
    while (seg + 1 < topo.size() && topo[seg + 1] <= i) ++seg;
    switch (tok.kind) {
      case TokenKind::Indent:
        ++profundidade;
        break;
      case TokenKind::Dedent:
        --profundidade;
        if (dados_ate >= 0 && profundidade <= dados_ate) dados_ate = -1;
        break;
      case TokenKind::Newline:
        chaves = 0;
        colchetes = 0;
        break;
      case TokenKind::LBrace:
        ++chaves;
        break;
      case TokenKind::RBrace:
        if (chaves > 0) --chaves;
        break;
      case TokenKind::LBracket:
        ++colchetes;
        break;
      case TokenKind::RBracket:
        if (colchetes > 0) --colchetes;
        break;
      default:
        break;
    }
    if (tok.kind != TokenKind::Identifier) continue;

    const std::string_view w = tok.lexeme;
    const bool tem_proximo = i + 1 < t.size();
    const bool apos_ponto =
        i > 0 && (t[i - 1].kind == TokenKind::Dot || t[i - 1].kind == TokenKind::QuestionDot);
    const bool dois_pontos = tem_proximo && t[i + 1].kind == TokenKind::Colon;
    const bool igual = tem_proximo && t[i + 1].kind == TokenKind::Equal;
    const bool ini = inicio_de_linha(t, i);
    const bool em_dados = dados_ate >= 0;

    // ---- depois de '.': so metodo chamado ----
    if (apos_ponto) {
      std::string_view pt = idx.buscar(w, 'M');
      if (!pt.empty() && tem_proximo && comeca_argumento(t[i + 1])) {
        tok.lexeme = pt;
        continue;
      }
      pt = idx.buscar(w, 'Z');
      if (!pt.empty() && !receptor_de_dados(t, i - 1)) tok.lexeme = pt;
      continue;
    }

    // chave de mapa literal ou campo de um bloco de nomes do usuario: dado —
    // exceto as chaves fixas do vocabulario (`split: { train: 0.8, test: 0.2 }`)
    if (dois_pontos && (chaves > 0 || (ini && em_dados))) {
      if (chaves > 0 && (chave_linha == "dividir" || chave_linha == "limite_taxa")) {
        if (const std::string_view b = idx.buscar(w, 'B'); !b.empty()) tok.lexeme = b;
      }
      continue;
    }

    // ---- controle: qualquer posicao ----
    if (!igual) {
      const std::string_view pt = idx.buscar(w, 'C');
      if (!pt.empty()) {
        const std::string_view anterior = i > 0 ? t[i - 1].lexeme : std::string_view();
        const bool par_ok =
            (w != "each" || anterior == "para") && (w != "device" || anterior == "no");
        if (par_ok) {
          tok.lexeme = pt;
          continue;
        }
      }
      if (w == "on" && tem_proximo && t[i + 1].lexeme == "device") {
        tok.lexeme = "no";
        continue;
      }
    }

    // ---- chave de bloco: inicio de linha, antes de ':' ----
    if (ini && !igual && !(tem_proximo && t[i + 1].kind == TokenKind::Dot) &&
        linha_tem_dois_pontos(t, i)) {
      const std::string_view chave = idx.buscar(w, 'K');
      std::string_view canon = w;
      if (!chave.empty() && !em_dados) {
        tok.lexeme = chave;
        canon = chave;
      }
      chave_linha = canon;
      // Filhos deste bloco sao nomes do usuario: `type Pedido:`, `input:`, `data:`.
      if (dono_de_dados(canon) && !em_dados && abre_bloco(t, i)) dados_ate = profundidade;
      if (dois_pontos || !chave.empty()) continue;
    }

    // ---- argumento nomeado: `nome: valor` fora de `{ }` ----
    if (dois_pontos && !ini) {
      const std::string_view pt = idx.buscar(w, 'A');
      if (!pt.empty()) tok.lexeme = pt;
      continue;
    }
    if (dois_pontos) continue;  // chave de linha que nao e do vocabulario: dado

    // nomes de tipo (`text`, `list[text]`, `-> integer`): valem mesmo que o arquivo
    // tenha uma variavel com esse nome (`text: text` numa ferramenta)
    if (const std::string_view ty = idx.buscar(w, 'T');
        !ty.empty() && chaves == 0 && i > 0 &&
        (t[i - 1].kind == TokenKind::Colon || t[i - 1].kind == TokenKind::LBracket ||
         t[i - 1].kind == TokenKind::Greater ||
         (t[i - 1].kind == TokenKind::Comma && colchetes > 0))) {
      tok.lexeme = ty;
      continue;
    }
    if (w == "map" && tem_proximo && t[i + 1].kind == TokenKind::LBracket) {
      tok.lexeme = "mapa";  // tipo `map[text, integer]`
      continue;
    }
    if (do_usuario(w)) continue;
    // valores de vocabulario (`accuracy`, `sequential`) valem ate dentro de listas
    if (const std::string_view v = idx.buscar(w, 'V'); !v.empty()) {
      tok.lexeme = v;
      continue;
    }
    // elemento solto de lista (`[a, b]`) costuma ser coluna: nao traduz
    const bool elemento_de_lista =
        colchetes > 0 && i > 0 &&
        (t[i - 1].kind == TokenKind::LBracket || t[i - 1].kind == TokenKind::Comma) &&
        tem_proximo && (t[i + 1].kind == TokenKind::Comma || t[i + 1].kind == TokenKind::RBracket);
    if (elemento_de_lista) continue;
    if (const std::string_view n = idx.buscar(w, 'N'); !n.empty()) tok.lexeme = n;
  }
}

}  // namespace tilt
