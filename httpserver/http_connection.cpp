#include "http_connection.h"
#include "Database.hpp"
#include "Item.hpp"

#include "dev_sink.h"
#include "net_stats.h"
#include "rate_limiter.h"
#include "security.h"
#include "serialization.h"
#include "server_certificates.h"
#include "service_node.h"
#include "signature.h"
#include "utils.hpp"

// needed for proxy requests
#include "https_client.h"

#include "request_handler.h"

#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <openssl/sha.h>
#include <sodium.h>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;
using namespace std::chrono_literals;

using tcp = boost::asio::ip::tcp;    // from <boost/asio.hpp>
namespace http = boost::beast::http; // from <boost/beast/http.hpp>

/// +===========================================

static constexpr auto LOKI_EPHEMKEY_HEADER = "X-Loki-EphemKey";

static constexpr auto LOKI_FILE_SERVER_TARGET_HEADER =
    "X-Loki-File-Server-Target";
static constexpr auto LOKI_FILE_SERVER_VERB_HEADER = "X-Loki-File-Server-Verb";
static constexpr auto LOKI_FILE_SERVER_HEADERS_HEADER =
    "X-Loki-File-Server-Headers";

using loki::storage::Item;

using error_code = boost::system::error_code;

namespace loki {

constexpr auto TEST_RETRY_PERIOD = std::chrono::milliseconds(50);

std::shared_ptr<request_t> build_post_request(const char* target,
                                              std::string&& data) {
    auto req = std::make_shared<request_t>();
    req->body() = std::move(data);
    req->method(http::verb::post);
    req->set(http::field::host, "service node");
    req->target(target);
    req->prepare_payload();
    return req;
}

void make_http_request(boost::asio::io_context& ioc,
                       const std::string& sn_address, uint16_t port,
                       const std::shared_ptr<request_t>& req,
                       http_callback_t&& cb) {

    error_code ec;
    tcp::endpoint endpoint;
    tcp::resolver resolver(ioc);
#ifdef INTEGRATION_TEST
    tcp::resolver::iterator destination =
        resolver.resolve("0.0.0.0", "http", ec);
#else
    tcp::resolver::iterator destination =
        resolver.resolve(sn_address, "http", ec);
#endif
    if (ec) {
        LOKI_LOG(error,
                 "http: Failed to parse the IP address <{}>. Error code = {}. "
                 "Message: {}",
                 sn_address, ec.value(), ec.message());
        return;
    }
    while (destination != tcp::resolver::iterator()) {
        const tcp::endpoint thisEndpoint = (destination++)->endpoint();
        if (!thisEndpoint.address().is_v4()) {
            continue;
        }
        endpoint = thisEndpoint;
    }
    endpoint.port(port);

    auto session =
        std::make_shared<HttpClientSession>(ioc, endpoint, req, std::move(cb));

    session->start();
}

// ======================== Lokid Client ========================
LokidClient::LokidClient(boost::asio::io_context& ioc, std::string ip,
                         uint16_t port)
    : ioc_(ioc), lokid_rpc_ip_(std::move(ip)), lokid_rpc_port_(port) {}

void LokidClient::make_lokid_request(boost::string_view method,
                                     const nlohmann::json& params,
                                     http_callback_t&& cb) const {

    make_custom_lokid_request(lokid_rpc_ip_, lokid_rpc_port_, method, params,
                              std::move(cb));
}

void LokidClient::make_custom_lokid_request(const std::string& daemon_ip,
                                            const uint16_t daemon_port,
                                            boost::string_view method,
                                            const nlohmann::json& params,
                                            http_callback_t&& cb) const {

    auto req = std::make_shared<request_t>();

    const std::string target = "/json_rpc";

    nlohmann::json req_body;
    req_body["jsonrpc"] = "2.0";
    req_body["id"] = "0";
    req_body["method"] = method;
    req_body["params"] = params;

    req->body() = req_body.dump();
    req->method(http::verb::post);
    req->target(target);
    req->prepare_payload();

    LOKI_LOG(trace, "Making lokid request, method: {}", method.to_string());

    make_http_request(ioc_, daemon_ip, daemon_port, req, std::move(cb));
}

static bool validateHexKey(const std::string& key,
                           const size_t key_length = loki::KEY_LENGTH) {
    return key.size() == 2 * key_length &&
           std::all_of(key.begin(), key.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::tuple<private_key_t, private_key_ed25519_t, private_key_t>
LokidClient::wait_for_privkey() {
    // fetch SN private key from lokid; do this synchronously because we can't
    // finish startup until we have it.
    loki::private_key_t private_key;
    loki::private_key_ed25519_t private_key_ed;
    loki::private_key_t private_key_x;
    LOKI_LOG(info, "Retrieving SN key from lokid");
    boost::asio::steady_timer delay{ioc_};
    std::function<void(loki::sn_response_t && res)> key_fetch;
    key_fetch = [&](loki::sn_response_t res) {
        try {
            if (res.error_code != loki::SNodeError::NO_ERROR)
                throw std::runtime_error(loki::error_string(res.error_code));
            else if (!res.body)
                throw std::runtime_error("empty body");
            else {
                auto r = nlohmann::json::parse(*res.body);
                const auto& legacy_privkey = r.at("result")
                                                 .at("service_node_privkey")
                                                 .get_ref<const std::string&>();
                const auto& privkey_ed = r.at("result")
                                             .at("service_node_ed25519_privkey")
                                             .get_ref<const std::string&>();
                const auto& privkey_x = r.at("result")
                                            .at("service_node_x25519_privkey")
                                            .get_ref<const std::string&>();
                if (!validateHexKey(legacy_privkey) ||
                    !validateHexKey(privkey_ed,
                                    private_key_ed25519_t::LENGTH) ||
                    !validateHexKey(privkey_x))
                    throw std::runtime_error("returned value is not hex");
                else {
                    private_key = loki::lokidKeyFromHex(legacy_privkey);
                    // TODO: check that one is derived from the other as a
                    // sanity check?
                    private_key_ed =
                        private_key_ed25519_t::from_hex(privkey_ed);
                    private_key_x = loki::lokidKeyFromHex(privkey_x);
                    // run out of work, which will end the event loop
                }
            }
        } catch (const std::exception& e) {
            LOKI_LOG(critical,
                     "Error retrieving SN privkey from lokid @ {}:{}: {}.  Is "
                     "lokid running?  Retrying in 5s",
                     lokid_rpc_ip_, lokid_rpc_port_, e.what());

            delay.expires_after(std::chrono::seconds{5});
            delay.async_wait([this,
                              &key_fetch](const boost::system::error_code&) {
                make_lokid_request("get_service_node_privkey", {}, key_fetch);
            });
        }
    };
    make_lokid_request("get_service_node_privkey", {}, key_fetch);
    ioc_.run(); // runs until we get success above
    ioc_.restart();

    return std::tuple<private_key_t, private_key_ed25519_t, private_key_t>{
        private_key, private_key_ed, private_key_x};
}

// =============================================================

namespace http_server {

// "Loop" forever accepting new connections.
static void accept_connection(boost::asio::io_context& ioc,
                              boost::asio::ssl::context& ssl_ctx,
                              tcp::acceptor& acceptor, ServiceNode& sn,
                              RequestHandler& rh, RateLimiter& rate_limiter,
                              const Security& security) {

    static boost::asio::steady_timer acceptor_timer(ioc);
    constexpr std::chrono::milliseconds ACCEPT_DELAY = 50ms;

    acceptor.async_accept([&](const error_code& ec, tcp::socket socket) {
        LOKI_LOG(trace, "connection accepted");
        if (!ec) {

            std::make_shared<connection_t>(ioc, ssl_ctx, std::move(socket), sn,
                                           rh, rate_limiter, security)
                ->start();

            accept_connection(ioc, ssl_ctx, acceptor, sn, rh, rate_limiter,
                              security);
        } else {

            // TODO: remove this once we confirmed that there is
            // no more socket leaking
            if (ec == boost::system::errc::too_many_files_open) {
                LOKI_LOG(critical, "Too many open files, aborting");
                abort();
            }

            LOKI_LOG(
                error,
                "Could not accept a new connection {}: {}. Will only start "
                "accepting new connections after a short delay.",
                ec.value(), ec.message());

            // If we fail here we are unlikely to be able to accept a new
            // connection immediately, hence the delay
            acceptor_timer.expires_after(ACCEPT_DELAY);
            acceptor_timer.async_wait([&](const error_code& ec) {
                if (ec && ec != boost::asio::error::operation_aborted) {
                    // Not sure how to recover here, so it is probably the
                    // safest to simply abort and let the launcher/systemd
                    // restart us
                    abort();
                }

                accept_connection(ioc, ssl_ctx, acceptor, sn, rh, rate_limiter,
                                  security);
            });
        }
    });
}

void run(boost::asio::io_context& ioc, const std::string& ip, uint16_t port,
         const boost::filesystem::path& base_path, ServiceNode& sn,
         RequestHandler& rh, RateLimiter& rate_limiter, Security& security) {

    LOKI_LOG(trace, "http server run");

    const auto address =
        boost::asio::ip::make_address(ip); /// throws if incorrect

    tcp::acceptor acceptor{ioc, {address, port}};

    ssl::context ssl_ctx{ssl::context::tlsv12};

    load_server_certificate(base_path, ssl_ctx);

    security.generate_cert_signature();

    accept_connection(ioc, ssl_ctx, acceptor, sn, rh, rate_limiter, security);

    ioc.run();
}

/// ============ connection_t ============

connection_t::connection_t(boost::asio::io_context& ioc, ssl::context& ssl_ctx,
                           tcp::socket socket, ServiceNode& sn,
                           RequestHandler& rh, RateLimiter& rate_limiter,
                           const Security& security)
    : ioc_(ioc), ssl_ctx_(ssl_ctx), socket_(std::move(socket)),
      stream_(socket_, ssl_ctx_), service_node_(sn), request_handler_(rh),
      rate_limiter_(rate_limiter), repeat_timer_(ioc),
      deadline_(ioc, SESSION_TIME_LIMIT), notification_ctx_{boost::none},
      security_(security) {

    static uint64_t instance_counter = 0;
    conn_idx = instance_counter++;

    get_net_stats().connections_in++;

    LOKI_LOG(trace, "connection_t [{}]", conn_idx);

    request_.body_limit(1024 * 1024 * 10); // 10 mb

    start_timestamp_ = std::chrono::steady_clock::now();
}

connection_t::~connection_t() {

    // Safety net
    if (stream_.lowest_layer().is_open()) {
        LOKI_LOG(debug, "Client socket should be closed by this point, but "
                        "wasn't. Closing now.");
        stream_.lowest_layer().close();
    }

    get_net_stats().connections_in--;

    LOKI_LOG(trace, "~connection_t [{}]", conn_idx);
}

void connection_t::start() {
    register_deadline();
    do_handshake();
}

void connection_t::do_handshake() {
    // Perform the SSL handshake
    stream_.async_handshake(ssl::stream_base::server,
                            std::bind(&connection_t::on_handshake,
                                      shared_from_this(),
                                      std::placeholders::_1));
}

void connection_t::on_handshake(boost::system::error_code ec) {

    const auto sockfd = stream_.lowest_layer().native_handle();
    LOKI_LOG(trace, "Open https socket: {}", sockfd);
    get_net_stats().record_socket_open(sockfd);
    if (ec) {
        LOKI_LOG(warn, "ssl handshake failed: ec: {} ({})", ec.value(),
                 ec.message());
        this->clean_up();
        deadline_.cancel();
        return;
    }

    read_request();
}

void connection_t::clean_up() { this->do_close(); }

void connection_t::notify(boost::optional<const message_t&> msg) {

    if (!notification_ctx_) {
        LOKI_LOG(error,
                 "Trying to notify a connection without notification context");
        return;
    }

    if (msg) {
        LOKI_LOG(trace, "Processing message notification: {}", msg->data);
        // save messages, so we can access them once the timer event happens
        notification_ctx_->message = msg;
    }
    // the timer callback will be called once we complete the current callback
    notification_ctx_->timer.cancel();
}

// Asynchronously receive a complete request message.
void connection_t::read_request() {

    auto on_data = [self = shared_from_this()](error_code ec,
                                               size_t bytes_transferred) {
        LOKI_LOG(trace, "on data: {} bytes", bytes_transferred);

        if (ec) {
            LOKI_LOG(
                error,
                "Failed to read from a socket [{}: {}], connection idx: {}",
                ec.value(), ec.message(), self->conn_idx);
            self->clean_up();
            self->deadline_.cancel();
            return;
        }

        // NOTE: this is blocking, we should make this asynchronous
        try {
            self->process_request();
        } catch (const std::exception& e) {
            LOKI_LOG(critical, "Exception caught processing a request: {}",
                     e.what());
            self->body_stream_ << e.what();
        }

        if (!self->delay_response_) {
            self->write_response();
        }
    };

    http::async_read(stream_, buffer_, request_, on_data);
}

// This doesn't need to be a method...
static bool verify_signature(const std::string& payload,
                             const std::string& signature,
                             const std::string& public_key_b32z) {
    const auto body_hash = hash_data(payload);
    return check_signature(signature, body_hash, public_key_b32z);
}

bool connection_t::validate_snode_request() {
    if (!parse_header(LOKI_SENDER_SNODE_PUBKEY_HEADER,
                      LOKI_SNODE_SIGNATURE_HEADER)) {
        LOKI_LOG(debug, "Missing signature headers for a Service Node request");
        return false;
    }
    const auto& signature = header_[LOKI_SNODE_SIGNATURE_HEADER];
    const auto& public_key_b32z = header_[LOKI_SENDER_SNODE_PUBKEY_HEADER];

    /// Known service node
    const std::string snode_address = public_key_b32z + ".snode";
    if (!service_node_.is_snode_address_known(snode_address)) {
        body_stream_ << "Unknown service node\n";
        LOKI_LOG(debug, "Discarding signature from unknown service node: {}",
                 public_key_b32z);
        response_.result(http::status::unauthorized);
        return false;
    }

    if (!verify_signature(request_.get().body(), signature, public_key_b32z)) {
        constexpr auto msg = "Could not verify batch signature";
        LOKI_LOG(debug, "{}", msg);
        body_stream_ << msg;
        response_.result(http::status::unauthorized);
        return false;
    }
    if (rate_limiter_.should_rate_limit(public_key_b32z)) {
        this->body_stream_ << "Too many requests\n";
        response_.result(http::status::too_many_requests);
        return false;
    }
    return true;
}

void connection_t::process_storage_test_req(uint64_t height,
                                            const std::string& tester_pk,
                                            const std::string& msg_hash) {

    LOKI_LOG(trace, "Performing storage test, attempt: {}", repetition_count_);

    std::string answer;

    /// TODO: we never actually test that `height` is within any reasonable
    /// time window (or that it is not repeated multiple times), we should do
    /// that! This is done implicitly to some degree using
    /// `block_hashes_cache_`, which holds a limited number of recent blocks
    /// only and fails if an earlier block is requested
    const MessageTestStatus status = service_node_.process_storage_test_req(
        height, tester_pk, msg_hash, answer);
    const auto elapsed_time =
        std::chrono::steady_clock::now() - start_timestamp_;
    if (status == MessageTestStatus::SUCCESS) {
        LOKI_LOG(
            debug, "Storage test success! Attempts: {}. Took {} ms",
            repetition_count_,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time)
                .count());
        delay_response_ = true;

        nlohmann::json json_res;
        json_res["status"] = "OK";
        json_res["value"] = answer;

        this->body_stream_ << json_res.dump();

        response_.result(http::status::ok);
        this->write_response();
    } else if (status == MessageTestStatus::RETRY && elapsed_time < 1min) {
        delay_response_ = true;
        repetition_count_++;

        repeat_timer_.expires_after(TEST_RETRY_PERIOD);
        repeat_timer_.async_wait([self = shared_from_this(), height, msg_hash,
                                  tester_pk](const error_code& ec) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    LOKI_LOG(error,
                             "Repeat timer failed for storage test [{}: {}]",
                             ec.value(), ec.message());
                }
            } else {
                self->process_storage_test_req(height, tester_pk, msg_hash);
            }
        });

    } else if (status == MessageTestStatus::WRONG_REQ) {
        nlohmann::json json_res;
        json_res["status"] = "wrong request";
        this->body_stream_ << json_res.dump();
        response_.result(http::status::ok);
    } else {
        LOKI_LOG(error, "Failed storage test, tried {} times.",
                 repetition_count_);
        nlohmann::json json_res;
        json_res["status"] = "other";
        this->body_stream_ << json_res.dump();
        response_.result(http::status::ok);
    }
}

