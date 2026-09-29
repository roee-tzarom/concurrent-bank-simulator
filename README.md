# Concurrent Bank Simulator

A POSIX C simulation that coordinates account activity across processes and threads. Branch processes work on shared account balances while transaction and inquiry threads expose the difference between synchronized and unsynchronized updates. The checked-in account data is fictional.

## Architecture

```text
accounts.txt
    ↓
parent process ── shared mmap account state ── branch processes
    ↑                    │                         ├─ transaction workers
    └──── pipes ──────────┘                         └─ balance inquiries
                         └─ cross-process locks and semaphore
```

The parent loads balances, creates shared memory and forks branches. Workers perform transactions and read balances; a process-shared semaphore limits the number of active branches. Synchronization protects account changes across process boundaries. Pipes return branch results to the parent, and transaction activity is written to `logs/transactions.log`.

The `NO_LOCK` build flag deliberately removes transaction locking so race behavior can be compared with the normal build. The two race images in the repository document example runs; scheduling can produce different results from run to run.

## Build and run

Use Linux, WSL or another POSIX environment with `gcc`, pthreads, semaphores, `fork` and `mmap`:

```bash
mkdir -p logs
make
./bank
```

To run the unsynchronized variant as a separate binary:

```bash
gcc -Wall -Wextra -pthread -DNO_LOCK bank.c -o bank-no-lock
./bank-no-lock
```

A container setup is also included:

```bash
mkdir -p logs
docker compose up --build
```

`docker-compose.yml` supplies `N_BRANCHES=3`, `M_THREADS=5` and `N_TX=20`, and mounts the example account file and log directory.

## Explore the implementation

| File | Role |
| --- | --- |
| `bank.c` | Shared state, process and thread lifecycle, synchronization and logging |
| `accounts.txt` | Fictional starting balances |
| `Makefile` | Direct POSIX build |
| `Dockerfile`, `docker-compose.yml` | Repeatable Linux environment |

Start at `main` in `bank.c`, then follow the creation of shared account state and the branch worker functions. Compare the balance update path with and without `NO_LOCK` to see why a lock must be shared across processes rather than kept in ordinary thread-local heap memory.

This is an in-memory concurrency model. It does not implement a durable database, customer authentication or financial transaction guarantees.
