#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include "import_resolver.h"
#include "semantic.h"

using namespace ddsrpcgen;

namespace {
void Write(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(data);
}

LoadedProgram Load(const QString& path) {
  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  auto program = resolver.Load(path);
  EXPECT_TRUE(program.has_value()) << diagnostics.Format().toStdString();
  return std::move(program).value();
}

struct Analysis {
  std::optional<Model> model;
  QString diagnostics;
};

Analysis AnalyzeText(const QString& dir, const QString& name, const QByteArray& data,
                     const QString& name_space = QStringLiteral("demo")) {
  const QString path = QDir(dir).filePath(name);
  Write(path, data);
  const auto loaded = Load(path);
  Diagnostics diagnostics;
  auto model = Analyze(loaded, name_space, diagnostics);
  return Analysis{std::move(model), diagnostics.Format()};
}

QStringList PodNames(const FileModel& file) {
  QStringList names;
  for (const PodModel& pod : file.pods) names.append(pod.name);
  return names;
}

}  // namespace

TEST(SemanticTest, AssignsIdsResolvesEnumsAndRewritesDefault) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString path = tmp.filePath("c.idl");
  Write(path,
        "ENUM Mode { Idle, Running }\n"
        "class Calc {\n"
        "  PROP(Mode mode = Idle READWRITE)\n"
        "  PROP(int total = 3)\n"
        "  SIGNAL(updated(int total))\n"
        "  SLOT(int add(int a, int b))\n"
        "};\n");
  const auto loaded = Load(path);
  Diagnostics diagnostics;
  const auto model = Analyze(loaded, QStringLiteral("demo"), diagnostics);
  ASSERT_TRUE(model.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(model->files.size(), 1);
  const auto& cls = model->files[0].classes[0];
  ASSERT_EQ(cls.properties.size(), 2);
  EXPECT_EQ(cls.properties[0].default_value, QStringLiteral("Mode::Idle"));
  EXPECT_EQ(cls.properties[0].remote_setter_op, 1);
  EXPECT_EQ(cls.properties[1].remote_setter_op, 2);
  EXPECT_EQ(cls.slots[0].op, 3);
  EXPECT_EQ(cls.signals[0].event_id, 1);
  ASSERT_FALSE(model->files[0].schema_hash == 0);
}

TEST(SemanticTest, RejectsDuplicateNamesAndReservedMembers) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString path = tmp.filePath("bad.idl");
  Write(path,
        "class C {\n"
        "  PROP(int ready)\n"
        "  SLOT(void start())\n"
        "};\n");
  const auto loaded = Load(path);
  Diagnostics diagnostics;
  EXPECT_FALSE(Analyze(loaded, QString(), diagnostics).has_value());
  const auto text = diagnostics.Format();
  EXPECT_NE(text.indexOf(QStringLiteral("ready")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("start")), -1);
}

TEST(SemanticTest, SchemaChangesWhenInterfaceChanges) {
  // Both inputs use the same stem so that only the interface change can
  // explain a hash difference.
  QTemporaryDir first_dir;
  QTemporaryDir second_dir;
  ASSERT_TRUE(first_dir.isValid());
  ASSERT_TRUE(second_dir.isValid());
  const QString first = first_dir.filePath("x.idl");
  const QString second = second_dir.filePath("x.idl");
  Write(first, "class C { SLOT(int ping()) }\n");
  Write(second, "class C { SLOT(int ping(int x)) }\n");

  const auto loaded_first = Load(first);
  const auto loaded_second = Load(second);
  Diagnostics d1;
  Diagnostics d2;
  const auto m1 = Analyze(loaded_first, QStringLiteral("demo"), d1);
  const auto m2 = Analyze(loaded_second, QStringLiteral("demo"), d2);
  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m1->files[0].stem, m2->files[0].stem);
  EXPECT_NE(m1->files[0].schema_hash, m2->files[0].schema_hash);
}

