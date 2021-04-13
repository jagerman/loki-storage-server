#include <boost/test/unit_test.hpp>
#include <iostream>
#include <ostream>

#include "onion_processing.h"

using namespace oxen;

BOOST_AUTO_TEST_SUITE(onion_requests)

constexpr const char* plaintext = "plaintext";
constexpr const char* ciphertext = "ciphertext";

// Provided "headers", so the request terminates
// at a service node.
BOOST_AUTO_TEST_CASE(final_destination) {
    const std::string inner_json = R"#({
        "headers": "something"
    })#";

    CiphertextPlusJson combined{ ciphertext, inner_json };

    auto res = process_inner_request(combined, plaintext);

    auto expected = FinalDestinationInfo {
        ciphertext
    };

    BOOST_REQUIRE(std::holds_alternative<FinalDestinationInfo>(res));
    BOOST_CHECK_EQUAL(*std::get_if<FinalDestinationInfo>(&res), expected);

}

// Provided "host", so the request should go
// to an extrenal server. Default values will
// be used for port and protocol.
BOOST_AUTO_TEST_CASE(relay_to_server_legacy) {
    const std::string inner_json = R"#({
        "host": "host",
        "target": "target"
    })#";

    CiphertextPlusJson combined{ ciphertext, inner_json };

    auto res = process_inner_request(combined, plaintext);

    uint16_t port = 443;
    std::string protocol = "https";

    auto expected = RelayToServerInfo {
        plaintext,
        "host",
        port,
        protocol,
        "target"
    };

    BOOST_REQUIRE(std::holds_alternative<RelayToServerInfo>(res));
    BOOST_CHECK_EQUAL(*std::get_if<RelayToServerInfo>(&res), expected);

}

// Provided "host", so the request should go
// to an extrenal server.
BOOST_AUTO_TEST_CASE(relay_to_server) {
    const std::string inner_json = R"#({
        "host": "host",
        "target": "target",
        "port": 80,
        "protocol": "http"
    })#";

    CiphertextPlusJson combined{ ciphertext, inner_json };

    auto res = process_inner_request(combined, plaintext);

    uint16_t port = 80;
    std::string protocol = "http";

    auto expected = RelayToServerInfo {
        plaintext,
        "host",
        port,
        protocol,
        "target"
    };

    BOOST_REQUIRE(std::holds_alternative<RelayToServerInfo>(res));
    BOOST_CHECK_EQUAL(*std::get_if<RelayToServerInfo>(&res), expected);

}

/// No "host" or "headers", so we forward
/// the request to another node
BOOST_AUTO_TEST_CASE(relay_to_node) {

    const std::string inner_json = R"#({
        "destination": "ffffeeeeddddccccbbbbaaaa9999888877776666555544443333222211110000",
        "ephemeral_key": "ephemeral_key"
    })#";

    CiphertextPlusJson combined{ ciphertext, inner_json };

    auto res = process_inner_request(combined, plaintext);

    auto expected = RelayToNodeInfo {
        ciphertext,
        "ephemeral_key",
        ed25519_pubkey::from_hex("ffffeeeeddddccccbbbbaaaa9999888877776666555544443333222211110000")
    };

    BOOST_REQUIRE(std::holds_alternative<RelayToNodeInfo>(res));
    BOOST_CHECK_EQUAL(*std::get_if<RelayToNodeInfo>(&res), expected);

}

BOOST_AUTO_TEST_CASE(correctly_filters_urls) {

    BOOST_CHECK(is_server_url_allowed("/loki/v3/lsrpc"));
    BOOST_CHECK(is_server_url_allowed("/loki/oxen/v4/lsrpc"));
    BOOST_CHECK(is_server_url_allowed("/oxen/v3/lsrpc"));

    BOOST_CHECK(!is_server_url_allowed("/not_loki/v3/lsrpc"));
    BOOST_CHECK(!is_server_url_allowed("/loki/v3"));
    BOOST_CHECK(!is_server_url_allowed("/loki/v3/lsrpc?foo=bar"));

}

BOOST_AUTO_TEST_SUITE_END()