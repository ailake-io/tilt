#include "runtime/iceberg_catalog_server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <thread>

#include "runtime/compat.hpp"
#include "runtime/http_server.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

// ---------------------------------------------------------------------------
// Helpers de codificacao/JSON
// ---------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string erro_json(const std::string& msg, const std::string& tipo, int code) {
  return "{\"error\":{\"message\":\"" + json_escape(msg) + "\",\"type\":\"" + tipo +
         "\",\"code\":\"" + std::to_string(code) + "\"}}";
}

// Percent-decode de um segmento de path (RFC 3986: so %XX; '+' e literal).
std::string pct_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// Percent-encoding de um valor de query (unreserved RFC 3986, como no SigV4).
std::string pct_encode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Deteccao de tabelas + resolucao do metadata mais recente (mesma regra do
// modo Hadoop em iceberg.cpp: versao parseada do nome v<N>, empate
// lexicografico — "v10" > "v2".)
// ---------------------------------------------------------------------------

std::int64_t metadata_version_from_name(const std::string& name) {
  if (name.size() < 2 || name[0] != 'v' || name[1] < '0' || name[1] > '9') return -1;
  try {
    return std::stoll(name.substr(1));
  } catch (const std::exception&) {
    return -1;
  }
}

bool dir_existe(const std::string& path) { return tilt_is_directory(path); }

}  // namespace

std::vector<std::string> iceberg_catalog_tables(const std::string& root) {
  std::vector<std::string> entries;
  std::vector<std::string> out;
  if (!tilt_listdir(root, entries)) return out;
  std::sort(entries.begin(), entries.end());
  for (const std::string& e : entries) {
    if (e == "." || e == "..") continue;
    const std::string dir = root + "/" + e;
    if (dir_existe(dir) && dir_existe(dir + "/metadata")) out.push_back(e);
  }
  return out;
}

namespace {

// Caminho absoluto do v<N>.metadata.json mais recente de <dir>/metadata, ou
// "" se nao houver nenhum.
std::string latest_metadata_path(const std::string& table_dir) {
  const std::string meta_dir = table_dir + "/metadata";
  std::vector<std::string> entries;
  if (!tilt_listdir(meta_dir, entries)) return "";
  const std::string* best = nullptr;
  for (const std::string& e : entries) {
    if (e.size() <= 14 || e.compare(e.size() - 14, 14, ".metadata.json") != 0) continue;
    if (!best) {
      best = &e;
      continue;
    }
    const std::int64_t v = metadata_version_from_name(e);
    const std::int64_t bv = metadata_version_from_name(*best);
    if (v > bv || (v == bv && e > *best)) best = &e;
  }
  return best ? meta_dir + "/" + *best : "";
}

std::string read_file_bytes(const std::string& path, bool& ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ok = false;
    return "";
  }
  std::string out;
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n > 0) {
    out.resize(static_cast<std::size_t>(n));
    in.seekg(0, std::ios::beg);
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
  }
  ok = in.good() || out.empty();
  return out;
}

// ---------------------------------------------------------------------------
// Contexto do handler: imutavel apos o init (thread-safe com workers).
// ---------------------------------------------------------------------------

struct CatalogoCtx {
  std::string root;         // absoluto canonicalizado (weakly_canonical)
  std::string prefix;       // "/v1" (sem barra final)
  std::string anuncio;      // host:porta fallback p/ URLs quando sem Host header
  bool reescrever_manifests = true;
};

// Codifica um caminho relativo segmento a segmento ('/' preservado).
std::string encode_rel(const std::string& rel) {
  std::string out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t slash = rel.find('/', pos);
    out += pct_encode(rel.substr(pos, slash == std::string::npos ? std::string::npos
                                                                 : slash - pos));
    if (slash == std::string::npos) break;
    out += '/';
    pos = slash + 1;
  }
  return out;
}

std::string url_arquivo(const CatalogoCtx& ctx, const std::string& host_hdr,
                        const std::string& abs) {
  const std::string& autoridade = host_hdr.empty() ? ctx.anuncio : host_hdr;
  // Path-style de proposito: o Hadoop Path (cliente Spark) re-encodea query
  // strings ('?' vira %3F) ao montar java.net.URI — com o path relativo ao
  // root em um unico segmento escapado nao ha query para estragar.
  const std::string rel =
      abs.size() > ctx.root.size() ? abs.substr(ctx.root.size() + 1) : "";
  return "http://" + autoridade + ctx.prefix + "/files/" + encode_rel(rel);
}