TEST(SemanticTest, RejectsDuplicateDeclarationsAcrossImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("common.idl"), "POD Point(int x)\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"common.idl\";\n"
        "POD Point(int y)\n"
        "class C { }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics diagnostics;
  EXPECT_FALSE(Analyze(loaded, QStringLiteral("demo"), diagnostics).has_value());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("Point")), -1);
}

TEST(SemanticTest, ResolvesGeneratedAndExternalTypes) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const auto result = AnalyzeText(
      tmp.path(), "types.idl",
      "POD Point(int x, int y)\n"
      "ENUM Mode { Idle, Running }\n"
      "class Calc {\n"
      "  ENUM State { New, Ready }\n"
      "  PROP(Mode mode = Running READWRITE)\n"
      "  PROP(State state = New)\n"
      "  PROP(QList<Point> points)\n"
      "  SLOT(Calc::State next(State state, const QString& label))\n"
      "};\n");
  ASSERT_TRUE(result.model.has_value()) << result.diagnostics.toStdString();
  const auto& cls = result.model->files[0].classes[0];
  ASSERT_EQ(cls.properties.size(), 3);
  EXPECT_EQ(cls.properties[0].cpp_type, QStringLiteral("demo::Mode"));
  EXPECT_EQ(cls.properties[0].default_value, QStringLiteral("Mode::Running"));
  EXPECT_EQ(cls.properties[1].cpp_type, QStringLiteral("demo::Calc_State"));
  EXPECT_EQ(cls.properties[1].default_value, QStringLiteral("Calc_State::New"));
  EXPECT_EQ(cls.properties[2].cpp_type, QStringLiteral("QList<demo::Point>"));
  ASSERT_EQ(cls.slots.size(), 1);
  EXPECT_EQ(cls.slots[0].return_cpp_type, QStringLiteral("demo::Calc_State"));
  ASSERT_EQ(cls.slots[0].params.size(), 2);
  EXPECT_EQ(cls.slots[0].params[0].cpp_type, QStringLiteral("demo::Calc_State"));
  EXPECT_EQ(cls.slots[0].params[1].cpp_type, QStringLiteral("QString"));
  EXPECT_TRUE(cls.slots[0].params[1].named);
  ASSERT_EQ(cls.class_enums.size(), 1);
  EXPECT_EQ(cls.class_enums[0].idl_name, QStringLiteral("State"));
  EXPECT_EQ(cls.class_enums[0].cpp_name, QStringLiteral("Calc_State"));
  EXPECT_EQ(cls.class_enums[0].class_owner, QStringLiteral("Calc"));
}

TEST(SemanticTest, ResolvesMultiwordBuiltinsFromSpelling) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const auto result = AnalyzeText(
      tmp.path(), "builtins.idl",
      "class Big {\n"
      "  PROP(unsigned long long total = 0)\n"
      "  SLOT(unsigned long long tick(unsigned int amount))\n"
      "  SLOT(long double ratio())\n"
      "};\n",
      QString());
  ASSERT_TRUE(result.model.has_value()) << result.diagnostics.toStdString();
  const auto& cls = result.model->files[0].classes[0];
  ASSERT_EQ(cls.properties.size(), 1);
  EXPECT_EQ(cls.properties[0].cpp_type, QStringLiteral("unsigned long long"));
  ASSERT_EQ(cls.slots.size(), 2);
  EXPECT_EQ(cls.slots[0].return_cpp_type, QStringLiteral("unsigned long long"));
  ASSERT_EQ(cls.slots[0].params.size(), 1);
  EXPECT_EQ(cls.slots[0].params[0].cpp_type, QStringLiteral("unsigned int"));
  EXPECT_EQ(cls.slots[1].return_cpp_type, QStringLiteral("long double"));
}

