#include <iostream>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <grpcpp/grpcpp.h>
#include "allocator/allocator.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using allocator::v1::Allocator;
using allocator::v1::MemoryRequest;
using allocator::v1::MemoryReply;

class AllocatorClient {
public:
    explicit AllocatorClient(std::shared_ptr<Channel> channel)
        : stub_(Allocator::NewStub(channel)) {}

    std::pair<std::string, int> RequestSharedMemory() {
        MemoryRequest request;
        MemoryReply reply;
        ClientContext context;

        Status status = stub_->RequestSharedMemory(&context, request, &reply);

        if (!status.ok()) {
            std::cerr << "gRPC request failed: " << status.error_message() << std::endl;
            return {"", -1};
        }

        return {reply.shm_name(), reply.size()};
    }

private:
    std::unique_ptr<Allocator::Stub> stub_;
};

int main() {
    AllocatorClient client(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

    auto [shm_name, size] = client.RequestSharedMemory();
    if (shm_name.empty() || size <= 0) {
        std::cerr << "Failed to receive shared memory info." << std::endl;
        return 1;
    }

    std::cout << "Client received shared memory name: " << shm_name << ", size: " << size << " bytes\n";

    // Open shared memory
    int shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0644);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    // Allocate aligned memory for pinning
    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_size = ((size + page_size - 1) / page_size) * page_size;

    // Map shared memory to pinned pointer
    void* h_data = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }

    // Print message from server
    std::cout << "Shared memory message: " << static_cast<char*>(h_data) << "\n";

    // Cleanup
    munmap(h_data, aligned_size);
    close(shm_fd);

    return 0;
}