void connection_t::process_blockchain_test_req(uint64_t,
                                               const std::string& tester_pk,
                                               bc_test_params_t params) {

    // Note: `height` can be 0, which is the default value for old SS, allowed
    // pre HF13

    LOKI_LOG(debug, "Performing blockchain test");

    auto callback = [this](blockchain_test_answer_t answer) {
        this->response_.result(http::status::ok);

        nlohmann::json json_res;
        json_res["res_height"] = answer.res_height;

        this->body_stream_ << json_res.dump();
        this->write_response();
    };

    /// TODO: this should first check if tester/testee are correct! (use
    /// `height`)
    service_node_.perform_blockchain_test(params, std::move(callback));
}

static void print_headers(const request_t& req) {
    LOKI_LOG(info, "HEADERS:");
    for (const auto& field : req) {
        LOKI_LOG(info, "    [{}]: {}", field.name_string(), field.value());
    }
}

void connection_t::process_onion_req() {

    LOKI_LOG(debug, "Processing an onion request");

    const request_t& req = this->request_.get();

    // We are not expecting any headers, all parameters are in json body

    // Need to make sure we are not blocking waiting for the response
    delay_response_ = true;

    auto on_response = [this](loki::Response res) {
        LOKI_LOG(debug, "Got an onion response as guard node");

        if (res.status() == Status::OK) {
            response_.result(http::status::ok);

            // OK here simply means that the response we got is
            // coming from the target node as opposed to any other
            // node on the path. The encrypted body will contain
            // its own response status.

            this->body_stream_ << res.message();
        } else {
            // res.status() is for us, we only report a generic
            // error to indicate onion request failure

            LOKI_LOG(debug, "Would send 503 error");

            response_.result(http::status::service_unavailable);
        }

        this->write_response();
    };

    try {

        const json json_req = json::parse(req.body(), nullptr, true);
        // base64
        const auto& ciphertext =
            json_req.at("ciphertext").get_ref<const std::string&>();
        // hex
        const auto& ephem_key =
            json_req.at("ephemeral_key").get_ref<const std::string&>();

        request_handler_.process_onion_req(ciphertext, ephem_key, on_response);

    } catch (const std::exception& e) {
        auto msg = fmt::format("Error parsing outer JSON in onion request: {}",
                               e.what());
        LOKI_LOG(error, "{}", msg);
        response_.result(http::status::bad_request);
        this->body_stream_ << std::move(msg);
    }
}