TEST(SemanticTest, SortsPodDependenciesAndRejectsCycles) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const auto ordered = AnalyzeText(
      tmp.path(), "ordered.idl",
      "POD Envelope(Header header, QList<Payload> payloads)\n"
      "POD Payload(int value)\n"
      "POD Header(int version)\n"
      "class C { }\n");
  ASSERT_TRUE(ordered.model.has_value()) << ordered.diagnostics.toStdString();
  EXPECT_EQ(PodNames(ordered.model->files[0]),
            QStringList({QStringLiteral("Header"), QStringLiteral("Payload"),
                         QStringLiteral("Envelope")}));

  const auto cyclic = AnalyzeText(
      tmp.path(), "cyclic.idl",
      "POD A(B b)\n"
      "POD B(A a)\n"
      "class C { }\n");
  EXPECT_FALSE(cyclic.model.has_value());
  EXPECT_NE(cyclic.diagnostics.indexOf(QStringLiteral("cycle")), -1);
  EXPECT_NE(cyclic.diagnostics.indexOf(QStringLiteral("A")), -1);
}

TEST(SemanticTest, SchemaIsDeterministicAndTracksImportedTypes) {
  QTemporaryDir first_dir;
  QTemporaryDir second_dir;
  ASSERT_TRUE(first_dir.isValid());
  ASSERT_TRUE(second_dir.isValid());
  const QByteArray common = "POD Point(int x)\nENUM Mode { Idle }\n";
  const QByteArray root =
      "IMPORT \"common.idl\";\n"
      "class C { SLOT(void ping(Point p)) }\n";
  Write(first_dir.filePath("common.idl"), common);
  Write(first_dir.filePath("root.idl"), root);
  Write(second_dir.filePath("common.idl"), common);
  Write(second_dir.filePath("root.idl"), root);

  const auto loaded_first = Load(first_dir.filePath("root.idl"));
  const auto loaded_second = Load(second_dir.filePath("root.idl"));
  Diagnostics d1;
  Diagnostics d2;
  const auto m1 = Analyze(loaded_first, QStringLiteral("demo"), d1);
  const auto m2 = Analyze(loaded_second, QStringLiteral("demo"), d2);
  ASSERT_TRUE(m1.has_value()) << d1.Format().toStdString();
  ASSERT_TRUE(m2.has_value()) << d2.Format().toStdString();
  ASSERT_EQ(m1->files.size(), 2);
  const FileModel& root1 = m1->files[1];
  EXPECT_EQ(root1.stem, QStringLiteral("root"));
  EXPECT_EQ(root1.path, QFileInfo(first_dir.filePath("root.idl")).canonicalFilePath());
  ASSERT_EQ(root1.imports.size(), 1);
  EXPECT_EQ(root1.imports[0], QFileInfo(first_dir.filePath("common.idl")).canonicalFilePath());
  EXPECT_EQ(root1.schema_hash, m2->files[1].schema_hash);
  EXPECT_NE(root1.schema_hash, quint64(0));
  EXPECT_EQ(root1.classes[0].schema_hash, root1.schema_hash);

  Diagnostics d3;
  const auto renamed = Analyze(loaded_first, QStringLiteral("other"), d3);
  ASSERT_TRUE(renamed.has_value()) << d3.Format().toStdString();
  EXPECT_NE(renamed->files[1].schema_hash, root1.schema_hash);

  Write(second_dir.filePath("common.idl"), "POD Point(int x, int y)\nENUM Mode { Idle }\n");
  const auto changed_loaded = Load(second_dir.filePath("root.idl"));
  Diagnostics d4;
  const auto changed = Analyze(changed_loaded, QStringLiteral("demo"), d4);
  ASSERT_TRUE(changed.has_value()) << d4.Format().toStdString();
  EXPECT_NE(changed->files[1].schema_hash, root1.schema_hash);
}

