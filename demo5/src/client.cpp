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
using allocator::v1::ChunkInfo;

struct LocalChunkInfo {
    void* ptr;  // Pointer to the mapped chunk
    size_t size;  // Size of the chunk
    off_t offset;  // Offset in the shared memory pool
};

class AllocatorClient {
public:
    explicit AllocatorClient(std::shared_ptr<Channel> channel)
        : stub_(Allocator::NewStub(channel)),
          shm_fd_(-1),
          h_data_(nullptr),
          total_size_(0),
          offset_(-1) {}

    // Requests shared memory of 'size' bytes from the server,
    // maps it locally, and returns true on success.
    bool AllocateMemory(size_t size = 1) {
        MemoryRequest request;
        MemoryReply reply;
        ClientContext context;

        request.set_size(size);

        Status status = stub_->AllocateMemory(&context, request, &reply);
        if (!status.ok()) {
            std::cerr << "gRPC request failed: " << status.error_message() << std::endl;
            return false;
        }

        if (reply.chunks().empty() || reply.total_size() == 0) {
            std::cerr << "Failed to get valid memory allocation\n";
            return false;
        }

        shm_name_ = reply.shm_name();
        total_size_ = reply.total_size();
        offset_ = reply.chunks(0).offset();

        // Open shared memory segment
        shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0640);
        if (shm_fd_ == -1) {
            perror("shm_open");
            return false;
        }

        // Step 1: Reserve contiguous virtual address space
        h_data_ = mmap(nullptr, total_size_, PROT_NONE, 
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (h_data_ == MAP_FAILED) {
            perror("Failed to reserve virtual address space");
            close(shm_fd_);
            shm_fd_ = -1;
            return false;
        }

        // Step 2: Map each chunk into the reserved space
        size_t virtual_offset = 0;
        for (const auto& chunk_info : reply.chunks()) {
            std::cout << "Mapping chunk at offset " << chunk_info.offset() 
                      << " with size " << chunk_info.size() << std::endl;
            void* chunk_ptr = mmap(
                static_cast<char*>(h_data_) + virtual_offset,  // Fixed position
                chunk_info.size(),
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED,
                shm_fd_,
                chunk_info.offset()  // Pool offset
            );

            if (chunk_ptr == MAP_FAILED) {
                perror("Failed to map chunk");
                munmap(h_data_, total_size_);
                close(shm_fd_);
                shm_fd_ = -1;
                h_data_ = nullptr;
                total_size_ = 0;
                shm_name_.clear();
                chunks_.clear();
                return false;
            }

            chunks_.push_back({chunk_ptr, chunk_info.size(), chunk_info.offset()});
            virtual_offset += chunk_info.size();
        }

        std::cout << "Client mapped " << chunks_.size() << " chunks totaling " 
                  << total_size_ << " bytes at " << h_data_ << std::endl;

        return true;
    }

    // Frees the shared memory allocation on server and unmaps locally
    bool FreeSharedMemory() {
        if (shm_fd_ == -1 || h_data_ == nullptr) {
            std::cerr << "No shared memory mapped to free.\n";
            return false;
        }

        FreeRequest request;
        FreeReply reply;
        ClientContext context;

        for (const auto& chunk : chunks_) {
            request.add_offset(chunk.offset);  // Add each chunk's offset
        }

        Status status = stub_->FreeSharedMemory(&context, request, &reply);
        if (!status.ok()) {
            std::cerr << "gRPC free request failed: " << status.error_message() << std::endl;
            return false;
        }

        // Unmap and close local mapping
        if (munmap(h_data_, total_size_) == -1) {
            perror("munmap");
        }
        close(shm_fd_);

        // Reset members to safe state
        h_data_ = nullptr;
        shm_fd_ = -1;
        total_size_ = 0;
        offset_ = -1;
        shm_name_.clear();

        std::cout << "Shared memory freed and unmapped successfully.\n";
        return true;
    }

    // Accessor for the mapped memory pointer
    void* getDataPointer() const {
        return h_data_;
    }

    size_t getTotalSize() const {
        return total_size_;
    }

private:
    std::unique_ptr<Allocator::Stub> stub_;

    int shm_fd_;
    void* h_data_;
    size_t total_size_;
    off_t offset_;
    std::string shm_name_;
    std::vector<LocalChunkInfo> chunks_;
};

AllocatorClient* g_client_ptr = nullptr;

void sigint_handler(int signum) {
    std::cout << "Received SIGINT. Cleaning up...\n";
    if (g_client_ptr) {
        g_client_ptr->FreeSharedMemory();
    }
    std::cout << "Exiting program due to SIGINT.\n";
    exit(1);
}

int main(int argc, char *argv[]) {
    AllocatorClient client(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

    g_client_ptr = &client; // set global pointer

    signal(SIGINT, sigint_handler);

    int size = (argc > 1) ? std::stoi(argv[1]) * 1024 * 1024 : 4 * 1024 * 1024;

    if (!client.AllocateMemory(size)) {
        std::cerr << "Failed to request shared memory.\n";
        return -1;
    }

    char* h_data = static_cast<char*>(client.getDataPointer());
    size_t total_size = client.getTotalSize();

    // Print message from server
    std::cout << "Shared memory message: " << h_data << "\n";

    uint32_t* data = reinterpret_cast<uint32_t*>(h_data);
    size_t total_elements = total_size / sizeof(uint32_t);
    size_t step = (4 * 1024 * 1024) / sizeof(uint32_t);  // 4MB step in uint32_t units

    for (size_t i = 0; i < total_elements; i += step) {
        std::cout << "Int at offset " << i << ": " << data[i] << std::endl;
    }


    // wait for enter
    std::cout << "Press Enter to free shared memory...\n";
    std::cin.get();

    std::cout << "Freeing shared memory...\n";

    client.FreeSharedMemory();

    std::cout << "Hello World" << std::endl; 

    std::cout << "Shared memory message: " << h_data << "\n";

    return 0;
}
