#include "diagnostics/codes.hpp"

#include <cstdio>

namespace tilt {

std::string_view diag_code_string(DiagCode code) {
  static thread_local char buf[8];
  std::snprintf(buf, sizeof(buf), "T%03d", static_cast<int>(code));
  return buf;
}

std::string_view diag_code_title(DiagCode code) {
  switch (code) {
    case DiagCode::IndentNotMultipleOfTwo:
      return "indentacao nao e multiplo de 2";
    case DiagCode::TabInIndent:
      return "tab na indentacao";
    case DiagCode::UnterminatedString:
      return "texto nao terminado";
    case DiagCode::InvalidCharacter:
      return "caractere invalido";
    case DiagCode::UnexpectedIndent:
      return "indentacao inesperada";
    case DiagCode::ExpectedToken:
      return "token esperado";
    case DiagCode::UnexpectedToken:
      return "token inesperado";
    case DiagCode::TypeMismatch:
      return "tipo incompativel";
    case DiagCode::TensorShapeMismatch:
      return "forma de tensor incompativel";
    case DiagCode::SecretMustUseEnv:
      return "segredo deve usar env";
    case DiagCode::InvalidDevice:
      return "dispositivo invalido";
    case DiagCode::UndefinedName:
      return "nome nao definido";
    case DiagCode::UnknownReference:
      return "referencia desconhecida";
    case DiagCode::DuplicateDeclaration:
      return "declaracao duplicada";
    case DiagCode::UnknownType:
      return "tipo desconhecido";
    case DiagCode::InvalidTensorType:
      return "tipo de tensor invalido";
    case DiagCode::ConnectorNotImplemented:
      return "conector nao implementado";
    case DiagCode::RuntimeError:
      return "erro de execucao";
    case DiagCode::NotImplemented:
      return "recurso nao implementado";
    case DiagCode::DataQualityViolation:
      return "violacao de qualidade de dados";
  }
  return "desconhecido";
}

}  // namespace tilt
