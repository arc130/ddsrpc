#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include "cli.h"
#include "emit_types.h"
#include "model.h"

namespace {

void WriteFile(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(data);
}

QByteArray ReadFile(const QString& path) {
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

}  // namespace

TEST(CodegenTest, EmitsEnumPodAndStreamOperators) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("common");
  ddsrpcgen::PodModel pod;
  pod.name = QStringLiteral("Point");
  pod.fields = {
      ddsrpcgen::PodFieldModel{QStringLiteral("x"), QStringLiteral("int")},
      ddsrpcgen::PodFieldModel{QStringLiteral("y"), QStringLiteral("int")},
  };
  file.pods = {pod};
  ddsrpcgen::EnumModel mode;
  mode.idl_name = QStringLiteral("Mode");
  mode.cpp_name = QStringLiteral("Mode");
  mode.values = {{QStringLiteral("Idle"), 0}, {QStringLiteral("Running"), 1}};
  file.enums = {mode};

  const QString text = ddsrpcgen::EmitTypesHeader(file, QStringLiteral("demo"));
  EXPECT_NE(text.indexOf(QStringLiteral("enum class Mode : qint32")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("struct Point")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("operator<<(QDataStream")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("registerTypes")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("namespace demo")), -1);
  EXPECT_EQ(text.count(QStringLiteral("namespace demo")), 1);
}

TEST(CodegenTest, CliGeneratesTypesFileAtomically) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString idl = tmp.filePath("wire.idl");
  QFile in(idl);
  ASSERT_TRUE(in.open(QIODevice::WriteOnly | QIODevice::Truncate));
  in.write("POD Pair(int a, int b)\n");
  in.close();
  const QString out = tmp.filePath("generated");
  QStringList args{
      QStringLiteral("ddsrpcgen"),
      QStringLiteral("--role"), QStringLiteral("types"),
      QStringLiteral("--out-dir"), out,
      QStringLiteral("--namespace"), QStringLiteral("demo"),
      idl,
  };
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/wire.ddsrpc.types.h")));
  QFile generated(out + QStringLiteral("/wire.ddsrpc.types.h"));
  ASSERT_TRUE(generated.open(QIODevice::ReadOnly));
  const QString text = QString::fromUtf8(generated.readAll());
  EXPECT_NE(text.indexOf(QStringLiteral("namespace demo")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("struct Pair")), -1);
}

TEST(CodegenTest, CliMergesSharedImportsAcrossRoots) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  QFile common(tmp.filePath("common.idl"));
  ASSERT_TRUE(common.open(QIODevice::WriteOnly | QIODevice::Truncate));
  common.write("POD Point(int x, int y)\n");
  common.close();
  QFile a(tmp.filePath("a.idl"));
  ASSERT_TRUE(a.open(QIODevice::WriteOnly | QIODevice::Truncate));
  a.write("IMPORT \"common.idl\";\nclass A { }\n");
  a.close();
  QFile b(tmp.filePath("b.idl"));
  ASSERT_TRUE(b.open(QIODevice::WriteOnly | QIODevice::Truncate));
  b.write("IMPORT \"common.idl\";\nclass B { }\n");
  b.close();
  const QString out = tmp.filePath("generated");
  QStringList args{
      QStringLiteral("ddsrpcgen"), QStringLiteral("--role"), QStringLiteral("types"),
      QStringLiteral("--out-dir"), out, QStringLiteral("--namespace"),
      QStringLiteral("demo"), tmp.filePath("a.idl"), tmp.filePath("b.idl"),
  };
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/a.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/b.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/common.ddsrpc.types.h")));
}

