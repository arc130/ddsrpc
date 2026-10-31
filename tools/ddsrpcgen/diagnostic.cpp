#include "diagnostic.h"

#include <QStringList>

namespace ddsrpcgen {

void Diagnostics::Error(const QString& file, const SourceLocation& loc, const QString& message) {
  items_.push_back(Diagnostic{file, loc, message});
}
bool Diagnostics::HasErrors() const { return !items_.isEmpty(); }
const QVector<Diagnostic>& Diagnostics::All() const { return items_; }

QString Diagnostics::Format() const {
  QStringList lines;
  for (const auto& item : items_) {
    lines << QStringLiteral("%1:%2:%3: error: %4")
                 .arg(item.file)
                 .arg(item.loc.line)
                 .arg(item.loc.column)
                 .arg(item.message);
  }
  return lines.join(QLatin1Char('\n'));
}

}  // namespace ddsrpcgen
