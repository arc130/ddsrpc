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

TEST(ParserTest, ParsesClassMembersTypesAndDefaults) {
  const QString source = QStringLiteral(
      "POD Point(int x, int y)\n"
      "ENUM Mode { Idle, Running }\n"
      "class Calc {\n"
      "  ENUM State { Ready, Busy }\n"
      "  PROP(int total = 42)\n"
      "  PROP(QString name READONLY)\n"
      "  PROP(Mode mode = Idle READWRITE)\n"
      "  PROP(QString label = \"a b\")\n"
      "  PROP(QString quoted = \"a\\\"b\")\n"
      "  SIGNAL(updated(int total))\n"
      "  SLOT(int add(int a, const QString &label))\n"
      "  SLOT(void reset())\n"
      "  SLOT(QMap<QString, QList<Point>> history(const int &limit))\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("calc.idl"), source, diagnostics);
  Parser parser(QStringLiteral("calc.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->classes.size(), 1);
  const auto& cls = ast->classes[0];
  ASSERT_EQ(cls.properties.size(), 5);
  EXPECT_EQ(cls.properties[0].default_value, QStringLiteral("42"));
  EXPECT_EQ(cls.properties[1].modifier, PropModifier::kReadOnly);
  EXPECT_EQ(cls.properties[2].modifier, PropModifier::kReadWrite);
  EXPECT_EQ(cls.properties[3].default_value, QStringLiteral("\"a b\""));
  EXPECT_EQ(cls.properties[4].default_value, QStringLiteral("\"a\\\"b\""));
  ASSERT_EQ(cls.signals.size(), 1);
  ASSERT_EQ(cls.slots.size(), 3);
  EXPECT_EQ(cls.slots[1].return_type.spelling, QStringLiteral("void"));
  ASSERT_EQ(cls.slots[2].params.size(), 1);
  EXPECT_EQ(cls.slots[2].params[0].name, QStringLiteral("limit"));
  EXPECT_TRUE(cls.slots[2].params[0].type.is_const);
  EXPECT_TRUE(cls.slots[2].params[0].type.is_reference);
}

TEST(ParserTest, RejectsUnsupportedClassMembers) {
  for (const QString& member : {
           QStringLiteral("MODEL tracks(display)"),
           QStringLiteral("CLASS child(Other)"),
           QStringLiteral("USE_ENUM(Qt::DateFormat)"),
           QStringLiteral("PROP(int x PERSISTED)"),
       }) {
    Diagnostics diagnostics;
    const QString source = QStringLiteral("class C { %1 }\n").arg(member);
    Lexer lexer(QStringLiteral("bad.idl"), source, diagnostics);
    Parser parser(QStringLiteral("bad.idl"), lexer.Scan(), source, diagnostics);
    EXPECT_FALSE(parser.Parse().has_value()) << member.toStdString();
    EXPECT_TRUE(diagnostics.HasErrors());
  }
}

TEST(ParserTest, ParsesMultiWordQualifiersAndFoldedReferences) {
  const QString source = QStringLiteral(
      "POD Numbers(unsigned int a, unsigned long long b, long double ratio, "
      "const QString &&name, short s)\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("numbers.idl"), source, diagnostics);
  Parser parser(QStringLiteral("numbers.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->pods.size(), 1);
  const auto& fields = ast->pods[0].fields;
  ASSERT_EQ(fields.size(), 5);
  EXPECT_EQ(fields[0].type.spelling, QStringLiteral("unsigned int"));
  EXPECT_EQ(fields[0].type.base, QStringLiteral("int"));
  EXPECT_EQ(fields[1].type.spelling, QStringLiteral("unsigned long long"));
  EXPECT_EQ(fields[1].type.base, QStringLiteral("long"));
  EXPECT_EQ(fields[2].type.spelling, QStringLiteral("long double"));
  EXPECT_EQ(fields[2].type.base, QStringLiteral("double"));
  EXPECT_EQ(fields[3].type.spelling, QStringLiteral("const QString &&"));
  EXPECT_TRUE(fields[3].type.is_const);
  EXPECT_TRUE(fields[3].type.is_reference);
  EXPECT_EQ(fields[4].type.spelling, QStringLiteral("short"));
  EXPECT_EQ(fields[4].type.base, QStringLiteral("short"));
}

TEST(ParserTest, ParsesUnnamedParamsAndImplicitVoidReturn) {
  const QString source = QStringLiteral(
      "class C {\n"
      "  SIGNAL(updated(int, const QString &))\n"
      "  SLOT(reset())\n"
      "  SLOT(void named(int))\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("c.idl"), source, diagnostics);
  Parser parser(QStringLiteral("c.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->classes.size(), 1);
  const auto& cls = ast->classes[0];
  ASSERT_EQ(cls.signals.size(), 1);
  ASSERT_EQ(cls.signals[0].params.size(), 2);
  EXPECT_FALSE(cls.signals[0].params[0].named);
  EXPECT_TRUE(cls.signals[0].params[0].name.isEmpty());
  EXPECT_TRUE(cls.signals[0].params[1].type.is_reference);
  ASSERT_EQ(cls.slots.size(), 2);
  EXPECT_EQ(cls.slots[0].name, QStringLiteral("reset"));
  EXPECT_EQ(cls.slots[0].return_type.spelling, QStringLiteral("void"));
  EXPECT_TRUE(cls.slots[0].params.isEmpty());
  EXPECT_EQ(cls.slots[1].return_type.spelling, QStringLiteral("void"));
}

TEST(ParserTest, PreservesDefaultExpressionSourceSlices) {
  const QString source = QStringLiteral(
      "class C {\n"
      "  PROP(QString text = QStringLiteral(\"a b\"))\n"
      "  PROP(Mode mode = Mode::Idle)\n"
      "  PROP(int neg = -1)\n"
      "  PROP(QString escaped = \"a\\\"b\" READONLY)\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("defaults.idl"), source, diagnostics);
  Parser parser(QStringLiteral("defaults.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->classes.size(), 1);
  const auto& properties = ast->classes[0].properties;
  ASSERT_EQ(properties.size(), 4);
  EXPECT_TRUE(properties[0].has_default);
  EXPECT_EQ(properties[0].default_value, QStringLiteral("QStringLiteral(\"a b\")"));
  EXPECT_TRUE(properties[1].has_default);
  EXPECT_EQ(properties[1].default_value, QStringLiteral("Mode::Idle"));
  EXPECT_TRUE(properties[2].has_default);
  EXPECT_EQ(properties[2].default_value, QStringLiteral("-1"));
  EXPECT_TRUE(properties[3].has_default);
  EXPECT_EQ(properties[3].default_value, QStringLiteral("\"a\\\"b\""));
  EXPECT_EQ(properties[3].modifier, PropModifier::kReadOnly);
}

TEST(ParserTest, ParsesPropertyVisibilityModifiers) {
  const QString source = QStringLiteral(
      "class C {\n"
      "  PROP(int constant CONSTANT)\n"
      "  PROP(int push READPUSH)\n"
      "  PROP(int source SOURCEONLYSETTER)\n"
      "  PROP(int implicit)\n"
      "};\n");
  Diagnostics diagnostics;
  Lexer lexer(QStringLiteral("mods.idl"), source, diagnostics);
  Parser parser(QStringLiteral("mods.idl"), lexer.Scan(), source, diagnostics);
  const auto ast = parser.Parse();

  ASSERT_TRUE(ast.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(ast->classes.size(), 1);
  const auto& properties = ast->classes[0].properties;
  ASSERT_EQ(properties.size(), 4);
  EXPECT_EQ(properties[0].modifier, PropModifier::kConstant);
  EXPECT_EQ(properties[1].modifier, PropModifier::kReadPush);
  EXPECT_EQ(properties[2].modifier, PropModifier::kSourceOnlySetter);
  EXPECT_EQ(properties[3].modifier, PropModifier::kReadPush);
}

TEST(ParserTest, RejectsMalformedPropModifiersAndDefaults) {
  const struct {
    QString member;
    QString expected_message;
  } cases[] = {
      {QStringLiteral("PROP(int x READONLY READWRITE)"), QStringLiteral("multiple visibility")},
      {QStringLiteral("PROP(int x BOGUS)"), QStringLiteral("expected '='")},
      {QStringLiteral("PROP(int x = )"), QStringLiteral("default expression")},
      {QStringLiteral("PROP(int x"), QStringLiteral("unterminated")},
  };
  for (const auto& test_case : cases) {
    Diagnostics diagnostics;
    const QString source =
        QStringLiteral("class C { %1 }\n").arg(test_case.member);
    Lexer lexer(QStringLiteral("bad.idl"), source, diagnostics);
    Parser parser(QStringLiteral("bad.idl"), lexer.Scan(), source, diagnostics);
    EXPECT_FALSE(parser.Parse().has_value()) << test_case.member.toStdString();
    EXPECT_NE(diagnostics.Format().indexOf(test_case.expected_message), -1)
        << diagnostics.Format().toStdString();
  }
}

TEST(ParserTest, RejectsReferenceReturnAndPointerParameter) {
  const struct {
    QString member;
    QString expected_message;
  } cases[] = {
      {QStringLiteral("SLOT(int &bad())"), QStringLiteral("reference")},
      {QStringLiteral("SLOT(int *bad())"), QStringLiteral("pointer")},
      {QStringLiteral("SLOT(void f(int *p))"), QStringLiteral("pointer")},
  };
  for (const auto& test_case : cases) {
    Diagnostics diagnostics;
    const QString source =
        QStringLiteral("class C { %1 }\n").arg(test_case.member);
    Lexer lexer(QStringLiteral("bad.idl"), source, diagnostics);
    Parser parser(QStringLiteral("bad.idl"), lexer.Scan(), source, diagnostics);
    EXPECT_FALSE(parser.Parse().has_value()) << test_case.member.toStdString();
    EXPECT_NE(diagnostics.Format().indexOf(test_case.expected_message), -1)
        << diagnostics.Format().toStdString();
  }
}
