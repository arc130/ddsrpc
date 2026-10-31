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

[[nodiscard]] bool IsTypeQualifier(const QString& text) {
  return text == QStringLiteral("unsigned") || text == QStringLiteral("signed") ||
         text == QStringLiteral("long") || text == QStringLiteral("short");
}

// Built-in words that may follow a qualifier; used only for lookahead so
// "unsigned int value" is parsed as type "unsigned int" + name "value", while
// "unsigned value" keeps "unsigned" as the type.
[[nodiscard]] bool IsBuiltinTypeWord(const QString& text) {
  return text == QStringLiteral("int") || text == QStringLiteral("char") ||
         text == QStringLiteral("float") || text == QStringLiteral("double") ||
         text == QStringLiteral("bool") || text == QStringLiteral("void") ||
         text == QStringLiteral("wchar_t") || text == QStringLiteral("char16_t") ||
         text == QStringLiteral("char32_t");
}

[[nodiscard]] bool IsPropVisibilityModifier(const QString& text) {
  return text == QStringLiteral("CONSTANT") || text == QStringLiteral("READONLY") ||
         text == QStringLiteral("READPUSH") || text == QStringLiteral("READWRITE") ||
         text == QStringLiteral("SOURCEONLYSETTER");
}

[[nodiscard]] bool IsPropModifier(const QString& text) {
  return IsPropVisibilityModifier(text) || text == QStringLiteral("PERSISTED");
}

[[nodiscard]] PropModifier PropModifierFor(const QString& text) {
  if (text == QStringLiteral("CONSTANT")) return PropModifier::kConstant;
  if (text == QStringLiteral("READONLY")) return PropModifier::kReadOnly;
  if (text == QStringLiteral("READWRITE")) return PropModifier::kReadWrite;
  if (text == QStringLiteral("SOURCEONLYSETTER")) return PropModifier::kSourceOnlySetter;
  return PropModifier::kReadPush;
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

bool Parser::Expect(TokenKind kind, const QString& what) {
  if (Match(kind)) return true;
  diagnostics_.Error(file_, Peek().loc,
                     QStringLiteral("expected %1, found %2").arg(what, DescribeToken(Peek())));
  return false;
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
      auto cls = ParseClass();
      if (!cls) return std::nullopt;
      ast.classes.push_back(std::move(*cls));
      continue;
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

  // Consume multi-word builtin specifiers such as "unsigned long long" and
  // "long double". A qualifier is only consumed when another qualifier or a
  // builtin type word follows; otherwise it is itself the base type so that
  // "unsigned value" still parses as type "unsigned" + name "value".
  while (Check(TokenKind::Identifier) && IsTypeQualifier(Peek().text)) {
    const Token& next = Peek(1);
    const bool continues = next.kind == TokenKind::Identifier &&
                           (IsTypeQualifier(next.text) || IsBuiltinTypeWord(next.text));
    if (!continues) break;
    Consume();
  }

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

  if (Match(TokenKind::Ampersand)) {
    ref.is_reference = true;
    // "&&" is folded into a single reference; generated code takes values.
    Match(TokenKind::Ampersand);
  }

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

std::optional<TypeRef> Parser::ParseTypeFromSlice(int begin, int end) {
  const int saved = pos_;
  pos_ = begin;
  auto type = ParseType();
  if (!type) {
    pos_ = saved;
    return std::nullopt;
  }
  if (pos_ != end) {
    diagnostics_.Error(
        file_, Peek().loc,
        QStringLiteral("unexpected %1 in type").arg(DescribeToken(Peek())));
    pos_ = saved;
    return std::nullopt;
  }
  pos_ = saved;
  return type;
}

std::optional<QVector<ParamDecl>> Parser::ParseParams() {
  if (!Expect(TokenKind::LParen, QStringLiteral("'(' to start parameter list"))) {
    return std::nullopt;
  }

  QVector<ParamDecl> params;
  if (Match(TokenKind::RParen)) return params;

  for (;;) {
    ParamDecl param;
    param.loc = Peek().loc;
    auto type = ParseType();
    if (!type) return std::nullopt;
    param.type = std::move(*type);

    if (Check(TokenKind::Identifier)) {
      param.name = Consume().text;
      param.named = true;
    }
    params.push_back(std::move(param));

    if (!Match(TokenKind::Comma)) break;
    if (Check(TokenKind::RParen)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected a parameter after ','"));
      return std::nullopt;
    }
  }

  if (!Expect(TokenKind::RParen, QStringLiteral("')' after parameters"))) {
    return std::nullopt;
  }
  return params;
}

std::optional<SignalDecl> Parser::ParseSignal() {
  const Token keyword = Consume();
  SignalDecl signal;
  signal.loc = keyword.loc;

  if (!Expect(TokenKind::LParen, QStringLiteral("'(' after SIGNAL"))) {
    return std::nullopt;
  }

  if (!Check(TokenKind::Identifier)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected SIGNAL name, found %1").arg(DescribeToken(Peek())));
    return std::nullopt;
  }
  signal.name = Consume().text;

  auto params = ParseParams();
  if (!params) return std::nullopt;
  signal.params = std::move(*params);

  if (!Expect(TokenKind::RParen, QStringLiteral("')' after SIGNAL declaration"))) {
    return std::nullopt;
  }
  return signal;
}

std::optional<SlotDecl> Parser::ParseSlot() {
  const Token keyword = Consume();
  SlotDecl slot;
  slot.loc = keyword.loc;

  if (!Expect(TokenKind::LParen, QStringLiteral("'(' after SLOT"))) {
    return std::nullopt;
  }

  // Collect the tokens between "SLOT(" and the parameter list's "(". The last
  // identifier is the slot name; everything before it is the return type.
  const int return_begin = pos_;
  int name_pos = -1;
  while (!AtEnd() && !Check(TokenKind::LParen)) {
    if (Check(TokenKind::Identifier)) name_pos = pos_;
    Consume();
  }
  if (!Check(TokenKind::LParen)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected '(' to start SLOT parameters, found %1")
                           .arg(DescribeToken(Peek())));
    return std::nullopt;
  }
  if (name_pos < 0) {
    diagnostics_.Error(file_, Peek().loc, QStringLiteral("expected a SLOT name"));
    return std::nullopt;
  }
  slot.name = tokens_.at(name_pos).text;

  if (name_pos > return_begin) {
    auto return_type = ParseTypeFromSlice(return_begin, name_pos);
    if (!return_type) return std::nullopt;
    if (return_type->is_reference) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("SLOT '%1' cannot return a reference").arg(slot.name));
      return std::nullopt;
    }
    slot.return_type = std::move(*return_type);
  } else {
    slot.return_type.spelling = QStringLiteral("void");
    slot.return_type.base = QStringLiteral("void");
  }

  auto params = ParseParams();
  if (!params) return std::nullopt;
  slot.params = std::move(*params);

  if (!Expect(TokenKind::RParen, QStringLiteral("')' after SLOT declaration"))) {
    return std::nullopt;
  }
  return slot;
}

