#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <cuda_runtime_api.h>

const char* SHM_NAME = "/my_shared_mem";
const size_t SIZE = 4096;

#define CHECK(call)                                                   \
    {                                                                 \
        const cudaError_t error = call;                               \
        if (error != cudaSuccess) {                                   \
            std::cerr << "Error: " << __FILE__ << ":" << __LINE__     \
                      << ", code:" << error                           \
                      << ", reason: " << cudaGetErrorString(error)    \
                      << std::endl;                                   \
            return_code = 1;                                          \
            goto cleanup;                                             \
        }                                                             \
    }

int main() {
    char* h_data = nullptr;
    char* d_data = nullptr;
    char* h_result = new char[SIZE];
    int return_code = 0;

    // Open shared memory object
    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    void* ptr = aligned_alloc(4096, aligned_bytes);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        return 1;
    }

    // Pin memory
    CHECK(cudaHostRegister(ptr, aligned_bytes, cudaHostRegisterDefault));

    std::cout << "Shared memory opened and pinned successfully.\n";

    h_data = (char*)mmap(ptr, aligned_bytes, PROT_READ,
                        MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        cudaHostUnregister(ptr);
        free(ptr);
        close(shm_fd);
        return 1;
    }

    // Allocate GPU buffer
    CHECK(cudaMalloc((void**)&d_data, aligned_bytes));
    CHECK(cudaMemcpy(d_data, h_data, aligned_bytes, cudaMemcpyHostToDevice));

    // read from shared memory
    std::cout << "Client: Received message: " << h_data << std::endl;

cleanup:
    if (h_result) delete[] h_result;
    if (d_data) cudaFree(d_data);
    if (h_data != MAP_FAILED) munmap(h_data, aligned_bytes);
    if (ptr) {
        cudaHostUnregister(ptr);
        free(ptr);
    }
    if (shm_fd != -1) close(shm_fd);

    return return_code;
}