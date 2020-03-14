#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace lokimq {
class LokiMQ;
class simple_string_view;
using string_view = simple_string_view;
struct Allow;
class Message;
} // namespace lokimq

using lokimq::LokiMQ;

namespace loki {

struct lokid_key_pair_t;
class ServiceNode;
class RequestHandler;

class LokimqServer {

    std::unique_ptr<LokiMQ> lokimq_;

    // Has information about current SNs
    ServiceNode* service_node_;

    RequestHandler* request_handler_;

    // Get nodes' address
    std::string peer_lookup(lokimq::string_view pubkey_bin) const;

    // Check if the node is SN
    lokimq::Allow auth_level_lookup(lokimq::string_view ip,
                                    lokimq::string_view pubkey) const;

    // Handle Session data coming from peer SN
    void handle_sn_data(lokimq::Message& message);

    // Handle Session client requests arrived via proxy
    void handle_sn_proxy_exit(lokimq::Message& message);

    void handle_onion_request(lokimq::Message& message);

    uint16_t port_ = 0;

  public:
    LokimqServer(uint16_t port);
    ~LokimqServer();

    // Initialize lokimq
    void init(ServiceNode* sn, RequestHandler* rh,
              const lokid_key_pair_t& keypair);

    uint16_t port() { return port_; }

    // TODO: maybe we should separate LokiMQ and LokimqServer, so we don't have
    // to do this: Get underlying LokiMQ instance
    LokiMQ* lmq() { return lokimq_.get(); }
};

} // namespace loki