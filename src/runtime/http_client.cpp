#include "runtime/http_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

#if defined(TILT_HAS_CURL_HEADER)
#include <curl/curl.h>
#endif

#include "runtime/compat.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {



std::string truncar(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

bool metodo_com_payload(const std::string& m) {
  return m == "PUT" || m == "POST" || m == "PATCH";
}

bool header_seguro(const std::string& s) {
  return s.find_first_of("\r\n\0", 0, 3) == std::string::npos;
}

#if defined(TILT_HAS_CURL_HEADER)

struct CurlApi {
  void* lib = nullptr;
  decltype(&curl_global_init) global_init = nullptr;
  decltype(&curl_version_info) version_info = nullptr;
  decltype(&curl_easy_init) easy_init = nullptr;
  decltype(&curl_easy_cleanup) easy_cleanup = nullptr;
  decltype(&curl_easy_reset) easy_reset = nullptr;
  decltype(&curl_easy_setopt) easy_setopt = nullptr;
  decltype(&curl_easy_perform) easy_perform = nullptr;
  decltype(&curl_easy_getinfo) easy_getinfo = nullptr;
  decltype(&curl_easy_strerror) easy_strerror = nullptr;
  decltype(&curl_slist_append) slist_append = nullptr;
  decltype(&curl_slist_free_all) slist_free_all = nullptr;
  bool available = false;
};

const CurlApi& curl_api() {
  // Mantido ate o fim do processo: handles thread_local podem sobreviver a
  // destruicao de estaticos no encerramento.
  static const CurlApi* api = [] {
    auto* a = new CurlApi();
#if defined(_WIN32)
    for (const char* name : {"libcurl.dll", "libcurl-4.dll"}) {
#elif defined(__APPLE__)
    for (const char* name : {"libcurl.dylib", "libcurl.4.dylib"}) {
#else
    for (const char* name : {"libcurl.so.4", "libcurl.so"}) {
#endif
      a->lib = tilt_dlopen(name);
      if (a->lib) break;
    }
    if (!a->lib) return a;
    const auto bind = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::decay_t<decltype(fn)>>(tilt_dlsym(a->lib, name));
      return fn != nullptr;
    };
    if (!(bind(a->global_init, "curl_global_init") &&
          bind(a->version_info, "curl_version_info") &&
          bind(a->easy_init, "curl_easy_init") &&
          bind(a->easy_cleanup, "curl_easy_cleanup") &&
          bind(a->easy_reset, "curl_easy_reset") &&
          bind(a->easy_setopt, "curl_easy_setopt") &&
          bind(a->easy_perform, "curl_easy_perform") &&
          bind(a->easy_getinfo, "curl_easy_getinfo") &&
          bind(a->easy_strerror, "curl_easy_strerror") &&
          bind(a->slist_append, "curl_slist_append") &&
          bind(a->slist_free_all, "curl_slist_free_all"))) return a;
    const curl_version_info_data* v = a->version_info(CURLVERSION_NOW);
    // Inicializacao tardia deve ser segura quando o servico ja possui threads.
    if (!v || !(v->features & CURL_VERSION_THREADSAFE) ||
        a->global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return a;
    a->available = true;
    return a;
  }();
  return *api;
}

struct CurlThreadHandle {
  const CurlApi* api = nullptr;
  CURL* easy = nullptr;
  ~CurlThreadHandle() { if (easy) api->easy_cleanup(easy); }
};

size_t curl_write(char* data, size_t size, size_t count, void* target) {
  const std::size_t bytes = size * count;
  try { static_cast<std::string*>(target)->append(data, bytes); }
  catch (...) { return 0; }
  return bytes;
}

size_t curl_header(char* data, size_t size, size_t count, void* target) {
  const std::size_t bytes = size * count;
  auto* out = static_cast<std::vector<std::pair<std::string, std::string>>*>(target);
  try {
    std::string line(data, bytes);
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
    out->emplace_back(std::move(name), std::move(value));
  } catch (...) { return 0; }
  return bytes;
}

HttpClientResponse request_libcurl(
    const std::string& metodo, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body, int timeout_s, bool falhar,
    std::vector<std::pair<std::string, std::string>>* resp_headers) {
  HttpClientResponse result;
  const CurlApi& api = curl_api();
  thread_local CurlThreadHandle handle;
  if (!handle.easy) {
    handle.api = &api;
    handle.easy = api.easy_init();
  }
  if (!handle.easy) {
    result.error = "libcurl: nao foi possivel criar sessao";
    return result;
  }
  api.easy_reset(handle.easy);  // preserva conexoes e cache DNS da thread
  curl_slist* request_headers = nullptr;
  for (const auto& [name, value] : headers) {
    curl_slist* next = api.slist_append(request_headers, (name + ": " + value).c_str());
    if (!next) {
      if (request_headers) api.slist_free_all(request_headers);
      result.error = "libcurl: memoria insuficiente para headers";
      return result;
    }
    request_headers = next;
  }
  const auto set = [&](CURLoption option, auto value) {
    return api.easy_setopt(handle.easy, option, value) == CURLE_OK;
  };
  bool ok = set(CURLOPT_URL, url.c_str()) &&
            set(CURLOPT_NOSIGNAL, 1L) &&
            set(CURLOPT_WRITEFUNCTION, &curl_write) &&
            set(CURLOPT_WRITEDATA, &result.body);
  // curl CLI honors CURL_CA_BUNDLE; libcurl embedded does not necessarily read
  // that environment variable. Keep both backends' trust behavior aligned.
  const char* ca_bundle = std::getenv("CURL_CA_BUNDLE");
  if (!ca_bundle || !*ca_bundle) ca_bundle = std::getenv("SSL_CERT_FILE");
  if (ca_bundle && *ca_bundle) ok = ok && set(CURLOPT_CAINFO, ca_bundle);
  if (resp_headers) {
    ok = ok && set(CURLOPT_HEADERFUNCTION, &curl_header) &&
         set(CURLOPT_HEADERDATA, resp_headers);
  }
  if (request_headers) ok = ok && set(CURLOPT_HTTPHEADER, request_headers);
  if (timeout_s > 0) ok = ok && set(CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
  if (metodo == "HEAD") ok = ok && set(CURLOPT_NOBODY, 1L);
  if (metodo != "GET") ok = ok && set(CURLOPT_CUSTOMREQUEST, metodo.c_str());
  if (metodo_com_payload(metodo)) {
    ok = ok && set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())) &&
         set(CURLOPT_POSTFIELDS, body.c_str());
  }
  if (!ok) {
    result.error = "libcurl: configuracao da requisicao falhou";
  } else {
    const CURLcode code = api.easy_perform(handle.easy);
    long status = 0;
    (void)api.easy_getinfo(handle.easy, CURLINFO_RESPONSE_CODE, &status);
    result.status = static_cast<int>(status);
    if (code != CURLE_OK) {
      result.error = "requisicao falhou (libcurl codigo " +
                     std::to_string(static_cast<int>(code)) + ": " +
                     api.easy_strerror(code) + ")";
    } else if (falhar && status >= 400) {
      result.error = "requisicao falhou (HTTP " + std::to_string(status) + ")";
    }
  }
  if (request_headers) api.slist_free_all(request_headers);
  return result;
}

#endif  // TILT_HAS_CURL_HEADER

// Parse do arquivo `-D`: "Nome: valor" por linha; nomes em caixa baixa,
// espacos/tabs a esquerda do valor removidos; linhas de status e vazias
// ignoradas (mesma regra do antigo parser local do s3).
void parse_headers_arquivo(
    const std::string& path,
    std::vector<std::pair<std::string, std::string>>& out) {
  std::ifstream in(path);
  std::string linha;
  while (std::getline(in, linha)) {
    if (!linha.empty() && linha.back() == '\r') linha.pop_back();
    const std::size_t dois_pontos = linha.find(':');
    if (dois_pontos == std::string::npos) continue;  // status line / vazia
    std::string nome = linha.substr(0, dois_pontos);
    std::transform(nome.begin(), nome.end(), nome.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string valor = linha.substr(dois_pontos + 1);
    while (!valor.empty() && (valor.front() == ' ' || valor.front() == '\t')) {
      valor.erase(valor.begin());
    }
    out.emplace_back(std::move(nome), std::move(valor));
  }
}

void garantir_http_ok(const HttpClientResponse& r) {
  if (!r.error.empty()) throw std::runtime_error("http: " + r.error);
  if (r.status == 0) throw std::runtime_error("http: resposta sem codigo de status");
  if (r.status >= 400) {
    throw std::runtime_error("http: HTTP " + std::to_string(r.status) + ": " +
                             truncar(r.body, 200));
  }
}

Value json_do_corpo(const HttpClientResponse& r) {
  try {
    return json_parse(r.body);
  } catch (const std::exception& e) {
    throw std::runtime_error("http: corpo da resposta nao e JSON valido: " +
                             std::string(e.what()));
  }
}

}  // namespace

HttpClientResponse http_request(
    const std::string& metodo, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body, int timeout_s, bool falhar,
    std::vector<std::pair<std::string, std::string>>* resp_headers) {
  HttpClientResponse r;
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    r.error = "url deve comecar com 'http://' ou 'https://' (recebida '" +
              truncar(url, 60) + "')";
    return r;
  }
  if (!header_seguro(url) || !header_seguro(metodo)) {
    r.error = "URL ou metodo contem caractere invalido";
    return r;
  }
  for (const auto& [name, value] : headers) {
    if (!header_seguro(name) || !header_seguro(value)) {
      r.error = "header contem caractere invalido";
      return r;
    }
  }
#if defined(TILT_HAS_CURL_HEADER)
  const char* selected = std::getenv("TILT_HTTP_BACKEND");
  if ((!selected || std::string(selected) != "cli") && curl_api().available) {
    return request_libcurl(metodo, url, headers, body, timeout_s, falhar, resp_headers);
  }
  if (selected && std::string(selected) == "libcurl" && !curl_api().available) {
    r.error = "libcurl indisponivel; instale libcurl ou use TILT_HTTP_BACKEND=cli";
    return r;
  }
#endif

  std::string body_file;
  if (metodo_com_payload(metodo)) {
    std::string body_path;
    const int fd = tilt_tempfile("http_body", body_path);
    if (fd < 0) {
      r.error = "nao foi possivel criar arquivo temporario";
      return r;
    }
    tilt_close_file(fd);
    {
      std::ofstream out(body_path, std::ios::trunc);
      out << body;
    }
    body_file = body_path;
  }

  std::string out_path;
  const int fd_out = tilt_tempfile("http_resp", out_path);
  if (fd_out < 0) {
    if (!body_file.empty()) std::remove(body_file.c_str());
    r.error = "nao foi possivel criar arquivo temporario";
    return r;
  }
  tilt_close_file(fd_out);

  std::string hdr_path;
  if (resp_headers) {
    const int fd_hdr = tilt_tempfile("http_hdr", hdr_path);
    if (fd_hdr < 0) {
      if (!body_file.empty()) std::remove(body_file.c_str());
      std::remove(out_path.c_str());
      r.error = "nao foi possivel criar arquivo temporario";
      return r;
    }
    tilt_close_file(fd_hdr);
  }

  // URL (pode ter userinfo) e headers (chaves de API) vao num arquivo -K 0600:
  // no argv apareceriam em ps//proc para qualquer usuario da maquina.
  std::vector<std::string> linhas_header;
  linhas_header.reserve(headers.size());
  for (const auto& [nome, valor] : headers) linhas_header.push_back(nome + ": " + valor);
  std::string cfg_path;
  if (!tilt_curl_config(url, linhas_header, cfg_path)) {
    if (!body_file.empty()) std::remove(body_file.c_str());
    std::remove(out_path.c_str());
    if (!hdr_path.empty()) std::remove(hdr_path.c_str());
    r.error = "nao foi possivel montar a configuracao do curl (URL ou header invalido)";
    return r;
  }

  std::string cmd = "curl -s ";
  if (falhar) cmd += "--fail-with-body ";
  if (timeout_s > 0) cmd += "--max-time " + std::to_string(timeout_s) + " ";
  cmd += "-o " + tilt_shell_quote(out_path) + " ";
  if (resp_headers) cmd += "-D " + tilt_shell_quote(hdr_path) + " ";
  cmd += "-w " + tilt_shell_quote("%{http_code}") + " ";
  if (metodo == "HEAD") {
    cmd += "-I";
  } else {
    cmd += "-X " + metodo;
  }
  cmd += " -K " + tilt_shell_quote(cfg_path);
  if (!body_file.empty()) cmd += " --data @" + tilt_shell_quote(body_file);

  std::string resp;
  int rc = 0;
  bool popen_ok = true;
  {
    std::array<char, 4096> buf{};
    FILE* pipe = tilt_popen(cmd.c_str(), "r");
    if (!pipe) {
      popen_ok = false;
    } else {
      std::size_t n;
      while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) resp.append(buf.data(), n);
      rc = tilt_pclose(pipe);
    }
  }
  if (!body_file.empty()) std::remove(body_file.c_str());
  std::remove(cfg_path.c_str());

  {
    std::ifstream in(out_path, std::ios::binary);
    r.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::remove(out_path.c_str());
  if (resp_headers) {
    parse_headers_arquivo(hdr_path, *resp_headers);
    std::remove(hdr_path.c_str());
  }

  if (!popen_ok) {
    r.error = "nao foi possivel executar 'curl'";
    return r;
  }
  {
    std::istringstream iss(resp);
    iss >> r.status;
  }
  if (rc != 0) {
    r.error = "requisicao falhou (curl codigo " + std::to_string(rc) + ")";
  }
  return r;
}

Value http_get_json(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers, int timeout_s) {
  const HttpClientResponse r = http_request("GET", url, headers, "", timeout_s);
  garantir_http_ok(r);
  return json_do_corpo(r);
}

Value http_post_json(
    const std::string& url, const Value& corpo,
    const std::vector<std::pair<std::string, std::string>>& headers, int timeout_s) {
  std::vector<std::pair<std::string, std::string>> hs = headers;
  bool tem_content_type = false;
  for (const auto& [nome, valor] : hs) {
    std::string caixa = nome;
    std::transform(caixa.begin(), caixa.end(), caixa.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (caixa == "content-type") tem_content_type = true;
  }
  if (!tem_content_type) hs.insert(hs.begin(), {"Content-Type", "application/json"});
  const HttpClientResponse r = http_request("POST", url, hs, json_dump(corpo), timeout_s);
  garantir_http_ok(r);
  return json_do_corpo(r);
}

}  // namespace tilt::rt
