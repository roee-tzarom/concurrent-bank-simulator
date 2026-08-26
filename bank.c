#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>   // open(), O_RDONLY
#include <unistd.h>  // read(), close()
#include <sys/wait.h> // wait/waitpid
#include <signal.h> // Signals
#include <pthread.h>
#include <time.h>
#include <errno.h>  // saving/restoring errno in SIGCHLD handler
#include <sys/mman.h> // mmap(), munmap() For Shared Memory
#include <semaphore.h> // Bonus A + Bonus B: sem_t

#define MAX_ACCOUNTS 64
#define N_BRANCHES 3
#define M_THREADS 5
#define N_TX 20
#define ACTIVE_MAX 2   // Bonus A: max branches allowed to run threads at once
#define INQUIRY_THREADS 2 // Bonus B: reader threads per branch

// משתנים גלובליים שיכולים להשתנות ע"י משתני סביבה (חלק 4)
int g_n_branches = N_BRANCHES;
int g_m_threads = M_THREADS;
int g_n_tx = N_TX;

// 2.1: Argument structure that each thread will receive
typedef struct {
    int branch_id;
    int thread_id;
} thread_args_t;

// Bonus B: readers-writers lock, built manually with semaphores
// ("as long as at least one reader is inside, writers wait")
typedef struct {
    sem_t read_count_mutex; // protects read_count itself
    sem_t room_empty;       // held by a writer, or by the first reader in
    int read_count;         // how many readers are currently inside
} rw_lock_t;

// single bank account
typedef struct {
    int id;
    char owner[64];
    long balance;
    int tx_count; // number of transactions performed
    rw_lock_t rw; // Bonus B: readers-writers lock instead of a plain mutex
} account_t;

// 3.1: Branch Result Structure
typedef struct {
    int branch_id;
    int total_transactions;
    long total_deposited;
    long total_withdrawn;
} branch_result_t;

account_t *accounts; // Pointer for Shared Memory
int num_accounts;

// Bonus A: counting semaphore that throttles how many branches may run
// their worker threads at the same time (process-shared, like accounts)
sem_t *branch_sem;

long branch_total_deposited = 0;
long branch_total_withdrawn = 0;

pthread_mutex_t delta_lock = PTHREAD_MUTEX_INITIALIZER;

// 3.2: Global flag for SIGINT/SIGTERM
volatile sig_atomic_t stop_flag = 0;

// Handler for SIGINT (Ctrl-C) and SIGTERM
void sig_handler(int sig) {
    if (sig == SIGINT) {
        const char msg[] = "\nReceived SIGINT — waiting for branches to finish...\n";
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    } else if (sig == SIGTERM) {
        const char msg[] = "\nReceived SIGTERM — initiating graceful shutdown.\n";
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    }
    stop_flag = 1; // Update the global flag
}

// Handler for SIGCHLD to reap zombies in the background
void sigchld_handler(int sig) {
    (void)sig; // Suppress unused warning
    int saved_errno = errno;
    
    // Reap any child that has exited, without blocking (WNOHANG)
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // Just reap, nothing else to do here
    }
    
    errno = saved_errno;
}

/*
 * 1.3: תהליך Zombie - הסבר
כשתהליך-ילד מסיים לרוץ ויוצא, הוא לא נעלם מהמערכת באופן מיידי. הליבה של מערכת ההפעלה שומרת עבורו
רשומה קטנה - בעיקר את קוד היציאה שלו - כדי שתהליך ההורה יוכל לקרוא אותה בהמשך ולדעת איך הילד הסתיים.
הבעיה מתחילה כשההורה לא טורח לקרוא את הרשומה הזו. כל עוד הוא לא עושה זאת, הילד שכבר סיים לרוץ עדיין
תופס מקום בטבלת התהליכים של המערכת, למרות שהוא לא באמת פעיל יותר ולא מבצע שום עבודה.
תהליך כזה - שסיים, אבל אף אחד עוד לא "אסף" את התוצאה שלו - נקרא זומבי.
אם ההורה אף פעם לא קורא לפונקציית ההמתנה, הזומבי יכול להישאר שם לנצח,
ותאורטית אם יש הרבה כאלה, זה עלול למלא את טבלת התהליכים.
בתוכנית שלנו זה לא קורה: ההורה קורא לפונקציית ההמתנה פעם אחת עבור כל אחד
מהסניפים שהוא יצר, כך שכל ילד נאסף כראוי ולא נשארים זומבים.
 */

