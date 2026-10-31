#include "code_writer.h"

namespace ddsrpcgen {
namespace {

constexpr int kIndentWidth = 2;

}  // namespace

void CodeWriter::Line(const QString& text) {
  if (!text.isEmpty()) text_ += QString(indent_ * kIndentWidth, QLatin1Char(' '));
  text_ += text;
  text_ += QLatin1Char('\n');
}

void CodeWriter::Indent() { ++indent_; }

void CodeWriter::Dedent() {
  if (indent_ > 0) --indent_;
}

void CodeWriter::OpenNamespace(const QString& ns) {
  const QStringList parts = ns.split(QStringLiteral("::"), QString::SkipEmptyParts);
  for (const QString& part : parts) {
    const QString name = part.trimmed();
    if (name.isEmpty()) continue;
    open_namespaces_.append(name);
    Line(QStringLiteral("namespace ") + name + QStringLiteral(" {"));
    Indent();
  }
}

void CodeWriter::CloseNamespace() {
  while (!open_namespaces_.isEmpty()) {
    open_namespaces_.removeLast();
    Dedent();
    Line(QStringLiteral("}"));
  }
}

QString CodeWriter::Text() const { return text_; }

}  // namespace ddsrpcgen