TEST(SemanticTest, SchemaIgnoresImportPathSpelling) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString common = tmp.filePath("common.idl");
  const QString root = tmp.filePath("root.idl");
  Write(common, "POD Point(int x)\n");
  Write(root,
        "IMPORT \"common.idl\";\n"
        "class C { SLOT(void ping(Point p)) }\n");

  const auto relative_loaded = Load(root);
  Diagnostics d1;
  const auto relative_model = Analyze(relative_loaded, QStringLiteral("demo"), d1);
  ASSERT_TRUE(relative_model.has_value()) << d1.Format().toStdString();
  ASSERT_EQ(relative_model->files[1].imports.size(), 1);
  EXPECT_EQ(relative_model->files[1].imports[0], QFileInfo(common).canonicalFilePath());

  // Same root path and stem, only the IMPORT spelling changes.
  Write(root,
        QStringLiteral("IMPORT \"%1\";\nclass C { SLOT(void ping(Point p)) }\n")
            .arg(common)
            .toUtf8());
  const auto absolute_loaded = Load(root);
  Diagnostics d2;
  const auto absolute_model = Analyze(absolute_loaded, QStringLiteral("demo"), d2);
  ASSERT_TRUE(absolute_model.has_value()) << d2.Format().toStdString();
  ASSERT_EQ(absolute_model->files[1].imports.size(), 1);
  EXPECT_EQ(absolute_model->files[1].imports[0], QFileInfo(common).canonicalFilePath());
  EXPECT_EQ(relative_model->files[1].schema_hash, absolute_model->files[1].schema_hash);
}

TEST(SemanticTest, SchemaIncludesTransitiveImportedTypes) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("base.idl"), "POD Base(int value)\n");
  Write(tmp.filePath("mid.idl"), "IMPORT \"base.idl\";\nPOD Mid(Base base)\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"mid.idl\";\n"
        "class C { SLOT(void use(Mid mid)) }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics d1;
  const auto before = Analyze(loaded, QStringLiteral("demo"), d1);
  ASSERT_TRUE(before.has_value()) << d1.Format().toStdString();
  ASSERT_EQ(before->files.size(), 3);
  const quint64 root_hash = before->files[2].schema_hash;

  Write(tmp.filePath("base.idl"), "POD Base(int value, int extra)\n");
  const auto changed_loaded = Load(tmp.filePath("root.idl"));
  Diagnostics d2;
  const auto after = Analyze(changed_loaded, QStringLiteral("demo"), d2);
  ASSERT_TRUE(after.has_value()) << d2.Format().toStdString();
  EXPECT_NE(after->files[2].schema_hash, root_hash);
}

TEST(SemanticTest, SchemaOfUnrelatedImportBranchIsStable) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("a.idl"), "POD A(int x)\n");
  Write(tmp.filePath("b.idl"), "POD B(int y)\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"a.idl\";\n"
        "IMPORT \"b.idl\";\n"
        "class C { }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics d1;
  const auto before = Analyze(loaded, QStringLiteral("demo"), d1);
  ASSERT_TRUE(before.has_value()) << d1.Format().toStdString();
  ASSERT_EQ(before->files.size(), 3);
  EXPECT_EQ(before->files[0].stem, QStringLiteral("a"));
  EXPECT_EQ(before->files[1].stem, QStringLiteral("b"));
  const quint64 a_hash = before->files[0].schema_hash;
  const quint64 b_hash = before->files[1].schema_hash;

  Write(tmp.filePath("b.idl"), "POD B(int y, int extra)\n");
  const auto changed_loaded = Load(tmp.filePath("root.idl"));
  Diagnostics d2;
  const auto after = Analyze(changed_loaded, QStringLiteral("demo"), d2);
  ASSERT_TRUE(after.has_value()) << d2.Format().toStdString();
  EXPECT_EQ(after->files[0].schema_hash, a_hash);
  EXPECT_NE(after->files[1].schema_hash, b_hash);
}