/*
 * 1.3: שיטה חלופית למניעת Zombies - handler ל-SIGCHLD
 * יש עוד דרך נהוגה למנוע היווצרות של תהליכי זומבי, מלבד קריאה ישירה לפונקציית ההמתנה:
 * SIGCHLD לאות handler התקנת.
 * בכל פעם שתהליך-ילד מסיים לרוץ, הליבה של המערכת שולחת אוטומטית להורה שלו את האות הזה.
 * ירוץ בכל פעם שילד מסיים handler-לאות הזה מראש, ה handler אם ההורה התקין,
 * ויוכל לאסוף אותו מיד באמצעות קריאה לפונקציית ההמתנה במצב לא-חוסם,
 * כדי שהקריאה הזו לעולם לא תיתקע גם אם באותו רגע אין עדיין אף ילד שסיים.
 * * * בתוכנית שלנו לא השתמשנו בשיטה הזו בפועל, כי ההורה כבר אוסף כל ילד
 * באמצעות קריאה רגילה לפונקציית ההמתנה, בתוך הלולאה הראשית של הפונקציה הראשית.
 * הקוד:
 * #include <signal.h>
 *
 * void sigchld_handler(int sig) {
 * (void)sig;
 * int saved_errno = errno;
 * while (waitpid(-1, NULL, WNOHANG) > 0) {
 * // אוספים כל ילד שכבר יצא, אחד אחרי השני
 * }
 * errno = saved_errno;
 * }
 *
 * // main()-לפני ה fork-ב
 * // signal(SIGCHLD, sigchld_handler);
 */

int load_accounts(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open accounts.txt failed");
        return -1;
    }

    char buffer[4096];
    ssize_t total_read = 0;
    ssize_t n;

    while ((n = read(fd, buffer + total_read, sizeof(buffer) - total_read - 1)) > 0) {
        total_read += n;
    }
    close(fd);

    if (n < 0) {
        perror("read accounts.txt failed");
        return -1;
    }

    buffer[total_read] = '\0';
    
    printf("%-12s | %-12s | %-12s\n", "Account ID", "Owner Name", "Balance");
    printf("------------------------------------------\n");

    char *line = strtok(buffer, "\n");
    while (line != NULL) {
        if (line[0] != '#' && line[0] != '\0') {
            account_t acc;
            int parsed = sscanf(line, "%d %63s %ld", &acc.id, acc.owner, &acc.balance);
            if (parsed == 3) {
                acc.tx_count = 0;
                if (num_accounts < MAX_ACCOUNTS) {
                    accounts[num_accounts] = acc;
                    
                    // Bonus B: initialize the readers-writers lock as PROCESS-SHARED
                    // (the "1" second argument to sem_init means "shared between processes")
                    sem_init(&accounts[num_accounts].rw.read_count_mutex, 1, 1);
                    sem_init(&accounts[num_accounts].rw.room_empty, 1, 1);
                    accounts[num_accounts].rw.read_count = 0;
                    
                    num_accounts++;
                    printf("%-12d | %-12s | %-12ld\n", acc.id, acc.owner, acc.balance);
                } else {
                    fprintf(stderr, "Warning: MAX_ACCOUNTS exceeded\n");
                }
            }
        }
        line = strtok(NULL, "\n");
    }   
    printf("Total accounts loaded: %d\n\n", num_accounts);
    return num_accounts;
}

