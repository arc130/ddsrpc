#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <QString>
#include <QUuid>
#include <boost/fiber/operations.hpp>

#include "transport/codec/DdsCodec.hpp"
#include "transport/io/dds/DdsConfig.hpp"
#include "transport/io/dds/DdsProviderRegistry.hpp"
#include "transport/io/dds/DdsTransport.hpp"
#include "transport/io/dds/FakeDdsProvider.hpp"
#include "transport/node/DdsNode.hpp"

namespace test_support {

struct FakeDdsFixture {
  FakeDdsFixture() {
    const QString raw = QUuid::createUuid().toString(QUuid::WithoutBraces);
    provider_name = "ddsrpc-test-" + raw.toStdString();
    transport::DdsProviderRegistry::RegisterProvider(
        provider_name,
        [bus = bus] {
          return std::unique_ptr<transport::IDdsProvider>(
              new transport::FakeDdsProvider(bus));
        });
  }

  [[nodiscard]] transport::DdsConfig config() const {
    transport::DdsConfig cfg;
    cfg.provider = provider_name;
    cfg.domain_id = 0;
    return cfg;
  }

  std::shared_ptr<transport::FakeDdsProvider::Bus> bus =
      std::make_shared<transport::FakeDdsProvider::Bus>();
  std::string provider_name;
};

class Host {
 public:
  explicit Host(const FakeDdsFixture& fixture, std::string uuid = {})
      : transport_(fixture.config()) {
    transport::DdsNodeConfig node_config;
    node_config.uuid_override = std::move(uuid);
    node_ = std::make_unique<transport::DdsNode>(
        transport_, std::make_unique<transport::DdsCodec>(), node_config);
  }

  ~Host() {
    node_.reset();
    (void)transport_.Close();
    transport_.WaitClosed();
  }

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  [[nodiscard]] bool start_node() {
    if (!transport_.Start()) return false;
    return static_cast<bool>(node_->Start());
  }

  [[nodiscard]] transport::DdsNode& node() { return *node_; }
  [[nodiscard]] transport::DdsTransport& transport() { return transport_; }

 private:
  transport::DdsTransport transport_;
  std::unique_ptr<transport::DdsNode> node_;
};

inline bool PumpUntil(const std::function<bool()>& predicate,
                      int budget_ms = 3000) {
  for (int waited = 0; waited < budget_ms; waited += 2) {
    if (predicate()) return true;
    boost::this_fiber::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

inline bool WaitLinkUp(transport::DdsTransport& transport,
                       int budget_ms = 3000) {
  return PumpUntil(
      [&transport] {
        return transport.CurrentLinkState() == transport::LinkState::kUp;
      },
      budget_ms);
}

}  // namespace test_support