TEST(SemanticTest, SchemaTracksDirectivesDefaultsAndModifiers) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  // Each variant uses the same file stem so that only its content can change
  // the hash.
  const auto baseline =
      AnalyzeText(tmp.filePath("baseline"), "x.idl", "class C { SLOT(int ping()) }\n");
  ASSERT_TRUE(baseline.model.has_value()) << baseline.diagnostics.toStdString();
  const quint64 baseline_hash = baseline.model->files[0].schema_hash;

  const auto directive = AnalyzeText(
      tmp.filePath("directive"), "x.idl", "#include <QString>\nclass C { SLOT(int ping()) }\n");
  ASSERT_TRUE(directive.model.has_value()) << directive.diagnostics.toStdString();
  EXPECT_NE(directive.model->files[0].schema_hash, baseline_hash);

  const auto with_default =
      AnalyzeText(tmp.filePath("default_one"), "x.idl", "class C { PROP(int x = 1) }\n");
  ASSERT_TRUE(with_default.model.has_value()) << with_default.diagnostics.toStdString();
  const auto other_default =
      AnalyzeText(tmp.filePath("default_two"), "x.idl", "class C { PROP(int x = 2) }\n");
  ASSERT_TRUE(other_default.model.has_value()) << other_default.diagnostics.toStdString();
  EXPECT_NE(with_default.model->files[0].schema_hash,
            other_default.model->files[0].schema_hash);

  const auto read_only =
      AnalyzeText(tmp.filePath("read_only"), "x.idl", "class C { PROP(int x READONLY) }\n");
  ASSERT_TRUE(read_only.model.has_value()) << read_only.diagnostics.toStdString();
  const auto read_write =
      AnalyzeText(tmp.filePath("read_write"), "x.idl", "class C { PROP(int x READWRITE) }\n");
  ASSERT_TRUE(read_write.model.has_value()) << read_write.diagnostics.toStdString();
  EXPECT_NE(read_only.model->files[0].schema_hash,
            read_write.model->files[0].schema_hash);
}

TEST(SemanticTest, RejectsMemberAndGeneratedNameCollisions) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  {
    const auto result = AnalyzeText(
        tmp.path(), "duplicate_member.idl",
        "class C { PROP(int foo) SIGNAL(foo(int x)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("foo")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "changed.idl",
        "class C { PROP(int foo) SIGNAL(fooChanged(int x)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("fooChanged")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "setter.idl",
        "class C { PROP(int foo READWRITE) SLOT(void setFoo()) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("setFoo")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "constant_changed.idl",
        "class C { PROP(int foo CONSTANT) SIGNAL(fooChanged(int x)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("fooChanged")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "changed_property.idl",
        "class C { PROP(int foo) PROP(int fooChanged) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("fooChanged")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "reserved_signal.idl",
        "class C { SIGNAL(syncFailed(QString message)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("syncFailed")), -1);
  }
  {
    const auto result = AnalyzeText(tmp.path(), "options.idl",
                                    "class C { PROP(int Options) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("Options")), -1);
  }
}

TEST(SemanticTest, RejectsInvalidIdentifiersAndParameterCollisions) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  {
    const auto result =
        AnalyzeText(tmp.path(), "keyword.idl", "class C { PROP(int class) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("class")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "argn.idl", "class C { SLOT(void run(int, int arg0)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("arg0")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "argn_named_first.idl", "class C { SLOT(void run(int arg0, int)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("arg0")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "params.idl", "class C { SLOT(void run(int x, int x)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("'x'")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "fields.idl", "POD P(int x, int x)\nclass C { }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("'x'")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "values.idl", "ENUM E { A, A }\nclass C { }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("'A'")), -1);
  }
}

TEST(SemanticTest, RejectsVoidPropertyAndPodField) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  {
    const auto result =
        AnalyzeText(tmp.path(), "void_prop.idl", "class C { PROP(void nothing) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("void")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "void_field.idl", "POD P(void field)\nclass C { }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("void")), -1);
  }
}

TEST(SemanticTest, AssignsOpsAndEventIdsInDeclarationOrder) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const auto result = AnalyzeText(
      tmp.path(), "ids.idl",
      "class C {\n"
      "  PROP(int a READWRITE)\n"
      "  PROP(int b READONLY)\n"
      "  PROP(int c READPUSH)\n"
      "  PROP(int d CONSTANT)\n"
      "  SIGNAL(s1())\n"
      "  SIGNAL(s2(int value))\n"
      "  SLOT(void x())\n"
      "  SLOT(int y(int v))\n"
      "};\n");
  ASSERT_TRUE(result.model.has_value()) << result.diagnostics.toStdString();
  const auto& cls = result.model->files[0].classes[0];
  ASSERT_EQ(cls.properties.size(), 4);
  EXPECT_EQ(cls.properties[0].property_id, 1);
  EXPECT_EQ(cls.properties[1].property_id, 2);
  EXPECT_EQ(cls.properties[2].property_id, 3);
  EXPECT_EQ(cls.properties[3].property_id, 4);
  EXPECT_EQ(cls.properties[0].remote_setter_op, 1);
  EXPECT_EQ(cls.properties[1].remote_setter_op, 0);
  EXPECT_EQ(cls.properties[2].remote_setter_op, 2);
  EXPECT_EQ(cls.properties[3].remote_setter_op, 0);
  ASSERT_EQ(cls.slots.size(), 2);
  EXPECT_EQ(cls.slots[0].op, 3);
  EXPECT_EQ(cls.slots[1].op, 4);
  ASSERT_EQ(cls.signals.size(), 2);
  EXPECT_EQ(cls.signals[0].event_id, 1);
  EXPECT_EQ(cls.signals[1].event_id, 2);
}