// Simple function to write a log entry (2.1)
void write_log(int branch_id, int thread_id, int account_id, const char *operation, int amount, long new_balance) {
    char buffer[256];
    char time_str[64];
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    
    sprintf(buffer, "[%s] Branch: %d, Thread: %d, Account: %d, Op: %s, Amount: %d, Balance: %ld\n",
            time_str, branch_id, thread_id, account_id, operation, amount, new_balance);

    // השתמש בתיקיית logs עבור Docker (חלק 4)
    int fd = open("logs/transactions.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, buffer, strlen(buffer)); 
        close(fd);
    }
}

// Bonus B: reader enters - only the FIRST reader locks room_empty,
// which blocks writers as long as at least one reader is inside.
void reader_lock(rw_lock_t *rw) {
    sem_wait(&rw->read_count_mutex);
    rw->read_count++;
    if (rw->read_count == 1) {
        sem_wait(&rw->room_empty);
    }
    sem_post(&rw->read_count_mutex);
}

// Bonus B: reader leaves - only the LAST reader releases room_empty
void reader_unlock(rw_lock_t *rw) {
    sem_wait(&rw->read_count_mutex);
    rw->read_count--;
    if (rw->read_count == 0) {
        sem_post(&rw->room_empty);
    }
    sem_post(&rw->read_count_mutex);
}

// Bonus B: a writer just needs the room to itself
void writer_lock(rw_lock_t *rw) {
    sem_wait(&rw->room_empty);
}

void writer_unlock(rw_lock_t *rw) {
    sem_post(&rw->room_empty);
}

// 2.1: The worker thread function
void *worker_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    unsigned int seed = time(NULL) + args->branch_id + args->thread_id;
    long local_dep = 0;
    long local_with = 0;

    for (int i = 0; i < g_n_tx; i++) {
        int acc_idx = rand_r(&seed) % num_accounts; // Random account
        int amount = (rand_r(&seed) % 500) + 1;     // Random amount 1-500
        int is_deposit = rand_r(&seed) % 2;         // 0 (withdraw) or 1 (deposit)
        
        account_t *acc = &accounts[acc_idx];
        
#ifdef NO_LOCK
        // 2.2: No mutex — run without protection
        if (is_deposit) {
            acc->balance += amount;
            write_log(args->branch_id, args->thread_id, acc->id, "DEPOSIT", amount, acc->balance);
            local_dep += amount;
        } else {
            // Never let balance drop below 0
            if (acc->balance >= amount) {
                acc->balance -= amount;
                write_log(args->branch_id, args->thread_id, acc->id, "WITHDRAW", amount, acc->balance);
                local_with += amount;
            }
        }
        acc->tx_count++;
#else
        // 2.3: Mutex protection per-account - keep the locked section as short
        // as possible: only touch the balance itself, nothing else.
        int did_deposit = 0;
        int did_withdraw = 0;
        long balance_after = 0;

        writer_lock(&acc->rw); // Bonus B: deposit/withdraw is a WRITE

        if (is_deposit) {
            acc->balance += amount;
            did_deposit = 1;
        } else if (acc->balance >= amount) {
            // Never let balance drop below 0
            acc->balance -= amount;
            did_withdraw = 1;
        }
        acc->tx_count++;
        balance_after = acc->balance;

        writer_unlock(&acc->rw);

        // Logging (disk I/O) happens AFTER the unlock, so the lock is held
        // only for the in-memory balance update, not for the slower write.
        if (did_deposit) {
            write_log(args->branch_id, args->thread_id, acc->id, "DEPOSIT", amount, balance_after);
            local_dep += amount;
        } else if (did_withdraw) {
            write_log(args->branch_id, args->thread_id, acc->id, "WITHDRAW", amount, balance_after);
            local_with += amount;
        }
#endif
    }
    
    // Update branch total delta for verification safely
    pthread_mutex_lock(&delta_lock);
    branch_total_deposited += local_dep;
    branch_total_withdrawn += local_with;
    pthread_mutex_unlock(&delta_lock);
    
    return NULL;
}