TEST(CodegenTest, EmitsNestedNamespaceClassEnumsAndImports) {
  ddsrpcgen::FileModel file;
  file.stem = QStringLiteral("wire");
  file.imports = {QStringLiteral("/tmp/idl/common.idl")};
  file.directives = {ddsrpcgen::Directive{QStringLiteral("#include <QString>"),
                                          ddsrpcgen::SourceLocation{}}};
  ddsrpcgen::EnumModel mode;
  mode.idl_name = QStringLiteral("Mode");
  mode.cpp_name = QStringLiteral("Mode");
  mode.values = {{QStringLiteral("Idle"), 0}};
  ddsrpcgen::EnumModel state;
  state.idl_name = QStringLiteral("State");
  state.cpp_name = QStringLiteral("Foo_State");
  state.class_owner = QStringLiteral("Foo");
  state.values = {{QStringLiteral("Off"), 0}};
  ddsrpcgen::ClassModel cls;
  cls.class_enums = {state};
  file.classes = {cls};
  file.enums = {mode};
  ddsrpcgen::PodModel point;
  point.name = QStringLiteral("Point");
  point.fields = {
      ddsrpcgen::PodFieldModel{QStringLiteral("mode"), QStringLiteral("a::b::Mode")},
  };
  file.pods = {point};

  const QString text = ddsrpcgen::EmitTypesHeader(file, QStringLiteral("a::b"));
  EXPECT_TRUE(text.startsWith(QStringLiteral("#ifndef DDSRPC_GEN_TYPES_WIRE_H")));
  EXPECT_NE(text.indexOf(QStringLiteral("#include \"common.ddsrpc.types.h\"")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("enum class Mode : qint32")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("enum class Foo_State : qint32")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("struct Point")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("a::b::Mode mode{};")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("Q_DECLARE_METATYPE(a::b::Mode)")), -1);
  EXPECT_NE(text.indexOf(QStringLiteral("Q_DECLARE_METATYPE(a::b::Foo_State)")), -1);
  EXPECT_EQ(text.count(QStringLiteral("namespace a")), 1);
  EXPECT_EQ(text.count(QStringLiteral("namespace b")), 1);
  EXPECT_NE(text.indexOf(QStringLiteral("static std::once_flag")), -1);
  EXPECT_EQ(text.indexOf(QStringLiteral("operator<(")), -1);
  EXPECT_LT(text.indexOf(QStringLiteral("#include <QString>")),
            text.indexOf(QStringLiteral("#include <QtCore/QDataStream>")));
  EXPECT_TRUE(text.endsWith(QLatin1Char('\n')));
}

TEST(CodegenTest, CliRejectsUsageErrorsAndUnimplementedRoles) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString idl = tmp.filePath("wire.idl");
  WriteFile(idl, "POD Pair(int a)\n");
  const QString out = tmp.filePath("generated");
  using ddsrpcgen::RunCli;
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen")}), 2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--bogus"),
                    QStringLiteral("--out-dir"), out, idl}),
            2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                    QStringLiteral("types"), idl}),
            2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                    QStringLiteral("bogus"), QStringLiteral("--out-dir"), out, idl}),
            2);
  EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                    QStringLiteral("types"), QStringLiteral("--out-dir"), out}),
            2);
  for (const QString& role : {QStringLiteral("service"), QStringLiteral("client"),
                              QStringLiteral("all")}) {
    EXPECT_EQ(RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"), role,
                      QStringLiteral("--out-dir"), out, idl}),
              1);
  }
  EXPECT_FALSE(QFile::exists(out));
}

