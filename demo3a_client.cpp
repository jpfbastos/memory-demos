#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <cuda_runtime_api.h>

const char* SHM_NAME = "/my_shared_mem";
const size_t SIZE = 10ULL * 1024 * 1024 * 1024; // 10 GB
const int NUM_WARM_UP = 1; // Number of warm-up iterations
const int NUM_TRIALS = 10; // Number of trials for averaging

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
    char* d_data;
    double total_time = 0;
    auto start = std::chrono::high_resolution_clock::now();
    auto end = std::chrono::high_resolution_clock::now();
    double bandwidth_gbps;
    double elapsed_ms;
    double avg_time;
    double avg_bandwidth;
    int return_code;

    // Open shared memory object
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    size_t page_size = 4 * 1024 * 1024; // 4MB
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    h_data = (char*)mmap(nullptr, aligned_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }
    std::cout << "Shared memory opened successfully.\n";

    //CHECK(cudaHostRegister((void*)h_data, aligned_bytes, cudaHostRegisterDefault));

    CHECK(cudaMalloc((void**)&d_data, aligned_bytes));

    // Warm-up the memory (touch all pages)
    for (int i = 0; i < NUM_WARM_UP; ++i) {
        CHECK(cudaMemcpy(d_data, h_data, SIZE, cudaMemcpyHostToDevice));
        CHECK(cudaDeviceSynchronize());
    }
    
    // Benchmark multiple trials
    total_time = 0;
    std::cout << "Running " << NUM_TRIALS << " trials...\n";
    
    for (int trial = 0; trial < NUM_TRIALS; trial++) {
        start = std::chrono::high_resolution_clock::now();
        
        CHECK(cudaMemcpy(d_data, h_data, SIZE, cudaMemcpyHostToDevice));
        CHECK(cudaDeviceSynchronize());

        end = std::chrono::high_resolution_clock::now();
        
        elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        total_time += elapsed_ms;
        
        bandwidth_gbps = (SIZE / (1024.0 * 1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
        std::cout << "Trial " << trial + 1 << ": " << elapsed_ms << " ms, " 
                  << bandwidth_gbps << " GB/s\n";
    }

    avg_time = total_time / NUM_TRIALS;
    avg_bandwidth = (SIZE / (1024.0 * 1024.0 * 1024.0)) / (avg_time / 1000.0);
    
    std::cout << "\nAverage H2D: " << avg_time << " ms, " 
              << avg_bandwidth << " GB/s" << std::endl;

    std::cout << "Message from writer: " << std::string(h_data, 24) << std::endl;
    CHECK(cudaHostUnregister((void*)h_data));

cleanup:
    if (d_data) cudaFree(d_data);
    if (h_data != MAP_FAILED) munmap(h_data, aligned_bytes);
    if (shm_fd != -1) close(shm_fd);

    return 0;
}