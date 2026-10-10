#include <iostream>
#include <libcec/cec.h>
#include <libcec/cecloader.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

using namespace CEC;
using Clock = std::chrono::steady_clock;

static constexpr auto WATCHDOG = std::chrono::milliseconds(1200);
static constexpr auto HEARTBEAT = std::chrono::milliseconds(300);

struct KeyEvent {
    cec_user_control_code code;
    unsigned int duration;
    Clock::time_point received;
};

static std::atomic<bool> running{true};
static std::mutex queueMutex;
static std::deque<KeyEvent> queue;
static int serialFd = -1;

static void onSignal(int) {
    running = false;
}

static void onKey(void*, const cec_keypress* key) {
    if (!key) return;

    std::lock_guard<std::mutex> lock(queueMutex);
    queue.push_back({key->keycode, key->duration, Clock::now()});
}

static bool sendArduino(char command, const char* description) {
    if (serialFd < 0) {
        std::fprintf(stderr, "Arduino serial not connected\n");
        return false;
    }

    const ssize_t written = write(serialFd, &command, 1);
    if (written != 1) {
        if (written < 0) perror("Arduino serial write");
        else std::fprintf(stderr, "Arduino serial: short write\n");
        return false;
    }

    if (description)
        std::printf("Arduino '%c' %s\n", command, description);
    return true;
}

static bool sendXS8(unsigned int code, const char* description) {
    char command = 0;
    switch (code) {
        case 0xF9: command = 'U'; break;
        case 0xE9: command = 'D'; break;
        case 0x09: command = 'S'; break;
        case 0xD8: command = 'M'; break;
        default: return false;
    }
    return sendArduino(command, description);
}

static bool openArduino() {
    serialFd = open("/dev/ttyACM0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serialFd < 0) {
        perror("Cannot open Arduino");
        return false;
    }

    termios tty{};
    if (tcgetattr(serialFd, &tty) != 0) {
        perror("tcgetattr");
        close(serialFd);
        serialFd = -1;
        return false;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(serialFd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(serialFd);
        serialFd = -1;
        return false;
    }

    std::puts("Arduino Serial ready");
    return true;
}

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!openArduino()) return 1;

    // Synchronize the Arduino's state before accepting CEC input.
    if (!sendXS8(0x09, "STOP (startup)")) {
        close(serialFd);
        return 1;
    }

    libcec_configuration config;
    config.Clear();
    config.bActivateSource = 0;
    config.wakeDevices.Clear();
    config.clientVersion = LIBCEC_VERSION_CURRENT;
    config.deviceTypes.Add(CEC_DEVICE_TYPE_AUDIO_SYSTEM);

    std::snprintf(config.strDeviceName,
                  sizeof(config.strDeviceName), "Kenwood Bridge");

    ICECCallbacks callbacks{};
    callbacks.keyPress = onKey;
    config.callbacks = &callbacks;

    ICECAdapter* cec = LibCecInitialise(&config);
    if (!cec) {
        std::fprintf(stderr, "Cannot initialize libCEC\n");
        close(serialFd);
        return 1;
    }

    cec_adapter_descriptor adapters[10]{};
    const int count = cec->DetectAdapters(adapters, 10, nullptr, true);

    if (count <= 0 || !cec->Open(adapters[0].strComName)) {
        std::fprintf(stderr, "Cannot open CEC adapter\n");
        UnloadLibCec(cec);
        close(serialFd);
        return 1;
    }

    cec->SetInactiveView();
    std::puts("CEC Audio System ready");

    int activeVolume = 0;
    Clock::time_point lastVolumeEvent{};
    Clock::time_point lastHeartbeat{};

    while (running) {
        std::deque<KeyEvent> pending;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pending.swap(queue);
        }

        for (const auto& event : pending) {
            const int code = event.code;
            const bool released = event.duration > 0;

            if (code == CEC_USER_CONTROL_CODE_VOLUME_UP ||
                code == CEC_USER_CONTROL_CODE_VOLUME_DOWN) {
                if (released) {
                    if (activeVolume == code) {
                        sendXS8(0x09, "STOP (release)");
                        activeVolume = 0;
                    }
                } else {
                    if (activeVolume != code) {
                        if (activeVolume != 0)
                            sendXS8(0x09, "STOP (direction change)");

                        if (sendXS8(code == CEC_USER_CONTROL_CODE_VOLUME_UP
                                        ? 0xF9 : 0xE9,
                                    code == CEC_USER_CONTROL_CODE_VOLUME_UP
                                        ? "VOL+" : "VOL-")) {
                            activeVolume = code;
                            lastHeartbeat = Clock::now();
                        } else {
                            activeVolume = 0;
                        }
                    }
                    lastVolumeEvent = event.received;
                }
            } else if (code == CEC_USER_CONTROL_CODE_MUTE) {
                if (!released)
                    sendXS8(0xD8, "MUTE");
            }
        }

        const auto now = Clock::now();
        if (activeVolume != 0 && now - lastVolumeEvent > WATCHDOG) {
            sendXS8(0x09, "STOP (watchdog)");
            activeVolume = 0;
        }

        if (activeVolume != 0 && now - lastHeartbeat >= HEARTBEAT) {
            if (!sendArduino('H', nullptr)) {
                sendXS8(0x09, "STOP (heartbeat failure)");
                activeVolume = 0;
            }
            lastHeartbeat = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    sendXS8(0x09, "STOP (shutdown)");
    cec->Close();
    UnloadLibCec(cec);
    close(serialFd);
    return 0;
}