TEST(SemanticTest, RejectsDuplicateGlobalAndGeneratedTypeNames) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  {
    const auto result =
        AnalyzeText(tmp.path(), "dup_kind.idl", "ENUM Mode { A }\nPOD Mode(int x)\nclass C { }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("Mode")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "generated.idl",
        "POD CService(int x)\n"
        "class C { }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("CService")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "class_enum.idl",
        "ENUM C_State { A }\n"
        "class C { ENUM State { B } }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("C_State")), -1);
  }
}

TEST(SemanticTest, PreservesBareEnumFromOtherClassAsExternalType) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const auto result = AnalyzeText(
      tmp.path(), "bare.idl",
      "class A { ENUM Mode { Idle } }\n"
      "class B { SLOT(A::Mode use(Mode mode)) }\n");
  ASSERT_TRUE(result.model.has_value()) << result.diagnostics.toStdString();
  const auto& cls = result.model->files[0].classes[1];
  ASSERT_EQ(cls.slots.size(), 1);
  EXPECT_EQ(cls.slots[0].return_cpp_type, QStringLiteral("demo::A_Mode"));
  EXPECT_EQ(cls.slots[0].params[0].cpp_type, QStringLiteral("Mode"));
}

TEST(SemanticTest, RejectsGeneratedClassNameMemberCollisions) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  {
    const auto result =
        AnalyzeText(tmp.path(), "property_service.idl", "class C { PROP(int CService) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("CService")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "slot_client.idl", "class C { SLOT(void CClient()) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("CClient")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "enum_service.idl", "class C { ENUM CService { A } }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("CService")), -1);
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "property_client.idl", "class C { PROP(int CClient) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("CClient")), -1);
  }
}

TEST(SemanticTest, RejectsPodFieldWithPodName) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const auto result =
      AnalyzeText(tmp.path(), "pod_name.idl", "POD Point(int Point)\nclass C { }\n");
  EXPECT_FALSE(result.model.has_value());
  EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("same name")), -1);
}

