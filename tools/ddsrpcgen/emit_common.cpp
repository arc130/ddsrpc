#include "emit_common.h"

#include <QFileInfo>

namespace ddsrpcgen {
namespace {

// Copied from the types emitter: guard escaping must stay identical so a
// service header for "foo-bar" cannot collide with one for "foo_bar".
[[nodiscard]] QString EscapeStem(const QString& stem) {
  QString sanitized;
  sanitized.reserve(stem.size());
  for (const QChar c : stem) {
    if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
        (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
        (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
      sanitized.append(c.toUpper());
    } else if (c == QLatin1Char('_')) {
      sanitized.append(QLatin1Char('_'));
    } else {
      sanitized += QStringLiteral("_x") +
                   QString::number(static_cast<uint>(c.unicode()), 16).toUpper() +
                   QLatin1Char('_');
    }
  }
  if (sanitized.isEmpty()) sanitized = QStringLiteral("EMPTY");
  return sanitized;
}

}  // namespace

QString StemOf(const FileModel& file) {
  if (!file.stem.isEmpty()) return file.stem;
  return QFileInfo(file.path).completeBaseName();
}

QString SanitizeStem(const QString& stem) {
  if (stem.isEmpty()) return QStringLiteral("EMPTY");
  return EscapeStem(stem);
}

QString MakeIncludeGuard(const QString& kind, const QString& stem) {
  return QStringLiteral("DDSRPC_GEN_") + kind + QLatin1Char('_') +
         SanitizeStem(stem) + QStringLiteral("_H");
}

QStringList NamespaceParts(const QString& name_space) {
  QStringList parts;
  const QStringList raw =
      name_space.split(QStringLiteral("::"), QString::SkipEmptyParts);
  for (const QString& part : raw) {
    const QString trimmed = part.trimmed();
    if (!trimmed.isEmpty()) parts.append(trimmed);
  }
  return parts;
}

QString SchemaHashLiteral(quint64 hash) {
  return QStringLiteral("0x") +
         QString::number(hash, 16).rightJustified(16, QLatin1Char('0')) +
         QStringLiteral("ULL");
}

}  // namespace ddsrpcgen
