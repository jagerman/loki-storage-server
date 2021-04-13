#include "stats.h"
#include <algorithm>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

namespace oxen {

static void cleanup_old(std::deque<test_result_t>& tests, time_t cutoff_time) {

    const auto it = std::find_if(tests.begin(), tests.end(),
                                 [cutoff_time](const test_result_t& res) {
                                     return res.timestamp > cutoff_time;
                                 });

    tests.erase(tests.begin(), it);
}

static constexpr std::chrono::seconds ROLLING_WINDOW_SIZE = 120min;

void all_stats_t::cleanup() {

    using std::chrono::duration_cast;
    using std::chrono::seconds;

    const auto cutoff = time(nullptr) - ROLLING_WINDOW_SIZE.count();

    for (auto& [kv, stats] : peer_report_) {
        cleanup_old(stats.storage_tests, cutoff);
    }

    /// updated stats for "previous period"
    this->next_period();
}

} // namespace oxen
