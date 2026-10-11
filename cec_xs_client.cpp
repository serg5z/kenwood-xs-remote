#include <iostream>
#include "cec_client_core.h"
#include "cec_service_socket.h"

#include <libcec/cec.h>
#include <libcec/cecloader.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace CEC;

namespace {
volatile std::sig_atomic_t stopping = 0;
BoundedCecQueue events;

void on_signal(int) { stopping = 1; }

void on_key(void*, const cec_keypress* key)
{
    if (!key) return;
    CecKey code = CecKey::OTHER;
    if (key->keycode == CEC_USER_CONTROL_CODE_VOLUME_UP) code = CecKey::UP;
    else if (key->keycode == CEC_USER_CONTROL_CODE_VOLUME_DOWN) code = CecKey::DOWN;
    else if (key->keycode == CEC_USER_CONTROL_CODE_MUTE) code = CecKey::MUTE;
    if (code != CecKey::OTHER)
        events.push({code, key->duration, CecClock::now()});
}
}

int main(int argc, char** argv)
{
    std::string socket_path = "/tmp/kenwood-xs8.sock";
    long max_hold = 2000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket_path = argv[++i];
        else if (std::strcmp(argv[i], "--max-hold-ms") == 0 && i + 1 < argc) {
            char* end = nullptr;
            errno = 0;
            max_hold = std::strtol(argv[++i], &end, 10);
            if (!end || *end || errno == ERANGE || max_hold < 100 || max_hold > 3000) {
                std::fprintf(stderr, "Invalid max hold (100..3000 ms)\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "Usage: %s [--socket PATH] [--max-hold-ms 100..3000]\n",
                         argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    UnixServiceTransport transport(socket_path);
    CecClientCore client(transport, CecClientCore::Ms(1200),
                         CecClientCore::Ms(max_hold));

    libcec_configuration config;
    config.Clear();
    config.bActivateSource = 0;
    config.wakeDevices.Clear();
    config.clientVersion = LIBCEC_VERSION_CURRENT;
    config.deviceTypes.Add(CEC_DEVICE_TYPE_AUDIO_SYSTEM);
    std::snprintf(config.strDeviceName, sizeof(config.strDeviceName), "Kenwood XS");
    ICECCallbacks callbacks{};
    callbacks.keyPress = on_key;
    config.callbacks = &callbacks;
    ICECAdapter* cec = LibCecInitialise(&config);
    if (!cec) { std::fprintf(stderr, "Cannot initialize libCEC\n"); return 1; }
    cec_adapter_descriptor adapters[10]{};
    const int count = cec->DetectAdapters(adapters, 10, nullptr, true);
    if (count <= 0 || !cec->Open(adapters[0].strComName)) {
        std::fprintf(stderr, "Cannot open CEC adapter\n");
        UnloadLibCec(cec);
        return 1;
    }
    cec->SetInactiveView();
    std::puts("CEC XS8 client ready (no XS8 command sent on startup)");
    while (!stopping) {
        bool overflowed = false;
        auto pending = events.drain(overflowed);
        if (overflowed) {
            std::fprintf(stderr, "CEC event queue overflow; ending any active hold\n");
            client.overflow(CecClock::now());
        } else {
            for (const auto& event : pending) {
                if (stopping) break;
                auto now = CecClock::now();
                client.event(event, now);
            }
        }
        client.tick(CecClock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client.shutdown();
    cec->Close();
    UnloadLibCec(cec);
    return 0;
}
