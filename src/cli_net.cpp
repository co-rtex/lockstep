// `enginex serve` (TCP server) and `enginex client` (submit / subscribe / snapshot).
#include <pthread.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

#include "cli.hpp"
#include "enginex/canonical.hpp"
#include "enginex/client.hpp"
#include "enginex/server.hpp"

namespace enginex::cli {

namespace {

using codec::Json;
using namespace std::chrono_literals;

std::uint16_t port_arg(const Args& args, std::uint64_t fallback) {
    const std::uint64_t port = args.get_u64("port", fallback);
    if (port > 65535) {
        throw UsageError("--port must be in [0, 65535]");
    }
    return static_cast<std::uint16_t>(port);
}

Json stats_json(const net::ServerStats& s) {
    return Json{{"connections_accepted", s.connections_accepted},
                {"connections_rejected", s.connections_rejected},
                {"requests", s.requests},
                {"protocol_errors", s.protocol_errors},
                {"overloaded", s.overloaded},
                {"slow_consumer_disconnects", s.slow_consumer_disconnects},
                {"batches", s.batches},
                {"commands_staged", s.commands_staged},
                {"events_published", s.events_published},
                {"max_batch_seen", s.max_batch_seen}};
}

}  // namespace

int cmd_serve(const std::vector<std::string>& argv) {
    const Args args(argv,
                    {"host", "port", "journal", "durability", "symbols", "max-clients",
                     "ingress-capacity", "outbound-capacity", "max-batch", "send-timeout-ms"},
                    {"refuse-tail"});
    net::ServerOptions opts;
    opts.host = args.get_or("host", "127.0.0.1");
    opts.port = port_arg(args, 9000);
    if (const auto symbols = args.get("symbols")) {
        opts.state.symbols = split_list(*symbols);
    }
    opts.max_clients = args.get_u64("max-clients", 64);
    opts.ingress_capacity = args.get_u64("ingress-capacity", 4096);
    opts.outbound_capacity = args.get_u64("outbound-capacity", 1024);
    opts.max_batch = args.get_u64("max-batch", 512);
    opts.send_timeout = std::chrono::milliseconds(args.get_u64("send-timeout-ms", 2000));
    std::string durability = "memory";
    if (const auto path = args.get("journal")) {
        DurableOptions d;
        d.journal_path = *path;
        const auto mode = journal::parse_sync_mode(args.get_or("durability", "sync"));
        if (!mode) {
            throw UsageError("--durability must be sync, fsync or none");
        }
        d.sync = *mode;
        d.state = opts.state;
        d.tail_policy = args.flag("refuse-tail") ? TailPolicy::Refuse : TailPolicy::Repair;
        if (const char* spec = std::getenv("ENGINEX_FAULT"); spec != nullptr && *spec != '\0') {
            std::cerr << "enginex serve: WARNING: fault injection active (ENGINEX_FAULT=" << spec
                      << "); for tests and demos only\n";
            d.faults = journal::FaultPlan::parse(spec);
        }
        opts.journal = d;
        durability = std::string(journal::to_string(*mode));
    } else if (args.get("durability")) {
        throw UsageError("--durability requires --journal");
    }

    // Block SIGINT/SIGTERM before any thread starts so only sigwait() below receives them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    net::ignore_sigpipe();

    std::unique_ptr<net::Server> server;
    try {
        server = std::make_unique<net::Server>(opts);
    } catch (const journal::JournalError& e) {
        std::cout << Json{{"type", "journal_error"},
                          {"error", {{"code", to_string(e.code())}, {"message", e.what()}}}}
                         .dump()
                  << std::endl;
        return e.code() == ErrorCode::JournalCorrupt ? 1 : 4;
    }
    server->start();
    const RecoveryReport& rep = server->recovery();
    Json ready{{"type", "listening"},
               {"host", opts.host},
               {"port", server->port()},
               {"journal", opts.journal ? opts.journal->journal_path : ""},
               {"durability", durability},
               {"records_replayed", rep.records_replayed},
               {"tail", rep.tail == journal::TailState::Clean ? "clean" : "incomplete"},
               {"pid", ::getpid()}};
    if (rep.tail_repaired) {
        ready["preserved_tail_path"] = rep.preserved_tail_path;
    }
    std::cout << ready.dump() << std::endl;

    int sig = 0;
    sigwait(&signals, &sig);
    std::cerr << "enginex serve: signal " << sig << ", shutting down\n";
    server->stop();
    Json bye{{"type", "shutdown"},
             {"signal", sig},
             {"journal_failed", server->journal_failed()},
             {"stats", stats_json(server->stats())}};
    bye.update(codec::summary_to_json(server->state()));
    std::cout << bye.dump() << std::endl;
    return server->journal_failed() ? 4 : 0;
}

namespace {

int client_submit(const Args& args) {
    const std::string host = args.get_or("host", "127.0.0.1");
    const auto timeout = std::chrono::milliseconds(args.get_u64("timeout-ms", 10000));
    net::Client client = net::Client::connect(host, port_arg(args, 9000));
    std::ifstream file;
    std::istream* in = &std::cin;
    const std::string input = args.get_or("input", "-");
    if (input != "-") {
        file.open(input);
        if (!file) {
            throw UsageError("cannot open input file: " + input);
        }
        in = &file;
    }
    std::string line;
    int queries = 0;
    int failures = 0;
    while (std::getline(*in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        Json msg = Json::parse(line, nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) {
            std::cout << Json{{"type", "client_error"}, {"message", "malformed input line"}}.dump()
                      << std::endl;
            ++failures;
            continue;
        }
        // Accept simulate-style query lines too.
        if (msg.value("type", "") == "snapshot") {
            msg["type"] = "snapshot_request";
        }
        if (!msg.contains("request_id")) {
            msg["request_id"] = "q" + std::to_string(++queries);
        }
        auto reply = client.request(msg, timeout);
        if (!reply) {
            std::cout << Json{{"type", "client_error"},
                              {"request_id", msg["request_id"]},
                              {"message", client.closed() ? "connection closed" : "timed out"}}
                             .dump()
                      << std::endl;
            return 1;  // outcome unknown: the caller must retry with the same request ids
        }
        std::cout << reply->dump() << std::endl;
    }
    return failures == 0 ? 0 : 1;
}

int client_subscribe(const Args& args) {
    const std::string host = args.get_or("host", "127.0.0.1");
    const std::uint64_t count = args.get_u64("count", 0);  // 0 = until EOF/duration
    const auto duration = std::chrono::milliseconds(args.get_u64("duration-ms", 0));
    const auto slow = std::chrono::milliseconds(args.get_u64("slow-ms", 0));
    const bool quiet = args.flag("quiet");
    net::Client client = net::Client::connect(host, port_arg(args, 9000));
    if (!client.send(Json{{"type", "subscribe"}, {"request_id", "subscribe"}})) {
        throw std::runtime_error("cannot send subscribe");
    }
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t expected = 0;
    std::uint64_t events = 0;
    std::uint64_t gaps = 0;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    bool resync = false;
    while (true) {
        if (duration.count() > 0 && std::chrono::steady_clock::now() - start > duration) {
            break;
        }
        auto msg = client.receive(duration.count() > 0 ? 200ms : 3600s);
        if (!msg) {
            if (client.closed()) {
                break;
            }
            continue;
        }
        const std::string type = msg->value("type", "");
        if (type == "snapshot_response") {
            expected = (*msg)["event_watermark"].get<std::uint64_t>() + 1;
        } else if (type == "book_update") {
            const auto seq = (*msg)["event_sequence"].get<std::uint64_t>();
            if (seq != expected) {
                ++gaps;
            }
            expected = seq + 1;
            first = first == 0 ? seq : first;
            last = seq;
            ++events;
        } else if (type == "resync_required") {
            resync = true;
        }
        if (!quiet || type != "book_update") {
            std::cout << msg->dump() << std::endl;
        }
        if (resync || (count > 0 && events >= count)) {
            break;
        }
        if (slow.count() > 0) {
            std::this_thread::sleep_for(slow);
        }
    }
    std::cout << Json{{"type", "subscriber_summary"},
                      {"events", events},
                      {"first_event_sequence", first},
                      {"last_event_sequence", last},
                      {"gaps", gaps},
                      {"resync_required", resync},
                      {"disconnected", client.closed()}}
                     .dump()
              << std::endl;
    return gaps == 0 ? 0 : 1;
}

int client_snapshot(const Args& args) {
    net::Client client =
        net::Client::connect(args.get_or("host", "127.0.0.1"), port_arg(args, 9000));
    Json req{{"type", "snapshot_request"},
             {"request_id", "snapshot"},
             {"depth", args.get_u64("depth", 10)}};
    if (const auto symbol = args.get("symbol")) {
        req["symbol"] = *symbol;
    }
    auto reply = client.request(req, 10s);
    if (!reply) {
        std::cerr << "enginex client snapshot: no reply\n";
        return 1;
    }
    std::cout << reply->dump(2) << std::endl;
    return reply->value("type", "") == "snapshot_response" ? 0 : 1;
}

}  // namespace

int cmd_client(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        throw UsageError("expected a subcommand: submit, subscribe or snapshot");
    }
    net::ignore_sigpipe();
    const std::string sub = argv[0];
    const std::vector<std::string> rest(argv.begin() + 1, argv.end());
    if (sub == "submit") {
        return client_submit(Args(rest, {"host", "port", "input", "timeout-ms"}, {}));
    }
    if (sub == "subscribe") {
        return client_subscribe(
            Args(rest, {"host", "port", "count", "duration-ms", "slow-ms"}, {"quiet"}));
    }
    if (sub == "snapshot") {
        return client_snapshot(Args(rest, {"host", "port", "symbol", "depth"}, {}));
    }
    throw UsageError("unknown client subcommand: " + sub);
}

}  // namespace enginex::cli