// O caminho absoluto resolvido fica dentro do root? (canonicalizacao real —
// resolve symlinks das porcoes existentes; o tail inexistente normaliza).
bool dentro_do_root(const CatalogoCtx& ctx, const std::string& abs, std::string& canon) {
  std::error_code ec;
  const std::filesystem::path c = std::filesystem::weakly_canonical(abs, ec);
  if (ec) return false;
  canon = c.string();
  const std::string& r = ctx.root;
  return canon == r || (canon.size() > r.size() && canon.compare(0, r.size(), r) == 0 &&
                        canon[r.size()] == '/');
}

// Reescreve uma location do metadata: file://<abs> (ou <abs> puro) dentro do
// root vira URL deste servidor; qualquer outra coisa (location externa,
// s3://, ...) passa intacta.
std::string reescrever_location(const CatalogoCtx& ctx, const std::string& host_hdr,
                                const std::string& loc) {
  std::string abs;
  if (loc.rfind("file://", 0) == 0) {
    abs = loc.substr(7);
  } else if (!loc.empty() && loc.front() == '/') {
    abs = loc;
  } else {
    return loc;
  }
  std::string canon;
  if (!dentro_do_root(ctx, abs, canon)) return loc;
  return url_arquivo(ctx, host_hdr, canon);
}

// Reescreve recursivamente as locations conhecidas do metadata servido:
// snapshots[].manifest-list e metadata-log[].metadata-file. O campo
// "location" (base da tabela) fica como esta — e so informativo para leitura.
void reescrever_locations(const CatalogoCtx& ctx, const std::string& host_hdr, Value& v) {
  if (v.kind == ValueKind::Texto) return;
  if (v.kind == ValueKind::Lista) {
    if (!v.list) return;
    for (Value& e : *v.list) reescrever_locations(ctx, host_hdr, e);
    return;
  }
  if (v.kind != ValueKind::Mapa || !v.map) return;
  for (auto& kv : v.map->items) {
    const bool chave_reecrita = kv.first == "metadata-file" ||
                                (ctx.reescrever_manifests && kv.first == "manifest-list");
    if (chave_reecrita && kv.second.kind == ValueKind::Texto) {
      kv.second.s = reescrever_location(ctx, host_hdr, kv.second.s);
    } else {
      reescrever_locations(ctx, host_hdr, kv.second);
    }
  }
}

// ---------------------------------------------------------------------------
// Respostas por rota
// ---------------------------------------------------------------------------

HttpResponse resposta_erro(int code, const std::string& msg, const std::string& tipo) {
  HttpResponse resp;
  resp.status = code;
  resp.body = erro_json(msg, tipo, code);
  return resp;
}

HttpResponse resposta_501(const std::string& op) {
  HttpResponse resp = resposta_erro(
      501, "catalogo read-only: " + op +
               " nao e suportado pelo 'tilt servir-catalogo' (1a passada; apenas leitura)",
      "UnsupportedOperationException");
  return resp;
}

HttpResponse rota_arquivos(const CatalogoCtx& ctx, const HttpRequest& req,
                           const std::string& path_param) {
  std::string alvo = path_param;
  if (alvo.rfind("file://", 0) == 0) alvo = alvo.substr(7);
  if (alvo.empty()) {
    return resposta_erro(400, "path esperado em 'path'", "IllegalArgumentException");
  }
  // Caminho relativo ao root (forma path-style /files/<rel>) ou absoluto
  // (forma /files?path=<abs>); ambos canonicalizados e conferidos contra o
  // root — traversal (../, symlink para fora) recebe 403.
  if (alvo.front() != '/') alvo = ctx.root + "/" + alvo;
  std::string canon;
  if (!dentro_do_root(ctx, alvo, canon)) {
    return resposta_erro(403, "path fora do diretorio-raiz do catalogo", "ForbiddenException");
  }
  if (!tilt_file_exists(canon) || tilt_is_directory(canon)) {
    return resposta_erro(404, "arquivo nao encontrado: " + alvo, "NotFoundException");
  }
  const std::string suffix = std::filesystem::path(canon).extension().string();
  HttpResponse resp;
  resp.content_type = suffix == ".json" ? "application/json" : "application/octet-stream";
  if (req.method == "HEAD") return resp;  // 200, so o status importa
  bool ok = false;
  resp.body = read_file_bytes(canon, ok);
  if (!ok) return resposta_erro(500, "falha ao ler '" + canon + "'", "RuntimeException");
  return resp;
}

