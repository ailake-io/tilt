#include "runtime/iceberg_catalog_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <system_error>
#include <thread>

#include "runtime/compat.hpp"
#include "runtime/http_server.hpp"
#include "runtime/json.hpp"
#include "runtime/table_lock.hpp"

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

std::string catalog_uuid() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  char buf[37];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(gen() & 0xffffffffu),
                static_cast<unsigned>(gen() & 0xffffu),
                static_cast<unsigned>(0x4000u | (gen() & 0xfffu)),
                static_cast<unsigned>(0x8000u | (gen() & 0x3fffu)),
                static_cast<unsigned long long>(gen() & 0xffffffffffffull));
  return buf;
}

std::int64_t catalog_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// Contexto do handler: imutavel apos o init (thread-safe com workers).
// ---------------------------------------------------------------------------

struct CatalogoCtx {
  std::string root;         // absoluto canonicalizado (weakly_canonical)
  std::string prefix;       // "/v1" (sem barra final)
  std::string anuncio;      // host:porta fallback p/ URLs quando sem Host header
  bool reescrever_manifests = true;
  std::string auth_token;  // opcional: ICEBERG_CATALOG_TOKEN
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
    if (!v.list_ref()) return;
    for (Value& e : *v.list_ref()) reescrever_locations(ctx, host_hdr, e);
    return;
  }
  if (v.kind != ValueKind::Mapa || !v.map_ref()) return;
  for (auto& kv : v.map_ref()->items) {
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

HttpResponse resposta_405(const std::string& op, const std::string& allow = {}) {
  HttpResponse resp = resposta_erro(
      405, "metodo nao permitido para " + op, "MethodNotAllowedException");
  if (!allow.empty()) resp.headers.emplace_back("Allow", allow);
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
  out.map_ref()->set("metadata-location", Value::texto(url_arquivo(ctx, req.host, meta_path)));
  out.map_ref()->set("metadata", std::move(md));
  out.map_ref()->set("config", Value::mapa());
  HttpResponse resp;
  if (req.method == "HEAD") {
    resp.body.clear();
    return resp;
  }
  resp.body = json_dump(out);
  return resp;
}

std::optional<std::int64_t> metadata_snapshot(const std::string& path) {
  bool ok = false;
  const std::string raw = read_file_bytes(path, ok);
  if (!ok) return std::nullopt;
  try {
    const Value md = json_parse(raw);
    const Value* current = md.map_ref() ? md.map_ref()->find("current-snapshot-id") : nullptr;
    if (!current || current->kind != ValueKind::Inteiro) return std::nullopt;
    return current->i;
  } catch (...) {
    return std::nullopt;
  }
}

HttpResponse rota_create_table(const CatalogoCtx& ctx, const HttpRequest& req) {
  Value body;
  try {
    body = json_parse(req.body);
  } catch (const std::exception& e) {
    return resposta_erro(400, "createTable: JSON invalido: " + std::string(e.what()),
                         "IllegalArgumentException");
  }
  const Value* name = body.map_ref() ? body.map_ref()->find("name") : nullptr;
  const Value* location = body.map_ref() ? body.map_ref()->find("location") : nullptr;
  if (!name || name->kind != ValueKind::Texto || name->s.empty() ||
      name->s.find('/') != std::string::npos || name->s == "." || name->s == "..") {
    return resposta_erro(400, "createTable: name invalido", "IllegalArgumentException");
  }
  if (!location || location->kind != ValueKind::Texto || location->s.empty()) {
    return resposta_erro(400, "createTable: location ausente", "IllegalArgumentException");
  }
  std::string loc = location->s;
  if (loc.rfind("file://", 0) == 0) loc = loc.substr(7);
  if (loc.front() != '/') loc = ctx.root + "/" + loc;
  std::string canon;
  if (!dentro_do_root(ctx, loc, canon)) {
    return resposta_erro(403, "location fora do diretorio-raiz do catalogo", "ForbiddenException");
  }
  const std::string expected = ctx.root + "/" + name->s;
  std::string expected_canon;
  if (!dentro_do_root(ctx, expected, expected_canon) || canon != expected_canon) {
    return resposta_erro(400, "createTable: location deve apontar para a tabela no root",
                         "IllegalArgumentException");
  }
  TableLock table_lock(canon + "/.tilt.rest", "Iceberg REST");
  if (latest_metadata_path(canon).empty()) {
    const Value* schema = body.map_ref()->find("schema");
    if (!schema || schema->kind != ValueKind::Mapa || !schema->map_ref()) {
      return resposta_erro(400, "createTable: schema ausente", "IllegalArgumentException");
    }
    std::error_code ec;
    std::filesystem::create_directories(canon + "/metadata", ec);
    if (ec) return resposta_erro(500, "createTable: falha ao criar metadata: " + ec.message(),
                                  "RuntimeException");
    Value schema_copy = *schema;
    if (!schema_copy.map_ref()->find("type"))
      schema_copy.map_ref()->set("type", Value::texto("struct"));
    schema_copy.map_ref()->set("schema-id", Value::inteiro(0));
    std::int64_t last_column_id = 0;
    if (const Value* fields = schema_copy.map_ref()->find("fields");
        fields && fields->kind == ValueKind::Lista && fields->list_ref()) {
      for (const Value& field : *fields->list_ref()) {
        if (field.map_ref()) {
          if (const Value* id = field.map_ref()->find("id");
              id && id->kind == ValueKind::Inteiro) last_column_id = std::max(last_column_id, id->i);
        }
      }
    }
    Value spec = Value::mapa();
    if (const Value* requested = body.map_ref()->find("partition-spec");
        requested && requested->kind == ValueKind::Mapa) spec = *requested;
    spec.map_ref()->set("spec-id", Value::inteiro(0));
    Value metadata = Value::mapa();
    metadata.map_ref()->set("format-version", Value::inteiro(2));
    metadata.map_ref()->set("table-uuid", Value::texto(catalog_uuid()));
    metadata.map_ref()->set("location", Value::texto(location->s));
    metadata.map_ref()->set("last-sequence-number", Value::inteiro(0));
    metadata.map_ref()->set("last-updated-ms", Value::inteiro(catalog_now_ms()));
    metadata.map_ref()->set("last-column-id", Value::inteiro(last_column_id));
    metadata.map_ref()->set("last-partition-id", Value::inteiro(999));
    Value schemas = Value::lista();
    schemas.list_ref()->push_back(std::move(schema_copy));
    metadata.map_ref()->set("schemas", std::move(schemas));
    metadata.map_ref()->set("current-schema-id", Value::inteiro(0));
    metadata.map_ref()->set("partition-spec", Value::lista());
    Value specs = Value::lista();
    specs.list_ref()->push_back(std::move(spec));
    metadata.map_ref()->set("partition-specs", std::move(specs));
    metadata.map_ref()->set("default-spec-id", Value::inteiro(0));
    const Value* properties = body.map_ref()->find("properties");
    metadata.map_ref()->set("properties", properties && properties->kind == ValueKind::Mapa
                                             ? *properties : Value::mapa());
    metadata.map_ref()->set("current-snapshot-id", Value::inteiro(-1));
    metadata.map_ref()->set("snapshots", Value::lista());
    metadata.map_ref()->set("snapshot-log", Value::lista());
    metadata.map_ref()->set("metadata-log", Value::lista());
    Value orders = Value::lista();
    Value order = Value::mapa();
    order.map_ref()->set("order-id", Value::inteiro(0));
    order.map_ref()->set("fields", Value::lista());
    orders.list_ref()->push_back(std::move(order));
    metadata.map_ref()->set("sort-orders", std::move(orders));
    metadata.map_ref()->set("default-sort-order-id", Value::inteiro(0));
    metadata.map_ref()->set("refs", Value::mapa());
    const std::string final_path = canon + "/metadata/v0.metadata.json";
    const std::string temp_path = final_path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) return resposta_erro(500, "createTable: falha ao abrir metadata", "RuntimeException");
    out << json_dump(metadata);
    out.close();
    if (!out || std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
      std::remove(temp_path.c_str());
      return resposta_erro(500, "createTable: falha ao materializar metadata", "RuntimeException");
    }
  }
  HttpRequest load_req;
  load_req.method = "GET";
  load_req.host = req.host;
  return rota_load_table(ctx, load_req, name->s);
}

