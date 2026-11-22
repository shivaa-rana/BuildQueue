#include "BatchQueue.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <json.hpp>
#include <string_view>

namespace {
using nlohmann::json;
using namespace bq;
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void requestStop(int) {
    stopRequested.store(true, std::memory_order_relaxed);
}

int number(std::string_view value, int min, int max) {
    int parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < min || parsed > max)
        throw std::invalid_argument("Invalid numeric option.");
    return parsed;
}

Policy readPolicy(const std::string& value) {
    if (value == "fcfs")
        return Policy::Fcfs;
    if (value == "priority")
        return Policy::Priority;
    if (value == "sjf")
        return Policy::Sjf;
    if (value == "rr")
        return Policy::RoundRobin;
    throw std::invalid_argument("Choose fcfs, priority, sjf, or rr scheduling.");
}

const char* policyName(Policy policy) {
    switch (policy) {
        case Policy::Fcfs: return "fcfs";
        case Policy::Priority: return "priority";
        case Policy::Sjf: return "sjf";
        case Policy::RoundRobin: return "rr";
    }
    return "fcfs";
}

const char* stateName(State state) {
    switch (state) {
        case State::Queued: return "Queued";
        case State::Running: return "Running";
        case State::Completed: return "Completed";
        case State::Failed: return "Failed";
        case State::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

double milliseconds(Job::Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

json describe(const Snapshot& snapshot) {
    auto now = Job::Clock::now();
    const auto origin = snapshot.jobs.empty() ? now : snapshot.jobs.front().submitted;
    auto jobs = json::array();
    for (const auto& job : snapshot.jobs) {
        bool started = job.started != Job::Clock::time_point{};
        bool finished = job.finished != Job::Clock::time_point{};
        auto slices = json::array();
        for (const auto& [start, end] : job.slices)
            slices.push_back({milliseconds(start - origin), milliseconds(end - origin)});
        if (job.state == State::Running && slices.size() < 256)
            slices.push_back({milliseconds(job.sliceStarted - origin), milliseconds(now - origin)});
        jobs.push_back({
            {"id", job.id}, {"file", job.file}, {"state", stateName(job.state)},
            {"submitted_ms", milliseconds(job.submitted - origin)},
            {"priority", job.priority}, {"wait_ms", milliseconds(job.waitTime(now))},
            {"estimated_bytes", job.estimatedBytes ? json(*job.estimatedBytes) : json(nullptr)},
            {"run_ms", started ? json(milliseconds(job.burstTime(now))) : json(nullptr)},
            {"completion_ms", finished ? json(milliseconds(job.finished - origin)) : json(nullptr)},
            {"turnaround_ms", finished ? json(milliseconds(job.finished - job.submitted)) : json(nullptr)},
            {"elapsed_ms", milliseconds((finished ? job.finished : now) - origin)},
            {"slices", std::move(slices)},
            {"timeline_truncated", job.timelineTruncated || (job.state == State::Running && job.slices.size() == 256)},
            {"pid", job.result.pid < 0 ? json(nullptr) : json(job.result.pid)},
            {"exit_code", job.result.exitCode < 0 ? json(nullptr) : json(job.result.exitCode)},
            {"checksum", job.state == State::Completed ? json(job.result.checksum) : json(nullptr)},
            {"memory_limit_bytes", job.result.addressSpaceLimitBytes
                ? json(job.result.addressSpaceLimitBytes) : json(nullptr)},
            {"peak_rss_kib", job.result.peakRssKiB ? json(*job.result.peakRssKiB) : json(nullptr)},
            {"bytes", job.state == State::Completed ? json(job.result.bytes) : json(nullptr)},
            {"error", job.result.error}
        });
    }
    return {{"jobs", jobs}, {"workers", snapshot.workers}, {"slots", snapshot.slots},
            {"running", snapshot.running}, {"peak", snapshot.peak},
            {"deadlocks", {{"detected", snapshot.deadlocks.count},
                           {"type", "Wait-for graph cycle detection"},
                           {"recovery", "Reject the lock request that would close the cycle."},
                           {"last_cycle", snapshot.deadlocks.lastCycle}}},
            {"policy", policyName(snapshot.policy)}};
}

void respond(httplib::Response& response, const json& value, int status = 200) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(value.dump(-1, ' ', false, json::error_handler_t::replace),
                         "application/json");
}

json readBody(const httplib::Request& request) {
    auto type = request.get_header_value("Content-Type");
    if (type != "application/json" && type.rfind("application/json;", 0) != 0)
        throw std::invalid_argument("Send an application/json request.");
    return json::parse(request.body);
}

int serve(BatchQueue& queue, const Checksum& checksum, const std::filesystem::path& web, int port) {
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(4); };
    server.set_socket_options([](socket_t socket) {
        int enabled = 1;
        ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    });
    server.set_default_headers({
        {"X-Content-Type-Options", "nosniff"},
        {"Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'"}
    });
    server.set_payload_max_length(64 * 1024);
    const auto suffix = ":" + std::to_string(port);
    server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
        const auto host = request.get_header_value("Host");
        if (host != "127.0.0.1" + suffix && host != "localhost" + suffix) {
            respond(response, {{"error", "Open BuildQueue through its localhost address."}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        const auto origin = request.get_header_value("Origin");
        if (request.method == "POST" && !origin.empty() && origin != "http://" + host) {
            respond(response, {{"error", "Cross-origin changes are forbidden."}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Get("/api/files", [&](const auto&, auto& response) {
        auto files = json::array();
        for (const auto& info : checksum.files()) {
            json item = {{"name", info.name}, {"size", info.size}};
            // File names sent to the browser must round-trip through UTF-8 JSON.
            try {
                item.dump();
                files.push_back(std::move(item));
            } catch (const json::type_error&) {}
        }
        respond(response, {{"files", files}});
    });
    server.Get("/api/jobs", [&](const auto&, auto& response) {
        respond(response, describe(queue.snapshot()));
    });
    server.Post("/api/jobs", [&](const auto& request, auto& response) {
        auto body = readBody(request);
        auto files = body.at("files").template get<std::vector<std::string>>();
        const auto priority = body.value("priority", json(0));
        if (!priority.is_number_integer())
            throw std::invalid_argument("Priority must be an integer from 0 to 9.");
        respond(response, {{"ids", queue.submit(files, number(priority.dump(), 0, 9))}}, 201);
    });
    server.Post("/api/policy", [&](const auto& request, auto& response) {
        auto body = readBody(request);
        queue.setPolicy(readPolicy(body.at("policy").template get<std::string>()));
        respond(response, {{"ok", true}});
    });
    server.Post("/api/cancel", [&](const auto& request, auto& response) {
        auto body = readBody(request);
        const auto& id = body.at("id");
        if (!id.is_number_unsigned())
            throw std::invalid_argument("Job ID must be a positive integer.");
        if (queue.cancel(id.template get<std::uint64_t>()))
            respond(response, {{"ok", true}});
        else
            respond(response, {{"error", "Only queued jobs can be cancelled."}}, 409);
    });
    server.Post("/api/clear", [&](const auto& request, auto& response) {
        readBody(request);
        queue.clearFinished();
        respond(response, {{"ok", true}});
    });
    server.set_exception_handler([](const auto&, auto& response, std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const json::exception&) {
            respond(response, {{"error", "Invalid JSON or request fields."}}, 400);
        } catch (const std::invalid_argument& exception) {
            respond(response, {{"error", exception.what()}}, 400);
        } catch (const std::exception& exception) {
            respond(response, {{"error", exception.what()}}, 500);
        }
    });
    if (!server.set_mount_point("/", web.string()))
        throw std::runtime_error("Could not find dashboard assets. Run from the project root or pass --web.");
    if (!server.bind_to_port("127.0.0.1", port))
        throw std::runtime_error("Could not bind dashboard socket.");
    auto previousInt = std::signal(SIGINT, requestStop);
    auto previousTerm = std::signal(SIGTERM, requestStop);
    std::atomic<bool> listenerDone{false};
    bool listened = false;
    std::thread listener([&] {
        listened = server.listen_after_bind();
        listenerDone.store(true, std::memory_order_release);
    });
    std::cout << "BuildQueue dashboard: http://127.0.0.1:" << port << std::endl;
    // Wait for listen_after_bind to enter its loop before calling stop().
    while (!listenerDone.load(std::memory_order_acquire) &&
           !(stopRequested.load(std::memory_order_relaxed) && server.is_running()))
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    server.stop();
    listener.join();
    std::signal(SIGINT, previousInt);
    std::signal(SIGTERM, previousTerm);
    return listened ? 0 : 1;
}

void help() {
    std::cout << "BuildQueue - Live batch file checksum manager\n"
                 "  ./buildqueue --input DIR --serve [--port 8081] [--web web]\n"
                 "  ./buildqueue --input DIR --file NAME [--file NAME ...]\n"
                 "  Both modes accept --workers 1..16, --slots 1..workers,\n"
                 "  and --policy fcfs|priority|sjf|rr (default: fcfs).\n"
                 "  SJF uses file size. CLI batches also accept --priority 0..9.\n"
                 "  --memory-mib 8..4096 caps each checksum process (default: 64).\n";
}
}

int main(int argc, char* argv[]) {
    try {
        std::filesystem::path input;
        std::filesystem::path web = "web";
        std::vector<std::string> files;
        int workers = 4, slots = 0, priority = 0, port = 8081;
        int memoryMiB = defaultMemoryMiB;
        Policy policy = Policy::Fcfs;
        bool dashboard = false;
        if (argc == 1) {
            help();
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            auto arg = [&]() -> std::string {
                if (++i == argc)
                    throw std::invalid_argument("Missing argument for " + option);
                return argv[i];
            };
            if (option == "--input") input = arg();
            else if (option == "--web") web = arg();
            else if (option == "--serve") dashboard = true;
            else if (option == "--file") files.push_back(arg());
            else if (option == "--workers") workers = number(arg(), 1, 16);
            else if (option == "--slots") slots = number(arg(), 1, 16);
            else if (option == "--memory-mib") memoryMiB = number(arg(), 8, 4096);
            else if (option == "--priority") priority = number(arg(), 0, 9);
            else if (option == "--port") port = number(arg(), 1024, 65535);
            else if (option == "--policy") policy = readPolicy(arg());
            else if (option == "--help") { help(); return 0; }
            else throw std::invalid_argument("Unknown option: " + option);
        }
        if (input.empty() || (dashboard && !files.empty()) || (!dashboard && files.empty()))
            throw std::invalid_argument("Specify --input DIR and either --serve or one or more --file options.");

        Checksum checksum(input);
        BatchQueue queue(workers, slots ? slots : std::min(workers, 2),
                         [&](const auto& name) {
                             return checksum.run(name, std::chrono::minutes(2), memoryMiB * bytesPerMiB);
                         }, Input::FolderNames,
                         [&](const auto& name) { return checksum.size(name); },
                         [&](const auto& name, ChecksumProgress& progress) { return checksum.slice(name, progress); });
        queue.setPolicy(policy);
        if (dashboard)
            return serve(queue, checksum, web, port);

        queue.submit(files, priority);
        queue.waitUntilIdle();
        auto snapshot = queue.snapshot();
        std::cout << describe(snapshot).dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        for (const auto& job : snapshot.jobs)
            if (job.state == State::Failed)
                return 1;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BuildQueue: " << error.what() << '\n';
        return 1;
    }
}
