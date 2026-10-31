#pragma once
#include <QString>
#include <QStringList>

namespace ddsrpcgen {
class CodeWriter {
 public:
  void Line(const QString& text = {});
  void Indent();
  void Dedent();
  // Opens a namespace, expanding "a::b" into nested namespace declarations.
  // Namespace lines are never indented; their contents are.
  void OpenNamespace(const QString& ns);
  // Closes every namespace opened by OpenNamespace, most recent first.
  void CloseNamespace();
  [[nodiscard]] QString Text() const;

 private:
  QString text_;
  int indent_ = 0;
  QStringList open_namespaces_;
};
}  // namespace ddsrpcgen