std::optional<PropDecl> Parser::ParseProp() {
  const Token keyword = Consume();
  PropDecl prop;
  prop.loc = keyword.loc;

  if (!Expect(TokenKind::LParen, QStringLiteral("'(' after PROP"))) {
    return std::nullopt;
  }

  auto type = ParseType();
  if (!type) return std::nullopt;
  prop.type = std::move(*type);

  if (!Check(TokenKind::Identifier)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected PROP name, found %1").arg(DescribeToken(Peek())));
    return std::nullopt;
  }
  prop.name = Consume().text;

  // Collect the remaining tokens up to the PROP's closing ')' while balancing
  // nested parentheses in the default expression.
  QVector<Token> tail;
  Token close;
  bool closed = false;
  int depth = 1;
  while (!AtEnd()) {
    const Token token = Consume();
    if (token.kind == TokenKind::LParen) {
      ++depth;
    } else if (token.kind == TokenKind::RParen) {
      if (depth == 1) {
        close = token;
        closed = true;
        break;
      }
      --depth;
    }
    tail.push_back(token);
  }
  if (!closed) {
    diagnostics_.Error(file_, keyword.loc, QStringLiteral("unterminated PROP declaration"));
    return std::nullopt;
  }

  // Strip trailing modifiers. Visibility modifiers are recognized only at the
  // end; anything unrecognized before the closing ')' is a syntax error.
  int content_end = tail.size();
  int visibility_count = 0;
  SourceLocation modifier_loc = close.loc;
  SourceLocation persisted_loc = close.loc;
  while (content_end > 0 && tail.at(content_end - 1).kind == TokenKind::Identifier &&
         IsPropModifier(tail.at(content_end - 1).text)) {
    const Token& modifier = tail.at(content_end - 1);
    if (modifier.text == QStringLiteral("PERSISTED")) {
      prop.persisted = true;
      persisted_loc = modifier.loc;
    } else {
      ++visibility_count;
      modifier_loc = modifier.loc;
      prop.modifier = PropModifierFor(modifier.text);
    }
    --content_end;
  }

  if (visibility_count > 1) {
    diagnostics_.Error(
        file_, modifier_loc,
        QStringLiteral("PROP '%1' has multiple visibility modifiers").arg(prop.name));
    return std::nullopt;
  }
  if (prop.persisted) {
    diagnostics_.Error(file_, persisted_loc,
                       QStringLiteral("PERSISTED properties are not supported"));
    return std::nullopt;
  }

  if (content_end > 0) {
    if (tail.first().kind != TokenKind::Equals) {
      diagnostics_.Error(file_, tail.first().loc,
                         QStringLiteral("expected '=' or a PROP modifier, found %1")
                             .arg(DescribeToken(tail.first())));
      return std::nullopt;
    }
    if (content_end == 1) {
      diagnostics_.Error(file_, tail.first().loc,
                         QStringLiteral("expected a default expression after '='"));
      return std::nullopt;
    }

    // Slice the default expression from the original source. String token
    // text is decoded (quotes removed, escapes folded), so offsets, not text
    // lengths, must delimit the expression.
    const int begin_offset = tail.at(1).loc.offset;
    const int end_offset = content_end < tail.size() ? tail.at(content_end).loc.offset
                                                     : close.loc.offset;
    prop.default_value =
        source_.mid(begin_offset, end_offset - begin_offset).trimmed();
    prop.has_default = true;
  }

  return prop;
}

