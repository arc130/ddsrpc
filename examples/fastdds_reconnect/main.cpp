#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <boost/fiber/operations.hpp>

#include "await/delay.hpp"
#include "detail/asyncdefine.h"
#include "reconnect.ddsrpc.client.h"
#include "reconnect.ddsrpc.service.h"
#include "task/fiberapplication.h"
#include "task/fibertask.h"
#include "transport/codec/DdsCodec.hpp"
#include "transport/io/dds/DdsConfig.hpp"
#include "transport/io/dds/DdsTransport.hpp"
#include "transport/node/DdsNode.hpp"

namespace {

using namespace std::chrono_literals;

bool PumpUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    boost::this_fiber::sleep_for(25ms);
  }
  return predicate();
}

transport::DdsConfig MakeConfig(int domain) {
  transport::DdsConfig config;
  config.domain_id = domain;
  config.provider = "fastdds";
  config.qos.reliability = transport::DdsQos::Reliability::kReliable;
  config.qos.durability = transport::DdsQos::Durability::kVolatile;
  config.qos.history_depth = 10;
  config.qos.max_blocking_time = 200ms;
  config.qos.liveliness_lease = 2000ms;
  return config;
}

class ReconnectGreeterImpl final : public demo::ReconnectGreeterService {
 public:
  using ReconnectGreeterService::ReconnectGreeterService;

 protected:
  Coro::Result<QString> greet(QString name) override {
    return QStringLiteral("reconnect hello ") + name;
  }
};

int RunServer(int domain, const std::string& name, int count) {
  int exit_code = 1;
  auto task = Coro::makeTask([&] {
    transport::DdsTransport transport(MakeConfig(domain));
    if (!transport.Start()) {
      std::cerr << "server transport start failed\n";
      Coro::quit();
      return;
    }
    transport::DdsNode node(transport, std::make_unique<transport::DdsCodec>());
    ReconnectGreeterImpl service(node);
    if (!service.prepare() || !node.Start() || !service.start()) {
      std::cerr << "server endpoint start failed\n";
      Coro::quit();
      return;
    }
    auto set_name = service.setName(QString::fromStdString(name));
    auto set_count = service.setCount(count);
    if (!set_name) {
      std::cerr << "setName failed: " << set_name.error().message() << "\n";
      Coro::quit();
      return;
    }
    if (!set_count) {
      std::cerr << "setCount failed: " << set_count.error().message() << "\n";
      Coro::quit();
      return;
    }
    std::cout << "RECONNECT_SERVER_READY name=" << name
              << " count=" << count << std::endl;
    auto publisher = std::make_shared<Coro::FiberTask<void>>(
        Coro::makeTask([&service, count] {
          for (;;) {
            const auto delayed = Coro::delay_for(300ms);
            if (!delayed) break;
            const auto published = service.publishAnnounced(count);
            if (!published) break;
          }
        }));
    Coro::msleep(60000);
    publisher->cancel();
    (void)publisher->get();
    (void)service.stop();
    node.Close();
    node.WaitClosed();
    transport.Close();
    transport.WaitClosed();
    exit_code = 0;
    Coro::quit();
  });
  Coro::exec();
  (void)task;
  return exit_code;
}

int RunClient(int domain) {
  int exit_code = 1;
  transport::DdsTransport transport(MakeConfig(domain));
  if (!transport.Start()) {
    std::cerr << "client transport start failed\n";
    return 1;
  }
  transport::DdsNode node(transport, std::make_unique<transport::DdsCodec>());
  demo::ReconnectGreeterClient client(node, transport);
  std::atomic_int announced{-1};
  QObject::connect(&client, &demo::ReconnectGreeterClient::announced,
                   [&announced](int value) { announced.store(value); });

  auto task = Coro::makeTask([&] {
    if (!client.prepare() || !node.Start() || !client.start()) {
      std::cerr << "client endpoint start failed\n";
      Coro::quit();
      return;
    }
    if (!PumpUntil([&] { return client.ready(); }, 20000ms)) {
      std::cerr << "first ready timeout: " << client.lastErrorText().toStdString() << "\n";
      exit_code = 2;
    } else {
      const QString name = client.name();
      std::cout << "RECONNECT_CLIENT_READY_1 name="
                << name.toStdString() << " count=" << client.count() << std::endl;
      if (name != QStringLiteral("before") || client.count() != 1) {
        exit_code = 6;
      } else if (!PumpUntil([&] { return !client.ready(); }, 60000ms)) {
        std::cerr << "link-down timeout\n";
        exit_code = 3;
      } else {
        std::cout << "RECONNECT_CLIENT_DOWN" << std::endl;
        if (!PumpUntil([&] { return client.ready(); }, 30000ms)) {
          std::cerr << "second ready timeout: "
                    << client.lastErrorText().toStdString() << "\n";
          exit_code = 4;
        } else {
          const QString refreshed = client.name();
          const int refreshed_count = client.count();
          std::cout << "RECONNECT_CLIENT_READY_2 name="
                    << refreshed.toStdString() << " count=" << refreshed_count
                    << std::endl;
          if (refreshed != QStringLiteral("after") || refreshed_count != 2) {
            exit_code = 5;
          } else {
            const auto greeting = client.greet(QStringLiteral("after"));
            if (!greeting) {
              std::cerr << "post-reconnect rpc failed: "
                        << greeting.error().message() << "\n";
              exit_code = 7;
            } else if (greeting.value() !=
                       QStringLiteral("reconnect hello after")) {
              std::cerr << "post-reconnect rpc bad reply\n";
              exit_code = 7;
            } else if (!PumpUntil(
                           [&] { return announced.load() == 2; }, 5000ms)) {
              std::cerr << "post-reconnect signal timeout, last="
                        << announced.load() << "\n";
              exit_code = 8;
            } else {
              std::cout << "RECONNECT_CLIENT_GREET="
                        << greeting.value().toStdString()
                        << " announced=" << announced.load() << std::endl;
              exit_code = 0;
            }
          }
        }
      }
    }
    (void)client.stop();
    node.Close();
    node.WaitClosed();
    transport.Close();
    transport.WaitClosed();
    Coro::quit();
  });

  Coro::exec();
  (void)task;
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  Coro::installFiberApplication();
  int domain = 43;
  int count = 1;
  std::string role = "client";
  std::string name = "server";
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--role") role = argv[i + 1];
    if (std::string(argv[i]) == "--domain") domain = std::stoi(argv[i + 1]);
    if (std::string(argv[i]) == "--name") name = argv[i + 1];
    if (std::string(argv[i]) == "--count") count = std::stoi(argv[i + 1]);
  }
  const int rc = role == "server" ? RunServer(domain, name, count) : RunClient(domain);
  return rc;
}
