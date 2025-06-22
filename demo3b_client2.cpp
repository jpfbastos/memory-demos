#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <semaphore.h>
#include <cuda_runtime_api.h>

const char* SHM_NAME = "/my_shared_mem";
const char* SEM_GRANT1 = "/sem_grant_2";
const char* SEM_DONE1  = "/sem_done_2";
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
    int return_code = 0;

    sem_t* grant_sem = sem_open(SEM_GRANT1, 0);
    sem_t* done_sem  = sem_open(SEM_DONE1, 0);
    if (grant_sem == SEM_FAILED || done_sem == SEM_FAILED) {
        std::cerr << "Failed to open semaphores: " << strerror(errno) << std::endl;
        return 1;
    }
    std::cout << "Client 2: Waiting for access...\n";
    sem_wait(grant_sem);
    
    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        sem_close(grant_sem);
        sem_close(done_sem);
        return 1;
    }

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;
    
    void* ptr = aligned_alloc(4096, aligned_bytes);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        sem_close(grant_sem);
        sem_close(done_sem);
        return 1;
    }

    CHECK(cudaHostRegister(ptr, aligned_bytes, cudaHostRegisterDefault));
    
    h_data = (char*)mmap(ptr, aligned_bytes, PROT_READ,
                        MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        cudaHostUnregister(ptr);
        free(ptr);
        close(shm_fd);
        sem_close(grant_sem);
        sem_close(done_sem);
        return 1;
    }

    CHECK(cudaMalloc((void**)&d_data, aligned_bytes));
    cudaMemcpy(d_data, h_data, aligned_bytes, cudaMemcpyHostToDevice);

    // print message from shared memory
    std::cout << "Client 2: Received message: " << h_data << std::endl;

    sem_post(done_sem);

cleanup:
    if (d_data) cudaFree(d_data);
    if (h_data != MAP_FAILED) munmap(h_data, aligned_bytes);
    if (ptr) {
        cudaHostUnregister(ptr);
        free(ptr);
    }
    if (shm_fd != -1) close(shm_fd);
    sem_close(grant_sem);
    sem_close(done_sem);
    return return_code;
}
