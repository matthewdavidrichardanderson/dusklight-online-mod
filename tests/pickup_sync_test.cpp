#include "dusklight_online/game/pickup_sync.hpp"

#include <iostream>
#include <set>
#include <string>

int main() {
    using namespace dusklight_online::game;
    std::set<std::string> completed;
    const auto accept = [&](std::string name) {
        return name.empty() || repeatable_pickup_check(name) || completed.insert(name).second;
    };
    for (int i = 0; i < 100; ++i) {
        if (!accept("freestanding:F_SP122:255")) {
            std::cerr << "Repeatable pickups were deduplicated\n";
            return 1;
        }
    }
    if (!accept("freestanding:F_SP122:133") || accept("freestanding:F_SP122:133") ||
        !accept("freestanding:F_SP112:133") || !accept("chest:F_SP122:11") ||
        accept("chest:F_SP122:11") || !accept("") || !accept("")) {
        std::cerr << "One-time check identity handling failed\n";
        return 1;
    }
    if (rupee_pickup_amount(1) != 1 || rupee_pickup_amount(2) != 5 ||
        rupee_pickup_amount(3) != 10 || rupee_pickup_amount(4) != 20 ||
        rupee_pickup_amount(5) != 50 || rupee_pickup_amount(6) != 100 ||
        rupee_pickup_amount(7) != 200 || rupee_pickup_amount(0) != 0 ||
        rupee_pickup_amount(0x13) != 0 || rupee_pickup_amount(-1) != 0 ||
        rupee_pickup_amount(255) != 0) {
        std::cerr << "Currency item classification failed\n";
        return 1;
    }
    const auto expect = [](bool condition, const char* message) {
        if (!condition) std::cerr << message << '\n';
        return condition;
    };
    using A = WalletTotalAction;
    if (!expect(wallet_total_action(125, 100, 0, 300) == A::Apply, "Gain rejected") ||
        !expect(wallet_total_action(75, 100, 0, 300) == A::Apply, "Spending rejected") ||
        !expect(wallet_total_action(0, 100, 0, 300) == A::Apply, "Empty wallet rejected") ||
        !expect(wallet_total_action(300, 299, 0, 300) == A::Apply, "Full wallet rejected") ||
        !expect(wallet_total_action(-1, 0, 0, 300) == A::Reject, "Negative total accepted") ||
        !expect(wallet_total_action(301, 0, 0, 300) == A::Reject, "Overfull total accepted") ||
        !expect(wallet_total_action(120, 100, 20, 300) == A::Coalesce, "Matching pending gain lost") ||
        !expect(wallet_total_action(80, 100, -20, 300) == A::Coalesce, "Matching pending spend lost") ||
        !expect(wallet_total_action(300, 295, 20, 300) == A::Coalesce, "Capped gain not coalesced") ||
        !expect(wallet_total_action(0, 5, -20, 300) == A::Coalesce, "Clamped spend not coalesced") ||
        !expect(wallet_total_action(120, 100, 5, 300) == A::Apply, "Different pending pickup discarded")) {
        return 1;
    }

    // A Randomizer rupee reward carries metadata, not another additive wallet
    // grant. Either delivery order must finish at the sender's total, once.
    for (int item = 1; item <= 7; ++item) {
        for (bool rewardFirst : {false, true}) {
            int wallet = 50;
            const int total = 50 + rupee_pickup_amount(item);
            const auto reward = [&] {
                if (remote_pickup_requires_grant(item)) wallet += rupee_pickup_amount(item);
            };
            const auto receive = [&] {
                if (wallet_total_action(total, wallet, 0, 1000) == A::Apply) wallet = total;
            };
            if (rewardFirst) reward();
            receive();
            if (!rewardFirst) reward();
            receive(); // Repeated absolute total is idempotent.
            if (!expect(wallet == total, "Randomizer currency was counted twice")) return 1;
        }
    }
    if (!expect(remote_pickup_requires_grant(0), "Heart grants must remain") ||
        !expect(remote_pickup_requires_grant(0x0a), "Bomb ammo grants must remain") ||
        !expect(remote_pickup_requires_grant(0x13), "Foolish Item grants must remain") ||
        !expect(remote_pickup_requires_grant(0x60), "Non-currency grants must remain") ||
        !expect(!remote_pickup_requires_grant(255), "NONE must not grant anything")) return 1;
    return 0;
}
