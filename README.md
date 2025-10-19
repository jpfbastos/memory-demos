These are demos to show understanding of shared memory and shared pinned memory with a server/client interaction,
and calculate memory througput from shared pinned memory to GPU.

- Demo 1 uses a simple System V shared memory to share memory between two processes
- Demo 2 creates CUDA pinned memory, and shares it, with the client copying the memory to a GPU
- Demo 3a builds on this and calculates the time taken for CUDA to register memory in host memory (`demo3a_register_time.cpp`) and the copy throughput from shared pinned memory to GPU (`demo3a_throughput.cpp`)
- Demo 4a uses a gRPC server to manage a shared memory pool, and a fixed-size chunk is returned to the client for use
- Demo 4b would add a permission check, where only the specific PID could access the shared memory \[NOT WORKING]
- Demo 5 allows for clients to allocate and free memory variable sizes. Chunks are collected and mapped to form seemingly contiguous memory for the client to use under a single shared memory name

### Setup
- NVIDIA GeForce RTX 4060 Ti (PCIE4.0x8)
- 5 GB of data
- Expected Throughput - 12GB/s

The following were the results of opening the server, and running Demo 3a 5 times in a row

|                   | 1 | 2 | 3 | 4 | 5 | Mean |
|-------------------|---|---|---|---|---|---|
| Throughput (GB/s) | 11.6596 | 11.6552 | 11.6480 | 11.6427 | 11.6537 | 11.6518 |
| Latency (ms)      | 428.832 | 428.993 | 429.256 | 429.455 | 429.049 | 429.117 |
