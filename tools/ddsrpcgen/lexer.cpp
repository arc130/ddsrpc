#include "lexer.h"

#include <QChar>

#include <utility>

namespace ddsrpcgen {
namespace {
[[nodiscard]] bool IsIdentifierStart(QChar c) {
  return c.isLetter() || c == QLatin1Char('_');
}
[[nodiscard]] bool IsIdentifierPart(QChar c) {
  return c.isLetterOrNumber() || c == QLatin1Char('_');
}
}  // namespace

Lexer::Lexer(QString file, QString source, Diagnostics& diagnostics)
    : file_(std::move(file)), source_(std::move(source)), diagnostics_(diagnostics) {}

QChar Lexer::Peek(int lookahead) const {
  const int pos = offset_ + lookahead;
  return pos >= 0 && pos < source_.size() ? source_.at(pos) : QChar();
}
bool Lexer::AtEnd() const { return offset_ >= source_.size(); }

void Lexer::Advance() {
  if (AtEnd()) return;
  if (source_.at(offset_) == QLatin1Char('\n')) {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  ++offset_;
}

void Lexer::SkipWhitespaceAndComments() {
  for (;;) {
    while (!AtEnd() && Peek().isSpace()) Advance();
    if (Peek() == QLatin1Char('/') && Peek(1) == QLatin1Char('/')) {
      while (!AtEnd() && Peek() != QLatin1Char('\n')) Advance();
      continue;
    }
    if (Peek() == QLatin1Char('/') && Peek(1) == QLatin1Char('*')) {
      const SourceLocation start{line_, column_, offset_};
      Advance();
      Advance();
      while (!AtEnd() && !(Peek() == QLatin1Char('*') && Peek(1) == QLatin1Char('/'))) {
        Advance();
      }
      if (AtEnd()) {
        diagnostics_.Error(file_, start, QStringLiteral("unterminated block comment"));
        return;
      }
      Advance();
      Advance();
      continue;
    }
    break;
  }
}

Token Lexer::MakeToken(TokenKind kind, int start_offset, const SourceLocation& start) const {
  Token token;
  token.kind = kind;
  token.loc = start;
  token.text = source_.mid(start_offset, offset_ - start_offset);
  return token;
}

void Lexer::ScanPreprocessor() {
  const SourceLocation start{line_, column_, offset_};
  const int start_offset = offset_;
  // Stop before either newline character so CRLF input does not leak '\r'
  // into the token text; SkipWhitespaceAndComments() consumes the line ending.
  while (!AtEnd() && Peek() != QLatin1Char('\n') && Peek() != QLatin1Char('\r')) {
    Advance();
  }
  tokens_.push_back(MakeToken(TokenKind::Preprocessor, start_offset, start));
}

Token Lexer::ScanString() {
  const SourceLocation start{line_, column_, offset_};
  const int start_offset = offset_;
  Advance();  // opening quote
  QString value;
  while (!AtEnd() && Peek() != QLatin1Char('"') && Peek() != QLatin1Char('\n') &&
         Peek() != QLatin1Char('\r')) {
    if (Peek() == QLatin1Char('\\') && Peek(1) == QLatin1Char('"')) {
      value += QLatin1Char('"');
      Advance();
      Advance();
    } else if (Peek() == QLatin1Char('\\') && Peek(1) == QLatin1Char('\\')) {
      value += QLatin1Char('\\');
      Advance();
      Advance();
    } else {
      value += Peek();
      Advance();
    }
  }
  // Raw line endings inside a quoted string are errors. Leave the newline
  // unconsumed so the outer scan loop can treat it as ordinary whitespace and
  // keep lexing the rest of the file.
  if (AtEnd() || Peek() == QLatin1Char('\n') || Peek() == QLatin1Char('\r')) {
    diagnostics_.Error(file_, start, QStringLiteral("unterminated string literal"));
    Token token;
    token.kind = TokenKind::String;
    token.loc = start;
    token.text = value;
    return token;
  }
  Advance();  // closing quote
  Token token = MakeToken(TokenKind::String, start_offset, start);
  token.text = value;
  return token;
}

Token Lexer::ScanNumber() {
  const SourceLocation start{line_, column_, offset_};
  const int start_offset = offset_;
  if (Peek() == QLatin1Char('0') && (Peek(1) == QLatin1Char('x') || Peek(1) == QLatin1Char('X'))) {
    Advance();
    Advance();
    while (!AtEnd() && (Peek().isDigit() ||
                        (Peek().toLower() >= QLatin1Char('a') &&
                         Peek().toLower() <= QLatin1Char('f')))) {
      Advance();
    }
  } else {
    while (!AtEnd() && Peek().isDigit()) Advance();
  }
  return MakeToken(TokenKind::Number, start_offset, start);
}

Token Lexer::ScanIdentifier() {
  const SourceLocation start{line_, column_, offset_};
  const int start_offset = offset_;
  while (!AtEnd() && IsIdentifierPart(Peek())) Advance();
  return MakeToken(TokenKind::Identifier, start_offset, start);
}

QVector<Token> Lexer::Scan() {
  while (true) {
    SkipWhitespaceAndComments();
    if (AtEnd()) break;
    const QChar c = Peek();
    const SourceLocation start{line_, column_, offset_};
    const int start_offset = offset_;

    if (c == QLatin1Char('#')) {
      ScanPreprocessor();
    } else if (c == QLatin1Char('"')) {
      tokens_.push_back(ScanString());
    } else if (c.isDigit()) {
      tokens_.push_back(ScanNumber());
    } else if (IsIdentifierStart(c)) {
      tokens_.push_back(ScanIdentifier());
    } else if (c == QLatin1Char('{')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::LBrace, start_offset, start));
    } else if (c == QLatin1Char('}')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::RBrace, start_offset, start));
    } else if (c == QLatin1Char('(')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::LParen, start_offset, start));
    } else if (c == QLatin1Char(')')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::RParen, start_offset, start));
    } else if (c == QLatin1Char('<')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Less, start_offset, start));
    } else if (c == QLatin1Char('>')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Greater, start_offset, start));
    } else if (c == QLatin1Char(',')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Comma, start_offset, start));
    } else if (c == QLatin1Char(';')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Semicolon, start_offset, start));
    } else if (c == QLatin1Char('=')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Equals, start_offset, start));
    } else if (c == QLatin1Char('&')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Ampersand, start_offset, start));
    } else if (c == QLatin1Char('*')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Star, start_offset, start));
    } else if (c == QLatin1Char('-')) {
      Advance();
      tokens_.push_back(MakeToken(TokenKind::Minus, start_offset, start));
    } else if (c == QLatin1Char(':') && Peek(1) == QLatin1Char(':')) {
      Advance();
      Advance();
      tokens_.push_back(MakeToken(TokenKind::ColonColon, start_offset, start));
    } else {
      diagnostics_.Error(file_, start, QStringLiteral("unexpected character '%1'").arg(c));
      Advance();
    }
  }

  Token end;
  end.kind = TokenKind::End;
  end.loc = SourceLocation{line_, column_, offset_};
  tokens_.push_back(end);
  return tokens_;
}

}  // namespace ddsrpcgen
