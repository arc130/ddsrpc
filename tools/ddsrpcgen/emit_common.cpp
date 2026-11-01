#include "emit_common.h"

#include <QFileInfo>
#include <QSet>
#include <QStringList>

namespace ddsrpcgen {
namespace {

[[nodiscard]] bool IsBuiltinSpelling(const QString& type) {
  static const QSet<QString> kWords = {
      QStringLiteral("signed"),   QStringLiteral("unsigned"), QStringLiteral("short"),
      QStringLiteral("long"),     QStringLiteral("int"),      QStringLiteral("char"),
      QStringLiteral("bool"),     QStringLiteral("float"),    QStringLiteral("double"),
      QStringLiteral("void"),     QStringLiteral("wchar_t"),  QStringLiteral("char16_t"),
      QStringLiteral("char32_t"),
  };
  const QStringList words = type.split(QLatin1Char(' '), QString::SkipEmptyParts);
  if (words.isEmpty()) return false;
  for (const QString& word : words) {
    if (!kWords.contains(word)) return false;
  }
  return true;
}

// Splits template arguments at commas that are not nested inside another
// template argument list.
void SplitTemplateArguments(const QString& text, QStringList* arguments) {
  int depth = 0;
  int begin = 0;
  for (int index = 0; index < text.size(); ++index) {
    const QChar c = text.at(index);
    if (c == QLatin1Char('<')) {
      ++depth;
    } else if (c == QLatin1Char('>')) {
      --depth;
    } else if (c == QLatin1Char(',') && depth == 0) {
      arguments->append(text.mid(begin, index - begin).trimmed());
      begin = index + 1;
    }
  }
  arguments->append(text.mid(begin).trimmed());
}

[[nodiscard]] QString QualifiedCppTypeImpl(const QString& raw_type) {
  const QString type = raw_type.trimmed();
  if (type.isEmpty() || type.startsWith(QStringLiteral("::"))) return type;

  const int angle = type.indexOf(QLatin1Char('<'));
  if (angle < 0) {
    return IsBuiltinSpelling(type) ? type : QStringLiteral("::") + type;
  }
  if (!type.endsWith(QLatin1Char('>'))) {
    // Defensive: malformed template spellings should still produce an
    // absolute name rather than silently resolving to a local one.
    return QStringLiteral("::") + type;
  }

  const QString base = type.left(angle).trimmed();
  if (base.isEmpty()) return QStringLiteral("::") + type;
  const QString arguments_text = type.mid(angle + 1, type.size() - angle - 2);
  QStringList arguments;
  SplitTemplateArguments(arguments_text, &arguments);
  QStringList qualified;
  qualified.reserve(arguments.size());
  for (const QString& argument : arguments) {
    qualified.append(QualifiedCppTypeImpl(argument));
  }
  return QStringLiteral("::") + base + QLatin1Char('<') +
         qualified.join(QStringLiteral(", ")) + QLatin1Char('>');
}

}  // namespace

QString StemOf(const FileModel& file) {
  if (!file.stem.isEmpty()) return file.stem;
  return QFileInfo(file.path).completeBaseName();
}

// A raw alphanumeric is always uppercased; an escape always contains a
// lowercase 'x', so "foo-bar" and "foo_bar" cannot share a guard. Kept in one
// place so the types and service/client emitters cannot drift apart.
QString EscapeStem(const QString& stem) {
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
  return sanitized;
}

QString SanitizeStem(const QString& stem) {
  const QString escaped = EscapeStem(stem);
  if (escaped.isEmpty()) return QStringLiteral("EMPTY");
  return escaped;
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

QString QualifiedCppType(const QString& cpp_type) {
  return QualifiedCppTypeImpl(cpp_type);
}

}  // namespace ddsrpcgen
