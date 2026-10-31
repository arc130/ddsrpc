#pragma once
#include <QChar>
#include <QString>
#include <QVector>

#include "diagnostic.h"
#include "token.h"

namespace ddsrpcgen {

// A Lexer is single-use: construct one instance per source text and call Scan()
// exactly once. Reusing an instance for a second Scan() is unsupported. Errors
// are appended to the caller-owned Diagnostics and are never cleared by the
// lexer, so one Diagnostics may collect errors from several files.
class Lexer {
 public:
  Lexer(QString file, QString source, Diagnostics& diagnostics);

  // Tokenizes the entire source and returns tokens terminated by TokenKind::End.
  // Call exactly once per Lexer instance.
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
