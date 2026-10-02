#include "runtime/fuso.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "runtime/tabela_ops.hpp"

namespace tilt::rt {

namespace {

struct Civil {
  int a = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
};

// Algoritmo de Howard Hinnant (dominio publico): dias desde 1970-01-01.
std::int64_t dias_desde_epoca(int a, int m, int d) {
  a -= m <= 2 ? 1 : 0;
  const std::int64_t era = (a >= 0 ? a : a - 399) / 400;
  const auto yoe = static_cast<unsigned>(a - era * 400);
  const unsigned doy =
      (153 * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

Civil civil_de_epoca_utc(std::int64_t epoca) {
  std::int64_t dias = epoca / 86400;
  std::int64_t resto = epoca % 86400;
  if (resto < 0) {
    resto += 86400;
    --dias;
  }
  dias += 719468;
  const std::int64_t era = (dias >= 0 ? dias : dias - 146096) / 146097;
  const auto doe = static_cast<unsigned>(dias - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  Civil c;
  c.d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  c.m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  c.a = static_cast<int>(y + (c.m <= 2 ? 1 : 0));
  c.hh = static_cast<int>(resto / 3600);
  c.mm = static_cast<int>(resto % 3600 / 60);
  c.ss = static_cast<int>(resto % 60);
  return c;
}

std::int64_t epoca_utc(const Civil& c) {
  return dias_desde_epoca(c.a, c.m, c.d) * 86400 + c.hh * 3600 + c.mm * 60 + c.ss;
}

// Deslocamento fixo em segundos: "UTC", "Z", "+03:00", "-0300", "+5".
std::optional<int> deslocamento_fixo(const std::string& z) {
  if (z == "UTC" || z == "utc" || z == "Z" || z == "GMT") return 0;
  if (z.empty() || (z[0] != '+' && z[0] != '-')) return std::nullopt;
  std::string digitos;
  for (std::size_t i = 1; i < z.size(); ++i) {
    if (z[i] == ':') continue;
    if (std::isdigit(static_cast<unsigned char>(z[i])) == 0) return std::nullopt;
    digitos += z[i];
  }
  if (digitos.empty() || digitos.size() > 4) return std::nullopt;
  int h = 0;
  int m = 0;
  if (digitos.size() <= 2) {
    h = std::atoi(digitos.c_str());
  } else {
    h = std::atoi(digitos.substr(0, digitos.size() - 2).c_str());
    m = std::atoi(digitos.substr(digitos.size() - 2).c_str());
  }
  if (h > 14 || m > 59) return std::nullopt;
  const int seg = h * 3600 + m * 60;
  return z[0] == '-' ? -seg : seg;
}

#if !defined(_WIN32)
// O tzdata do sistema so e alcancado pela variavel TZ (global do processo): as trocas
// ficam num mutex e o valor anterior e restaurado.
std::mutex& mutex_tz() {
  static std::mutex* const m = new std::mutex();
  return *m;
}

class FusoTemporario {
 public:
  explicit FusoTemporario(const std::string& nome) {
    if (const char* antigo = std::getenv("TZ")) antigo_ = antigo;
    tinha_ = std::getenv("TZ") != nullptr;
    ::setenv("TZ", nome.c_str(), 1);
    ::tzset();
  }
  ~FusoTemporario() {
    if (tinha_) {
      ::setenv("TZ", antigo_.c_str(), 1);
    } else {
      ::unsetenv("TZ");
    }
    ::tzset();
  }
  FusoTemporario(const FusoTemporario&) = delete;
  FusoTemporario& operator=(const FusoTemporario&) = delete;

 private:
  std::string antigo_;
  bool tinha_ = false;
};

void valida_nome_iana(const std::string& nome) {
  bool ok =
      !nome.empty() && nome.size() < 64 && nome.find("..") == std::string::npos && nome[0] != '/';
  for (const char ch : nome) {
    const auto c = static_cast<unsigned char>(ch);
    ok = ok && (std::isalnum(c) != 0 || ch == '/' || ch == '_' || ch == '-' || ch == '+');
  }
  if (ok) {
    const char* dir = std::getenv("TZDIR");
    std::ifstream f(std::string(dir && *dir ? dir : "/usr/share/zoneinfo") + "/" + nome);
    ok = static_cast<bool>(f);
  }
  if (!ok) {
    throw std::runtime_error("fuso horario desconhecido '" + nome +
                             "' (use um nome IANA como America/Sao_Paulo, UTC ou um "
                             "deslocamento como -03:00; instale o tzdata se faltar)");
  }
}
#endif

// Hora local `c` no fuso `zona` -> epoca UTC.
std::int64_t para_epoca(const Civil& c, const std::string& zona) {
  if (const auto off = deslocamento_fixo(zona)) return epoca_utc(c) - *off;
#if defined(_WIN32)
  throw std::runtime_error("fuso horario '" + zona +
                           "': no Windows so UTC e deslocamentos fixos (-03:00)");
#else
  valida_nome_iana(zona);
  const std::lock_guard<std::mutex> lk(mutex_tz());
  const FusoTemporario tz(zona);
  std::tm t{};
  t.tm_year = c.a - 1900;
  t.tm_mon = c.m - 1;
  t.tm_mday = c.d;
  t.tm_hour = c.hh;
  t.tm_min = c.mm;
  t.tm_sec = c.ss;
  t.tm_isdst = -1;
  return static_cast<std::int64_t>(::mktime(&t));
#endif
}

Civil de_epoca(std::int64_t epoca, const std::string& zona) {
  if (const auto off = deslocamento_fixo(zona)) return civil_de_epoca_utc(epoca + *off);
#if defined(_WIN32)
  throw std::runtime_error("fuso horario '" + zona +
                           "': no Windows so UTC e deslocamentos fixos (-03:00)");
#else
  valida_nome_iana(zona);
  const std::lock_guard<std::mutex> lk(mutex_tz());
  const FusoTemporario tz(zona);
  const auto t = static_cast<std::time_t>(epoca);
  std::tm out{};
  ::localtime_r(&t, &out);
  Civil c;
  c.a = out.tm_year + 1900;
  c.m = out.tm_mon + 1;
  c.d = out.tm_mday;
  c.hh = out.tm_hour;
  c.mm = out.tm_min;
  c.ss = out.tm_sec;
  return c;
#endif
}

}  // namespace

std::string converter_fuso(const std::string& texto, const std::string& origem,
                           const std::string& destino) {
  // Sufixo de fuso no proprio texto ("...Z", "...-03:00") vence `origem`.
  std::string base = texto;
  std::string zona = origem;
  while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back())) != 0)
    base.pop_back();
  if (!base.empty() && (base.back() == 'Z' || base.back() == 'z')) {
    base.pop_back();
    zona = "UTC";
  } else if (base.size() > 10) {
    for (const std::size_t n : {std::size_t{6}, std::size_t{5}}) {  // +HH:MM, +HHMM
      const bool tem_hora =
          base.find('T') != std::string::npos || base.find(' ') != std::string::npos;
      if (base.size() > n && (base[base.size() - n] == '+' || base[base.size() - n] == '-') &&
          tem_hora) {
        const std::string suf = base.substr(base.size() - n);
        if (deslocamento_fixo(suf)) {
          zona = suf;
          base.erase(base.size() - n);
          break;
        }
      }
    }
  }
  const std::string iso = normalizar_data(base);
  if (iso.empty()) return "";
  Civil c;
  if (std::sscanf(iso.c_str(), "%d-%d-%d", &c.a, &c.m, &c.d) != 3) return "";
  if (iso.size() > 10) {
    std::sscanf(iso.c_str() + 11, "%d:%d:%d", &c.hh, &c.mm, &c.ss);
  }
  const std::int64_t epoca = para_epoca(c, zona);
  const Civil r = de_epoca(epoca, destino);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", r.a, r.m, r.d, r.hh, r.mm, r.ss);
  return buf;
}

}  // namespace tilt::rt
