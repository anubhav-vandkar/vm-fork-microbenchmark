#pragma once
#include "vmbench/options.hpp"
#include "vmbench/process_manager.hpp"

namespace vmbench {
struct CloneResult { VmHandle child; Object metadata; };
class Backend {
public:
    virtual ~Backend() = default;
    virtual VmHandle boot() = 0;
    virtual void marker(const VmHandle& root, int round) = 0;
    virtual Object ready(const VmHandle& vm) = 0;
    virtual Object prepare_round(const VmHandle& root, int round) = 0;
    virtual CloneResult clone(const VmHandle& root, int id) = 0;
    virtual void destroy(const VmHandle& vm) = 0;
    virtual void release_round(const VmHandle& root) = 0;
    virtual json::array cleanup() = 0;
};
class ManagedBackend : public Backend {
public:
    ManagedBackend(const Options& options, fs::path work);
    void marker(const VmHandle& root, int round) override;
    Object ready(const VmHandle& vm) override;
    json::array cleanup() override;
protected:
    pid_t launch(const std::vector<std::string>& arguments, const std::string& log_label);
    const Options& options_;
    fs::path work_;
    ProcessManager processes_;
};
std::unique_ptr<Backend> make_backend(const Options& options, const fs::path& work);
}
