#pragma once
#include <optional>

#include "ast.h"
#include "diagnostic.h"
#include "token.h"

namespace ddsrpcgen {

class Parser {
 public:
  // tokens must be the output of one Lexer::Scan() call for source (including
  // the terminating TokenKind::End). source is retained to recover exact type
  // spellings from token offsets.
  Parser(QString file, QVector<Token> tokens, QString source, Diagnostics& diagnostics);

  [[nodiscard]] std::optional<AstFile> Parse();

 private:
  const Token& Peek(int lookahead = 0) const;
  bool AtEnd() const;
  Token Consume();
  bool Check(TokenKind kind) const;
  bool CheckIdentifier(const QString& text) const;
  bool Match(TokenKind kind);
  bool MatchIdentifier(const QString& text);
  bool Expect(TokenKind kind, const QString& what);
  bool ExpectIdentifierText(const QString& text, const QString& what);
  void SkipSemicolonsAndNewlines();

  std::optional<PodDecl> ParsePod();
  std::optional<EnumDecl> ParseEnum();
  std::optional<TypeRef> ParseType();
  std::optional<ClassDecl> ParseClass();
  std::optional<PropDecl> ParseProp();
  std::optional<SignalDecl> ParseSignal();
  std::optional<SlotDecl> ParseSlot();
  std::optional<QVector<ParamDecl>> ParseParams();
  std::optional<TypeRef> ParseTypeFromSlice(int begin, int end);

  QString file_;
  QVector<Token> tokens_;
  QString source_;
  Diagnostics& diagnostics_;
  int pos_ = 0;
};

}  // namespace ddsrpcgen