void connection_t::process_proxy_req() {

    LOKI_LOG(debug, "Processing proxy request: we are first hop");

    const request_t& req = this->request_.get();

#ifdef INTEGRATION_TEST
    // print_headers(req);
#endif

    if (!parse_header(LOKI_SENDER_KEY_HEADER, LOKI_TARGET_SNODE_KEY)) {
        LOKI_LOG(debug, "Missing headers for a proxy request");
        return;
    }

    delay_response_ = true;

    const auto& sender_key = header_[LOKI_SENDER_KEY_HEADER];
    const auto& target_snode_key = header_[LOKI_TARGET_SNODE_KEY];

    auto sn = service_node_.find_node_by_ed25519_pk(target_snode_key);

    static int req_counter = 0;

    const int req_idx = req_counter;

    // TODO: make an https response out of what we got back
    auto on_proxy_response = [this, req_idx](bool success,
                                             std::vector<std::string> data) {
        LOKI_LOG(debug, "on proxy response: {}",
                 success ? "success" : "failure");

        if (success && data.size() == 1) {

            LOKI_LOG(debug, "PROXY RESPONSE OK, idx: {}", req_idx);

            this->body_stream_ << data[0];
            response_.result(http::status::ok);
        } else {
            LOKI_LOG(debug, "PROXY RESPONSE FAILED, idx: {}", req_idx);
        }

        // This will return an empty, but failed response to the client
        // if the raw_response is empty (we should provide better errors)
        this->write_response();
    };

    if (!sn) {
        LOKI_LOG(debug, "Could not find target snode for proxy: {}",
                 target_snode_key);
        on_proxy_response(false, {});
        return;
    }

    LOKI_LOG(debug, "Target Snode: {}", target_snode_key);

    // Send this request to SN over either HTTP or LOKIMQ
    auto sn_req =
        ss_client::Request{req.body(), {{LOKI_SENDER_KEY_HEADER, sender_key}}};

    LOKI_LOG(debug, "About to send a proxy exit requst, idx: {}", req_counter);
    req_counter += 1;

    service_node_.send_to_sn(*sn, ss_client::ReqMethod::PROXY_EXIT,
                             std::move(sn_req), on_proxy_response);
}