std::optional<ClassDecl> Parser::ParseClass() {
  const Token keyword = Consume();
  ClassDecl decl;
  decl.loc = keyword.loc;

  if (!Check(TokenKind::Identifier)) {
    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("expected class name, found %1").arg(DescribeToken(Peek())));
    return std::nullopt;
  }
  decl.name = Consume().text;

  if (!Expect(TokenKind::LBrace, QStringLiteral("'{' after class name"))) {
    return std::nullopt;
  }

  while (!Check(TokenKind::RBrace)) {
    if (AtEnd()) {
      diagnostics_.Error(file_, keyword.loc,
                         QStringLiteral("unterminated class '%1'").arg(decl.name));
      return std::nullopt;
    }
    if (Match(TokenKind::Semicolon)) continue;

    if (!Check(TokenKind::Identifier)) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("expected a class member, found %1")
                             .arg(DescribeToken(Peek())));
      return std::nullopt;
    }

    const QString member = Peek().text;
    if (member == QStringLiteral("ENUM")) {
      auto enum_decl = ParseEnum();
      if (!enum_decl) return std::nullopt;
      decl.enums.push_back(std::move(*enum_decl));
      continue;
    }
    if (member == QStringLiteral("PROP")) {
      auto prop = ParseProp();
      if (!prop) return std::nullopt;
      decl.properties.push_back(std::move(*prop));
      continue;
    }
    if (member == QStringLiteral("SIGNAL")) {
      auto signal = ParseSignal();
      if (!signal) return std::nullopt;
      decl.signals.push_back(std::move(*signal));
      continue;
    }
    if (member == QStringLiteral("SLOT")) {
      auto slot = ParseSlot();
      if (!slot) return std::nullopt;
      decl.slots.push_back(std::move(*slot));
      continue;
    }
    if (member == QStringLiteral("MODEL") || member == QStringLiteral("CLASS") ||
        member == QStringLiteral("FLAG") || member == QStringLiteral("USE_ENUM")) {
      diagnostics_.Error(file_, Peek().loc,
                         QStringLiteral("%1 is not supported inside a class").arg(member));
      return std::nullopt;
    }

    diagnostics_.Error(file_, Peek().loc,
                       QStringLiteral("unsupported class member '%1'").arg(member));
    return std::nullopt;
  }

  Consume();  // '}'
  SkipSemicolonsAndNewlines();
  return decl;
}

}  // namespace ddsrpcgen