HttpResponse rota_commit_table(const CatalogoCtx& ctx, const HttpRequest& req,
                               const std::string& tabela) {
  Value body;
  try {
    body = json_parse(req.body);
  } catch (const std::exception& e) {
    return resposta_erro(400, "transactions: JSON invalido: " + std::string(e.what()),
                         "IllegalArgumentException");
  }
  const Value* requirements = body.map_ref() ? body.map_ref()->find("requirements") : nullptr;
  if (!requirements || requirements->kind != ValueKind::Lista || !requirements->list_ref()) {
    return resposta_erro(400, "transactions: requirements ausente", "IllegalArgumentException");
  }
  std::optional<std::int64_t> expected;
  for (const Value& requirement : *requirements->list_ref()) {
    const Value* type = requirement.map_ref() ? requirement.map_ref()->find("type") : nullptr;
    if (!type || type->kind != ValueKind::Texto) continue;
    if (type->s == "assert-current-snapshot-id") {
      const Value* id = requirement.map_ref()->find("snapshot-id");
      if (!id || id->kind != ValueKind::Inteiro) {
        return resposta_erro(400, "transactions: snapshot-id invalido", "IllegalArgumentException");
      }
      expected = id->i;
    }
  }
  const std::string table_dir = ctx.root + "/" + tabela;
  TableLock table_lock(table_dir + "/.tilt.rest", "Iceberg REST");
  const std::string latest = latest_metadata_path(table_dir);
  if (latest.empty()) return resposta_erro(404, "tabela nao encontrada: " + tabela,
                                           "NoSuchTableException");
  const std::int64_t latest_version = metadata_version_from_name(
      latest.substr(latest.find_last_of('/') + 1));
  std::optional<std::int64_t> previous;
  if (latest_version > 0) {
    const std::string prev = table_dir + "/metadata/v" + std::to_string(latest_version - 1) +
                             ".metadata.json";
    previous = metadata_snapshot(prev);
  }
  if (expected.has_value()) {
    const std::int64_t actual = previous.value_or(-1);
    if (actual != *expected) {
      return resposta_erro(409, "transactions: conflito de snapshot (esperado " +
                               std::to_string(*expected) + ", atual " + std::to_string(actual) + ")",
                           "CommitFailedException");
    }
  }
  HttpRequest load_req;
  load_req.method = "GET";
  load_req.host = req.host;
  return rota_load_table(ctx, load_req, tabela);
}