void connection_t::process_file_proxy_req() {

    LOKI_LOG(debug, "Processing a file proxy request: we are first hop");

    const request_t& original_req = this->request_.get();

    delay_response_ = true;

    if (!parse_header(LOKI_FILE_SERVER_TARGET_HEADER,
                      LOKI_FILE_SERVER_VERB_HEADER,
                      LOKI_FILE_SERVER_HEADERS_HEADER)) {
        LOKI_LOG(error, "Missing headers for a file proxy request");
        // TODO: The connection should be closed by the timer if we return
        // early, but need to double-check that! (And close it early if
        // possible)
        return;
    }

    const auto& target = header_[LOKI_FILE_SERVER_TARGET_HEADER];
    const auto& verb_str = header_[LOKI_FILE_SERVER_VERB_HEADER];
    const auto& headers_str = header_[LOKI_FILE_SERVER_HEADERS_HEADER];

    LOKI_LOG(trace, "Target: {}", target);
    LOKI_LOG(trace, "Verb: {}", verb_str);
    LOKI_LOG(trace, "Headers json: {}", headers_str);

    const json headers_json = json::parse(headers_str, nullptr, false);

    if (headers_json.is_discarded()) {
        LOKI_LOG(debug, "Bad file proxy request: invalid header json");
        response_.result(http::status::bad_request);
        return;
    }

    auto req = std::make_shared<request_t>();

    namespace http = boost::beast::http;

    if (verb_str == "POST") {
        req->method(http::verb::post);
    } else if (verb_str == "PATCH") {
        req->method(http::verb::patch);
    } else if (verb_str == "PUT") {
        req->method(http::verb::put);
    } else if (verb_str == "DELETE") {
        req->method(http::verb::delete_);
    } else {
        req->method(http::verb::get);
    }

    {
        const auto it = original_req.find(http::field::content_type);
        if (it != original_req.end()) {
            LOKI_LOG(trace, "Content-Type: {}", it->value().to_string());
            req->set(http::field::content_type, it->value().to_string());
        }
    }

    req->body() = std::move(original_req.body());
    req->target(target);
    req->set(http::field::host, "file.lokinet.org");

    req->prepare_payload();

    for (auto& el : headers_json.items()) {
        req->insert(el.key(), el.value());
    }

    auto cb = [this](sn_response_t res) {
        LOKI_LOG(trace, "Successful file proxy request!");

        if (res.raw_response) {
            this->response_ = *res.raw_response;
            LOKI_LOG(trace, "Response: {}", this->response_);
        } else {
            LOKI_LOG(debug, "No response from file server!");
        }

        this->write_response();
    };

    make_https_request(ioc_, "https://file.lokinet.org", req, cb);
}

