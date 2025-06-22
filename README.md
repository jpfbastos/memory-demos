These are demos to show understanding of shared memory and shared pinned memory with a server/client interaction,
and calculate memory througput from shared pinned memory to GPU.

- Demo 1 uses a simple System V shared memory to share memory between two processes
- Demo 2 creates CUDA pinned memory, and shares it, with the client copying the memory to a GPU
- Demo 3a builds on this and calculates the copy throughput from shared pinned memory to GPU
- Demo 3b builds on Demo 2 by allowing two clients access the memory (one at a time) using semaphores,
  which revokes the current client's access once they have accessed the shared memory, allowing the next client to do so

### Setup
- NVIDIA GeForce RTX 4060 Ti (PCIE4.0x8)
- 5 GB of data
- Expected Throughput - 12GB/s

The following were the results of opening the server, and running Demo 3a 5 times in a row

|                   | 1 | 2 | 3 | 4 | 5 | Mean |
|-------------------|---|---|---|---|---|---|
| Throughput (GB/s) | 11.9394 | 11.9349 | 11.9276 | 11.9221 | 11.9334 | 11.9315 |
| Latency (ms)      | 428.832 | 428.993 | 429.256 | 429.455 | 429.049 | 429.117 |
