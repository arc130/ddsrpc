#pragma once
#include <QString>
#include <QVector>

#include "diagnostic.h"
#include "token.h"

namespace ddsrpcgen {

class Lexer {
 public:
  Lexer(QString file, QString source, Diagnostics& diagnostics);

  [[nodiscard]] QVector<Token> Scan();

 private:
  QChar Peek(int lookahead = 0) const;
  bool AtEnd() const;
  void Advance();
  void SkipWhitespaceAndComments();
  Token MakeToken(TokenKind kind, int start_offset, const SourceLocation& start) const;
  void ScanPreprocessor();
  Token ScanString();
  Token ScanNumber();
  Token ScanIdentifier();

  QString file_;
  QString source_;
  Diagnostics& diagnostics_;
  int offset_ = 0;
  int line_ = 1;
  int column_ = 1;
  QVector<Token> tokens_;
};

}  // namespace ddsrpcgen
