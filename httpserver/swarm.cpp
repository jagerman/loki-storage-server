#include "swarm.h"
#include "http_connection.h"
#include "oxen_logger.h"

#include "service_node.h"

#include <cstdlib>
#include <ostream>
#include <unordered_map>

#include "utils.hpp"

namespace oxen {

static bool swarm_exists(const all_swarms_t& all_swarms,
                         const swarm_id_t& swarm) {

    const auto it = std::find_if(
        all_swarms.begin(), all_swarms.end(),
        [&swarm](const SwarmInfo& si) { return si.swarm_id == swarm; });

    return it != all_swarms.end();
}

void debug_print(std::ostream& os, const block_update_t& bu) {

    os << "Block update: {\n";
    os << "     height: " << bu.height << '\n';
    os << "     block hash: " << bu.block_hash << '\n';
    os << "     hardfork: " << bu.hardfork << '\n';
    os << "     swarms: [\n";

    for (const SwarmInfo& swarm : bu.swarms) {
        os << "         {\n";
        os << "             id: " << swarm.swarm_id << '\n';
        os << "         }\n";
    }

    os << "     ]\n";
    os << "}\n";
}

Swarm::~Swarm() = default;

bool Swarm::is_existing_swarm(swarm_id_t sid) const {

    return std::any_of(all_valid_swarms_.begin(), all_valid_swarms_.end(),
                       [sid](const SwarmInfo& cur_swarm_info) {
                           return cur_swarm_info.swarm_id == sid;
                       });
}

SwarmEvents Swarm::derive_swarm_events(const all_swarms_t& swarms) const {

    SwarmEvents events = {};

    const auto our_swarm_it = std::find_if(
        swarms.begin(), swarms.end(), [this](const SwarmInfo& swarm_info) {
            const auto& snodes = swarm_info.snodes;
            return std::find(snodes.begin(), snodes.end(), our_address_) !=
                   snodes.end();
        });

    if (our_swarm_it == swarms.end()) {
        // We are not in any swarm, nothing to do
        events.our_swarm_id = INVALID_SWARM_ID;
        return events;
    }

    const auto& new_swarm_snodes = our_swarm_it->snodes;
    const auto new_swarm_id = our_swarm_it->swarm_id;

    events.our_swarm_id = new_swarm_id;
    events.our_swarm_members = new_swarm_snodes;

    if (cur_swarm_id_ == INVALID_SWARM_ID) {
        // Only started in a swarm, nothing to do at this stage
        return events;
    }

    if (cur_swarm_id_ != new_swarm_id) {
        // Got moved to a new swarm
        if (!swarm_exists(swarms, cur_swarm_id_)) {
            // Dissolved, new to push all our data to new swarms
            events.dissolved = true;
        }

        // If our old swarm is still alive, there is nothing for us to do
        return events;
    }

    /// --- WE are still in the same swarm if we reach here ---

    /// See if anyone joined our swarm
    for (const auto& sn : new_swarm_snodes) {

        const auto it = std::find(swarm_peers_.begin(), swarm_peers_.end(), sn);

        if (it == swarm_peers_.end() && sn != our_address_) {
            events.new_snodes.push_back(sn);
        }
    }

    /// See if there are any new swarms

    for (const auto& swarm_info : swarms) {

        const bool found = this->is_existing_swarm(swarm_info.swarm_id);

        if (!found) {
            events.new_swarms.push_back(swarm_info.swarm_id);
        }
    }

    /// NOTE: need to be careful and make sure we don't miss any
    /// swarm update (e.g. if we don't update frequently enough)

    return events;
}

void Swarm::set_swarm_id(swarm_id_t sid) {

    if (sid == INVALID_SWARM_ID) {
        OXEN_LOG(warn, "We are not currently an active Service Node");
    } else {

        if (cur_swarm_id_ == INVALID_SWARM_ID) {
            OXEN_LOG(info, "EVENT: started SN in swarm: {}", sid);
        } else if (cur_swarm_id_ != sid) {
            OXEN_LOG(info, "EVENT: got moved into a new swarm: {}", sid);
        }
    }

    cur_swarm_id_ = sid;
}

static auto get_snode_map_from_swarms(const all_swarms_t& swarms) {

    std::unordered_map<legacy_pubkey, sn_record_t> snode_map;
    for (const auto& swarm : swarms) {
        for (const auto& snode : swarm.snodes) {
            snode_map.emplace(snode.pubkey_legacy, snode);
        }
    }
    return snode_map;
}

template <typename T>
bool update_if_changed(T& val, const T& new_val, const std::common_type_t<T>& ignore_val) {
    if (new_val != ignore_val && new_val != val) {
        val = new_val;
        return true;
    }
    return false;
}

auto apply_ips(const all_swarms_t& swarms_to_keep,
               const all_swarms_t& other_swarms) -> all_swarms_t {

    all_swarms_t result_swarms = swarms_to_keep;
    const auto other_snode_map = get_snode_map_from_swarms(other_swarms);

    int updates_count = 0;
    for (auto& [swarm_id, snodes] : result_swarms) {
        for (auto& snode : snodes) {
            const auto other_snode_it =
                other_snode_map.find(snode.pubkey_legacy);
            if (other_snode_it != other_snode_map.end()) {
                auto& sn = other_snode_it->second;
                // Keep swarms_to_keep but don't overwrite with default IPs/ports
                bool updated = false;
                updated += update_if_changed(snode.ip, sn.ip, "0.0.0.0");
                updated += update_if_changed(snode.port, sn.port, 0);
                updated += update_if_changed(snode.lmq_port, sn.lmq_port, 0);
                if (updated)
                    updates_count++;
            }
        }
    }

    OXEN_LOG(debug, "Updated {} entries from oxend", updates_count);
    return result_swarms;
}

void Swarm::apply_swarm_changes(const all_swarms_t& new_swarms) {

    OXEN_LOG(trace, "Applying swarm changes");

    all_valid_swarms_ = apply_ips(new_swarms, all_valid_swarms_);
}

void Swarm::update_state(const all_swarms_t& swarms,
                         const std::vector<sn_record_t>& decommissioned,
                         const SwarmEvents& events, bool active) {

    if (active) {

        // The following only makes sense for active nodes in a swarm

        if (events.dissolved) {
            OXEN_LOG(info, "EVENT: our old swarm got DISSOLVED!");
        }

        for (const sn_record_t& sn : events.new_snodes) {
            OXEN_LOG(info, "EVENT: detected new SN: {}", sn.pubkey_legacy);
        }

        for (swarm_id_t swarm : events.new_swarms) {
            OXEN_LOG(info, "EVENT: detected a new swarm: {}", swarm);
        }

        apply_swarm_changes(swarms);

        const auto& members = events.our_swarm_members;

        /// sanity check
        if (members.empty())
            return;

        swarm_peers_.clear();
        swarm_peers_.reserve(members.size() - 1);

        std::copy_if(members.begin(), members.end(),
                     std::back_inserter(swarm_peers_),
                     [this](const sn_record_t& record) {
                         return record != our_address_;
                     });
    }

    // Store a copy of every node in a separate data structure
    all_funded_nodes_.clear();
    all_funded_ed25519_.clear();
    all_funded_x25519_.clear();

    for (const auto& si : swarms) {
        for (const auto& sn : si.snodes) {
            all_funded_nodes_.emplace(sn.pubkey_legacy, sn);
        }
    }

    for (const auto& sn : decommissioned) {
        all_funded_nodes_.emplace(sn.pubkey_legacy, sn);
    }

    for (const auto& [pk, sn] : all_funded_nodes_) {
        all_funded_ed25519_.emplace(sn.pubkey_ed25519, pk);
        all_funded_x25519_.emplace(sn.pubkey_x25519, pk);
    }
}

std::optional<sn_record_t>
Swarm::find_node(const legacy_pubkey& pk) const {
    if (auto it = all_funded_nodes_.find(pk); it != all_funded_nodes_.end())
        return it->second;
    return std::nullopt;
}

std::optional<sn_record_t>
Swarm::find_node(const ed25519_pubkey& pk) const {
    if (auto it = all_funded_ed25519_.find(pk); it != all_funded_ed25519_.end())
        return find_node(it->second);
    return std::nullopt;
}

std::optional<sn_record_t>
Swarm::find_node(const x25519_pubkey& pk) const {
    if (auto it = all_funded_x25519_.find(pk); it != all_funded_x25519_.end())
        return find_node(it->second);
    return std::nullopt;
}

static uint64_t hex_to_u64(const user_pubkey_t& pk) {

    /// Create a buffer for 16 characters null terminated
    char buf[17] = {};

    /// Note: pk is expected to contain two leading characters
    /// (05 for the messenger) that do not participate in mapping

    /// Note: if conversion is not possible, we will still
    /// get a value in res (possibly 0 or UINT64_MAX), which
    /// we are not handling at the moment
    uint64_t res = 0;
    for (auto it = pk.str().begin() + 2; it < pk.str().end(); it += 16) {
        memcpy(buf, &(*it), 16);
        res ^= strtoull(buf, nullptr, 16);
    }

    return res;
}

bool Swarm::is_pubkey_for_us(const user_pubkey_t& pk) const {

    /// TODO: Make sure no exceptions bubble up from here!
    return cur_swarm_id_ == get_swarm_by_pk(all_valid_swarms_, pk);
}

swarm_id_t get_swarm_by_pk(const std::vector<SwarmInfo>& all_swarms,
                           const user_pubkey_t& pk) {

    const uint64_t res = hex_to_u64(pk);

    /// We reserve UINT64_MAX as a sentinel swarm id for unassigned snodes
    constexpr swarm_id_t MAX_ID = INVALID_SWARM_ID - 1;

    swarm_id_t cur_best = INVALID_SWARM_ID;
    uint64_t cur_min = INVALID_SWARM_ID;

    /// We don't require that all_swarms is sorted, so we find
    /// the smallest/largest elements in the same loop
    swarm_id_t leftmost_id = INVALID_SWARM_ID;
    swarm_id_t rightmost_id = 0;

    for (const auto& si : all_swarms) {

        if (si.swarm_id == INVALID_SWARM_ID) {
            /// Just to be sure we check again that no decomissioned
            /// node is exposed to clients
            continue;
        }

        uint64_t dist =
            (si.swarm_id > res) ? (si.swarm_id - res) : (res - si.swarm_id);
        if (dist < cur_min) {
            cur_best = si.swarm_id;
            cur_min = dist;
        }

        /// Find the letfmost
        if (si.swarm_id < leftmost_id) {
            leftmost_id = si.swarm_id;
        }

        if (si.swarm_id > rightmost_id) {
            rightmost_id = si.swarm_id;
        }
    }

    // handle special case
    if (res > rightmost_id) {
        // since rightmost is at least as large as leftmost,
        // res >= leftmost_id in this branch, so the value will
        // not overflow; the same logic applies to the else branch
        const uint64_t dist = (MAX_ID - res) + leftmost_id;
        if (dist < cur_min) {
            cur_best = leftmost_id;
        }
    } else if (res < leftmost_id) {
        const uint64_t dist = res + (MAX_ID - rightmost_id);
        if (dist < cur_min) {
            cur_best = rightmost_id;
        }
    }

    return cur_best;
}

std::pair<int, int> count_missing_data(const block_update_t& bu) {
    auto result = std::make_pair(0, 0);
    auto& [missing, total] = result;

    for (auto& swarm : bu.swarms) {
        for (auto& snode : swarm.snodes) {
            total++;
            if (snode.ip.empty() || snode.ip == "0.0.0.0" || !snode.port || !snode.lmq_port ||
                    !snode.pubkey_ed25519 || !snode.pubkey_x25519)
            { OXEN_LOG(warn, "well wtf {} {} {} {} {}",
                    snode.ip, snode.port, snode.lmq_port, snode.pubkey_ed25519, snode.pubkey_x25519);
                missing++;
            }
        }
    }
    return result;
}

} // namespace oxen
