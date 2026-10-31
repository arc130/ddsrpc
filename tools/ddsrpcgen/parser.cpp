#include "parser.h"

#include <cstdint>
#include <limits>
#include <utility>

namespace ddsrpcgen {
namespace {

[[nodiscard]] QString DescribeToken(const Token& token) {
  if (token.kind == TokenKind::End) return QStringLiteral("end of input");
  if (token.text.isEmpty()) return QStringLiteral("<token>");
  return QStringLiteral("'%1'").arg(token.text);
}

}  // namespace

Parser::Parser(QString file, QVector<Token> tokens, QString source, Diagnostics& diagnostics)
    : file_(std::move(file)),
      tokens_(std::move(tokens)),
      source_(std::move(source)),
      diagnostics_(diagnostics) {
  if (tokens_.isEmpty() || tokens_.last().kind != TokenKind::End) {
    Token end;
    end.kind = TokenKind::End;
    if (!tokens_.isEmpty()) {
      end.loc = tokens_.last().loc;
      end.loc.offset += tokens_.last().text.size();
      end.loc.column += tokens_.last().text.size();
    }
    tokens_.push_back(end);
  }
}

const Token& Parser::Peek(int lookahead) const {
  const int index = pos_ + lookahead;
  if (index <= 0) return tokens_.constFirst();
  if (index >= tokens_.size()) return tokens_.constLast();
  return tokens_.at(index);
}

bool Parser::AtEnd() const { return Peek().kind == TokenKind::End; }

Token Parser::Consume() {
  const Token token = Peek();
  if (!AtEnd()) ++pos_;
  return token;
}

bool Parser::Check(TokenKind kind) const { return Peek().kind == kind; }

bool Parser::CheckIdentifier(const QString& text) const {
  return Peek().kind == TokenKind::Identifier && Peek().text == text;
}

bool Parser::Match(TokenKind kind) {
  if (!Check(kind)) return false;
  Consume();
  return true;
}

bool Parser::MatchIdentifier(const QString& text) {
  if (!CheckIdentifier(text)) return false;
  Consume();
  return true;
}

void Parser::Expect(TokenKind kind, const QString& what) {
  if (Match(kind)) return;
  diagnostics_.Error(file_, Peek().loc,
                     QStringLiteral("expected %1, found %2").arg(what, DescribeToken(Peek())));
}

bool Parser::ExpectIdentifierText(const QString& text, const QString& what) {
  if (MatchIdentifier(text)) return true;
  diagnostics_.Error(file_, Peek().loc,
                     QStringLiteral("expected %1, found %2").arg(what, DescribeToken(Peek())));
  return false;
}

void Parser::SkipSemicolonsAndNewlines() {
  while (Match(TokenKind::Semicolon)) {
  }
}

std::optional<AstFile> Parser::Parse() {
  AstFile ast;
  ast.path = file_;

  while (!AtEnd()) {
    if (Match(TokenKind::Semicolon)) continue;

    if (Check(TokenKind::Preprocessor)) {
      const Token token = Consume();
      ast.directives.push_back(Directive{token.text, token.loc});
      continue;
    }

    if (!Check(TokenKind::Identifier)) {
      diagnostics_.Error(
          file_, Peek().loc,
          QStringLiteral("expected a top-level declaration, found %1").arg(DescribeToken(Peek())));
      return std::nullopt;
    }

    const QString keyword = Peek().text;
    if (keyword == QStringLiteral("IMPORT")) {
      const Token keyword_token = Consume();
      if (!Check(TokenKind::String)) {
        diagnostics_.Error(file_, Peek().loc,
                           QStringLiteral("expected a string literal after IMPORT, found %1")
                               .arg(DescribeToken(Peek())));
        return std::nullopt;
      }
      ImportDecl import;
      import.loc = keyword_token.loc;
      import.path = Consume().text;
      const int before = pos_;
      Expect(TokenKind::Semicolon, QStringLiteral("';' after IMPORT"));
      if (pos_ == before) return std::nullopt;
      ast.imports.push_back(std::move(import));
      continue;
    }

    if (keyword == QStringLiteral("POD")) {
      auto pod = ParsePod();
      if (!pod) return std::nullopt;
      ast.pods.push_back(std::move(*pod));
      continue;
    }

    if (keyword == QStringLiteral("ENUM")) {
      auto decl = ParseEnum();
      if (!decl) return std::nullopt;
      ast.enums.push_back(std::move(*decl));
      continue;
    }

    if (keyword == QStringLiteral("class")) {
      // Task 4 replaces this branch with ParseClass().
      diagnostics_.Error(file_, Peek().loc, QStringLiteral("class parsing not yet available"));
      return std::nullopt;
    }

    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("unsupported top-level construct '%1'").arg(keyword));
    return std::nullopt;
  }

  return ast;
}

std::optional<PodDecl> Parser::ParsePod() {
  const Token keyword = Consume();
  PodDecl pod;
  pod.loc = keyword.loc;

  if (!Check(TokenKind::Identifier)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected POD name, found %1").arg(DescribeToken(Peek())));
    return std::nullopt;
  }
  pod.name = Consume().text;

  if (!Check(TokenKind::LParen)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected '(' after POD name '%1', found %2")
                           .arg(pod.name, DescribeToken(Peek())));
    return std::nullopt;
  }
  Consume();

  if (Check(TokenKind::RParen)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("POD '%1' must declare at least one field").arg(pod.name));
    return std::nullopt;
  }

  for (;;) {
    const SourceLocation field_loc = Peek().loc;
    auto type = ParseType();
    if (!type) return std::nullopt;

    if (!Check(TokenKind::Identifier)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected field name in POD '%1', found %2")
                             .arg(pod.name, DescribeToken(Peek())));
      return std::nullopt;
    }

    FieldDecl field;
    field.type = std::move(*type);
    field.name = Consume().text;
    field.loc = field_loc;
    pod.fields.push_back(std::move(field));

    if (!Match(TokenKind::Comma)) break;
    if (Check(TokenKind::RParen)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected field after ',' in POD '%1'").arg(pod.name));
      return std::nullopt;
    }
  }

  const int before = pos_;
  Expect(TokenKind::RParen, QStringLiteral("')' after POD fields"));
  if (pos_ == before) return std::nullopt;

  SkipSemicolonsAndNewlines();
  return pod;
}

