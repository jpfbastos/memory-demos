#include <iostream>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <grpcpp/grpcpp.h>
#include "allocator/allocator.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using allocator::v1::Allocator;
using allocator::v1::MemoryRequest;
using allocator::v1::MemoryReply;
using allocator::v1::FreeRequest;
using allocator::v1::FreeReply;

void* h_data = nullptr; // Pointer to the mapped shared memory
size_t aligned_size = 0; // Size of the aligned memory
void* ptr = nullptr; // Pointer to the allocated memory for pinning
int shm_fd; // File descriptor for the shared memory

class AllocatorClient {
public:
    explicit AllocatorClient(std::shared_ptr<Channel> channel)
        : stub_(Allocator::NewStub(channel)) {}

    std::pair<std::string, int> RequestSharedMemory() {
        MemoryRequest request;
        MemoryReply reply;
        ClientContext context;

        request.set_pid(getpid());

        Status status = stub_->RequestSharedMemory(&context, request, &reply);

        if (!status.ok()) {
            std::cerr << "gRPC request failed: " << status.error_message() << std::endl;
            return {"", -1};
        }

        return {reply.shm_name(), reply.size()};
    }

    int FreeSharedMemory(std::string shm_name) {
        FreeRequest request;
        FreeReply reply;
        ClientContext context;

        request.set_shm_name(shm_name);

        Status status = stub_->FreeSharedMemory(&context, request, &reply);

        if (!status.ok()) {
            std::cerr << "gRPC request failed: " << status.error_message() << std::endl;
            return -1;
        }

        return 0;
    }

private:
    std::unique_ptr<Allocator::Stub> stub_;
};

void sigbus_handler(int signum) {
    std::cout << "Received SIGBUS. Cleaning up...\n";
    munmap(h_data, aligned_size);
    close(shm_fd);
    std::cout << "Exiting program due to SIGBUS.\n";
    exit(1);
}

int main() {
    signal(SIGBUS, sigbus_handler);

    AllocatorClient client(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

    auto [shm_name, size] = client.RequestSharedMemory();
    if (shm_name.empty() || size <= 0) {
        std::cerr << "Failed to receive shared memory info." << std::endl;
        return 1;
    }

    std::cout << "Client received shared memory name: " << shm_name << ", size: " << size << " bytes\n";

    // Open shared memory
    shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    // Allocate aligned memory for pinning
    size_t page_size = sysconf(_SC_PAGESIZE);
    aligned_size = ((size + page_size - 1) / page_size) * page_size;

    // Map shared memory to pinned pointer
    h_data = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }

    // Print message from server
    std::cout << "Shared memory message: " << static_cast<char*>(h_data) << "\n";

    // access the first byte
    

    // wait 30 seconds
    std::cout << "Waiting for 20 seconds before freeing shared memory...\n";
    std::this_thread::sleep_for(std::chrono::seconds(20));
    std::cout << "Freeing shared memory...\n";

    client.FreeSharedMemory(shm_name);

    std::cout << "Hello World" << std::endl; 

    std::cout << "Shared memory message: " << static_cast<char*>(h_data) << "\n";

    return 0;
}
