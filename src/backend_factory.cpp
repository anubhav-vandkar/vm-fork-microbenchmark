#include "vmbench/backend.hpp"
#include "vmbench/firecracker_backend.hpp"
#include "vmbench/lightning_backend.hpp"
#include <stdexcept>

namespace vmbench {
std::unique_ptr<Backend> make_backend(const Options& options, const fs::path& work) {
    if (options.backend == "lightning") return std::make_unique<LightningBackend>(options, work);
    if (options.backend == "firecracker") return std::make_unique<FirecrackerBackend>(options, work);
    throw std::runtime_error("Choose --backend lightning or firecracker");
}
}