void connection_t::process_swarm_req(boost::string_view target) {

    const request_t& req = this->request_.get();

    // allow ping request as a quick workaround (and they are cheap)
    if (!validate_snode_request() && (target != "/swarms/ping_test/v1")) {
        return;
    }

    response_.set(LOKI_SNODE_SIGNATURE_HEADER, security_.get_cert_signature());

    if (target == "/swarms/push_batch/v1") {

        response_.result(http::status::ok);
        service_node_.process_push_batch(req.body());

    } else if (target == "/swarms/storage_test/v1") {

        /// Set to "bad request" by default
        response_.result(http::status::bad_request);
        LOKI_LOG(trace, "Got storage test request");

        using nlohmann::json;

        const json body = json::parse(req.body(), nullptr, false);

        if (body == nlohmann::detail::value_t::discarded) {
            LOKI_LOG(debug, "Bad snode test request: invalid json");
            body_stream_ << "invalid json\n";
            response_.result(http::status::bad_request);
            return;
        }

        uint64_t blk_height;
        std::string msg_hash;

        try {
            blk_height = body.at("height").get<uint64_t>();
            msg_hash = body.at("hash").get<std::string>();
        } catch (...) {
            this->body_stream_
                << "Bad snode test request: missing fields in json";
            response_.result(http::status::bad_request);
            LOKI_LOG(debug, "Bad snode test request: missing fields in json");
            return;
        }

        const auto it = header_.find(LOKI_SENDER_SNODE_PUBKEY_HEADER);
        if (it != header_.end()) {
            const std::string& tester_pk = it->second;
            this->process_storage_test_req(blk_height, tester_pk, msg_hash);
        } else {
            LOKI_LOG(debug, "Ignoring test request, no pubkey present");
        }
    } else if (target == "/swarms/blockchain_test/v1") {
        LOKI_LOG(debug, "Got blockchain test request");

        using nlohmann::json;

        const json body = json::parse(req.body(), nullptr, false);

        if (body.is_discarded()) {
            LOKI_LOG(debug, "Bad snode test request: invalid json");
            response_.result(http::status::bad_request);
            return;
        }

        bc_test_params_t params;

        // Height that should be used to check derive tester/testee
        uint64_t height = 0;

        try {
            params.max_height = body.at("max_height").get<uint64_t>();
            params.seed = body.at("seed").get<uint64_t>();

            if (body.find("height") != body.end()) {
                height = body.at("height").get<uint64_t>();
            } else {
                LOKI_LOG(debug, "No tester height, defaulting to {}", height);
            }
        } catch (...) {
            response_.result(http::status::bad_request);
            LOKI_LOG(debug, "Bad snode test request: missing fields in json");
            return;
        }

        /// TODO: only check pubkey field once (in validate snode req)
        const auto it = header_.find(LOKI_SENDER_SNODE_PUBKEY_HEADER);
        if (it != header_.end()) {
            const std::string& tester_pk = it->second;
            delay_response_ = true;
            this->process_blockchain_test_req(height, tester_pk, params);
        } else {
            LOKI_LOG(debug, "Ignoring test request, no pubkey present");
        }

    } else if (target == "/swarms/ping_test/v1") {
        LOKI_LOG(trace, "Received ping_test");
        response_.result(http::status::ok);
    } else if (target == "/swarms/proxy_exit") {
        LOKI_LOG(debug,
                 "Processing proxy request: we are the destination node");

        const auto it = req.find(LOKI_SENDER_KEY_HEADER);
        /// TODO: handle the error better?
        if (it != req.end()) {

            const std::string key = {it->value().data(), it->value().size()};

            auto res = request_handler_.process_proxy_exit(key, req.body());
            this->set_response(res);
        } else {
            LOKI_LOG(debug, "Error: {} header is missing",
                     LOKI_SENDER_KEY_HEADER);
        }
    }
}

