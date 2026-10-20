#pragma once
#include <unistd.h>
#include <utility>

namespace vmbench {
class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) reset(std::exchange(other.descriptor_, -1));
        return *this;
    }
    ~FileDescriptor() { reset(); }
    int get() const { return descriptor_; }
    void reset(int descriptor = -1) {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = descriptor;
    }
private:
    int descriptor_;
};
}
