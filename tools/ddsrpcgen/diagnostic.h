#pragma once
#include <QString>
#include <QVector>

namespace ddsrpcgen {

struct SourceLocation {
  int line = 1;
  int column = 1;
  int offset = 0;
};

struct Diagnostic {
  QString file;
  SourceLocation loc;
  QString message;
};

class Diagnostics {
 public:
  void Error(const QString& file, const SourceLocation& loc, const QString& message);
  [[nodiscard]] bool HasErrors() const;
  [[nodiscard]] const QVector<Diagnostic>& All() const;
  [[nodiscard]] QString Format() const;

 private:
  QVector<Diagnostic> items_;
};

}  // namespace ddsrpcgen
