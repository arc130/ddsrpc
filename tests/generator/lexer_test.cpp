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

TEST(LexerTest, ReportsUnterminatedStringAtItsStart) {
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("bad.idl"), QStringLiteral("IMPORT \"x.idl;\n"), diagnostics);
  (void)lexer.Scan();

  ASSERT_TRUE(diagnostics.HasErrors());
  ASSERT_EQ(diagnostics.All().size(), 1) << diagnostics.Format().toStdString();
  EXPECT_EQ(diagnostics.All()[0].file, QStringLiteral("bad.idl"));
  EXPECT_EQ(diagnostics.All()[0].loc.line, 1);
  EXPECT_EQ(diagnostics.All()[0].loc.column, 8);
  EXPECT_NE(diagnostics.All()[0].message.indexOf(QStringLiteral("unterminated string")), -1);
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("bad.idl")), -1);
}

TEST(LexerTest, HandlesCrlfPreprocessorLines) {
  const QString source = QStringLiteral(
      "#include <QString>\r\n"
      "class Foo {\r\n"
      "};\r\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("crlf.idl"), source, diagnostics);
  const auto tokens = lexer.Scan();

  ASSERT_FALSE(diagnostics.HasErrors()) << diagnostics.Format().toStdString();
  ASSERT_GE(tokens.size(), 3);
  EXPECT_EQ(tokens[0].kind, TokenKind::Preprocessor);
  EXPECT_EQ(tokens[0].text, QStringLiteral("#include <QString>"));
  EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[1].text, QStringLiteral("class"));
  EXPECT_EQ(tokens[1].loc.line, 2);
  EXPECT_EQ(tokens[1].loc.column, 1);
}

TEST(LexerTest, ReportsStringCrossingNewlineAndRecovers) {
  const QString source = QStringLiteral(
      "IMPORT \"bad\n"
      "class Foo;\n"
      "IMPORT \"good.idl\";\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("bad.idl"), source, diagnostics);
  const auto tokens = lexer.Scan();

  ASSERT_TRUE(diagnostics.HasErrors());
  ASSERT_EQ(diagnostics.All().size(), 1) << diagnostics.Format().toStdString();
  EXPECT_EQ(diagnostics.All()[0].loc.line, 1);
  EXPECT_EQ(diagnostics.All()[0].loc.column, 8);
  EXPECT_NE(diagnostics.All()[0].message.indexOf(QStringLiteral("unterminated string")), -1);

  ASSERT_GE(tokens.size(), 7);
  EXPECT_EQ(tokens[1].kind, TokenKind::String);
  EXPECT_EQ(tokens[1].text, QStringLiteral("bad"));
  EXPECT_EQ(tokens[2].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[2].text, QStringLiteral("class"));
  EXPECT_EQ(tokens[2].loc.line, 2);
  EXPECT_EQ(tokens[2].loc.column, 1);

  bool saw_second_string = false;
  for (const auto& token : tokens) {
    saw_second_string =
        saw_second_string ||
        (token.kind == TokenKind::String && token.text == QStringLiteral("good.idl"));
  }
  EXPECT_TRUE(saw_second_string);
}

TEST(LexerTest, TokenizesHexNegativeScopeResolutionAndStar) {
  const QString source = QStringLiteral("0x1F -2 A::B *value");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("tokens.idl"), source, diagnostics);
  const auto tokens = lexer.Scan();

  ASSERT_FALSE(diagnostics.HasErrors()) << diagnostics.Format().toStdString();
  ASSERT_EQ(tokens.size(), 9);
  EXPECT_EQ(tokens[0].kind, TokenKind::Number);
  EXPECT_EQ(tokens[0].text, QStringLiteral("0x1F"));
  EXPECT_EQ(tokens[1].kind, TokenKind::Minus);
  EXPECT_EQ(tokens[2].kind, TokenKind::Number);
  EXPECT_EQ(tokens[2].text, QStringLiteral("2"));
  EXPECT_EQ(tokens[3].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[3].text, QStringLiteral("A"));
  EXPECT_EQ(tokens[4].kind, TokenKind::ColonColon);
  EXPECT_EQ(tokens[5].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[5].text, QStringLiteral("B"));
  EXPECT_EQ(tokens[6].kind, TokenKind::Star);
  EXPECT_EQ(tokens[7].kind, TokenKind::Identifier);
  EXPECT_EQ(tokens[7].text, QStringLiteral("value"));
  EXPECT_EQ(tokens[8].kind, TokenKind::End);
}
