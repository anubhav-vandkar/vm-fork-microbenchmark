#include "vmbench/signals.hpp"
#include <csignal>
#include <stdexcept>

namespace vmbench {
namespace {
volatile sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
}
void install_signal_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
}
void check_interrupted() {
    if (interrupted) throw std::runtime_error("Interrupted");
}
}
