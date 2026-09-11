#include "runtime/http_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

#include "runtime/compat.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string truncar(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

bool metodo_com_payload(const std::string& m) {
  return m == "PUT" || m == "POST" || m == "PATCH";
}

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

  std::string cmd = "curl -s ";
  if (falhar) cmd += "--fail-with-body ";
  if (timeout_s > 0) cmd += "--max-time " + std::to_string(timeout_s) + " ";
  cmd += "-o " + shell_quote(out_path) + " ";
  if (resp_headers) cmd += "-D " + shell_quote(hdr_path) + " ";
  cmd += "-w '%{http_code}' ";
  if (metodo == "HEAD") {
    cmd += "-I";
  } else {
    cmd += "-X " + metodo;
  }
  for (const auto& [nome, valor] : headers) {
    cmd += " -H " + shell_quote(nome + ": " + valor);
  }
  if (!body_file.empty()) cmd += " --data @" + body_file;
  cmd += " " + shell_quote(url);

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
