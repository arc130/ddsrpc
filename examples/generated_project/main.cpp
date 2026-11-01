// Minimal consumer of checked-in ddsrpc generated code.
//
// The service and client classes below come from generated/greeter.*,
// not from this file. See README.md for the generation and build steps.

#include <QCoreApplication>
#include <QObject>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>

#include <boost/fiber/operations.hpp>

#include "detail/asyncdefine.h"
#include "greeter.ddsrpc.client.h"
#include "greeter.ddsrpc.service.h"
#include "task/fiberapplication.h"
#include "task/fibertask.h"
#include "transport/codec/DdsCodec.hpp"
#include "transport/io/dds/DdsConfig.hpp"
#include "transport/io/dds/DdsProviderRegistry.hpp"
#include "transport/io/dds/DdsTransport.hpp"
#include "transport/io/dds/FakeDdsProvider.hpp"
#include "transport/node/DdsNode.hpp"

namespace {

using namespace std::chrono_literals;

bool PumpUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    boost::this_fiber::sleep_for(10ms);
  }
  return predicate();
}

// Generated GreeterService declares greet() and bump() as pure virtual.
// Property setters and publishAnnounced() are concrete helpers generated
// from PROP/SIGNAL declarations in greeter.idl.
class DemoGreeterService final : public demo::GreeterService {
 public:
  using GreeterService::GreeterService;

 protected:
  Coro::Result<QString> greet(QString name) override {
    return QStringLiteral("hello ") + name;
  }

  Coro::Result<void> bump() override {
    const int next = count() + 1;
    auto updated = this->setCount(next);
    if (!updated) return updated.error();
    auto published = this->publishAnnounced(next);
    if (!published) return published.error();
    return {};
  }
};

int RunDemo() {
  constexpr const char* kProvider = "ddsrpc-generated-project";
  auto bus = std::make_shared<transport::FakeDdsProvider::Bus>();
  transport::DdsProviderRegistry::RegisterProvider(
      kProvider, [bus] {
        return std::unique_ptr<transport::IDdsProvider>(
            new transport::FakeDdsProvider(bus));
      });

  transport::DdsConfig config;
  config.provider = kProvider;
  transport::DdsTransport server_transport(config);
  transport::DdsTransport client_transport(config);
  if (!server_transport.Start() || !client_transport.Start()) {
    std::cerr << "generated-project: transport start failed\n";
    return 1;
  }

  int rc = 1;
  {
    transport::DdsNode server_node(server_transport,
                                   std::make_unique<transport::DdsCodec>());
    transport::DdsNode client_node(client_transport,
                                   std::make_unique<transport::DdsCodec>());
    DemoGreeterService service(server_node);
    demo::GreeterClient client(client_node, client_transport);

    std::atomic_int last_count{-1};
    std::atomic_int last_announced{-1};
    QObject::connect(&client, &demo::GreeterClient::countChanged,
                     [&last_count](int value) { last_count.store(value); });
    QObject::connect(&client, &demo::GreeterClient::announced,
                     [&last_announced](int value) { last_announced.store(value); });

    do {
      if (!service.prepare() || !client.prepare() ||
          !server_node.Start() || !client_node.Start() || !client.start()) {
        std::cerr << "generated-project: endpoint start failed\n";
        break;
      }

      auto label = service.setLabel(QStringLiteral("demo"));
      if (!label) {
        std::cerr << "generated-project: setLabel failed: "
                  << label.error().message() << "\n";
        break;
      }
      const bool ready_before = client.ready();

      // Start the service only after the client, proving that client.start()
      // does not require a live peer and that ready flips asynchronously.
      if (!service.start()) {
        std::cerr << "generated-project: service start failed\n";
        break;
      }
      if (!PumpUntil([&] { return client.ready(); }, 5000ms)) {
        std::cerr << "generated-project: ready timeout\n";
        break;
      }

      const auto greeting = client.greet(QStringLiteral("generated"));
      if (!greeting) {
        std::cerr << "generated-project: greet failed: "
                  << greeting.error().message() << "\n";
        break;
      }
      const auto bumped = client.bump();
      if (!bumped) {
        std::cerr << "generated-project: bump failed: "
                  << bumped.error().message() << "\n";
        break;
      }
      if (!PumpUntil(
              [&] {
                return client.count() == 1 && last_count.load() == 1 &&
                       last_announced.load() == 1;
              },
              5000ms)) {
        std::cerr << "generated-project: property/signal timeout\n";
        break;
      }

      std::cout << "generated-project ready-before-service="
                << (ready_before ? 1 : 0) << "\n"
                << "generated-project ready-after-service="
                << (client.ready() ? 1 : 0) << "\n"
                << "generated-project greet=" << greeting.value().toStdString()
                << "\n"
                << "generated-project count=" << client.count() << "\n"
                << "generated-project announced=" << last_announced.load()
                << "\n"
                << "generated-project label=" << client.label().toStdString()
                << "\n";
      rc = 0;
    } while (false);

    (void)client.stop();
    (void)service.stop();
    client_node.Close();
    client_node.WaitClosed();
    server_node.Close();
    server_node.WaitClosed();
  }

  client_transport.Close();
  client_transport.WaitClosed();
  server_transport.Close();
  server_transport.WaitClosed();
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  Coro::installFiberApplication();
  int rc = 1;
  auto task = Coro::makeTask([&rc] {
    rc = RunDemo();
    Coro::quit();
  });
  Coro::exec();
  (void)task;
  return rc;
}
