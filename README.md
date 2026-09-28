# Concurrent Bank Simulator

A POSIX C simulation of account operations across multiple bank branches. It combines processes, threads and shared memory to make race conditions and synchronization strategies observable.

## Architecture

1. The parent loads fictional balances from `accounts.txt` into `mmap` shared memory and creates branch processes with `fork()`.
2. Branches start transaction and balance-inquiry threads.
3. Per-account synchronization protects writes and supports concurrent readers. A process-shared semaphore limits active branches.
4. Pipes return branch results to the parent; transactions are appended to `logs/transactions.log`.

The implementation also handles child-process signals and cleans up synchronization objects. The `NO_LOCK` compile flag provides a deliberate race-condition demonstration.

## Run

Requires Linux, WSL or another POSIX environment with threads, semaphores and `mmap`.

```bash
mkdir -p logs
make
./bank
```

Compare behavior without transaction locking:

```bash
gcc -Wall -Wextra -pthread -DNO_LOCK bank.c -o bank-no-lock
./bank-no-lock
```

Or run the supplied container configuration:

```bash
mkdir -p logs
docker compose up --build
```

The Compose file sets `N_BRANCHES=3`, `M_THREADS=5` and `N_TX=20`, and mounts the sample accounts and log directory. This is a concurrency exercise with fictional data, not a persistent banking service.


## Why this design is interesting

The same balance can be observed by threads in more than one branch process. Ordinary heap memory and an ordinary thread mutex would not be enough for that process boundary, so the program places account state in shared `mmap` memory and initializes synchronization objects for cross-process use. A branch-level semaphore limits concurrency. Parent/child pipes carry branch completion information back to the process that started the simulation.

The locking path protects account changes while allowing the program to demonstrate what goes wrong when protection is disabled. `race_mutex.png` and `race_no_lock.png` document the contrasting runs included in the repository. Results depend on scheduling, so a single run is not proof that a race cannot occur.

## Trace a transaction

1. `accounts.txt` seeds fictional account IDs and balances.
2. The parent allocates shared state and forks the configured number of branches.
3. Each branch launches workers that perform transfers and balance inquiries.
4. Workers update protected state, log transactions and report branch results.
5. The parent collects child results and releases shared resources.

`bank.c` contains the simulation and synchronization logic; the `Makefile` is the direct build path. The `Dockerfile` and `docker-compose.yml` supply a repeatable Linux environment and mount the example input and log directory. Environment values such as `N_BRANCHES`, `M_THREADS` and `N_TX` change the workload. This code is intended for studying inter-process coordination, not financial correctness, durable transactions or customer data.