HttpResponse rota_load_table(const CatalogoCtx& ctx, const HttpRequest& req,
                             const std::string& tabela) {
  if (std::find(tabela.begin(), tabela.end(), '/') != tabela.end() || tabela.empty() ||
      tabela == "." || tabela == "..") {
    return resposta_erro(404, "tabela nao encontrada: " + tabela, "NoSuchTableException");
  }
  const std::string table_dir = ctx.root + "/" + tabela;
  const std::string meta_path = latest_metadata_path(table_dir);
  if (meta_path.empty()) {
    return resposta_erro(404, "tabela nao encontrada: " + tabela, "NoSuchTableException");
  }
  bool ok = false;
  const std::string raw = read_file_bytes(meta_path, ok);
  if (!ok) {
    return resposta_erro(500, "falha ao ler '" + meta_path + "'", "RuntimeException");
  }
  Value md;
  try {
    md = json_parse(raw);
  } catch (const std::exception& e) {
    return resposta_erro(500, "metadata malformado em '" + meta_path + "': " + e.what(),
                         "RuntimeException");
  }
  reescrever_locations(ctx, req.host, md);

  Value out = Value::mapa();
  out.map->set("metadata-location", Value::texto(url_arquivo(ctx, req.host, meta_path)));
  out.map->set("metadata", std::move(md));
  out.map->set("config", Value::mapa());
  HttpResponse resp;
  resp.body = json_dump(out);
  return resp;
}

HttpResponse rota_list_tables(const CatalogoCtx& ctx) {
  const std::vector<std::string> tabelas = iceberg_catalog_tables(ctx.root);
  Value ids = Value::lista();
  for (const std::string& t : tabelas) {
    Value id = Value::mapa();
    Value ns = Value::lista();
    ns.list->push_back(Value::texto("default"));
    id.map->set("namespace", std::move(ns));
    id.map->set("name", Value::texto(t));
    ids.list->push_back(std::move(id));
  }
  Value out = Value::mapa();
  out.map->set("identifiers", std::move(ids));
  HttpResponse resp;
  resp.body = json_dump(out);
  return resp;
}