void connection_t::set_response(const Response& res) {

    response_.result(static_cast<unsigned int>(res.status()));

    std::string content_type;

    switch (res.content_type()) {
    case ContentType::plaintext:
        content_type = "text/plain";
        break;
    case ContentType::json:
        content_type = "application/json";
        break;
    default:
        LOKI_LOG(critical, "Unrecognized content type");
    }

    response_.set(http::field::content_type, content_type);
    body_stream_ << res.message();
}

// Determine what needs to be done with the request message.
void connection_t::process_request() {

    const request_t& req = this->request_.get();

    /// This method is responsible for filling out response_

    LOKI_LOG(trace, "connection_t::process_request");
    response_.version(req.version());
    response_.keep_alive(false);

    /// TODO: make sure that we always send a response!

    response_.result(http::status::internal_server_error);

    const auto target = req.target();

    const bool is_swarm_req = (target.find("/swarms/") == 0);

    if (is_swarm_req) {
        LOKI_LOG(debug, "Processing a swarm request: {}", target);
    }

    switch (req.method()) {
    case http::verb::post: {
        std::string reason;

        // Respond to ping even if we are not ready
        if (target == "/swarms/ping_test/v1") {
            this->process_swarm_req(target);
            break;
        }
        if (!service_node_.snode_ready(reason)) {
            LOKI_LOG(debug,
                     "Ignoring post request; storage server not ready: {}",
                     reason);
            LOKI_LOG(debug, "Would send 503 error (2)");
            response_.result(http::status::service_unavailable);
            body_stream_ << fmt::format("Service node is not ready: {}\n",
                                        reason);
            break;
        }
        if (target == "/storage_rpc/v1") {
            /// Store/load from clients
            LOKI_LOG(trace, "POST /storage_rpc/v1");

            try {
                process_client_req_rate_limited();
            } catch (std::exception& e) {
                this->body_stream_ << fmt::format(
                    "Exception caught while processing client request: {}",
                    e.what());
                response_.result(http::status::internal_server_error);
                LOKI_LOG(critical,
                         "Exception caught while processing client request: {}",
                         e.what());
            }

        } else if (is_swarm_req) {
            this->process_swarm_req(target);
        } else if (target == "/proxy") {
            this->process_proxy_req();
        } else if (target == "/onion_req") {
            this->process_onion_req();
        } else if (target == "/file_proxy") {
            this->process_file_proxy_req();
        }
#ifdef INTEGRATION_TEST
        else if (target == "/retrieve_all") {

            const auto res = request_handler_.process_retrieve_all();
            this->set_response(res);

        } else if (target == "/quit") {
            LOKI_LOG(info, "POST /quit");
            // a bit of a hack: sending response manually
            delay_response_ = true;
            response_.result(http::status::ok);
            write_response();
            ioc_.stop();
        } else if (target == "/sleep") {
            ioc_.post([]() {
                LOKI_LOG(warn, "Sleeping for some time...");
                std::this_thread::sleep_for(std::chrono::seconds(30));
            });
            response_.result(http::status::ok);
        }
#endif
        else {
            LOKI_LOG(debug, "unknown target for POST: {}", target.to_string());
            this->body_stream_ << fmt::format("unknown target for POST: {}",
                                              target.to_string());
            response_.result(http::status::not_found);
        }
        break;
    }
    case http::verb::get:

        if (target == "/get_stats/v1") {
            this->on_get_stats();
        } else if (target == "/get_logs/v1") {
            this->on_get_logs();
        } else {
            this->body_stream_ << fmt::format("unknown target for GET: {}",
                                              target.to_string());
            LOKI_LOG(debug, "unknown target for GET: {}", target.to_string());
            response_.result(http::status::not_found);
        }
        break;
    default:
        LOKI_LOG(debug, "bad request");
        response_.result(http::status::bad_request);
        break;
    }
}

// Asynchronously transmit the response message.
void connection_t::write_response() {

    LOKI_LOG(trace, "write response, {} bytes", response_.body().size());

    const std::string body_stream = body_stream_.str();

    if (!body_stream.empty()) {

        if (!response_.body().empty()) {
            LOKI_LOG(debug, "Overwritting non-empty body in response!");
        }

        response_.body() = body_stream_.str();
    }

    // Our last change to change the response before we start sending
    if (this->response_modifier_) {
        (*this->response_modifier_)(response_);
    }

    response_.set(http::field::content_length, response_.body().size());

    /// This attempts to write all data to a stream
    /// TODO: handle the case when we are trying to send too much
    http::async_write(
        stream_, response_, [self = shared_from_this()](error_code ec, size_t) {
            if (ec && ec != boost::asio::error::operation_aborted) {
                LOKI_LOG(error, "Failed to write to a socket: {}",
                         ec.message());
            }

            self->clean_up();
            /// Is it too early to cancel the deadline here?
            self->deadline_.cancel();
        });
}

