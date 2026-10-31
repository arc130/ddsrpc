#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include "import_resolver.h"

using namespace ddsrpcgen;

namespace {
void WriteFile(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(data);
}
}  // namespace

TEST(ImportResolverTest, ResolvesRelativeAndIncludeDirImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString root = tmp.filePath("root.idl");
  const QString common = tmp.filePath("common.idl");
  const QString extra = tmp.filePath("extra/shared.idl");
  WriteFile(common, "POD Point(int x, int y)\n");
  WriteFile(extra, "POD Size(int w, int h)\n");
  WriteFile(root, "IMPORT \"common.idl\";\nIMPORT \"shared.idl\";\nclass C { }\n");

  Diagnostics diagnostics;
  ImportResolver resolver({tmp.filePath("extra")}, diagnostics);
  const auto program = resolver.Load(root);
  ASSERT_TRUE(program.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(program->files.size(), 3);
  EXPECT_EQ(program->files[0].ast.pods.size(), 1);
  EXPECT_EQ(program->files[0].ast.pods[0].name, QStringLiteral("Point"));
  EXPECT_EQ(program->files[1].ast.pods[0].name, QStringLiteral("Size"));
  EXPECT_EQ(program->files[2].ast.classes.size(), 1);
  EXPECT_TRUE(program->files[0].resolved_imports.isEmpty());
  EXPECT_TRUE(program->files[1].resolved_imports.isEmpty());
  ASSERT_EQ(program->files[2].resolved_imports.size(), 2);
  EXPECT_EQ(program->files[2].resolved_imports[0], QFileInfo(common).canonicalFilePath());
  EXPECT_EQ(program->files[2].resolved_imports[1], QFileInfo(extra).canonicalFilePath());
}

TEST(ImportResolverTest, RejectsLexerOnlyDiagnostics) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString path = tmp.filePath("bad.idl");
  WriteFile(path, "POD A(int x)\n@\n");
  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  EXPECT_FALSE(resolver.Load(path).has_value());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("unexpected character")), -1);
}

TEST(ImportResolverTest, RejectsMissingAndCyclicImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString a = tmp.filePath("a.idl");
  const QString b = tmp.filePath("b.idl");
  WriteFile(a, "IMPORT \"b.idl\";\n");
  WriteFile(b, "IMPORT \"a.idl\";\n");

  {
    Diagnostics diagnostics;
    ImportResolver resolver({}, diagnostics);
    EXPECT_FALSE(resolver.Load(tmp.filePath("missing.idl")).has_value());
    EXPECT_TRUE(diagnostics.HasErrors());
  }
  {
    Diagnostics diagnostics;
    ImportResolver resolver({}, diagnostics);
    EXPECT_FALSE(resolver.Load(a).has_value());
    EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("cycle")), -1);
  }
}

TEST(ImportResolverTest, RequiresIdlSuffixForRootAndImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString txt_root = tmp.filePath("root.txt");
  WriteFile(txt_root, "POD X(int x)\n");

  {
    Diagnostics diagnostics;
    ImportResolver resolver({}, diagnostics);
    EXPECT_FALSE(resolver.Load(txt_root).has_value());
    EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral(".idl")), -1);
  }

  const QString dep = tmp.filePath("dep.txt");
  const QString root = tmp.filePath("root.idl");
  WriteFile(dep, "POD Y(int y)\n");
  WriteFile(root, "IMPORT \"dep.txt\";\n");

  {
    Diagnostics diagnostics;
    ImportResolver resolver({}, diagnostics);
    EXPECT_FALSE(resolver.Load(root).has_value());
    EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral(".idl")), -1);
  }
}

TEST(ImportResolverTest, ResolvesAbsoluteImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString dep = tmp.filePath("dep.idl");
  const QString root = tmp.filePath("root.idl");
  WriteFile(dep, "POD D(int x)\n");
  WriteFile(root, QStringLiteral("IMPORT \"%1\";\n").arg(dep).toUtf8());

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  const auto program = resolver.Load(root);
  ASSERT_TRUE(program.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(program->files.size(), 2);
  EXPECT_EQ(program->files[0].canonical_path, QFileInfo(dep).canonicalFilePath());
  EXPECT_EQ(program->files[1].canonical_path, QFileInfo(root).canonicalFilePath());
  EXPECT_EQ(program->root_path, QFileInfo(root).canonicalFilePath());
  EXPECT_EQ(program->files[0].ast.pods[0].name, QStringLiteral("D"));
}

TEST(ImportResolverTest, PrefersImporterDirectoryThenIncludeDirOrder) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString inc1 = tmp.filePath("inc1");
  const QString inc2 = tmp.filePath("inc2");
  WriteFile(tmp.filePath("dep.idl"), "POD Relative(int x)\n");
  WriteFile(QDir(inc1).filePath("dep.idl"), "POD IncludeFirst(int x)\n");
  WriteFile(QDir(inc1).filePath("shared.idl"), "POD First(int x)\n");
  WriteFile(QDir(inc2).filePath("shared.idl"), "POD Second(int x)\n");
  const QString root = tmp.filePath("root.idl");
  WriteFile(root, "IMPORT \"dep.idl\";\nIMPORT \"shared.idl\";\n");

  Diagnostics diagnostics;
  ImportResolver resolver({inc1, inc2}, diagnostics);
  const auto program = resolver.Load(root);
  ASSERT_TRUE(program.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(program->files.size(), 3);
  EXPECT_EQ(program->files[0].ast.pods[0].name, QStringLiteral("Relative"));
  EXPECT_EQ(program->files[1].ast.pods[0].name, QStringLiteral("First"));
}

TEST(ImportResolverTest, ReportsCyclePath) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString a = tmp.filePath("a.idl");
  const QString b = tmp.filePath("b.idl");
  WriteFile(a, "IMPORT \"b.idl\";\n");
  WriteFile(b, "IMPORT \"a.idl\";\n");

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  EXPECT_FALSE(resolver.Load(a).has_value());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("IMPORT cycle: a.idl -> b.idl -> a.idl")),
            -1);
}