// Query string -> mapa (k=v separados por '&'; '+' = espaco, form-encoding).
std::string query_param(const std::string& query, const std::string& chave) {
  std::size_t pos = 0;
  while (pos <= query.size()) {
    const std::size_t amp = query.find('&', pos);
    const std::string par = query.substr(pos, amp == std::string::npos ? std::string::npos
                                                                       : amp - pos);
    const std::size_t eq = par.find('=');
    std::string k = pct_decode(par.substr(0, eq));
    for (char& c : k) {
      if (c == '+') c = ' ';
    }
    if (k == chave) {
      std::string v = eq == std::string::npos ? "" : pct_decode(par.substr(eq + 1));
      for (char& c : v) {
        if (c == '+') c = ' ';
      }
      return v;
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return "";
}

HttpResponse despachar(const CatalogoCtx& ctx, const HttpRequest& req) {
  // Separa path e query; o roteamento ignora a query (o cliente REST do
  // Spark anexa parametros ao /config, por exemplo).
  const std::size_t q = req.path.find('?');
  const std::string query = q == std::string::npos ? "" : req.path.substr(q + 1);
  const std::string full_path = q == std::string::npos ? req.path : req.path.substr(0, q);

  const std::string& base = ctx.prefix;
  if (full_path != base && full_path.rfind(base + "/", 0) != 0) {
    return resposta_erro(404, "rota nao encontrada: " + full_path, "NotFoundException");
  }
  const std::string rest =
      full_path.size() == base.size() ? "" : full_path.substr(base.size());

  if (rest == "/config") {
    if (req.method != "GET") return resposta_501("config write");
    return HttpResponse{200, "application/json", "{\"defaults\":{},\"overrides\":{}}"};
  }
  if (rest == "/namespaces") {
    if (req.method != "GET") return resposta_501("namespace write");
    return HttpResponse{200, "application/json", "{\"namespaces\":[[\"default\"]]}"};
  }
  if (rest == "/namespaces/default") {
    if (req.method != "GET") return resposta_501("namespace write");
    return HttpResponse{200, "application/json",
                        "{\"namespace\":[\"default\"],\"properties\":{}}"};
  }
  if (rest == "/namespaces/default/tables") {
    if (req.method == "GET") return rota_list_tables(ctx);
    return resposta_501("createTable");
  }
  const std::string kTablesPrefix = "/namespaces/default/tables/";
  if (rest.rfind(kTablesPrefix, 0) == 0) {
    const std::string sub = rest.substr(kTablesPrefix.size());
    const std::size_t txs = sub.find("/transactions");
    if (txs != std::string::npos && txs + std::string("/transactions").size() == sub.size()) {
      return resposta_501("commit (transactions)");
    }
    if (sub.find('/') != std::string::npos) {
      return resposta_erro(404, "rota nao encontrada: " + full_path, "NotFoundException");
    }
    if (req.method == "GET") return rota_load_table(ctx, req, pct_decode(sub));
    return resposta_501("escrita em tabela (" + req.method + ")");
  }
  if (rest == "/files" || rest.rfind("/files/", 0) == 0) {
    if (req.method == "GET" || req.method == "HEAD") {
      if (rest.size() > std::string("/files").size()) {
        // /files/<rel-ao-root>: cada segmento vem percent-encoded ('/'
        // separa segmentos). Decodifica segmento a segmento e remonta.
        const std::string rel_raw = rest.substr(std::string("/files/").size());
        std::string rel;
        std::size_t pos = 0;
        while (true) {
          const std::size_t slash = rel_raw.find('/', pos);
          rel += pct_decode(rel_raw.substr(
              pos, slash == std::string::npos ? std::string::npos : slash - pos));
          if (slash == std::string::npos) break;
          rel += '/';
          pos = slash + 1;
        }
        return rota_arquivos(ctx, req, rel);
      }
      return rota_arquivos(ctx, req, query_param(query, "path"));
    }
    return resposta_501("escrita de arquivo (" + req.method + ")");
  }
  return resposta_erro(404, "rota nao encontrada: " + full_path, "NotFoundException");
}

}  // namespace

int iceberg_catalog_serve(const IcebergCatalogConfig& cfg) {
  CatalogoCtx ctx;
  std::error_code ec;
  const std::filesystem::path root_canon = std::filesystem::weakly_canonical(cfg.root, ec);
  if (ec || root_canon.empty()) {
    std::cerr << "servir-catalogo: diretorio-raiz invalido: '" << cfg.root << "'\n";
    return 1;
  }
  ctx.root = root_canon.string();
  ctx.prefix = cfg.prefix;
  ctx.reescrever_manifests = cfg.reescrever_manifests;
  while (ctx.prefix.size() > 1 && ctx.prefix.back() == '/') ctx.prefix.pop_back();
  if (ctx.prefix.empty() || ctx.prefix.front() != '/') ctx.prefix = "/v1";
  const std::string bind_host = cfg.host.empty() ? "0.0.0.0" : cfg.host;
  const std::string anuncio_host = bind_host == "0.0.0.0" ? "127.0.0.1" : bind_host;
  ctx.anuncio = anuncio_host + ":" + std::to_string(cfg.port);

  int threads = cfg.threads;
  if (threads <= 0) {
    const unsigned hc = std::thread::hardware_concurrency();
    threads = hc > 0 ? static_cast<int>(std::min(4u, hc)) : 1;
  }

  HttpServer server;
  const std::string err = server.listen_on(bind_host, cfg.port);
  if (!err.empty()) {
    std::cerr << "servir-catalogo: " << err << "\n";
    return 1;
  }
  std::cout << "servir-catalogo: escutando http://" << ctx.anuncio << ctx.prefix
            << " (root: " << ctx.root << ")\n"
            << std::flush;

  const int rc = server.run(
      [&ctx](const HttpRequest& req) -> HttpResponse {
        try {
          return despachar(ctx, req);
        } catch (const std::exception& e) {
          return resposta_erro(500, std::string("falha interna: ") + e.what(),
                               "RuntimeException");
        } catch (...) {
          return resposta_erro(500, "falha interna", "RuntimeException");
        }
      },
      /*max_requests=*/0, threads);
  if (rc < 0) {
    std::cerr << "servir-catalogo: " << server.last_error() << "\n";
    return 1;
  }
  return 0;
}

}  // namespace tilt::rt
