#include <QCoreApplication>
#include <QObject>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <QUuid>
#include <boost/fiber/operations.hpp>

#include "detail/asyncdefine.h"
#include "hello.ddsrpc.client.h"
#include "hello.ddsrpc.service.h"
#include "task/fiberapplication.h"
#include "task/fibertask.h"
#include "transport/codec/DdsCodec.hpp"
#include "transport/io/dds/DdsConfig.hpp"
#include "transport/io/dds/DdsProviderRegistry.hpp"
#include "transport/io/dds/DdsTransport.hpp"
#include "transport/io/dds/FakeDdsProvider.hpp"
#include "transport/node/DdsNode.hpp"

namespace {

bool PumpUntil(const std::function<bool()>& predicate, int budget_ms = 5000) {
  for (int waited = 0; waited < budget_ms; waited += 5) {
    if (predicate()) return true;
    boost::this_fiber::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

class GreeterImpl final : public demo::GreeterService {
 public:
  using GreeterService::GreeterService;

 protected:
  Coro::Result<QString> greet(QString name) override {
    auto published = this->publishAnnounced(1);
    if (!published) return published.error();
    return QStringLiteral("hello ") + name;
  }
  Coro::Result<void> reset() override { return {}; }
};

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  Coro::installFiberApplication();
  int exit_code = 1;

  auto task = Coro::makeTask([&] {
    const QString provider = QStringLiteral("hello-%1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto bus = std::make_shared<transport::FakeDdsProvider::Bus>();
    transport::DdsProviderRegistry::RegisterProvider(
        provider.toStdString(), [bus] {
          return std::unique_ptr<transport::IDdsProvider>(
              new transport::FakeDdsProvider(bus));
        });

    transport::DdsConfig config;
    config.provider = provider.toStdString();
    transport::DdsTransport server_transport(config);
    transport::DdsTransport client_transport(config);
    if (!server_transport.Start() || !client_transport.Start()) {
      std::cerr << "transport start failed\n";
      Coro::quit();
      return;
    }

    transport::DdsNode server_node(server_transport,
                                   std::make_unique<transport::DdsCodec>());
    transport::DdsNode client_node(client_transport,
                                   std::make_unique<transport::DdsCodec>());
    GreeterImpl service(server_node);
    demo::GreeterClient client(client_node, client_transport);

    if (!service.prepare() || !client.prepare() ||
        !server_node.Start() || !client_node.Start() || !client.start()) {
      std::cerr << "endpoint start failed\n";
      Coro::quit();
      return;
    }

    Coro::msleep(100);
    const bool ready_before = client.ready();
    if (!service.start()) {
      std::cerr << "service start failed\n";
      Coro::quit();
      return;
    }
    const bool ready_after = PumpUntil([&] { return client.ready(); });

    std::atomic_int count{-1};
    std::atomic_int announced{0};
    QObject::connect(&client, &demo::GreeterClient::countChanged,
                     [&count](int value) { count.store(value); });
    QObject::connect(&client, &demo::GreeterClient::announced,
                     [&announced](int value) { announced.store(value); });

    const auto greeting = client.greet(QStringLiteral("demo"));
    if (!greeting || !service.setCount(1) ||
        !PumpUntil([&] { return count.load() == 1 && announced.load() == 1; })) {
      std::cerr << "rpc/property/signal flow failed\n";
      Coro::quit();
      return;
    }

    std::cout << "ready-before-service=" << (ready_before ? 1 : 0) << "\n";
    std::cout << "ready-after-service=" << (ready_after ? 1 : 0) << "\n";
    std::cout << "greet=" << greeting.value().toStdString() << "\n";
    std::cout << "count=" << count.load() << "\n";
    std::cout << "announced=" << announced.load() << "\n";
    exit_code = 0;

    (void)client.stop();
    (void)service.stop();
    client_node.Close();
    client_node.WaitClosed();
    server_node.Close();
    server_node.WaitClosed();
    (void)client_transport.Close();
    client_transport.WaitClosed();
    (void)server_transport.Close();
    server_transport.WaitClosed();
    Coro::quit();
  });

  Coro::exec();
  (void)task;
  return exit_code;
}