bool connection_t::parse_header(const char* key) {
    const auto it = request_.get().find(key);
    if (it == request_.get().end()) {
        body_stream_ << "Missing field in header : " << key << "\n";
        return false;
    }
    header_[key] = it->value().to_string();
    return true;
}

template <typename... Args>
bool connection_t::parse_header(const char* first, Args... args) {
    return parse_header(first) && parse_header(args...);
}

constexpr auto LONG_POLL_TIMEOUT = std::chrono::milliseconds(20000);

/// Move this out of `connection_t` Process client request
/// Decouple responding from http

void connection_t::process_client_req_rate_limited() {

    const request_t& req = this->request_.get();
    std::string plain_text = req.body();
    const std::string client_ip =
        socket_.remote_endpoint().address().to_string();
    if (rate_limiter_.should_rate_limit_client(client_ip)) {
        this->body_stream_ << "too many requests\n";
        response_.result(http::status::too_many_requests);
        LOKI_LOG(debug, "Rate limiting client request.");
        return;
    }

    // Not sure what the original idea was to distinguish between headers
    // in request_ and the actual header_ field, but it is useful for
    // "proxy" client requests as we can have both true html headers
    // and the headers that came encrypted in body
    if (req.find(LOKI_LONG_POLL_HEADER) != req.end()) {
        header_[LOKI_LONG_POLL_HEADER] =
            req.at(LOKI_LONG_POLL_HEADER).to_string();
    }

    const bool lp_requested = header_.find(LOKI_LONG_POLL_HEADER) != header_.end();

    // Annoyingly, we might still have old clients that expect long-polling
    // to work, spamming us with "retrieve" requests. The workaround for now
    // is to delay responding to the request for a few seconds

    if (lp_requested) {
        LOKI_LOG(debug, "Received a long-polling request");
        this->delay_response_ = true;

        auto delay_timer = std::make_shared<boost::asio::steady_timer>(ioc_);

        delay_timer->expires_after(std::chrono::seconds(2));
        delay_timer->async_wait([this, delay_timer, plaintext = std::move(plain_text)](const error_code& ec) {

            const auto res = this->request_handler_.process_client_req(plaintext);

            LOKI_LOG(debug, "Respond to a long-polling client");
            this->set_response(res);
            this->write_response();
        });


    } else {
        const auto res = request_handler_.process_client_req(plain_text);
        LOKI_LOG(debug, "Respond to a non-long polling client");
        this->set_response(res);
    }


}

void connection_t::register_deadline() {

    auto self = shared_from_this();

    // Note: deadline callback captures a shared pointer to this, so
    // the connection will not be destroyed until the timer goes off.
    // If we want to destroy it earlier, we need to manually cancel the timer.
    deadline_.async_wait([self = std::move(self)](error_code ec) {
        const bool cancelled =
            (ec && ec == boost::asio::error::operation_aborted);

        if (cancelled)
            return;

        // Note: cancelled timer does absolutely nothing, so we need to make
        // sure we close the socket (and unsubscribe from notifications)
        // elsewhere if we cancel it.
        if (ec) {
            LOKI_LOG(error, "Deadline timer error [{}]: {}", ec.value(),
                     ec.message());
        }

        LOKI_LOG(debug, "Closing [connection_t] socket due to timeout");
        self->clean_up();
    });
}

void connection_t::do_close() {
    // Perform the SSL shutdown
    stream_.async_shutdown(std::bind(
        &connection_t::on_shutdown, shared_from_this(), std::placeholders::_1));
}

void connection_t::on_shutdown(boost::system::error_code ec) {
    if (ec == boost::asio::error::eof) {
        // Rationale:
        // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
        ec.assign(0, ec.category());
    } else if (ec) {
        LOKI_LOG(debug, "Could not close ssl stream gracefully, ec: {} ({})",
                 ec.message(), ec.value());
    }

    const auto sockfd = stream_.lowest_layer().native_handle();
    LOKI_LOG(trace, "Close https socket: {}", sockfd);
    get_net_stats().record_socket_close(sockfd);
    stream_.lowest_layer().close();
}

void connection_t::on_get_stats() {
    this->body_stream_ << service_node_.get_stats();
    this->response_.result(http::status::ok);
}

void connection_t::on_get_logs() {

    /// Limit this call to 1 request per second
    static time_t last_req_time = 0;
    const time_t now = time(nullptr);
    constexpr time_t PERIOD = 1;

    if (now - last_req_time < PERIOD) {
        this->body_stream_ << "Too many request, try again later.";
        this->response_.result(http::status::too_many_requests);
        return;
    }

    last_req_time = now;

    auto dev_sink = dynamic_cast<loki::dev_sink_mt*>(
        spdlog::get("loki_logger")->sinks()[2].get());

    if (dev_sink == nullptr) {
        LOKI_LOG(critical, "Sink #3 should be dev sink");
        assert(false);
        this->body_stream_ << "Developer error: sink #3 is not a dev sink.";
        this->response_.result(http::status::not_implemented);
        return;
    }

    nlohmann::json val;
    val["entries"] = dev_sink->peek();
    this->body_stream_ << val.dump(4);
    this->response_.result(http::status::ok);
}

/// ============

} // namespace http_server