TEST(ImportResolverTest, LoadsEachCanonicalFileOnce) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString common = tmp.filePath("common.idl");
  const QString a = tmp.filePath("a.idl");
  const QString b = tmp.filePath("b.idl");
  const QString root = tmp.filePath("root.idl");
  WriteFile(common, "POD Common(int x)\n");
  WriteFile(a, "IMPORT \"common.idl\";\nPOD A(int x)\n");
  WriteFile(b, "IMPORT \"./common.idl\";\nPOD B(int x)\n");
  WriteFile(root, "IMPORT \"a.idl\";\nIMPORT \"b.idl\";\nPOD Root(int x)\n");

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  const auto program = resolver.Load(root);
  ASSERT_TRUE(program.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(program->files.size(), 4);
  EXPECT_EQ(program->files[0].ast.pods[0].name, QStringLiteral("Common"));
  EXPECT_EQ(program->files[1].ast.pods[0].name, QStringLiteral("A"));
  EXPECT_EQ(program->files[2].ast.pods[0].name, QStringLiteral("B"));
  EXPECT_EQ(program->files[3].ast.pods[0].name, QStringLiteral("Root"));

  QSet<QString> paths;
  for (const auto& file : program->files) paths.insert(file.canonical_path);
  EXPECT_EQ(paths.size(), 4);
  EXPECT_EQ(program->files[0].canonical_path, QFileInfo(common).canonicalFilePath());

  const QString common_path = QFileInfo(common).canonicalFilePath();
  EXPECT_EQ(program->files[1].resolved_imports, QVector<QString>{common_path});
  EXPECT_EQ(program->files[2].resolved_imports, QVector<QString>{common_path});
  EXPECT_EQ(program->files[3].resolved_imports,
            (QVector<QString>{QFileInfo(a).canonicalFilePath(),
                              QFileInfo(b).canonicalFilePath()}));
}

TEST(ImportResolverTest, DeduplicatesRepeatedImportsInResolvedImports) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString common = tmp.filePath("common.idl");
  const QString root = tmp.filePath("root.idl");
  WriteFile(common, "POD Common(int x)\n");
  WriteFile(root,
            "IMPORT \"common.idl\";\n"
            "IMPORT \"./common.idl\";\n"
            "POD Root(Common common)\n");

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  const auto program = resolver.Load(root);
  ASSERT_TRUE(program.has_value()) << diagnostics.Format().toStdString();
  ASSERT_EQ(program->files.size(), 2);
  EXPECT_EQ(program->files[1].resolved_imports,
            QVector<QString>{QFileInfo(common).canonicalFilePath()});
}

TEST(ImportResolverTest, FailsNestedImportWithoutPartialProgram) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString good = tmp.filePath("good.idl");
  const QString root = tmp.filePath("root.idl");
  WriteFile(good, "POD Good(int x)\n");
  WriteFile(root, "IMPORT \"good.idl\";\nIMPORT \"missing.idl\";\n");

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  EXPECT_FALSE(resolver.Load(root).has_value());
  EXPECT_TRUE(diagnostics.HasErrors());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("missing.idl")), -1);
}

TEST(ImportResolverTest, RejectsParserDiagnostics) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString bad = tmp.filePath("bad.idl");
  const QString root = tmp.filePath("root.idl");
  WriteFile(bad, "POD Broken(int x\n");
  WriteFile(root, "IMPORT \"bad.idl\";\n");

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  EXPECT_FALSE(resolver.Load(root).has_value());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("expected")), -1);
}

TEST(ImportResolverTest, RejectsInvalidUtf8Source) {
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString path = tmp.filePath("bad.idl");
  QByteArray source = QStringLiteral("POD A(int x)\n").toUtf8();
  source.append('\xff');
  WriteFile(path, source);

  Diagnostics diagnostics;
  ImportResolver resolver({}, diagnostics);
  EXPECT_FALSE(resolver.Load(path).has_value());
  EXPECT_NE(diagnostics.Format().indexOf(QStringLiteral("UTF-8")), -1);
}
