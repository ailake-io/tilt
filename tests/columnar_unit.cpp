#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/columnar.hpp"

int main() {
  using tilt::rt::ColumnarColumn;
  using tilt::rt::ColumnarTable;
  using tilt::rt::Value;
  using tilt::rt::ValueKind;
  const auto check = [](bool ok) {
    if (!ok) throw std::runtime_error("columnar unit: resultado incorreto");
  };

  ColumnarColumn mixed;
  mixed.append(Value::inteiro(1));
  mixed.append(Value::nulo());
  mixed.append(Value::decimal(2.5));
  check(mixed.type == ColumnarColumn::Type::Mixed);
  check(mixed.at(0).kind == ValueKind::Inteiro && mixed.at(0).i == 1);
  check(mixed.at(1).kind == ValueKind::Nulo);
  check(mixed.at(2).kind == ValueKind::Decimal && mixed.number_at(2) == 2.5);

  ColumnarColumn typed;
  typed.append(Value::nulo());
  typed.append_integer(42);
  typed.append(Value::nulo());
  check(typed.type == ColumnarColumn::Type::Integer && typed.integers.size() == 3);
  check(typed.at(0).kind == ValueKind::Nulo && typed.at(1).i == 42 &&
        typed.at(2).kind == ValueKind::Nulo);
  typed.append_decimal(1.25);
  check(typed.type == ColumnarColumn::Type::Mixed && typed.at(1).i == 42 &&
        typed.at(3).d == 1.25);

  ColumnarColumn flags;
  flags.append_boolean(true);
  flags.append(Value::nulo());
  flags.append_boolean(false);
  check(flags.type == ColumnarColumn::Type::Boolean && flags.booleans.size() == 3);
  check(flags.at(0).b && flags.at(1).kind == ValueKind::Nulo && !flags.at(2).b);

  ColumnarColumn lists;
  lists.append(Value::nulo());
  Value first = Value::lista();
  first.list_ref()->push_back(Value::inteiro(7));
  first.list_ref()->push_back(Value::nulo());
  lists.append(first);
  lists.append(Value::lista());
  check(lists.type == ColumnarColumn::Type::List && lists.offsets.size() == 4);
  check(lists.at(0).kind == ValueKind::Nulo);
  check(lists.at(1).list_ref()->size() == 2 && (*lists.at(1).list_ref())[0].i == 7 &&
        (*lists.at(1).list_ref())[1].kind == ValueKind::Nulo);
  check(lists.at(2).list_ref()->empty());

  ColumnarColumn nested;
  Value outer = Value::lista();
  outer.list_ref()->push_back(first);
  outer.list_ref()->push_back(Value::lista());
  nested.append(outer);
  check(nested.type == ColumnarColumn::Type::List &&
        nested.elements->type == ColumnarColumn::Type::List);
  check((*nested.at(0).list_ref())[0].list_ref()->size() == 2);

  ColumnarColumn structs;
  structs.append(Value::nulo());
  Value record = Value::mapa();
  record.map_ref()->set("id", Value::inteiro(3));
  record.map_ref()->set("tags", first);
  structs.append(record);
  check(structs.type == ColumnarColumn::Type::Struct && structs.fields.size() == 2);
  check(structs.at(0).kind == ValueKind::Nulo);
  check(structs.at(1).map_ref()->find("id")->i == 3);
  check(structs.at(1).map_ref()->find("tags")->list_ref()->size() == 2);
  Value different = Value::mapa();
  different.map_ref()->set("id", Value::inteiro(4));
  structs.append(different);
  check(structs.type == ColumnarColumn::Type::Mixed &&
        structs.at(2).map_ref()->find("id")->i == 4);

  const std::vector<std::size_t> selected{2, 0};
  ColumnarColumn selected_structs = structs.take_rows(selected);
  check(selected_structs.type == ColumnarColumn::Type::Mixed &&
        selected_structs.at(0).map_ref()->find("id")->i == 4 &&
        selected_structs.at(1).kind == ValueKind::Nulo);
  ColumnarColumn selected_lists = lists.take_rows(std::vector<std::size_t>{1, 2, 0});
  check(selected_lists.type == ColumnarColumn::Type::List &&
        selected_lists.at(0).list_ref()->size() == 2 &&
        selected_lists.at(1).list_ref()->empty() &&
        selected_lists.at(2).kind == ValueKind::Nulo);

  ColumnarColumn repeated;
  for (int i = 0; i < 100; ++i) repeated.append(Value::texto("sul"));
  check(repeated.type == ColumnarColumn::Type::Text && repeated.dictionary.size() == 1);
  check(repeated.key_at(99) == "sul");

  ColumnarColumn unique;
  for (int i = 0; i < 65536; ++i)
    unique.append(Value::texto("id" + std::to_string(i)));
  check(unique.type == ColumnarColumn::Type::TextPlain);
  check(unique.at(0).s == "id0" && unique.at(65535).s == "id65535");

  auto table = std::make_shared<ColumnarTable>(std::vector<std::string>{"chave", "numero"});
  std::vector<Value> row{Value::texto("a"), Value::inteiro(4)};
  table->append(row);
  Value value = Value::tabela_colunar(table);
  check(!value.list_ref() && table->rows == 1);
  value.materialize_rows();
  check(value.list_ref() && value.list_ref()->size() == 1);
  check((*value.list_ref())[0].map_ref()->find("numero")->i == 4);
  std::cout << "columnar ok\n";
}
