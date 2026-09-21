#include "runtime/sql_tabelas.hpp"

#include <stdexcept>

#include "runtime/duckdb.hpp"
#include "runtime/sqlite.hpp"

namespace tilt::rt {

Value sql_tabelas(const std::string& motor, const std::string& sql,
                  const std::vector<std::pair<std::string, Value>>& tabelas,
                  const std::vector<SqlParam>& params) {
  if (motor == "sqlite") return sqlite_consulta_tabelas(sql, tabelas, params);
  if (motor == "duckdb") return duckdb_consulta_tabelas(sql, tabelas, params);
  if (motor == "auto") {
    // SQL que le arquivos (`from 'x.parquet'`, read_csv...) so o DuckDB entende.
    for (const char* marca :
         {".parquet'", ".csv'", ".json'", "read_csv", "read_parquet", "read_json"}) {
      if (sql.find(marca) != std::string::npos)
        return duckdb_consulta_tabelas(sql, tabelas, params);
    }
    return sqlite_consulta_tabelas(sql, tabelas, params);
  }
  throw std::runtime_error("sql: motor desconhecido '" + motor +
                           "' (use \"sqlite\", \"duckdb\" ou \"auto\")");
}

}  // namespace tilt::rt
