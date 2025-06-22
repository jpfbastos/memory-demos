#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <semaphore.h>
#include <cuda_runtime_api.h>

const char* SHM_NAME = "/my_shared_mem";
const char* MESSAGE = "Writer says hello :)";
const char* SEM_GRANT1 = "/sem_grant_1";
const char* SEM_DONE1 = "/sem_done_1";
const char* SEM_GRANT2 = "/sem_grant_2";
const char* SEM_DONE2 = "/sem_done_2";

const size_t SIZE = 4096;

int main() {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0640);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }

    if (ftruncate(shm_fd, SIZE) == -1) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    size_t page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_bytes = ((SIZE + page_size - 1) / page_size) * page_size;

    // Allocate aligned memory
    void* ptr = aligned_alloc(4096, aligned_bytes);
    if (!ptr) {
        std::cerr << "aligned_alloc failed\n";
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    // Pin memory for CUDA
    cudaError_t err = cudaHostRegister(ptr, aligned_bytes, cudaHostRegisterDefault);
    if (err != cudaSuccess) {
        std::cerr << "cudaHostRegister failed: " << cudaGetErrorString(err) << std::endl;
        free(ptr);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    std::cout << "Shared memory created and pinned successfully.\n";
    
    void* h_data = mmap(ptr, aligned_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_LOCKED, shm_fd, 0);
    if (h_data == MAP_FAILED) {
        perror("mmap");
        cudaHostUnregister(ptr);
        free(ptr);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }
    
    strcpy(static_cast<char*>(h_data), MESSAGE);

    sem_t* grant_sem1 = sem_open(SEM_GRANT1, O_CREAT, 0640, 0);
    sem_t* done_sem1  = sem_open(SEM_DONE1, O_CREAT, 0640, 0);
    sem_t* grant_sem2 = sem_open(SEM_GRANT2, O_CREAT, 0640, 0);
    sem_t* done_sem2  = sem_open(SEM_DONE2, O_CREAT, 0640, 0);

    std::cout << "Server: Granting access to Client 1...\n";
    sem_post(grant_sem1);
    sem_wait(done_sem1);
    std::cout << "Server: Client 1 finished. Revoking access and granting Client 2...\n";

    sem_post(grant_sem2);
    sem_wait(done_sem2);
    std::cout << "Server: Client 2 finished. Cleanup.\n";

    // Cleanup
    cudaHostUnregister(ptr);
    munmap(h_data, aligned_bytes);
    free(ptr);
    close(shm_fd);
    shm_unlink(SHM_NAME);

    sem_close(grant_sem1); sem_unlink(SEM_GRANT1);
    sem_close(done_sem1);  sem_unlink(SEM_DONE1);
    sem_close(grant_sem2); sem_unlink(SEM_GRANT2);
    sem_close(done_sem2);  sem_unlink(SEM_DONE2);

    return 0;
}
