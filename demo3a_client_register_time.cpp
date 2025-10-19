#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <cuda_runtime_api.h>

const char* SHM_NAME = "/my_shared_mem";
const size_t SIZE = 40ULL * 1024 * 1024 * 1024; // 40 GB

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
    char* h_data;
    auto start = std::chrono::high_resolution_clock::now();
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms;
    int return_code = 0;

    // Open shared memory object
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    size_t page_size = 4 * 1024 * 1024; // 4MB
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;
    std::cout << "Aligned bytes: " << aligned_bytes << std::endl;

    h_data = (char*)mmap(nullptr, aligned_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }
    std::cout << "Shared memory opened successfully.\n";

    CHECK(cudaHostRegister((void*)h_data, aligned_bytes, cudaHostRegisterDefault));
    CHECK(cudaHostUnregister((void*)h_data));

    CHECK(cudaDeviceSynchronize());
    start = std::chrono::high_resolution_clock::now();
    CHECK(cudaHostRegister((void*)h_data, aligned_bytes, cudaHostRegisterDefault));
    CHECK(cudaDeviceSynchronize());
    end = std::chrono::high_resolution_clock::now();
    elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "cudaHostRegister took " << elapsed_ms << " ms\n";

    CHECK(cudaHostUnregister((void*)h_data));

cleanup:
    if (h_data != MAP_FAILED) munmap(h_data, aligned_bytes);
    if (shm_fd != -1) close(shm_fd);

    return return_code;
}