// Bonus B: a reader thread that only checks balances, never modifies them
void *inquiry_thread(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    unsigned int seed = time(NULL) + args->branch_id + args->thread_id + 1000;

    for (int i = 0; i < g_n_tx; i++) {
        int acc_idx = rand_r(&seed) % num_accounts;
        account_t *acc = &accounts[acc_idx];

        reader_lock(&acc->rw);
        long snapshot = acc->balance; // just reading, never writing
        reader_unlock(&acc->rw);

        printf("[Inquiry] Branch %d Thread %d: account %d balance = %ld\n",
               args->branch_id, args->thread_id, acc->id, snapshot);
    }

    return NULL;
}

void run_branch_simulation(int branch_id, int write_fd) {
    printf("Branch %d started simulation...\n", branch_id);

   pthread_t threads[g_m_threads];
    thread_args_t args[g_m_threads];

    // Bonus B: separate reader threads for balance inquiries
    pthread_t inquiry_threads[INQUIRY_THREADS];
    thread_args_t inquiry_args[INQUIRY_THREADS];

    // Bonus A: wait for a free "slot" before this branch's threads start
    sem_wait(branch_sem);

    // 2.1: Spawn threads
    for (int i = 0; i < g_m_threads; i++) {
        args[i].branch_id = branch_id;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    // Bonus B: spawn the inquiry (reader) threads alongside the workers
    for (int i = 0; i < INQUIRY_THREADS; i++) {
        inquiry_args[i].branch_id = branch_id;
        inquiry_args[i].thread_id = i;
        pthread_create(&inquiry_threads[i], NULL, inquiry_thread, &inquiry_args[i]);
    }

    // 2.4: Call pthread_join for each thread
    for (int i = 0; i < g_m_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // Bonus B: join the inquiry threads too
    for (int i = 0; i < INQUIRY_THREADS; i++) {
        pthread_join(inquiry_threads[i], NULL);
    }

    // Bonus A: release the slot - another waiting branch can now proceed
    sem_post(branch_sem);

    // Since accounts are in shared memory, calculate local branch TX exactly
    int total_branch_tx = g_m_threads * g_n_tx;
    
    // Part 3: Populate the result structure and write to pipe
    branch_result_t result;
    result.branch_id = branch_id;
    result.total_transactions = total_branch_tx; 
    result.total_deposited = branch_total_deposited;
    result.total_withdrawn = branch_total_withdrawn;

    // Write exactly sizeof(branch_result_t) bytes to the write end of the pipe
    if (write(write_fd, &result, sizeof(branch_result_t)) < 0) {
        perror("write to pipe failed");
    }
    
    printf("Branch %d finished.\n", branch_id);
}

int main() {
    // קריאת משתני סביבה לפי דרישת סעיף 4 ודריסת ברירות המחדל אם הוגדרו 
    char *env_branches = getenv("N_BRANCHES");
    if (env_branches) g_n_branches = atoi(env_branches);

    char *env_threads = getenv("M_THREADS");
    if (env_threads) g_m_threads = atoi(env_threads);

    char *env_tx = getenv("N_TX");
    if (env_tx) g_n_tx = atoi(env_tx);

    // 3.2: Install signal handlers before fork
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART; // Prevent interrupted system calls (like read)
    sigaction(SIGCHLD, &sa_chld, NULL);

    // Allocate shared memory for accounts so all forks see the exact same memory
    accounts = mmap(NULL, sizeof(account_t) * MAX_ACCOUNTS, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (accounts == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    // Bonus A: shared memory for the throttle semaphore, same idea as accounts
    branch_sem = mmap(NULL, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (branch_sem == MAP_FAILED) {
        perror("mmap failed (branch_sem)");
        return 1;
    }
    sem_init(branch_sem, 1, ACTIVE_MAX); // "1" = shared between processes

    num_accounts = 0;

    if (load_accounts("accounts.txt") < 0) {
        return 1;
    }

    long initial_bank_sum = 0;
    for (int i = 0; i < num_accounts; i++) {
        initial_bank_sum += accounts[i].balance;
    }

    fflush(stdout); // flush now, so buffered text is not copied into every child by fork()

    // Dynamic arrays based on environment variables
    int pipes[g_n_branches][2];
    for (int i = 0; i < g_n_branches; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe failed");
            return 1;
        }
    }

    for (int i = 0; i < g_n_branches; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork failed");
            return 1;
        }

        if (pid == 0) {
            close(pipes[i][0]);
            
            run_branch_simulation(i, pipes[i][1]);
            
            close(pipes[i][1]);
            exit(0);
        } else {
            close(pipes[i][1]);
        }
    }

    // 3.1: Parent reads from pipes and prints unified report 
    branch_result_t results[g_n_branches];
    long grand_total_tx = 0;
    long grand_total_dep = 0;
    long grand_total_with = 0;

    // Step 1: Read data from pipes and wait for all children to finish
    int shutdown_message_shown = 0;
    for (int i = 0; i < g_n_branches; i++) {

        // 3.2: the main loop checks the flag set by the SIGINT/SIGTERM handler.
        // We never kill children forcefully - we just keep waiting for them
        // to finish naturally, exactly like the rest of this loop already does.
        if (stop_flag && !shutdown_message_shown) {
            printf("Shutdown requested - still waiting for remaining branches to finish naturally...\n");
            shutdown_message_shown = 1;
        }

        if (read(pipes[i][0], &results[i], sizeof(branch_result_t)) < 0) {
            perror("read from pipe failed");
        }
        close(pipes[i][0]);
        
        int status;
        wait(&status); // Wait for the child process to fully terminate
    }

    // Step 2: The terminal is now clear - safely print the unified report
    printf("\n=== Final Bank Report ===\n");
    for (int i = 0; i < g_n_branches; i++) {
        printf("Branch %d: %-4d transactions | +%-8ld deposited | -%-8ld withdrawn\n",
               results[i].branch_id, results[i].total_transactions, results[i].total_deposited, results[i].total_withdrawn);
        
        grand_total_tx += results[i].total_transactions;
        grand_total_dep += results[i].total_deposited;
        grand_total_with += results[i].total_withdrawn;
    }

    printf("Total   : %-4ld transactions | +%-8ld deposited | -%-8ld withdrawn\n", 
           grand_total_tx, grand_total_dep, grand_total_with);

    printf("\nAccount balances after all transactions:\n");
    long actual_final_sum = 0;
    for (int i = 0; i < num_accounts; i++) {
        printf("%-6d %-8s : $%ld\n", accounts[i].id, accounts[i].owner, accounts[i].balance);
        actual_final_sum += accounts[i].balance;
    }
    
    // Global balance conservation check
    long expected_final_sum = initial_bank_sum + grand_total_dep - grand_total_with;
    
    if (expected_final_sum == actual_final_sum) {
        printf("Balance conservation check: PASSED (Expected: %ld, Actual: %ld)\n", expected_final_sum, actual_final_sum);
    } else {
        printf("Balance conservation check: FAILED (Expected: %ld, Actual: %ld)\n", expected_final_sum, actual_final_sum);
    }

// Bonus B: destroy the readers-writers semaphores for every account
    for (int i = 0; i < num_accounts; i++) {
        sem_destroy(&accounts[i].rw.read_count_mutex);
        sem_destroy(&accounts[i].rw.room_empty);
    }
    
    munmap(accounts, sizeof(account_t) * MAX_ACCOUNTS); // Free shared memory

    // Bonus A: destroy and unmap the throttle semaphore
    sem_destroy(branch_sem);
    munmap(branch_sem, sizeof(sem_t));

    printf("\nAll branches finished. Bank Coordinator exiting.\n");
    return 0;
}