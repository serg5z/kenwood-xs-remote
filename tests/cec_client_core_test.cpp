#include "cec_client_core.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using Ms = CecClientCore::Ms;
static CecClock::time_point at(int ms) { return CecClock::time_point(Ms(ms)); }

struct FakeTransport final : CecServiceTransport {
    std::vector<std::string> commands;
    int connects = 0;
    int closes = 0;
    bool available = true;
    bool connected = false;
    bool fail_next = false;
    bool connect() override {
        ++connects;
        connected = available;
        return connected;
    }
    bool command(const std::string& request, std::string& reply) override {
        commands.push_back(request);
        if (!connected || fail_next) { fail_next = false; return false; }
        reply = request.find("VOLUME_PRESS ") == 0 ? "OK VOLUME_PRESS_TX" :
                request.find("VOLUME_RELEASE ") == 0 ? "OK VOLUME_RELEASE_TX" :
                request == "MUTE" ? "OK MUTE_TX" : "ERR UNKNOWN_COMMAND";
        return true;
    }
    bool alive() override { return connected && available; }
    void close() override { ++closes; connected = false; }
};

static void test_press_repeat_release_and_direction()
{
    FakeTransport tx;
    CecClientCore client(tx);
    client.event({CecKey::UP, 0, at(10)}, at(12));
    client.event({CecKey::UP, 0, at(50)}, at(51));
    assert(tx.commands == (std::vector<std::string>{"VOLUME_PRESS UP", "VOLUME_PRESS UP"}));
    assert(tx.connects == 1 && client.holding());
    client.event({CecKey::DOWN, 4, at(55)}, at(56)); // Wrong-direction release.
    assert(client.holding());
    client.event({CecKey::DOWN, 0, at(60)}, at(61)); // Service handles direction change.
    assert(tx.commands.back() == "VOLUME_PRESS DOWN");
    client.event({CecKey::UP, 4, at(62)}, at(63));
    assert(client.holding());
    client.event({CecKey::DOWN, 8, at(70)}, at(71));
    assert(tx.commands.back() == "VOLUME_RELEASE DOWN");
    assert(!client.holding() && !client.quarantined());
    client.event({CecKey::UP, 0, at(72)}, at(73));
    assert(tx.connects == 2 && tx.commands.back() == "VOLUME_PRESS UP");
    client.shutdown();
    assert(tx.commands.back() == "VOLUME_RELEASE UP");
}

static void test_missing_release_stale_and_max_hold()
{
    FakeTransport tx;
    CecClientCore client(tx, Ms(1200), Ms(2000));
    client.event({CecKey::DOWN, 0, at(10)}, at(12));
    client.tick(at(1211));
    assert(tx.commands.back() == "VOLUME_RELEASE DOWN");
    assert(client.quarantined());
    client.event({CecKey::DOWN, 0, at(1210)}, at(1212));
    assert(tx.connects == 1);
    client.event({CecKey::DOWN, 5, at(1214)}, at(1215));
    assert(!client.quarantined());
    client.event({CecKey::DOWN, 0, at(1000)}, at(1216)); // Queued before STOP.
    assert(tx.connects == 1);
    client.event({CecKey::DOWN, 0, at(1217)}, at(1218));
    assert(tx.connects == 2);
    for (int ms = 1400; ms < 3300; ms += 200)
        client.event({CecKey::DOWN, 0, at(ms)}, at(ms + 1));
    client.tick(at(3219));
    assert(!client.holding() && client.quarantined());
    assert(tx.commands.back() == "VOLUME_RELEASE DOWN");
}

static void test_disconnect_and_uncertain_no_replay()
{
    FakeTransport tx;
    CecClientCore client(tx);
    client.event({CecKey::UP, 0, at(10)}, at(11));
    tx.available = false;
    client.tick(at(20));
    assert(!client.holding() && client.quarantined());
    tx.available = true;
    client.event({CecKey::UP, 5, at(19)}, at(21)); // Captured before disconnect.
    assert(client.quarantined());
    client.event({CecKey::UP, 0, at(21)}, at(22));
    assert(tx.connects == 1); // Never silently resume the old hold.
    client.event({CecKey::UP, 8, at(23)}, at(24));
    client.event({CecKey::UP, 0, at(25)}, at(26));
    assert(tx.connects == 2);
    tx.fail_next = true;
    client.event({CecKey::DOWN, 0, at(30)}, at(31));
    assert(client.quarantined());
    assert(tx.commands.back() == "VOLUME_PRESS DOWN");
    const size_t count = tx.commands.size();
    client.event({CecKey::DOWN, 0, at(40)}, at(41));
    assert(tx.commands.size() == count);
}

static void test_unavailable_mute_and_duplicate_events()
{
    FakeTransport tx;
    CecClientCore client(tx);
    tx.available = false;
    client.event({CecKey::DOWN, 0, at(10)}, at(11));
    assert(client.quarantined() && tx.commands.empty());
    tx.available = true;
    client.event({CecKey::DOWN, 0, at(12)}, at(13));
    assert(tx.commands.empty());
    client.event({CecKey::DOWN, 4, at(14)}, at(15));
    client.event({CecKey::MUTE, 0, at(20)}, at(21));
    client.event({CecKey::MUTE, 0, at(22)}, at(23));
    assert(tx.commands == (std::vector<std::string>{"MUTE"}));
    client.event({CecKey::MUTE, 5, at(24)}, at(25));
    client.event({CecKey::MUTE, 0, at(26)}, at(27));
    assert(tx.commands == (std::vector<std::string>{"MUTE", "MUTE"}));
    client.event({CecKey::UP, 0, at(1)}, at(2000)); // Stale PRESS.
    assert(tx.commands.size() == 2);
}

static void test_bounded_queue()
{
    BoundedCecQueue queue;
    for (size_t i = 0; i <= BoundedCecQueue::kCapacity; ++i)
        queue.push({CecKey::UP, 0, at(static_cast<int>(i + 1))});
    bool overflowed = false;
    assert(queue.drain(overflowed).empty() && overflowed);
    queue.push({CecKey::DOWN, 5, at(100)});
    auto events = queue.drain(overflowed);
    assert(!overflowed && events.size() == 1 && events[0].key == CecKey::DOWN);

    FakeTransport tx;
    CecClientCore client(tx);
    client.event({CecKey::UP, 0, at(10)}, at(11));
    client.overflow(at(20));
    assert(tx.commands.back() == "VOLUME_RELEASE UP");
    assert(!client.holding() && client.quarantined());
}

int main()
{
    test_press_repeat_release_and_direction();
    test_missing_release_stale_and_max_hold();
    test_disconnect_and_uncertain_no_replay();
    test_unavailable_mute_and_duplicate_events();
    test_bounded_queue();
    std::cout << "CEC client core offline tests passed\n";
}