HttpResponse rota_drop_table(const CatalogoCtx& ctx, const std::string& tabela) {
  if (tabela.empty() || tabela.find('/') != std::string::npos || tabela == "." || tabela == "..") {
    return resposta_erro(404, "tabela nao encontrada: " + tabela, "NoSuchTableException");
  }
  const std::string dir = ctx.root + "/" + tabela;
  std::string canon;
  if (!dentro_do_root(ctx, dir, canon)) {
    return resposta_erro(403, "tabela fora do diretorio-raiz", "ForbiddenException");
  }
  if (!tilt_is_directory(canon)) return resposta_erro(404, "tabela nao encontrada: " + tabela,
                                                       "NoSuchTableException");
  TableLock table_lock(canon + "/.tilt.rest", "Iceberg REST");
  std::error_code ec;
  std::filesystem::remove_all(canon, ec);
  if (ec) return resposta_erro(500, "falha ao remover tabela: " + ec.message(), "RuntimeException");
  HttpResponse response;
  response.status = 204;
  response.body.clear();
  return response;
}

// Query string -> mapa (k=v separados por '&'; '+' = espaco, form-encoding).
std::string query_param(const std::string& query, const std::string& chave);

HttpResponse rota_list_tables(const CatalogoCtx& ctx, const std::string& query) {
  const std::vector<std::string> tabelas = iceberg_catalog_tables(ctx.root);
  std::size_t inicio = 0;
  std::size_t tamanho = tabelas.size();
  const std::string token = query_param(query, "page_token");
  const std::string page_size = query_param(query, "page_size");
  if (!token.empty()) {
    try {
      inicio = static_cast<std::size_t>(std::stoull(token));
    } catch (...) {
      return resposta_erro(400, "page_token invalido", "IllegalArgumentException");
    }
  }
  if (inicio > tabelas.size()) {
    return resposta_erro(400, "page_token fora do intervalo", "IllegalArgumentException");
  }
  if (!page_size.empty()) {
    try {
      const std::size_t parsed = static_cast<std::size_t>(std::stoull(page_size));
      if (parsed == 0 || parsed > 1000) {
        return resposta_erro(400, "page_size deve estar entre 1 e 1000", "IllegalArgumentException");
      }
      tamanho = parsed;
    } catch (...) {
      return resposta_erro(400, "page_size invalido", "IllegalArgumentException");
    }
  }
  const std::size_t fim = std::min(tabelas.size(), inicio + tamanho);
  Value ids = Value::lista();
  for (std::size_t i = inicio; i < fim; ++i) {
    const std::string& t = tabelas[i];
    Value id = Value::mapa();
    Value ns = Value::lista();
    ns.list_ref()->push_back(Value::texto("default"));
    id.map_ref()->set("namespace", std::move(ns));
    id.map_ref()->set("name", Value::texto(t));
    ids.list_ref()->push_back(std::move(id));
  }
  Value out = Value::mapa();
  out.map_ref()->set("identifiers", std::move(ids));
  if (fim < tabelas.size()) out.map_ref()->set("next-page-token", Value::texto(std::to_string(fim)));
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

  // Quando configurado, o token protege todas as rotas do catalogo, inclusive
  // /config e /namespaces. Isso evita que um endpoint de descoberta vire uma
  // forma de contornar a autenticacao exigida para as operacoes de tabela.
  if (!ctx.auth_token.empty()) {
    const std::string expected = "Bearer " + ctx.auth_token;
    if (req.authorization != expected) {
      return resposta_erro(401, "Authorization Bearer ausente ou invalido", "UnauthorizedException");
    }
  }

  if (rest == "/config") {
    if (req.method != "GET") return resposta_405("config", "GET");
    HttpResponse response;
    response.status = 200;
    response.content_type = "application/json";
    response.body = "{\"defaults\":{},\"overrides\":{}}";
    return response;
  }
  if (rest == "/namespaces") {
    if (req.method == "GET") {
      HttpResponse response;
      response.status = 200;
      response.content_type = "application/json";
      response.body = "{\"namespaces\":[[\"default\"]]}";
      return response;
    }
    if (req.method == "POST") {
      Value body;
      try { body = json_parse(req.body); }
      catch (...) { return resposta_erro(400, "namespace: JSON invalido", "IllegalArgumentException"); }
      const Value* ns = body.map_ref() ? body.map_ref()->find("namespace") : nullptr;
      if (!ns || ns->kind != ValueKind::Lista || !ns->list_ref() || ns->list_ref()->size() != 1 ||
          ns->list_ref()->front().kind != ValueKind::Texto || ns->list_ref()->front().s != "default")
        return resposta_erro(400, "somente o namespace default e suportado", "IllegalArgumentException");
      HttpResponse response;
      response.status = 200;
      response.body = "{\"namespace\":[\"default\"],\"properties\":{}}";
      return response;
    }
    return resposta_405("namespace", "GET, POST");
  }
  if (rest == "/namespaces/default") {
    if (req.method == "DELETE") {
      if (!iceberg_catalog_tables(ctx.root).empty())
        return resposta_erro(409, "namespace default nao esta vazio", "NamespaceNotEmptyException");
      HttpResponse response;
      response.status = 204;
      response.body.clear();
      return response;
    }
    if (req.method != "GET") return resposta_405("namespace default", "GET, DELETE");
    HttpResponse response;
    response.status = 200;
    response.content_type = "application/json";
    response.body = "{\"namespace\":[\"default\"],\"properties\":{}}";
    return response;
  }
  if (rest == "/namespaces/default/tables") {
    if (req.method == "GET") return rota_list_tables(ctx, query);
    if (req.method == "POST") return rota_create_table(ctx, req);
    return resposta_405("tables", "GET, POST");
  }
  const std::string kTablesPrefix = "/namespaces/default/tables/";
  if (rest.rfind(kTablesPrefix, 0) == 0) {
    const std::string sub = rest.substr(kTablesPrefix.size());
    const std::size_t txs = sub.find("/transactions");
    if (txs != std::string::npos && txs + std::string("/transactions").size() == sub.size()) {
      if (req.method == "POST") return rota_commit_table(ctx, req, pct_decode(sub.substr(0, txs)));
      return resposta_405("transactions", "POST");
    }
    if (sub.find('/') != std::string::npos) {
      return resposta_erro(404, "rota nao encontrada: " + full_path, "NotFoundException");
    }
    if (req.method == "GET" || req.method == "HEAD") return rota_load_table(ctx, req, pct_decode(sub));
    if (req.method == "DELETE") return rota_drop_table(ctx, pct_decode(sub));
    return resposta_405("tabela", "GET, HEAD, DELETE");
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
    return resposta_405("files", "GET, HEAD");
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
  if (const char* token = std::getenv("ICEBERG_CATALOG_TOKEN")) ctx.auth_token = token;
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
