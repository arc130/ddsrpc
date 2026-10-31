#pragma once
#include <QString>

#include "diagnostic.h"

namespace ddsrpcgen {

enum class TokenKind {
  End,
  Identifier,
  Number,
  String,
  Preprocessor,
  LBrace,
  RBrace,
  LParen,
  RParen,
  Less,
  Greater,
  Comma,
  Semicolon,
  Equals,
  ColonColon,
  Ampersand,
  Star,
  Minus,
};

struct Token {
  TokenKind kind = TokenKind::End;
  QString text;
  SourceLocation loc;
};

}  // namespace ddsrpcgen
