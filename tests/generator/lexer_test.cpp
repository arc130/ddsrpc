#include <gtest/gtest.h>

#include <QString>

#include "diagnostic.h"
#include "lexer.h"

using ddsrpcgen::Diagnostics;
using ddsrpcgen::Lexer;
using ddsrpcgen::TokenKind;

TEST(LexerTest, TokenizesDirectivesImportsAndClassBody) {
  const QString source = QStringLiteral(
      "#include <QString>\n"
      "IMPORT \"common.idl\";\n"
      "class Foo {\n"
      "  PROP(int count = 1)\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("test.idl"), source, diagnostics);
  const auto tokens = lexer.Scan();

  ASSERT_FALSE(diagnostics.HasErrors()) << diagnostics.Format().toStdString();
  ASSERT_GE(tokens.size(), 6);
  EXPECT_EQ(tokens[0].kind, TokenKind::Preprocessor);
  EXPECT_EQ(tokens[0].text, QStringLiteral("#include <QString>"));
  EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[1].text, QStringLiteral("IMPORT"));
  EXPECT_EQ(tokens[2].kind, TokenKind::String);
  EXPECT_EQ(tokens[2].text, QStringLiteral("common.idl"));
  EXPECT_EQ(tokens[3].kind, TokenKind::Semicolon);
  EXPECT_EQ(tokens[4].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[4].text, QStringLiteral("class"));
}

TEST(LexerTest, TracksLineAndColumnAndSkipsComments) {
  const QString source = QStringLiteral(
      "// a comment\n"
      "class /* block */ Foo {\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("test.idl"), source, diagnostics);
  const auto tokens = lexer.Scan();

  ASSERT_FALSE(diagnostics.HasErrors());
  ASSERT_GE(tokens.size(), 3);
  EXPECT_EQ(tokens[0].loc.line, 2);
  EXPECT_EQ(tokens[0].loc.column, 1);
  EXPECT_EQ(tokens[0].text, QStringLiteral("class"));
  EXPECT_EQ(tokens[1].loc.line, 2);
  EXPECT_EQ(tokens[1].text, QStringLiteral("Foo"));
}

TEST(LexerTest, TokenizesNestedTemplatesAndReferences) {
  const QString source = QStringLiteral(
      "SLOT(const QMap<QString, QList<Point>> &values)\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("test.idl"), source, diagnostics);
  const auto tokens = lexer.Scan();

  ASSERT_FALSE(diagnostics.HasErrors()) << diagnostics.Format().toStdString();
  int less_count = 0;
  int greater_count = 0;
  bool saw_ampersand = false;
  for (const auto& token : tokens) {
    less_count += token.kind == TokenKind::Less ? 1 : 0;
    greater_count += token.kind == TokenKind::Greater ? 1 : 0;
    saw_ampersand = saw_ampersand || token.kind == TokenKind::Ampersand;
  }
  EXPECT_EQ(less_count, 2);
  EXPECT_EQ(greater_count, 2);
  EXPECT_TRUE(saw_ampersand);
}

TEST(LexerTest, ReportsUnterminatedString) {
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("bad.idl"), QStringLiteral("IMPORT \"x.idl;\n"), diagnostics);
  (void)lexer.Scan();
  EXPECT_TRUE(diagnostics.HasErrors());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("bad.idl")), -1);
}
