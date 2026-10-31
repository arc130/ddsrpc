#include <gtest/gtest.h>

#include <QString>
#include <QVector>

#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"

using namespace ddsrpcgen;

TEST(ParserTest, ParsesImportsPodsAndEnums) {
  const QString source = QStringLiteral(
      "#include <QString>\n"
      "IMPORT \"common.idl\";\n"
      "POD Point(int x, int y)\n"
      "ENUM Mode { Idle, Running = 3, Fault = -1 }\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("sample.idl"), source, diagnostics);
  Parser parser(QStringLiteral("sample.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->directives.size(), 1);
  EXPECT_EQ(ast->directives[0].text, QStringLiteral("#include <QString>"));
  ASSERT_EQ(ast->imports.size(), 1);
  EXPECT_EQ(ast->imports[0].path, QStringLiteral("common.idl"));
  ASSERT_EQ(ast->pods.size(), 1);
  ASSERT_EQ(ast->pods[0].fields.size(), 2);
  EXPECT_EQ(ast->pods[0].name, QStringLiteral("Point"));
  EXPECT_EQ(ast->pods[0].fields[0].type.spelling, QStringLiteral("int"));
  ASSERT_EQ(ast->enums.size(), 1);
  ASSERT_EQ(ast->enums[0].values.size(), 3);
  EXPECT_EQ(ast->enums[0].values[1].name, QStringLiteral("Running"));
  EXPECT_EQ(ast->enums[0].values[1].value, 3);
  EXPECT_EQ(ast->enums[0].values[2].value, -1);
}

TEST(ParserTest, RejectsUnsupportedTopLevelConstruct) {
  const QString source = QStringLiteral("MODEL tracks(display)\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("bad.idl"), source, diagnostics);
  Parser parser(QStringLiteral("bad.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();
  EXPECT_FALSE(ast.has_value());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("MODEL")), -1);
}

TEST(ParserTest, ParsesEnumHexNegativeAndAutoIncrement) {
  const QString source = QStringLiteral(
      "ENUM Flags { A = 0x10, B, C = -2, D, E = -0x2 }\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("flags.idl"), source, diagnostics);
  Parser parser(QStringLiteral("flags.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->enums.size(), 1);
  const auto& values = ast->enums[0].values;
  ASSERT_EQ(values.size(), 5);
  EXPECT_EQ(values[0].name, QStringLiteral("A"));
  EXPECT_EQ(values[0].value, 16);
  EXPECT_EQ(values[1].name, QStringLiteral("B"));
  EXPECT_EQ(values[1].value, 17);
  EXPECT_EQ(values[2].name, QStringLiteral("C"));
  EXPECT_EQ(values[2].value, -2);
  EXPECT_EQ(values[3].name, QStringLiteral("D"));
  EXPECT_EQ(values[3].value, -1);
  EXPECT_EQ(values[4].name, QStringLiteral("E"));
  EXPECT_EQ(values[4].value, -2);
}

TEST(ParserTest, ParsesPodFieldTypesQualifiedTemplatesAndReferences) {
  const QString source = QStringLiteral(
      "POD Mapping(const QString &name, QMap<QString, QList<Point>> &lookup, A::B id)\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("types.idl"), source, diagnostics);
  Parser parser(QStringLiteral("types.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->pods.size(), 1);
  const auto& fields = ast->pods[0].fields;
  ASSERT_EQ(fields.size(), 3);

  EXPECT_EQ(fields[0].name, QStringLiteral("name"));
  EXPECT_EQ(fields[0].type.spelling, QStringLiteral("const QString &"));
  EXPECT_EQ(fields[0].type.base, QStringLiteral("QString"));
  EXPECT_TRUE(fields[0].type.is_const);
  EXPECT_TRUE(fields[0].type.is_reference);

  EXPECT_EQ(fields[1].name, QStringLiteral("lookup"));
  EXPECT_EQ(fields[1].type.base, QStringLiteral("QMap"));
  EXPECT_TRUE(fields[1].type.is_reference);
  EXPECT_FALSE(fields[1].type.is_const);
  ASSERT_EQ(fields[1].type.args.size(), 2);
  EXPECT_EQ(fields[1].type.args[0].base, QStringLiteral("QString"));
  EXPECT_EQ(fields[1].type.args[1].base, QStringLiteral("QList"));
  ASSERT_EQ(fields[1].type.args[1].args.size(), 1);
  EXPECT_EQ(fields[1].type.args[1].args[0].base, QStringLiteral("Point"));

  EXPECT_EQ(fields[2].name, QStringLiteral("id"));
  EXPECT_EQ(fields[2].type.spelling, QStringLiteral("A::B"));
  EXPECT_EQ(fields[2].type.base, QStringLiteral("A::B"));
  EXPECT_FALSE(fields[2].type.is_reference);
}

TEST(ParserTest, RejectsEmptyPodAndPointerField) {
  const struct {
    QString source;
    QString expected_message;
  } cases[] = {
      {QStringLiteral("POD Empty()\n"), QStringLiteral("at least one field")},
      {QStringLiteral("POD Bad(int *value)\n"), QStringLiteral("pointer")},
  };
  for (const auto& test_case : cases) {
    Diagnostics diagnostics;
    Lexer lexer(QStringLiteral("bad.idl"), test_case.source, diagnostics);
    Parser parser(QStringLiteral("bad.idl"), lexer.Scan(), test_case.source, diagnostics);
    const auto ast = parser.Parse();
    EXPECT_FALSE(ast.has_value()) << test_case.source.toStdString();
    EXPECT_NE(diagnostics.Format().indexOf(test_case.expected_message), -1)
        << diagnostics.Format().toStdString();
  }
}

TEST(ParserTest, ReportsImportSyntaxErrorsAtTokenLocation) {
  const QString source = QStringLiteral("IMPORT common;\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("bad.idl"), source, diagnostics);
  Parser parser(QStringLiteral("bad.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  EXPECT_FALSE(ast.has_value());
  ASSERT_TRUE(diagnostics.HasErrors());
  ASSERT_EQ(diagnostics.All().size(), 1) << diagnostics.Format().toStdString();
  EXPECT_EQ(diagnostics.All()[0].file, QStringLiteral("bad.idl"));
  EXPECT_EQ(diagnostics.All()[0].loc.line, 1);
  EXPECT_EQ(diagnostics.All()[0].loc.column, 8);
  EXPECT_NE(diagnostics.All()[0].message.indexOf(QStringLiteral("string")), -1);
}

TEST(ParserTest, ReportsClassParsingAsDeferred) {
  const QString source = QStringLiteral(
      "class Calc {\n"
      "  PROP(int total = 42)\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("calc.idl"), source, diagnostics);
  Parser parser(QStringLiteral("calc.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  EXPECT_FALSE(ast.has_value());
  ASSERT_TRUE(diagnostics.HasErrors());
  const QString formatted = diagnostics.Format();
  EXPECT_NE(formatted.indexOf(QStringLiteral("class")), -1);
  EXPECT_NE(formatted.indexOf(QStringLiteral("not yet available")), -1);
  ASSERT_EQ(diagnostics.All().size(), 1) << formatted.toStdString();
  EXPECT_EQ(diagnostics.All()[0].loc.line, 1);
  EXPECT_EQ(diagnostics.All()[0].loc.column, 1);
}
