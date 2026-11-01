#include <QCoreApplication>
#include <QObject>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <boost/fiber/operations.hpp>

#include "detail/asyncdefine.h"
#include "hello.ddsrpc.client.h"
#include "hello.ddsrpc.service.h"
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
    boost::this_fiber::sleep_for(20ms);
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

class GreeterImpl final : public demo::GreeterService {
 public:
  using GreeterService::GreeterService;

 protected:
  Coro::Result<QString> greet(QString name) override {
    auto published = this->publishAnnounced(1);
    if (!published) return published.error();
    return QStringLiteral("fastdds hello ") + name;
  }
  Coro::Result<void> reset() override { return {}; }
};

int RunServer(int domain) {
  int exit_code = 1;
  auto task = Coro::makeTask([&] {
    transport::DdsTransport transport(MakeConfig(domain));
    if (!transport.Start()) {
      std::cerr << "server transport start failed\n";
      Coro::quit();
      return;
    }
    transport::DdsNode node(transport, std::make_unique<transport::DdsCodec>());
    GreeterImpl service(node);
    if (!service.prepare() || !node.Start() || !service.start()) {
      std::cerr << "server endpoint start failed\n";
      transport.Close();
      transport.WaitClosed();
      Coro::quit();
      return;
    }
    std::cout << "FASTDDS_SERVER_READY domain=" << domain << std::endl;
    Coro::msleep(30000);
    (void)service.stop();
    node.Close();
    node.WaitClosed();
    transport.Close();
    transport.WaitClosed();
    std::cout << "FASTDDS_SERVER_STOP" << std::endl;
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
  demo::GreeterClient client(node, transport);

  auto task = Coro::makeTask([&] {
    if (!client.prepare() || !node.Start() || !client.start()) {
      std::cerr << "client endpoint start failed\n";
      Coro::quit();
      return;
    }
    if (!PumpUntil([&] { return client.ready(); }, 20000ms)) {
      std::cerr << "client not ready: " << client.lastErrorText().toStdString() << "\n";
      (void)client.stop();
      node.Close();
      node.WaitClosed();
      transport.Close();
      transport.WaitClosed();
      Coro::quit();
      return;
    }

    std::atomic_int count{-1};
    std::atomic_int announced{0};
    QObject::connect(&client, &demo::GreeterClient::countChanged,
                     [&count](int value) { count.store(value); });
    QObject::connect(&client, &demo::GreeterClient::announced,
                     [&announced](int value) { announced.store(value); });

    auto greeting = client.greet(QStringLiteral("client"));
    if (!greeting) {
      std::cerr << "greet failed: " << greeting.error().message() << "\n";
    } else {
      const bool ok = PumpUntil([&] { return announced.load() == 1; }, 5000ms) &&
                      client.pushCount(7) &&
                      PumpUntil([&] { return count.load() == 7; }, 5000ms);
      std::cout << "FASTDDS_CLIENT_GREET=" << greeting.value().toStdString() << "\n";
      std::cout << "FASTDDS_CLIENT_ANNOUNCED=" << announced.load() << "\n";
      std::cout << "FASTDDS_CLIENT_COUNT=" << count.load() << "\n";
      exit_code = ok ? 0 : 1;
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
  std::string role = "client";
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--role") role = argv[i + 1];
    if (std::string(argv[i]) == "--domain") domain = std::stoi(argv[i + 1]);
  }

  const int rc = role == "server" ? RunServer(domain) : RunClient(domain);
  return rc;
}
