#include "channel_encryption.hpp"
#include "http.h"
#include "oxen_logger.h"
#include "request_handler.h"
#include "service_node.h"
#include <boost/endian/conversion.hpp>
#include <nlohmann/json.hpp>
#include <oxenmq/base64.h>
#include <oxenmq/variant.h>

#include "onion_processing.h"

#include "utils.hpp"
#include "string_utils.hpp"

#include <variant>

using nlohmann::json;

namespace oxen {

auto process_inner_request(std::string plaintext) -> ParsedInfo {

    ParsedInfo ret;

    try {
        auto [ciphertext, inner_json] = parse_combined_payload(plaintext);

        /// Kind of unfortunate that we use "headers" (which is empty)
        /// to identify we are the final destination...
        if (inner_json.count("headers")) {
            OXEN_LOG(trace, "Found body: <{}>", ciphertext);
            auto& [body, json, b64] = ret.emplace<FinalDestinationInfo>();
            body = std::move(ciphertext);
            if (auto it = inner_json.find("json"); it != inner_json.end())
                json = it->get<bool>();
            if (auto it = inner_json.find("base64"); it != inner_json.end())
                b64 = it->get<bool>();
        } else if (auto it = inner_json.find("host"); it != inner_json.end()) {
            auto& [payload, host, port, protocol, target] = ret.emplace<RelayToServerInfo>();
            payload = std::move(plaintext);
            host = it->get<std::string>();
            target = inner_json.at("target").get<std::string>();

            if (auto p = inner_json.find("port"); p != inner_json.end())
                port = p->get<uint16_t>();
            else
                port = 443;

            if (auto p = inner_json.find("protocol"); p != inner_json.end())
                protocol = p->get<std::string>();
            else
                protocol = "https";
        } else {
            auto& [ctext, eph_key, enc_type, next] = ret.emplace<RelayToNodeInfo>();
            ctext = std::move(ciphertext);
            next = ed25519_pubkey::from_hex(
                inner_json.at("destination").get_ref<const std::string&>());
            eph_key = x25519_pubkey::from_hex(
                inner_json.at("ephemeral_key").get_ref<const std::string&>());
            if (auto it = inner_json.find("enc_type"); it != inner_json.end())
                enc_type = parse_enc_type(it->get_ref<const std::string&>());
            else
                enc_type = EncryptType::aes_gcm;
        }
    } catch (const std::exception& e) {
        OXEN_LOG(debug, "Error parsing inner JSON in onion request: {}",
                 e.what());
        ret = ProcessCiphertextError::INVALID_JSON;
    }

    return ret;
}

static auto
process_ciphertext_v2(const ChannelEncryption& decryptor,
                      std::string_view ciphertext,
                      const x25519_pubkey& ephem_key,
                      EncryptType enc_type) -> ParsedInfo {
    std::optional<std::string> plaintext;

    try {
        plaintext = decryptor.decrypt(enc_type, ciphertext, ephem_key);
    } catch (const std::exception& e) {
        OXEN_LOG(error, "Error decrypting {} bytes onion request using {}: {}",
                ciphertext.size(), enc_type,
                e.what());
    }
    if (!plaintext)
        return ProcessCiphertextError::INVALID_CIPHERTEXT;

    OXEN_LOG(debug, "onion request decrypted: (len: {})", plaintext->size());

    return process_inner_request(std::move(*plaintext));
}

// FIXME: why are these method definitions *here* instead of request_handler.cpp?
void RequestHandler::process_onion_req(std::string_view ciphertext,
                                       OnionRequestMetadata data) {
    if (!service_node_.snode_ready())
        return data.cb({
            http::SERVICE_UNAVAILABLE,
            fmt::format("Snode not ready: {}", service_node_.own_address().pubkey_ed25519)});

    OXEN_LOG(debug, "process_onion_req");

    var::visit([&](auto&& x) { process_onion_req(std::move(x), std::move(data)); },
            process_ciphertext_v2(channel_cipher_, ciphertext, data.ephem_key, data.enc_type));
}

void RequestHandler::process_onion_req(FinalDestinationInfo&& info,
        OnionRequestMetadata&& data) {
    OXEN_LOG(debug, "We are the final destination in the onion request!");

    process_onion_exit(
            info.body,
            [this, data = std::move(data), json = info.json, b64 = info.base64]
            (oxen::Response res) {
                data.cb(wrap_proxy_response(std::move(res), data.ephem_key, data.enc_type, json, b64));
            });
}

void RequestHandler::process_onion_req(RelayToNodeInfo&& info,
        OnionRequestMetadata&& data) {
    auto& [payload, ekey, etype, dest] = info;

    auto dest_node = service_node_.find_node(dest);
    if (!dest_node) {
        auto msg = fmt::format("Next node not found: {}", dest);
        OXEN_LOG(warn, "{}", msg);
        return data.cb({http::BAD_GATEWAY, std::move(msg)});
    }

    auto on_response = [cb=std::move(data.cb)](bool success, std::vector<std::string> data) {
        // Processing the result we got from upstream

        if (!success) {
            OXEN_LOG(debug, "[Onion request] Request time out");
            return cb({http::GATEWAY_TIMEOUT, "Request time out"});
        }

        // We expect a two-part message, but for forwards compatibility allow extra parts
        if (data.size() < 2) {
            OXEN_LOG(debug, "[Onion request] Invalid response; expected at least 2 parts");
            return cb({http::INTERNAL_SERVER_ERROR, "Invalid response from snode"});
        }

        Response res{http::INTERNAL_SERVER_ERROR, std::move(data[1]), http::json};
        if (int code; util::parse_int(data[0], code))
            res.status = http::from_code(code);

        /// We use http status codes (for now)
        if (res.status != http::OK)
            OXEN_LOG(debug, "Onion request relay failed with: {}", res.body);

        cb(std::move(res));
    };

    OXEN_LOG(debug, "send_onion_to_sn, sn: {}", dest_node->pubkey_legacy);

    data.ephem_key = ekey;
    data.enc_type = etype;
    service_node_.send_onion_to_sn(
            *dest_node, std::move(payload), std::move(data), std::move(on_response));
}

bool is_server_url_allowed(std::string_view url) {
    return
        (util::starts_with(url, "/loki/") || util::starts_with(url, "/oxen/")) &&
        util::ends_with(url, "/lsrpc") &&
        url.find('?') == std::string::npos;
}

void RequestHandler::process_onion_req(
        RelayToServerInfo&& info, OnionRequestMetadata&& data) {
    OXEN_LOG(debug, "We are to forward the request to url: {}{}",
            info.host, info.target);

    // Forward the request to url but only if it ends in `/lsrpc`
    if (is_server_url_allowed(info.target))
        return process_onion_to_url(info.protocol, std::move(info.host), info.port,
                std::move(info.target), std::move(info.payload), std::move(data.cb));

    return data.cb(wrap_proxy_response({http::BAD_REQUEST, "Invalid url"},
            data.ephem_key, data.enc_type));
}

void RequestHandler::process_onion_req(ProcessCiphertextError&& error,
        OnionRequestMetadata&& data) {

    switch (error) {
        case ProcessCiphertextError::INVALID_CIPHERTEXT:
            return data.cb({http::BAD_REQUEST, "Invalid ciphertext"});
        case ProcessCiphertextError::INVALID_JSON:
            return data.cb(wrap_proxy_response({http::BAD_REQUEST, "Invalid json"},
                    data.ephem_key, data.enc_type));
    }
}

/// We are expecting a payload of the following shape:
/// | <4 bytes>: N | <N bytes>: ciphertext | <rest>: json as utf8 |
auto parse_combined_payload(std::string_view payload) -> CiphertextPlusJson {

    OXEN_LOG(trace, "Parsing payload of length: {}", payload.size());

    /// First 4 bytes as number
    if (payload.size() < 4) {
        OXEN_LOG(warn, "Unexpected payload size; expected ciphertext size");
        throw std::runtime_error{"Unexpected payload size; expected ciphertext size"};
    }

    uint32_t n;
    std::memcpy(&n, payload.data(), 4);
    payload.remove_prefix(4);
    boost::endian::little_to_native_inplace(n);
    OXEN_LOG(trace, "Ciphertext length: {}", n);

    if (payload.size() < n) {
        auto msg = fmt::format("Unexpected payload size {}, expected >= {}", payload.size(), n);
        OXEN_LOG(warn, "{}", msg);
        throw std::runtime_error{msg};
    }

    CiphertextPlusJson result;
    auto& [ciphertext, json] = result;

    ciphertext = payload.substr(0, n);
    OXEN_LOG(debug, "ciphertext length: {}", ciphertext.size());
    payload.remove_prefix(ciphertext.size());

    json = json::parse(payload);

    return result;
}

std::ostream& operator<<(std::ostream& os, const FinalDestinationInfo& d) {
    return os << fmt::format("[\"body\": {}]", d.body);
}

bool operator==(const FinalDestinationInfo& lhs,
                const FinalDestinationInfo& rhs) {
    return lhs.body == rhs.body;
}

std::ostream& operator<<(std::ostream& os, const RelayToServerInfo& d) {
    return os << fmt::format("[\"protocol\": {}, \"host\": {}, \"port\": {}, "
                             "\"target\": {}, \"payload\": {}]",
                             d.protocol, d.host, d.port, d.target, d.payload);
}

bool operator==(const RelayToServerInfo& lhs, const RelayToServerInfo& rhs) {
    return (lhs.protocol == rhs.protocol) && (lhs.host == rhs.host) &&
           (lhs.port == rhs.port) && (lhs.target == rhs.target) &&
           (lhs.payload == rhs.payload);
}

std::ostream& operator<<(std::ostream& os, const RelayToNodeInfo& d) {
    return os << fmt::format(
               R"("["ciphertext": {}, "ephemeral_key": {}, "enc_type": {}, "next_node": {}])",
               d.ciphertext, d.ephemeral_key, d.enc_type, d.next_node);
}

bool operator==(const RelayToNodeInfo& a, const RelayToNodeInfo& b) {
    return std::tie(a.ciphertext, a.ephemeral_key, a.enc_type, a.next_node)
        == std::tie(b.ciphertext, b.ephemeral_key, b.enc_type, b.next_node);
}

} // namespace oxen
