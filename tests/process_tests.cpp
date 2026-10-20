#include "test_support.hpp"
#include "vmbench/process_manager.hpp"
#include "vmbench/temporary_directory.hpp"
#include <csignal>

namespace vmbench::tests {
void test_process_cleanup() {
    TemporaryDirectory temporary("/tmp");
    ProcessManager processes;
    const auto pid = processes.launch({"/bin/sleep", "30"}, temporary.path() / "process.log");
    auto vm = processes.track(1, pid, temporary.path() / "api", temporary.path() / "vsock");
    processes.stop(vm, SIGTERM);
    require(!vm->alive && vm->pidfd.get() < 0, "VM process descriptor retained after stop");
    require(processes.active().empty() && processes.launched_count() == 0, "Exited process records retained");
    processes.stop(vm, SIGTERM);
}
}
