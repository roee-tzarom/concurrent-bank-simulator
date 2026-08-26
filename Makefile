CC = gcc
CFLAGS = -Wall -Wextra -pthread

all: bank

bank: bank.c
	$(CC) $(CFLAGS) bank.c -o bank

clean:
	rm -f bank logs/transactions.log