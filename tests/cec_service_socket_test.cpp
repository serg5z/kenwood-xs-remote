#include "cec_client_core.h"
#include "cec_service_socket.h"

#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

static CecClock::time_point at(int ms)
{
    return CecClock::time_point(std::chrono::milliseconds(ms));
}

int main()
{
    char directory[] = "/tmp/cec-xs-client-test-XXXXXX";
    assert(mkdtemp(directory));
    const std::string path = std::string(directory) + "/service.sock";
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(path.size() < sizeof(address.sun_path));
    path.copy(address.sun_path, path.size());
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);

    std::vector<std::string> received;
    std::thread server([&] {
        int peer = accept(listener, nullptr, nullptr);
        assert(peer >= 0);
        std::string line;
        char byte;
        while (recv(peer, &byte, 1, 0) == 1) {
            if (byte != '\n') { line.push_back(byte); continue; }
            received.push_back(line);
            const char* reply = line.find("VOLUME_PRESS ") == 0
                ? "OK VOLUME_PRESS_TX\n" : "OK VOLUME_RELEASE_TX\n";
            assert(send(peer, reply, std::strlen(reply), 0) > 0);
            line.clear();
        }
        close(peer);
    });

    UnixServiceTransport transport(path);
    CecClientCore client(transport);
    client.event({CecKey::DOWN, 0, at(10)}, at(11));
    assert(client.holding());
    client.event({CecKey::DOWN, 0, at(20)}, at(21));
    client.event({CecKey::DOWN, 5, at(30)}, at(31));
    assert(!client.holding());
    server.join();
    assert(received == (std::vector<std::string>{
        "VOLUME_PRESS DOWN", "VOLUME_PRESS DOWN", "VOLUME_RELEASE DOWN"}));
    close(listener);
    unlink(path.c_str());
    rmdir(directory);
    std::cout << "CEC client fake Unix socket test passed\n";
}
