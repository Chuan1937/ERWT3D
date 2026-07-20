#include "erwt3d/contest_phase_plan.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <sstream>

namespace erwt3d {

ContestPhasePlan buildContestPhasePlan(
    const std::vector<uint64_t>& group_output_bytes,
    const std::vector<SliceAxis>& axes,
    const std::vector<std::string>& modes,
    uint64_t output_budget_bytes
) {
    ContestPhasePlan result;
    const size_t N = group_output_bytes.size();
    if (N == 0) return result;

    result.total_output_bytes = std::accumulate(
        group_output_bytes.begin(), group_output_bytes.end(), uint64_t(0)
    );

    std::vector<size_t> xGroups, yzRandom, yzCont;

    for (size_t i = 0; i < N; ++i) {
        if (i >= axes.size()) break;
        if (axes[i] == SliceAxis::X) {
            xGroups.push_back(i);
        } else if (i < modes.size() && modes[i] == "random") {
            yzRandom.push_back(i);
        } else {
            yzCont.push_back(i);
        }
    }

    auto groupOutBytes = [&](size_t g) -> uint64_t {
        return g < group_output_bytes.size() ? group_output_bytes[g] : 0;
    };

    auto totalFor = [&](const std::vector<size_t>& gids) -> uint64_t {
        uint64_t t = 0;
        for (size_t g : gids) t += groupOutBytes(g);
        return t;
    };

    auto appendPhase = [&](const std::vector<size_t>& gids) {
        if (gids.empty()) return;
        ContestPhase phase;
        phase.group_ids = gids;
        phase.output_bytes = totalFor(gids);
        result.phases.push_back(std::move(phase));
        result.max_phase_output_bytes = std::max(
            result.max_phase_output_bytes, phase.output_bytes
        );
    };

    if (result.total_output_bytes <= output_budget_bytes) {
        std::vector<size_t> all(N);
        for (size_t i = 0; i < N; ++i) all[i] = i;
        appendPhase(all);
        result.all_outputs_deferred = true;
        return result;
    }

    if (!xGroups.empty()) {
        uint64_t xBytes = totalFor(xGroups);
        if (xBytes <= output_budget_bytes) {
            appendPhase(xGroups);
        } else {
            uint64_t acc = 0;
            std::vector<size_t> batch;
            for (size_t g : xGroups) {
                uint64_t b = groupOutBytes(g);
                if (b > output_budget_bytes) {
                    appendPhase({g});
                    continue;
                }
                if (!batch.empty() && acc + b > output_budget_bytes) {
                    appendPhase(batch);
                    batch.clear();
                    acc = 0;
                }
                batch.push_back(g);
                acc += b;
            }
            if (!batch.empty()) appendPhase(batch);
        }
    }

    if (!yzRandom.empty()) {
        uint64_t rndBytes = totalFor(yzRandom);
        uint64_t contBytes = totalFor(yzCont);

        if (rndBytes + contBytes <= output_budget_bytes) {
            std::vector<size_t> combined = yzRandom;
            combined.insert(combined.end(), yzCont.begin(), yzCont.end());
            appendPhase(combined);
            yzCont.clear();
        } else if (rndBytes <= output_budget_bytes) {
            std::vector<size_t> combined = yzRandom;
            if (!yzCont.empty() && rndBytes + totalFor(yzCont) <= output_budget_bytes) {
                combined.insert(combined.end(), yzCont.begin(), yzCont.end());
                yzCont.clear();
            }
            appendPhase(combined);
        } else {
            uint64_t acc = 0;
            std::vector<size_t> batch;
            for (size_t g : yzRandom) {
                uint64_t b = groupOutBytes(g);
                if (b > output_budget_bytes) {
                    appendPhase({g});
                    continue;
                }
                if (!batch.empty() && acc + b > output_budget_bytes) {
                    appendPhase(batch);
                    batch.clear();
                    acc = 0;
                }
                batch.push_back(g);
                acc += b;
            }
            if (!batch.empty()) appendPhase(batch);
        }
    }

    if (!yzCont.empty()) {
        uint64_t contBytes = totalFor(yzCont);
        if (contBytes <= output_budget_bytes) {
            appendPhase(yzCont);
        } else {
            uint64_t acc = 0;
            std::vector<size_t> batch;
            for (size_t g : yzCont) {
                uint64_t b = groupOutBytes(g);
                if (b > output_budget_bytes) {
                    appendPhase({g});
                    continue;
                }
                if (!batch.empty() && acc + b > output_budget_bytes) {
                    appendPhase(batch);
                    batch.clear();
                    acc = 0;
                }
                batch.push_back(g);
                acc += b;
            }
            if (!batch.empty()) appendPhase(batch);
        }
    }

    return result;
}

bool validateContestPhasePlan(
    const ContestPhasePlan& plan,
    size_t expected_group_count,
    uint64_t output_budget_bytes,
    std::string* error
) {
    if (error) error->clear();

    std::set<size_t> seen;
    size_t total = 0;

    for (const auto& phase : plan.phases) {
        const bool isSingleGroup = phase.group_ids.size() == 1;
        if (!isSingleGroup && phase.output_bytes > output_budget_bytes) {
            if (error) {
                std::ostringstream oss;
                oss << "phase exceeds budget: " << phase.output_bytes
                    << " > " << output_budget_bytes;
                *error = oss.str();
            }
            return false;
        }

        for (size_t g : phase.group_ids) {
            if (g >= expected_group_count) {
                if (error) {
                    std::ostringstream oss;
                    oss << "unknown group id " << g
                        << " (expected < " << expected_group_count << ")";
                    *error = oss.str();
                }
                return false;
            }
            if (seen.count(g)) {
                if (error) {
                    std::ostringstream oss;
                    oss << "duplicate group id " << g;
                    *error = oss.str();
                }
                return false;
            }
            seen.insert(g);
            ++total;
        }
    }

    if (total != expected_group_count) {
        if (error) {
            std::ostringstream oss;
            oss << "expected " << expected_group_count
                << " groups but found " << total;
            *error = oss.str();
        }
        return false;
    }

    return true;
}

} // namespace erwt3d