/// TODO: make generic, avoid message copy
HttpClientSession::HttpClientSession(boost::asio::io_context& ioc,
                                     const tcp::endpoint& ep,
                                     const std::shared_ptr<request_t>& req,
                                     http_callback_t&& cb)
    : ioc_(ioc), socket_(ioc), endpoint_(ep), callback_(cb),
      deadline_timer_(ioc), req_(req) {
    get_net_stats().http_connections_out++;
}

void HttpClientSession::on_connect() {

    const auto sockfd = socket_.native_handle();
    LOKI_LOG(trace, "Open http socket: {}", sockfd);
    get_net_stats().record_socket_open(sockfd);
    http::async_write(socket_, *req_,
                      std::bind(&HttpClientSession::on_write,
                                shared_from_this(), std::placeholders::_1,
                                std::placeholders::_2));
}

void HttpClientSession::on_write(error_code ec, size_t bytes_transferred) {

    LOKI_LOG(trace, "on write");
    if (ec) {
        LOKI_LOG(error, "Http error on write, ec: {}. Message: {}", ec.value(),
                 ec.message());
        trigger_callback(SNodeError::ERROR_OTHER, nullptr);
        return;
    }

    LOKI_LOG(trace, "Successfully transferred {} bytes", bytes_transferred);

    // Receive the HTTP response
    http::async_read(socket_, buffer_, res_,
                     std::bind(&HttpClientSession::on_read, shared_from_this(),
                               std::placeholders::_1, std::placeholders::_2));
}

void HttpClientSession::on_read(error_code ec, size_t bytes_transferred) {

    if (!ec || (ec == http::error::end_of_stream)) {

        LOKI_LOG(trace, "Successfully received {} bytes.", bytes_transferred);

        if (http::to_status_class(res_.result_int()) ==
            http::status_class::successful) {
            std::shared_ptr<std::string> body =
                std::make_shared<std::string>(res_.body());
            trigger_callback(SNodeError::NO_ERROR, std::move(body));
        } else {
            LOKI_LOG(error, "Http request failed, error code: {}",
                     res_.result_int());
            trigger_callback(SNodeError::HTTP_ERROR, nullptr);
        }

    } else {

        if (ec != boost::asio::error::operation_aborted) {
            LOKI_LOG(error, "Error on read: {}. Message: {}", ec.value(),
                     ec.message());
        }
        trigger_callback(SNodeError::ERROR_OTHER, nullptr);
    }
}

void HttpClientSession::start() {
    socket_.async_connect(endpoint_, [this, self = shared_from_this()](
                                         const error_code& ec) {
        /// TODO: I think I should just call again if ec == EINTR
        if (ec) {
            // We should make sure that we print the error a few levels above,
            // where we have more context

            if (ec == boost::system::errc::connection_refused) {
                LOKI_LOG(debug,
                         "[http client]: could not connect to {}:{}, message: "
                         "{} ({})",
                         endpoint_.address().to_string(), endpoint_.port(),
                         ec.message(), ec.value());
            } else {
                LOKI_LOG(error,
                         "[http client]: could not connect to {}:{}, message: "
                         "{} ({})",
                         endpoint_.address().to_string(), endpoint_.port(),
                         ec.message(), ec.value());
            }

            trigger_callback(SNodeError::NO_REACH, nullptr);
            return;
        }

        self->on_connect();
    });

    deadline_timer_.expires_after(SESSION_TIME_LIMIT);
    deadline_timer_.async_wait(
        [self = shared_from_this()](const error_code& ec) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    LOKI_LOG(
                        error,
                        "Deadline timer failed in http client session [{}: {}]",
                        ec.value(), ec.message());
                }
            } else {
                LOKI_LOG(debug, "client socket timed out");
                self->clean_up();
            }
        });
}

void HttpClientSession::trigger_callback(SNodeError error,
                                         std::shared_ptr<std::string>&& body) {
    LOKI_LOG(trace, "Trigger callback");
    ioc_.post(std::bind(callback_, sn_response_t{error, body, boost::none}));
    used_callback_ = true;
    deadline_timer_.cancel();
}

void HttpClientSession::clean_up() {

    if (!needs_cleanup) {
        // This can happen because the deadline timer
        // triggered and cleaned up the connection already
        LOKI_LOG(debug, "No need for cleanup");
        return;
    }

    needs_cleanup = false;

    if (!socket_.is_open()) {
        /// This should never happen!
        LOKI_LOG(critical, "Socket is already closed");
        return;
    }

    error_code ec;

    /// From boost documentation: "For portable behaviour with respect to
    /// graceful closure of a connected socket, call shutdown() before closing
    /// the socket."
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    // not_connected happens sometimes so don't bother reporting it.
    if (ec && ec != boost::system::errc::not_connected) {
        LOKI_LOG(error, "Socket shutdown failure [{}: {}]", ec.value(),
                 ec.message());
    }

    const auto sockfd = socket_.native_handle();
    socket_.close(ec);

    if (ec) {
        LOKI_LOG(error, "Closing socket {} failed [{}: {}]", sockfd, ec.value(),
                 ec.message());
    } else {
        LOKI_LOG(trace, "Close http socket: {}", sockfd);
        get_net_stats().record_socket_close(sockfd);
    }
}

/// We execute callback (if haven't already) here to make sure it is called
HttpClientSession::~HttpClientSession() {

    if (!used_callback_) {
        // If we destroy the session before posting the callback,
        // it must be due to some error
        ioc_.post(std::bind(callback_,
                            sn_response_t{SNodeError::ERROR_OTHER, nullptr}));
    }

    get_net_stats().http_connections_out--;

    this->clean_up();
}

} // namespace loki
