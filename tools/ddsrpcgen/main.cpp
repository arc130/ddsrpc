#include <QString>
#include <QStringList>

#include "cli.h"

int main(int argc, char** argv) {
  QStringList args;
  args.reserve(argc);
  for (int index = 0; index < argc; ++index) {
    args.append(QString::fromLocal8Bit(argv[index]));
  }
  return ddsrpcgen::RunCli(args);
}
