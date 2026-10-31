#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

#include "ast.h"

namespace ddsrpcgen {

struct ParamModel {
  QString name;
  QString cpp_type;
  bool named = false;
};

struct EnumModel {
  QString idl_name;
  QString cpp_name;
  QString class_owner;
  QVector<EnumValue> values;
};

// Each POD field carries the fully resolved by-value C++ type that emitters
// print directly. Generated types are namespace-qualified and class enums use
// their canonical <Class>_<Enum> spelling; external C++ spellings are kept.
struct PodFieldModel {
  QString name;
  QString cpp_type;
};

struct PodModel {
  QString name;
  QVector<PodFieldModel> fields;
};

struct PropertyModel {
  QString name;
  QString cpp_type;
  QString default_value;
  bool has_default = false;
  PropModifier modifier = PropModifier::kReadPush;
  int property_id = 0;
  int remote_setter_op = 0;
};

struct SignalModel {
  QString name;
  QVector<ParamModel> params;
  int event_id = 0;
};

struct SlotModel {
  QString name;
  QString return_cpp_type;
  QVector<ParamModel> params;
  int op = 0;
};

struct ClassModel {
  QString idl_name;
  QVector<PropertyModel> properties;
  QVector<SignalModel> signals;
  QVector<SlotModel> slots;
  QVector<EnumModel> class_enums;
  quint64 schema_hash = 0;
};

struct FileModel {
  // Canonical source path and its complete base name (for example "wire" for
  // "wire.idl"). Emitters derive output names from stem.
  QString path;
  QString stem;
  QVector<Directive> directives;
  // Canonical paths of the directly imported files, in IMPORT declaration
  // order. The schema hash derives file stems from these paths; generated
  // includes derive their stem the same way.
  QVector<QString> imports;
  // PODs in dependency order; ENUMs and classes in source order.
  QVector<PodModel> pods;
  QVector<EnumModel> enums;
  QVector<ClassModel> classes;
  quint64 schema_hash = 0;
  // Set by Analyze; included in the schema hash so that moving an IDL to a
  // different namespace changes its wire identity. Kept last to preserve
  // brace-initialization compatibility with the model fields above.
  QString name_space;
};

struct Model {
  QString name_space;
  QVector<FileModel> files;
};

}  // namespace ddsrpcgen
