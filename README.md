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