TEST(SemanticTest, ResolvesPodFieldTypesAgainstVisibleSymbols) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("common.idl"),
        "ENUM Mode { Idle }\n"
        "POD Header(int version)\n"
        "class Box { ENUM State { New } }\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"common.idl\";\n"
        "POD Thing(Header header, Mode mode, Box::State state, Box_State canonical_state, "
        "const QString& label, QList<Header> items, unsigned long long bits)\n"
        "class C { }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics diagnostics;
  const auto model = Analyze(loaded, QStringLiteral("demo"), diagnostics);
  ASSERT_TRUE(model.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(model->files.size(), 2);
  const FileModel& root = model->files[1];
  ASSERT_EQ(root.pods.size(), 1);
  const PodModel& thing = root.pods[0];
  EXPECT_EQ(thing.name, QStringLiteral("Thing"));
  ASSERT_EQ(thing.fields.size(), 7);
  EXPECT_EQ(thing.fields[0].name, QStringLiteral("header"));
  EXPECT_EQ(thing.fields[0].cpp_type, QStringLiteral("demo::Header"));
  EXPECT_EQ(thing.fields[1].cpp_type, QStringLiteral("demo::Mode"));
  EXPECT_EQ(thing.fields[2].cpp_type, QStringLiteral("demo::Box_State"));
  EXPECT_EQ(thing.fields[3].cpp_type, QStringLiteral("demo::Box_State"));
  EXPECT_EQ(thing.fields[4].cpp_type, QStringLiteral("QString"));
  EXPECT_EQ(thing.fields[5].cpp_type, QStringLiteral("QList<demo::Header>"));
  EXPECT_EQ(thing.fields[6].cpp_type, QStringLiteral("unsigned long long"));
}

TEST(SemanticTest, ResolvesTypesFromTransitiveImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("base.idl"),
        "ENUM Mode { Idle }\n"
        "POD Base(int version)\n"
        "class Box { ENUM State { New } }\n");
  Write(tmp.filePath("mid.idl"),
        "IMPORT \"base.idl\";\n"
        "POD Mid(Base base, Mode mode)\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"mid.idl\";\n"
        "class C { SLOT(void use(Mid mid, Box::State state, Base base, Mode mode)) }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics diagnostics;
  const auto model = Analyze(loaded, QStringLiteral("demo"), diagnostics);
  ASSERT_TRUE(model.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(model->files.size(), 3);
  const FileModel& mid = model->files[1];
  ASSERT_EQ(mid.pods.size(), 1);
  ASSERT_EQ(mid.pods[0].fields.size(), 2);
  EXPECT_EQ(mid.pods[0].fields[0].cpp_type, QStringLiteral("demo::Base"));
  EXPECT_EQ(mid.pods[0].fields[1].cpp_type, QStringLiteral("demo::Mode"));
  const auto& slot = model->files[2].classes[0].slots[0];
  ASSERT_EQ(slot.params.size(), 4);
  EXPECT_EQ(slot.params[0].cpp_type, QStringLiteral("demo::Mid"));
  EXPECT_EQ(slot.params[1].cpp_type, QStringLiteral("demo::Box_State"));
  EXPECT_EQ(slot.params[2].cpp_type, QStringLiteral("demo::Base"));
  EXPECT_EQ(slot.params[3].cpp_type, QStringLiteral("demo::Mode"));
}

TEST(SemanticTest, RejectsInvisibleCrossFileReferences) {
  QTemporaryDir first;
  ASSERT_TRUE(first.isValid());
  {
    Write(first.filePath("a.idl"), "POD A(B b)\n");
    Write(first.filePath("b.idl"), "POD B(int x)\n");
    Write(first.filePath("root.idl"),
          "IMPORT \"a.idl\";\n"
          "IMPORT \"b.idl\";\n"
          "class C { }\n");
    const auto loaded = Load(first.filePath("root.idl"));
    Diagnostics diagnostics;
    EXPECT_FALSE(Analyze(loaded, QStringLiteral("demo"), diagnostics).has_value());
    const QString text = diagnostics.Format();
    EXPECT_NE(text.indexOf(QStringLiteral("'B'")), -1) << text.toStdString();
    EXPECT_NE(text.indexOf(QStringLiteral("not visible")), -1) << text.toStdString();
  }

  QTemporaryDir second;
  ASSERT_TRUE(second.isValid());
  {
    Write(second.filePath("a.idl"), "POD A(int x)\n");
    Write(second.filePath("b.idl"), "POD B(A a)\n");
    Write(second.filePath("root.idl"),
          "IMPORT \"a.idl\";\n"
          "IMPORT \"b.idl\";\n"
          "class C { }\n");
    const auto loaded = Load(second.filePath("root.idl"));
    Diagnostics diagnostics;
    EXPECT_FALSE(Analyze(loaded, QStringLiteral("demo"), diagnostics).has_value());
    const QString text = diagnostics.Format();
    EXPECT_NE(text.indexOf(QStringLiteral("'A'")), -1) << text.toStdString();
    EXPECT_NE(text.indexOf(QStringLiteral("not visible")), -1) << text.toStdString();
  }
}

TEST(SemanticTest, RejectsCrossFilePodCycles) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("a.idl"), "POD A(B b)\n");
  Write(tmp.filePath("b.idl"), "POD B(A a)\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"a.idl\";\n"
        "IMPORT \"b.idl\";\n"
        "class C { }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics diagnostics;
  EXPECT_FALSE(Analyze(loaded, QStringLiteral("demo"), diagnostics).has_value());
  const QString text = diagnostics.Format();
  EXPECT_NE(text.indexOf(QStringLiteral("'A'")), -1) << text.toStdString();
  EXPECT_NE(text.indexOf(QStringLiteral("'B'")), -1) << text.toStdString();
  EXPECT_NE(text.indexOf(QStringLiteral("not visible")), -1) << text.toStdString();
}

