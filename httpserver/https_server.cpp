#include "https_server.h"

#include "http.h"
#include "oxen_logger.h"
#include "request_handler.h"
#include "service_node.h"
#include "omq_server.h"
#include "signature.h"
#include "string_utils.hpp"
#include "file.hpp"

#include <boost/endian/conversion.hpp>
#include <chrono>
#include <oxenmq/base32z.h>
#include <oxenmq/base64.h>
#include <oxenmq/hex.h>
#include <oxenmq/oxenmq.h>
#include <nlohmann/json.hpp>

namespace oxen {

using nlohmann::json;

// Sends an error response and finalizes the response.
void HTTPSServer::error_response(
        HttpResponse& res,
        http::response_code code,
        std::optional<std::string_view> body) const {
    res.writeStatus(std::to_string(code.first) + " " + std::string{code.second});
    add_generic_headers(res);
    res.writeHeader("Content-Type", "text/plain");
    if (closing_) res.writeHeader("Connection", "close");
    if (body)
        res.end(*body);
    else
        res.end(std::string{code.second} + "\n");
    if (closing_) res.close();
}


void HTTPSServer::handle_cors(HttpRequest& req, http::headers& extra_headers) {
    if (cors_any_)
        extra_headers.emplace("Access-Control-Allow-Origin", "*");
    else if (!cors_.empty()) {
        if (std::string origin{req.getHeader("origin")}; !origin.empty() && cors_.count(origin)) {
            extra_headers.emplace("Access-Control-Allow-Origin", "*");
            extra_headers.emplace("Vary", "Origin");
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------
HTTPSServer::HTTPSServer(
        ServiceNode& sn,
        RequestHandler& rh,
        std::vector<std::tuple<std::string, uint16_t, bool>> bind,
        const std::filesystem::path& ssl_cert,
        const std::filesystem::path& ssl_key,
        const std::filesystem::path& ssl_dh,
        const legacy_keypair& sn_keys
        ) :
    service_node_{sn},
    omq_{*service_node_.omq_server()},
    request_handler_{rh},
    cert_signature_{oxenmq::to_base64(util::view_guts(
        generate_signature(hash_data(slurp_file(ssl_cert)), sn_keys)
    ))}
{

    // uWS is designed to work from a single thread, which is good (we pull off the requests and
    // then stick them into the LMQ job queue to be scheduled along with other jobs).  But as a
    // consequence, we need to create everything inside that thread.  We *also* need to get the
    // (thread local) event loop pointer back from the thread so that we can shut it down later
    // (injecting a callback into it is one of the few thread-safe things we can do across threads).
    //
    // Things we need in the owning thread, fulfilled from the http thread:

    // - the uWS::Loop* for the event loop thread (which is thread_local).  We can get this during
    //   thread startup, after the thread does basic initialization.
    std::promise<uWS::Loop*> loop_promise;
    auto loop_future = loop_promise.get_future();

    // - the us_listen_socket_t* on which the server is listening.  We can't get this until we
    //   actually start listening, so wait until `start()` for it.  (We also double-purpose it to
    //   send back an exception if one fires during startup).
    std::promise<std::vector<us_listen_socket_t*>> startup_success_promise;
    startup_success_ = startup_success_promise.get_future();

    // Things we need to send from the owning thread to the event loop thread:
    // - a signal when the thread should bind to the port and start the event loop (when we call
    //   start()).
    //startup_promise_

    uWS::SocketContextOptions https_opts{
        .key_file_name = ssl_key.c_str(),
        .cert_file_name = ssl_cert.c_str(),
        .dh_params_file_name = ssl_dh.c_str()};

    server_thread_ = std::thread{[this, bind=std::move(bind), &https_opts] (
            std::promise<uWS::Loop*> loop_promise,
            std::future<bool> startup_future,
            std::promise<std::vector<us_listen_socket_t*>> startup_success) {
        uWS::SSLApp https{https_opts};
        try {
            create_endpoints(https);
        } catch (...) {
            loop_promise.set_exception(std::current_exception());
            return;
        }
        // We've initialized, signal the calling thread
        loop_promise.set_value(uWS::Loop::get());
        // Now wait until we get the signal to go (sent when the caller calls start() call).
        if (!startup_future.get())
            // False means cancel, i.e. we got destroyed/shutdown without start() being called
            return;

        // we don't currently do cors
        //cors_ = {...};

        std::vector<us_listen_socket_t*> listening;
        try {
            bool required_bind_failed = false;
            for (const auto& [addr, port, required] : bind)
                https.listen(addr, port, [&listening, req=required, &required_bind_failed](us_listen_socket_t* sock) {
                    if (sock) listening.push_back(sock);
                    else if (req) required_bind_failed = true;
                });

            if (listening.empty() || required_bind_failed) {
                std::ostringstream error;
                error << "RPC HTTP server failed to bind; ";
                if (listening.empty()) error << "no valid bind address(es) given";
                error << "tried to bind to:";
                for (const auto& [addr, port, required] : bind)
                    error << ' ' << addr << ':' << port;
                throw std::runtime_error{error.str()};
            }
        } catch (...) {
            startup_success.set_exception(std::current_exception());
            return;
        }
        startup_success.set_value(std::move(listening));

        https.run();
    }, std::move(loop_promise), startup_promise_.get_future(), std::move(startup_success_promise)};

    loop_ = loop_future.get();
}

bool HTTPSServer::check_ready(HttpResponse& res) {
    if (std::string reason; !service_node_.snode_ready(&reason)) {
        OXEN_LOG(debug, "Storage server not ready ({}), replying with 503", reason);
        error_response(res, http::SERVICE_UNAVAILABLE, "Service node is not ready: " + reason + "\n");
        return false;
    }
    return true;
}

void HTTPSServer::add_generic_headers(HttpResponse& res) const {
    res.writeHeader("Server", server_header());
    res.writeHeader("X-Loki-Snode-Signature", cert_signature_);
}

namespace {

    struct call_data {
        HTTPSServer& https;
        oxenmq::OxenMQ& omq;
        HttpResponse& res;
        Request request;
        std::vector<std::pair<std::string, std::string>> extra_headers;
        bool aborted{false};
        bool replied{false};

        // If we have to drop the request because we are overloaded we want to reply with an error (so
        // that we close the connection instead of leaking it and leaving it hanging).  We don't do
        // this, of course, if the request got aborted and replied to.
        ~call_data() {
            if (replied || aborted) return;
            https.loop_defer([&https=https, &res=res] {
                https.error_response(res, http::SERVICE_UNAVAILABLE, "Server busy, try again later");
            });
        }

        call_data(const call_data&) = delete;
        call_data(call_data&&) = delete;
        call_data& operator=(const call_data&) = delete;
        call_data& operator=(call_data&&) = delete;

        template <typename... T>
        auto error_response(T&&... args) {
            if (replied || aborted) return;
            replied = true;
            return https.error_response(std::forward<T>(args)...);
        }
    };

    // Queues a response with the uWebSockets response object; this must only be called from the
    // http thread (typically you want to use `queue_response` instead).
    void queue_response_internal(HTTPSServer& https, HttpResponse& r, Response res, bool force_close = false) {
        r.cork([&https, &r, res=std::move(res), force_close] {
            r.writeStatus(std::to_string(res.status.first) + " " + std::string{res.status.second});
            https.add_generic_headers(r);
            if (!res.content_type.empty())
                r.writeHeader("Content-Type", res.content_type);
            for (const auto& [h, v] : res.headers)
                r.writeHeader(h, v);
            r.end(res.body, force_close || https.closing());
        });
    }

    // Queues a response for the HTTP thread to handle; the response can be in multiple string pieces
    // to be concatenated together.
    void queue_response(std::shared_ptr<call_data> data, Response res, bool force_close = false)
    {
        if (!data || data->replied) return;
        data->replied = true;
        data->https.loop_defer([data=std::move(data), res=std::move(res), force_close] () mutable {
            if (data->aborted)
                return;
            queue_response_internal(data->https, data->res, std::move(res), force_close);
        });
    }

    std::string get_remote_address(HttpResponse& res) {
        std::ostringstream result;
        bool first = true;
        auto addr = res.getRemoteAddress();
        if (addr.size() == 4)
        { // IPv4, packed into bytes
            for (auto c : addr) {
                if (first) first = false;
                else result << '.';
                result << +static_cast<uint8_t>(c);
            }
        }
        else if (addr.size() == 16)
        {
            // IPv6, packed into bytes.  Interpret as a series of 8 big-endian shorts and convert to hex,
            // joined with :.  But we also want to drop leading insignificant 0's (i.e. '34f' instead of
            // '034f'), and we want to collapse the longest sequence of 0's that we come across (so that,
            // for example, localhost becomes `::1` instead of `0:0:0:0:0:0:0:1`).
            std::array<uint16_t, 8> a;
            std::memcpy(a.data(), addr.data(), 16);
            for (auto& x : a) boost::endian::big_to_native_inplace(x);

            size_t zero_start = 0, zero_end = 0;
            for (size_t i = 0, start = 0, end = 0; i < a.size(); i++) {
                if (a[i] != 0)
                    continue;
                if (end != i) // This zero value starts a new zero sequence
                    start = i;
                end = i + 1;
                if (end - start > zero_end - zero_start)
                {
                    zero_end = end;
                    zero_start = start;
                }
            }
            result << '[' << std::hex;
            for (size_t i = 0; i < a.size(); i++)
            {
                if (i >= zero_start && i < zero_end)
                {
                    if (i == zero_start) result << "::";
                    continue;
                }
                if (i > 0 && i != zero_end)
                    result << ':';
                result << a[i];
            }
            result << ']';
        }
        else
            result << "{unknown:" << oxenmq::to_hex(addr) << "}";
        return result.str();
    }

    // Extracts a x25519 pubkey from a hex string. Warns and throws on invalid input.
    x25519_pubkey extract_x25519_from_hex(std::string_view hex) {
        try {
            return x25519_pubkey::from_hex(hex);
        } catch (const std::exception& e) {
            OXEN_LOG(warn, "Failed to decode ephemeral key in onion request: {}", e.what());
            throw;
        }
    }

    // Sets up a request handler that processes the initial incoming requests, sets up the appropriate
    // handlers for incoming data, and invokes the `ready` callback once all data has been received
    // (i.e. when the request is complete).  Can optionally call `prevalidate` on the partial
    // call_data: it will have everything except for the body set (and can be used, for instance, to
    // abort a request based only on headers); it will also be called from the same thread calling
    // handle_request (typically the http thread), *not* a worker thread.
    template <typename ReadyCallback>
    static void handle_request(
            HTTPSServer& https,
            oxenmq::OxenMQ& omq,
            HttpRequest& req,
            HttpResponse& res,
            ReadyCallback ready,
            std::function<void(call_data& c)> prevalidate = nullptr) {

        if (auto len = req.getHeader("content-length"); !len.empty()) {
            if (uint64_t length; !util::parse_int(len, length)) {
                OXEN_LOG(warn, "Received HTTPS request from {} with invalid Content-Length, dropping",
                        get_remote_address(res));
                queue_response_internal(https, res, Response{http::BAD_REQUEST, "invalid Content-Length"}, true);
            } else if (length > MAX_REQUEST_BODY_SIZE) {
                OXEN_LOG(warn, "Received HTTPS request from {} with too-large body ({} > {}), dropping",
                        get_remote_address(res), length, MAX_REQUEST_BODY_SIZE);
                queue_response_internal(https, res, Response{http::PAYLOAD_TOO_LARGE}, true);
            }
        }

        std::shared_ptr<call_data> data{new call_data{https, omq, res}};
        auto& request = data->request;
        request.remote_addr = get_remote_address(res);
        request.uri = req.getUrl();
        for (const auto& [header, value] : req)
            request.headers[std::string{header}] = value;

        https.handle_cors(req, request.headers);
        OXEN_LOG(debug, "Received {} {} request from {}", req.getMethod(), request.uri, request.remote_addr);

        if (prevalidate) prevalidate(*data);

        res.onAborted([data] { data->aborted = true; });
        res.onData([data=std::move(data), ready=std::move(ready)](std::string_view d, bool done) mutable {
            data->request.body += d;
            if (done)
                ready(std::move(data));
        });
    }

} // anonymous namespace


void HTTPSServer::create_endpoints(uWS::SSLApp& https)
{
    https.post("/swarms/ping_test/v1", [this](HttpResponse* res, HttpRequest* req) {
        OXEN_LOG(trace, "Received https ping_test");
        service_node_.update_last_ping(ReachType::HTTPS);
        add_generic_headers(*res);
        res->end();
    });

    https.post("/swarms/storage_test/v1", [this](HttpResponse* res, HttpRequest* req) {
        if (!check_ready(*res)) return;

        process_storage_test_req(*req, *res);
    });
    https.post("/storage_rpc/v1", [this](HttpResponse* res, HttpRequest* req) {
        if (!check_ready(*res)) return;
        OXEN_LOG(trace, "POST /storage_rpc/v1");
        process_storage_rpc_req(*req, *res);

    });
    https.post("/onion_req/v2", [this](HttpResponse* res, HttpRequest* req) {
        if (!check_ready(*res)) return;
        OXEN_LOG(trace, "POST /onion_req/v2");
        process_onion_req_v2(*req, *res);
    });
    https.get("/get_stats/v1", [this](HttpResponse* res, HttpRequest* req) {
        queue_response_internal(*this, *res, Response{
            http::OK, nlohmann::json{{"version", STORAGE_SERVER_VERSION_STRING}}.dump()});
    });

        

#ifdef INTEGRATION_TEST

    https.post("/retrieve_all", [this](HttpResponse* res, HttpRequest* req) {
        handle_request(req, res, [this, started=std::chrono::steady_clock::now()]
                (std::shared_ptr<call_data> data) mutable {
            data->omq.inject_task("https", "https:" + data->uri, data->remote_addr,
                    [data=std::move(data), started] mutable {

                queue_response(std::move(data), request_handler_.process_retrieve_all());
            });
        });
    });

    https.post("/quit", [this](HttpResponse* res, HttpRequest* req) {
        OXEN_LOG(info, "POST /quit");
        res.cork([this, &res] {
            res.writeStatus("200 OK");
            res.end();
            service_node_.shutdown();
        });
    });

#endif

    // Fallback to send a 404 for anything else:
    https.any("/*", [this](HttpResponse* res, HttpRequest* req) {
        OXEN_LOG(info, "Invalid HTTP request for {} {} from {}",
                req->getMethod(), req->getUrl(), get_remote_address(*res));
        error_response(*res, http::NOT_FOUND, fmt::format("{} {} Not Found", req->getMethod(), req->getUrl()));
    });
}


static void handle_storage_test_impl(
        std::shared_ptr<call_data> data,
        MessageTestStatus status,
        std::string answer,
        std::chrono::nanoseconds elapsed) {
    switch (status) {
        case MessageTestStatus::SUCCESS:
            OXEN_LOG(debug, "Storage test success after {}",
                    util::friendly_duration(elapsed));
            return queue_response(std::move(data), {http::OK,
                json{
                    {"status", "OK"},
                    {"value", std::move(answer)}
                }.dump()});
        case MessageTestStatus::WRONG_REQ:
            return queue_response(std::move(data), {http::OK,
                json{
                    {"status", "wrong request"}
                }.dump()});
        case MessageTestStatus::RETRY:
            [[fallthrough]]; // If we're getting called then a retry ran out of time
        case MessageTestStatus::ERROR:
            // Promote this to `error` once we enforce storage testing
            OXEN_LOG(debug, "Failed storage test, tried for {}", util::friendly_duration(elapsed));
            return queue_response(std::move(data), {http::OK,
                json{
                    {"status", "other"}
                }.dump()});
    }
}

void HTTPSServer::process_storage_test_req(HttpRequest& req, HttpResponse& res) {

    auto check_snode_headers = [this, &res](call_data& data) {
        // Before we read the body make sure we have the required headers (so that we can reject bad
        // requests earlier).
        if (auto prevalidate = request_handler_.validate_snode_signature(data.request, true);
                std::holds_alternative<Response>(prevalidate)) {
            queue_response_internal(*this, res, std::move(std::get<Response>(prevalidate)));
            data.replied = true;
        } else {
            assert(std::holds_alternative<legacy_pubkey>(prevalidate));
            if (rate_limiter_.should_rate_limit(std::get<legacy_pubkey>(prevalidate))) {
                queue_response_internal(*this, res,
                        Response{http::TOO_MANY_REQUESTS, "too many requests from this snode"});
                data.replied = true;
            }
        }
    };

    handle_request(*this, omq_, req, res, [this](std::shared_ptr<call_data> data) mutable {
        // Now that we have the body, fully validate the snode signature:
        if (auto validate = request_handler_.validate_snode_signature(data->request);
                std::holds_alternative<Response>(validate))
            return queue_response(std::move(data), std::move(std::get<Response>(validate)));

        data->omq.inject_task("https", "https:" + data->request.uri, data->request.remote_addr,
                [this, data=std::move(data)] () mutable {

            if (data->replied || data->aborted) return;

            auto& req = data->request;

            legacy_pubkey tester_pk;
            if (auto it = req.headers.find(http::SNODE_SENDER_HEADER); it != req.headers.end()) {
                if (tester_pk = parse_pubkey(it->second); !tester_pk) {
                    OXEN_LOG(debug, "Invalid test request: invalid pubkey");
                    return queue_response(std::move(data), {http::BAD_REQUEST, "invalid tester pubkey header"});
                }
            } else {
                OXEN_LOG(debug, "Invalid test request: missing pubkey");
                return queue_response(std::move(data), {http::BAD_REQUEST, "missing tester pubkey header"});
            }

            auto body = json::parse(data->request.body, nullptr, false);
            if (body.is_discarded()) {
                OXEN_LOG(debug, "Bad snode test request: invalid json");
                return queue_response(std::move(data), {http::BAD_REQUEST, "invalid json"});
            }

            uint64_t height;
            std::string msg_hash;
            try {
                height = body.at("height").get<uint64_t>();
                msg_hash = body.at("hash").get<std::string>();
            } catch (...) {
                std::string msg = "Bad snode test request: missing fields in json"s;
                OXEN_LOG(debug, "{}", msg);
                return queue_response(std::move(data), {http::BAD_REQUEST, std::move(msg)});
            }


            /// TODO: we never actually test that `height` is within any reasonable
            /// time window (or that it is not repeated multiple times), we should do
            /// that! This is done implicitly to some degree using
            /// `block_hashes_cache_`, which holds a limited number of recent blocks
            /// only and fails if an earlier block is requested

            auto started = std::chrono::steady_clock::now();
            auto [status, answer] = service_node_.process_storage_test_req(
                    height, tester_pk, msg_hash);

            // FIXME: need to cancel this timer if we're trying to shut down so that we don't have to
            // wait up to a minute for it.
            if (status == MessageTestStatus::RETRY) {
                // Our first attempt returned a RETRY, so set up a timer to keep retrying

                auto timer = std::make_shared<oxenmq::TimerID>();
                auto& timer_ref = *timer;
                data->omq.add_timer(timer_ref, [this, data=std::move(data), timer=std::move(timer), height, tester_pk, msg_hash, started] {
                    if (data->replied || data->aborted)
                        return data->omq.cancel_timer(*timer);

                    auto elapsed = std::chrono::steady_clock::now() - started;
                    OXEN_LOG(trace, "Performing storage test retry, {} since started",
                            util::friendly_duration(elapsed));

                    auto [status, answer] = service_node_.process_storage_test_req(
                            height, tester_pk, msg_hash);
                    if (status == MessageTestStatus::RETRY && elapsed < TEST_RETRY_PERIOD)
                        return; // Still retrying so wait for the next call
                    data->omq.cancel_timer(*timer);
                    handle_storage_test_impl(std::move(data), status, std::move(answer), elapsed);
                }, TEST_RETRY_INTERVAL);
            } else {
                handle_storage_test_impl(std::move(data), status, std::move(answer), 0s);
            }
        });
    }, std::move(check_snode_headers));
}

bool HTTPSServer::should_rate_limit_client(std::string_view addr) {
    if (addr.size() != 4) return true;
    uint32_t ip;
    std::memcpy(&ip, addr.data(), 4);
    boost::endian::big_to_native_inplace(ip);
    return rate_limiter_.should_rate_limit_client(ip);
}

void HTTPSServer::process_storage_rpc_req(HttpRequest& req, HttpResponse& res) {
    auto addr = res.getRemoteAddress();
    if (addr.size() != 4) {
        // We don't (currently?) support IPv6 at all (SS published IPs are only IPv4) so if we
        // somehow get an IPv6 address then it isn't a proper SS request so just drop it.
        OXEN_LOG(warn, "incoming client request is not IPv4; dropping it");
        return error_response(res, http::BAD_REQUEST);
    }
    if (should_rate_limit_client(addr)) {
        OXEN_LOG(debug, "Rate limiting client request from {}", get_remote_address(res));
        return error_response(res, http::TOO_MANY_REQUESTS);
    }
    if (!req.getHeader("x-loki-long-poll").empty()) {
        // Obsolete header, return an error code
        return error_response(res, http::GONE, "long polling is no longer supported, client upgrade required");
    }

    handle_request(*this, omq_, req, res, [this, started=std::chrono::steady_clock::now()]
            (std::shared_ptr<call_data> data) mutable {
        data->omq.inject_task("https", "https:" + data->request.uri, data->request.remote_addr,
                [this, data=std::move(data), started] () mutable {

            if (data->replied || data->aborted) return;

            try {
                request_handler_.process_client_req(data->request.body,
                        [data, started](Response response) mutable {
                    OXEN_LOG(debug, "Responding to a client request after {}",
                            util::friendly_duration(std::chrono::steady_clock::now() - started));
                    queue_response(std::move(data), std::move(response));
                });
            } catch (const std::exception& e) {
                auto error = "Exception caught with processing client request: "s + e.what();
                OXEN_LOG(critical, "{}", error);
                queue_response(std::move(data), {http::INTERNAL_SERVER_ERROR, error});
            }
        });
    });
}

void HTTPSServer::process_onion_req_v2(HttpRequest& req, HttpResponse& res) {
    handle_request(*this, omq_, req, res, [this, started=std::chrono::steady_clock::now()]
            (std::shared_ptr<call_data> data) mutable {
        auto& omq = data->omq;
        auto& request = data->request;
        omq.inject_task("https", "https:" + request.uri, request.remote_addr,
                [this, data=std::move(data), started] () mutable {

            if (data->replied || data->aborted) return;

            OnionRequestMetadata onion{
                x25519_pubkey{},
                [data, started](Response res) {
                    OXEN_LOG(debug, "Got an onion response ({} {}) as edge node (after {})",
                            res.status.first, res.status.second,
                            util::friendly_duration(std::chrono::steady_clock::now() - started));
                    queue_response(std::move(data), std::move(res));
                },
                0, // hopno
                EncryptType::aes_gcm,
            };

            try {
                auto [ciphertext, json_req] = parse_combined_payload(data->request.body);

                onion.ephem_key = extract_x25519_from_hex(
                        json_req.at("ephemeral_key").get_ref<const std::string&>());

                if (auto it = json_req.find("enc_type"); it != json_req.end())
                    onion.enc_type = parse_enc_type(it->get_ref<const std::string&>());
                // Otherwise stay at default aes-gcm

                // Allows a fake starting hop number (to make it harder for intermediate hops to know where
                // they are).  If omitted, defaults to 0.
                if (auto it = json_req.find("hop_no"); it != json_req.end())
                    onion.hop_no = std::max(0, it->get<int>());

                service_node_.record_onion_request();
                request_handler_.process_onion_req(ciphertext, std::move(onion));
            } catch (const std::exception& e) {
                auto msg = fmt::format("Error parsing onion request: {}", e.what());
                OXEN_LOG(err, "{}", msg);
                queue_response(std::move(data), {http::BAD_REQUEST, msg});
            }
        });
    });
}

void HTTPSServer::start()
{
    if (sent_startup_)
        throw std::logic_error{"Cannot call HTTPSServer::start() more than once"};

    startup_promise_.set_value(true);
    sent_startup_ = true;
    listen_socks_ = startup_success_.get();
}

void HTTPSServer::shutdown(bool join)
{
    if (!server_thread_.joinable())
        return;

    if (!sent_shutdown_)
    {
        OXEN_LOG(trace, "initiating shutdown");
        if (!sent_startup_)
        {
            startup_promise_.set_value(false);
            sent_startup_ = true;
        }
        else if (!listen_socks_.empty())
        {
            loop_defer([this] {
                OXEN_LOG(trace, "closing {} listening sockets", listen_socks_.size());
                for (auto* s : listen_socks_)
                    us_listen_socket_close(/*ssl=*/true, s);
                listen_socks_.clear();

                closing_ = true;
            });
        }
        sent_shutdown_ = true;
    }

    OXEN_LOG(trace, "joining https server thread");
    if (join)
        server_thread_.join();
    OXEN_LOG(trace, "done shutdown");
}

HTTPSServer::~HTTPSServer()
{
    shutdown(true);
}


} // namespace oxen
