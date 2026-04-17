#include "bench/registry.h"
#include <stdexcept>

namespace bench {

// ---------------------------------------------------------------------------
// BackendRegistry
// ---------------------------------------------------------------------------

BackendRegistry& BackendRegistry::instance() {
    static BackendRegistry inst;
    return inst;
}

void BackendRegistry::register_backend(const std::string& name, Factory f) {
    _factories[name] = std::move(f);
}

std::unique_ptr<Backend> BackendRegistry::create(const std::string& name) const {
    auto it = _factories.find(name);
    if (it == _factories.end())
        throw std::runtime_error("Unknown backend: " + name);
    return it->second();
}

std::vector<std::string> BackendRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& [k, _] : _factories) out.push_back(k);
    return out;
}

// ---------------------------------------------------------------------------
// WorkloadRegistry
// ---------------------------------------------------------------------------

WorkloadRegistry& WorkloadRegistry::instance() {
    static WorkloadRegistry inst;
    return inst;
}

void WorkloadRegistry::register_workload(const std::string& name, Factory f) {
    _factories[name] = std::move(f);
}

std::unique_ptr<Workload> WorkloadRegistry::create(const std::string& name) const {
    auto it = _factories.find(name);
    if (it == _factories.end())
        throw std::runtime_error("Unknown workload: " + name);
    return it->second();
}

std::vector<std::string> WorkloadRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& [k, _] : _factories) out.push_back(k);
    return out;
}

} // namespace bench