TEST(SemanticTest, RejectsDuplicateOutputStems) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  Write(tmp.filePath("one/x.idl"), "POD One(int x)\n");
  Write(tmp.filePath("two/x.idl"), "POD Two(int y)\n");
  Write(tmp.filePath("root.idl"),
        "IMPORT \"one/x.idl\";\n"
        "IMPORT \"two/x.idl\";\n"
        "class C { }\n");
  const auto loaded = Load(tmp.filePath("root.idl"));
  Diagnostics diagnostics;
  EXPECT_FALSE(Analyze(loaded, QStringLiteral("demo"), diagnostics).has_value());
  const QString text = diagnostics.Format();
  EXPECT_NE(text.indexOf(QStringLiteral("duplicate output stem")), -1) << text.toStdString();
  EXPECT_NE(text.indexOf(QStringLiteral("'x'")), -1) << text.toStdString();
}

TEST(SemanticTest, RejectsQtMacroNames) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QStringList names = {
      QStringLiteral("signals"), QStringLiteral("slots"),  QStringLiteral("emit"),
      QStringLiteral("Q_SIGNALS"), QStringLiteral("Q_SLOTS"), QStringLiteral("Q_EMIT"),
      QStringLiteral("Q_OBJECT"),  QStringLiteral("Q_PROPERTY"),
  };
  for (int index = 0; index < names.size(); ++index) {
    const QString& name = names.at(index);
    const auto result =
        AnalyzeText(tmp.path(), QStringLiteral("qt_member_%1.idl").arg(index),
                    QStringLiteral("class C { PROP(int %1) }\n").arg(name).toUtf8());
    EXPECT_FALSE(result.model.has_value()) << name.toStdString();
    EXPECT_NE(result.diagnostics.indexOf(name), -1) << name.toStdString();
  }
  {
    const auto result =
        AnalyzeText(tmp.path(), "qt_pod_field.idl", "POD P(int emit)\nclass C { }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("emit")), -1);
  }
  {
    const auto result = AnalyzeText(
        tmp.path(), "qt_param.idl", "class C { SLOT(void run(int signals)) }\n");
    EXPECT_FALSE(result.model.has_value());
    EXPECT_NE(result.diagnostics.indexOf(QStringLiteral("signals")), -1);
  }
}
