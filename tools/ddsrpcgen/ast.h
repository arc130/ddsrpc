#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

#include "diagnostic.h"

namespace ddsrpcgen {

struct TypeRef {
  QString spelling;
  QString base;
  QVector<TypeRef> args;
  bool is_const = false;
  bool is_reference = false;
};

struct FieldDecl {
  TypeRef type;
  QString name;
  SourceLocation loc;
};

struct PodDecl {
  QString name;
  QVector<FieldDecl> fields;
  SourceLocation loc;
};

struct EnumValue {
  QString name;
  std::int32_t value = 0;
};

struct EnumDecl {
  QString name;
  QVector<EnumValue> values;
  SourceLocation loc;
};

struct ParamDecl {
  TypeRef type;
  QString name;
  bool named = false;
  SourceLocation loc;
};

enum class PropModifier {
  kReadPush,
  kReadWrite,
  kReadOnly,
  kConstant,
  kSourceOnlySetter,
};

struct PropDecl {
  TypeRef type;
  QString name;
  QString default_value;
  bool has_default = false;
  PropModifier modifier = PropModifier::kReadPush;
  bool persisted = false;
  SourceLocation loc;
};

struct SignalDecl {
  QString name;
  QVector<ParamDecl> params;
  SourceLocation loc;
};

struct SlotDecl {
  TypeRef return_type;
  QString name;
  QVector<ParamDecl> params;
  SourceLocation loc;
};

struct ClassDecl {
  QString name;
  QVector<PropDecl> properties;
  QVector<SignalDecl> signals;
  QVector<SlotDecl> slots;
  QVector<EnumDecl> enums;
  SourceLocation loc;
};

struct ImportDecl {
  QString path;
  SourceLocation loc;
};

struct Directive {
  QString text;
  SourceLocation loc;
};

struct AstFile {
  QString path;
  QVector<ImportDecl> imports;
  QVector<Directive> directives;
  QVector<PodDecl> pods;
  QVector<EnumDecl> enums;
  QVector<ClassDecl> classes;
};

}  // namespace ddsrpcgen