std::optional<EnumDecl> Parser::ParseEnum() {
  const Token keyword = Consume();
  EnumDecl decl;
  decl.loc = keyword.loc;

  if (!Check(TokenKind::Identifier)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected ENUM name, found %1").arg(DescribeToken(Peek())));
    return std::nullopt;
  }
  decl.name = Consume().text;

  if (!Check(TokenKind::LBrace)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected '{' after ENUM name '%1', found %2")
                           .arg(decl.name, DescribeToken(Peek())));
    return std::nullopt;
  }
  Consume();

  if (Check(TokenKind::RBrace)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("ENUM '%1' must declare at least one value").arg(decl.name));
    return std::nullopt;
  }

  constexpr std::int64_t kMinValue = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t kMaxValue = std::numeric_limits<std::int32_t>::max();
  std::int64_t next_value = 0;

  for (;;) {
    if (!Check(TokenKind::Identifier)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected value name in ENUM '%1', found %2")
                             .arg(decl.name, DescribeToken(Peek())));
      return std::nullopt;
    }

    SourceLocation numeric_loc = Peek().loc;
    EnumValue value;
    value.name = Consume().text;

    std::int64_t numeric = next_value;
    if (Match(TokenKind::Equals)) {
      const bool negative = Match(TokenKind::Minus);
      if (!Check(TokenKind::Number)) {
        diagnostics_.Error(file_, Peek().loc,
                           QStringLiteral("expected an integer value after '=' in ENUM '%1', found %2")
                               .arg(decl.name, DescribeToken(Peek())));
        return std::nullopt;
      }
      const Token number = Consume();
      numeric_loc = number.loc;
      bool ok = false;
      const qint64 parsed = number.text.toLongLong(&ok, 0);
      if (!ok) {
        diagnostics_.Error(file_, number.loc,
                           QStringLiteral("invalid integer value '%1' in ENUM '%2'")
                               .arg(number.text, decl.name));
        return std::nullopt;
      }
      numeric = negative ? -static_cast<std::int64_t>(parsed)
                         : static_cast<std::int64_t>(parsed);
    }

    if (numeric < kMinValue || numeric > kMaxValue) {
      diagnostics_.Error(file_, numeric_loc,
                         QStringLiteral("value %1 for ENUM '%2' is out of range")
                             .arg(numeric)
                             .arg(decl.name));
      return std::nullopt;
    }

    value.value = static_cast<std::int32_t>(numeric);
    decl.values.push_back(value);
    next_value = numeric + 1;

    if (!Match(TokenKind::Comma)) break;
    if (Check(TokenKind::RBrace)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected value name after ',' in ENUM '%1'").arg(decl.name));
      return std::nullopt;
    }
  }

  const int before = pos_;
  Expect(TokenKind::RBrace, QStringLiteral("'}' after ENUM values"));
  if (pos_ == before) return std::nullopt;

  SkipSemicolonsAndNewlines();
  return decl;
}

std::optional<TypeRef> Parser::ParseType() {
  const int start_pos = pos_;
  TypeRef ref;
  ref.is_const = MatchIdentifier(QStringLiteral("const"));

  if (!Check(TokenKind::Identifier)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected a type name, found %1").arg(DescribeToken(Peek())));
    return std::nullopt;
  }

  QString base = Consume().text;
  while (Match(TokenKind::ColonColon)) {
    if (!Check(TokenKind::Identifier)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected an identifier after '::', found %1")
                             .arg(DescribeToken(Peek())));
      return std::nullopt;
    }
    base += QStringLiteral("::");
    base += Consume().text;
  }
  ref.base = base;

  if (Match(TokenKind::Less)) {
    if (Check(TokenKind::Greater)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected a template argument, found %1")
                             .arg(DescribeToken(Peek())));
      return std::nullopt;
    }
    for (;;) {
      auto argument = ParseType();
      if (!argument) return std::nullopt;
      ref.args.push_back(std::move(*argument));
      if (!Match(TokenKind::Comma)) break;
      if (Check(TokenKind::Greater)) {
        diagnostics_.Error(file_, Peek().loc,
                           QStringLiteral("expected a template argument after ',', found %1")
                               .arg(DescribeToken(Peek())));
        return std::nullopt;
      }
    }
    const int before = pos_;
    Expect(TokenKind::Greater, QStringLiteral("'>' to close template arguments"));
    if (pos_ == before) return std::nullopt;
  }

  if (Match(TokenKind::Ampersand)) ref.is_reference = true;

  if (Check(TokenKind::Star)) {
    diagnostics_.Error(file_, Peek().loc, QStringLiteral("pointer types are not supported"));
    return std::nullopt;
  }

  const Token& first = tokens_.at(start_pos);
  const Token& last = tokens_.at(pos_ - 1);
  const int end_offset = last.loc.offset + last.text.size();
  ref.spelling = source_.mid(first.loc.offset, end_offset - first.loc.offset).trimmed();
  return ref;
}

}  // namespace ddsrpcgen