TEST(CodegenTest, CliWritesDepfileAndReplacesOutputsDeterministically) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString idl = tmp.filePath("wire.idl");
  WriteFile(idl, "POD Pair(int a)\n");
  const QString out = tmp.filePath("generated");
  const QString header = out + QStringLiteral("/wire.ddsrpc.types.h");
  QStringList args{
      QStringLiteral("ddsrpcgen"), QStringLiteral("--role"), QStringLiteral("types"),
      QStringLiteral("--out-dir"), out, QStringLiteral("--namespace"), QStringLiteral("demo"),
      idl,
  };
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  const QByteArray first = ReadFile(header);
  EXPECT_NE(first.indexOf("struct Pair"), -1);
  EXPECT_TRUE(first.endsWith('\n'));

  const QString depfile = out + QStringLiteral("/ddsrpc.d");
  ASSERT_TRUE(QFile::exists(depfile));
  const QString dependencies = QString::fromUtf8(ReadFile(depfile));
  EXPECT_NE(dependencies.indexOf(QFileInfo(header).absoluteFilePath()), -1);
  EXPECT_NE(dependencies.indexOf(QFileInfo(idl).canonicalFilePath()), -1);
  EXPECT_NE(dependencies.indexOf(QStringLiteral(": ")), -1);

  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  EXPECT_EQ(ReadFile(header), first);

  WriteFile(idl, "POD Pair(int a, int b)\n");
  ASSERT_EQ(ddsrpcgen::RunCli(args), 0);
  const QByteArray second = ReadFile(header);
  EXPECT_NE(second.indexOf("int b"), -1);
  EXPECT_TRUE(QDir(out).entryList({QStringLiteral("*.tmp")}, QDir::Files).isEmpty());

  const QString custom_depfile = tmp.filePath("deps/custom.d");
  QStringList custom_args = args;
  custom_args.insert(custom_args.size() - 1, QStringLiteral("--depfile"));
  custom_args.insert(custom_args.size() - 1, custom_depfile);
  ASSERT_EQ(ddsrpcgen::RunCli(custom_args), 0);
  EXPECT_TRUE(QFile::exists(custom_depfile));
}

TEST(CodegenTest, EmitsDistinctGuardsForStemsThatSanitizeTheSame) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  WriteFile(tmp.filePath("foo-bar.idl"), "POD Alpha(int a)\n");
  WriteFile(tmp.filePath("foo_bar.idl"), "POD Beta(int b)\n");
  const QString out = tmp.filePath("generated");
  ASSERT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("types"), QStringLiteral("--out-dir"), out,
                               QStringLiteral("--namespace"), QStringLiteral("demo"),
                               tmp.filePath("foo-bar.idl"), tmp.filePath("foo_bar.idl")}),
            0);
  const QString dashed =
      QString::fromUtf8(ReadFile(out + QStringLiteral("/foo-bar.ddsrpc.types.h")));
  const QString underscored =
      QString::fromUtf8(ReadFile(out + QStringLiteral("/foo_bar.ddsrpc.types.h")));
  EXPECT_NE(dashed.indexOf(QStringLiteral("#ifndef DDSRPC_GEN_TYPES_FOO_x2D_BAR_H")), -1);
  EXPECT_NE(underscored.indexOf(QStringLiteral("#ifndef DDSRPC_GEN_TYPES_FOO_BAR_H")), -1);
  EXPECT_NE(dashed.indexOf(QStringLiteral("struct Alpha")), -1);
  EXPECT_NE(underscored.indexOf(QStringLiteral("struct Beta")), -1);
}

TEST(CodegenTest, CliHonorsIncludeDirsAndAnalyzeRejectsDuplicateStems) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  WriteFile(tmp.filePath("include/dep.idl"), "POD Dep(int value)\n");
  WriteFile(tmp.filePath("root.idl"), "IMPORT \"dep.idl\";\nPOD Root(Dep dep)\n");
  const QString out = tmp.filePath("generated");
  ASSERT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("types"), QStringLiteral("--out-dir"), out,
                               QStringLiteral("-I"), tmp.filePath("include"),
                               tmp.filePath("root.idl")}),
            0);
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/root.ddsrpc.types.h")));
  EXPECT_TRUE(QFile::exists(out + QStringLiteral("/dep.ddsrpc.types.h")));

  WriteFile(tmp.filePath("first/x.idl"), "POD X(int a)\n");
  WriteFile(tmp.filePath("second/x.idl"), "POD X(int b)\n");
  const QString duplicate_out = tmp.filePath("duplicate");
  EXPECT_EQ(ddsrpcgen::RunCli({QStringLiteral("ddsrpcgen"), QStringLiteral("--role"),
                               QStringLiteral("types"), QStringLiteral("--out-dir"),
                               duplicate_out, tmp.filePath("first/x.idl"),
                               tmp.filePath("second/x.idl")}),
            1);
  EXPECT_FALSE(QFile::exists(duplicate_out));
}
