#pragma once
#include "backend.h"
#include "workload.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// BackendRegistry
// ---------------------------------------------------------------------------
// Backends self-register via REGISTER_BACKEND() at static-init time.
class BackendRegistry {
public:
    using Factory = std::function<std::unique_ptr<Backend>()>;

    static BackendRegistry& instance();

    void register_backend(const std::string& name, Factory f);
    std::unique_ptr<Backend> create(const std::string& name) const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, Factory> _factories;
};

// ---------------------------------------------------------------------------
// WorkloadRegistry
// ---------------------------------------------------------------------------
class WorkloadRegistry {
public:
    using Factory = std::function<std::unique_ptr<Workload>()>;

    static WorkloadRegistry& instance();

    void register_workload(const std::string& name, Factory f);
    std::unique_ptr<Workload> create(const std::string& name) const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, Factory> _factories;
};

// ---------------------------------------------------------------------------
// Registration helpers
// ---------------------------------------------------------------------------

// REGISTER_BACKEND(ClassName, "name")
// Place in the .cpp file for each backend.  The static initializer runs
// before main() and inserts the factory into the registry.
#define REGISTER_BACKEND(cls, key)                                         \
    static bool _backend_reg_##cls = []() -> bool {                        \
        ::bench::BackendRegistry::instance().register_backend(             \
            key, []() { return std::make_unique<cls>(); });                \
        return true;                                                        \
    }()

// REGISTER_WORKLOAD(ClassName, "name")
#define REGISTER_WORKLOAD(cls, key)                                        \
    static bool _workload_reg_##cls = []() -> bool {                       \
        ::bench::WorkloadRegistry::instance().register_workload(           \
            key, []() { return std::make_unique<cls>(); });                \
        return true;                                                        \
    }()

} // namespace bench
