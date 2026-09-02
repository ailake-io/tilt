#pragma once

#include <string_view>

namespace tilt {

// Stable diagnostic codes. Rendered as `T###` (see CLAUDE.md section 18).
// The numeric value IS the code number; keep values stable across releases.
enum class DiagCode {
  // Lexer: 1-9
  IndentNotMultipleOfTwo = 1,
  TabInIndent = 2,
  UnterminatedString = 3,
  InvalidCharacter = 4,

  // Parser: 10-19
  UnexpectedIndent = 10,
  ExpectedToken = 13,
  UnexpectedToken = 14,

  // Semantic: 11-99
  TypeMismatch = 11,
  TensorShapeMismatch = 12,
  SecretMustUseEnv = 20,
  InvalidDevice = 21,
  UndefinedName = 30,
  UnknownReference = 31,
  DuplicateDeclaration = 32,
  UnknownType = 33,
  InvalidTensorType = 34,

  // Runtime: 900+
  ConnectorNotImplemented = 900,
  DataQualityViolation = 910,
};

// "T012" — valid until the next call on the same thread.
std::string_view diag_code_string(DiagCode code);

// Short human-readable title, in Portuguese.
std::string_view diag_code_title(DiagCode code);

}  // namespace tilt
