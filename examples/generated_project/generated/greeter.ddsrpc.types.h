#ifndef DDSRPC_GEN_TYPES_GREETER_H
#define DDSRPC_GEN_TYPES_GREETER_H

#include <QtCore/QDataStream>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <mutex>

namespace demo {

  #ifndef DDSRPC_GEN_TYPES_REGISTRATION_4demo
  #define DDSRPC_GEN_TYPES_REGISTRATION_4demo
  namespace DdsrpcGeneratedTypesDetail_4demo {
    using RegistrationHook = void (*)();

    inline QVector<RegistrationHook>& RegistrationHooks() {
      static QVector<RegistrationHook> hooks;
      return hooks;
    }
  }  // namespace DdsrpcGeneratedTypesDetail_4demo

  inline void registerTypes() {
    static std::once_flag once;
    std::call_once(once, [] {
      for (const DdsrpcGeneratedTypesDetail_4demo::RegistrationHook hook :
           DdsrpcGeneratedTypesDetail_4demo::RegistrationHooks()) {
        hook();
      }
    });
  }
  #endif  // DDSRPC_GEN_TYPES_REGISTRATION_4demo

}

#endif  // DDSRPC_GEN_TYPES_GREETER_H
