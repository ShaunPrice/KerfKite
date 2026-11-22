// kerfkite-studio: KerfKite Studio's workstation server. It serves the editor at http://127.0.0.1:8765/ in
// simulator mode; --enable-hardware lets it open controller ports and --monitor pairs the phone pendant.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "kerfkite/relief.hpp"
#include "phone_monitor.hpp"
#include "static_files.hpp"
#include "studio.hpp"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: kerfkite-studio [--port N] [--enable-hardware] [--monitor] [--monitor-port N] [--monitor-plaintext] [--static DIR]\n"
                 "  --enable-hardware   operator opt-in to physical controller ports; requires machine setup and supervised commissioning\n"
                 "  --monitor           enable paired phone monitor and job controls on the LAN\n");
}

}  // namespace

int main(int argc, char **argv) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    using namespace kerfkite::server;
    Options options;
    std::string static_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") options.port = std::atoi(value().c_str());
        else if (arg == "--enable-hardware") options.enable_hardware = true;
        else if (arg == "--monitor") options.monitor = true;
        else if (arg == "--monitor-port") options.monitor_port = std::atoi(value().c_str());
        else if (arg == "--monitor-plaintext") options.monitor_plaintext = true;
        else if (arg == "--static") static_dir = value();
        else if (arg == "--monitor-cert" || arg == "--monitor-key") {
            std::fprintf(stderr, "kerfkite-studio: monitor certificates are not supported by this program; use --monitor-plaintext\n");
            return 2;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    options.static_dir = find_static_dir(static_dir, argv[0]);
    try {
        Studio studio(options);
        kerfkite::http::Server server("127.0.0.1", options.port);
        if (options.monitor) {
            const kerfkite::Json pairing = studio.monitor().enable();
            std::printf("Phone pairing: %s://<PI-LAN-IP>:%lld/#%s\n", pairing.at("scheme").get<std::string>().c_str(),
                        pairing.at("port").get<long long>(), pairing.at("fragment").get<std::string>().c_str());
            std::printf("The phone monitor is using HTTP: the pairing token and commands are readable on this network.\n");
            std::printf("Open Control \xe2\x86\x92 Phone pendant to view or revoke pairing.\n");
        }
        // Measure this computer's relief contact once, after start-up, so large jobs go the way it favours.
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            try {
                kerfkite::relief_calibrate();
            } catch (...) {
            }
        }).detach();
        std::printf("KerfKite Studio at http://127.0.0.1:%d/\n", server.port());
        // The desktop launcher waits for this line before it opens the editor (studio/desktop_server.py).
        std::printf("KERFKITE_READY\n");
        std::fflush(stdout);
        server.serve([&](const kerfkite::http::Request &r) { return studio.handle(r); },
                     [&](const kerfkite::http::Request &r) { return studio.body_limit(r); });
    } catch (const std::exception &e) {
        std::fprintf(stderr, "kerfkite-studio: %s\n", e.what());
        return 1;
    }
    return 0;
}
