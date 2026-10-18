# Build the isolated-VM launcher. It talks to /dev/hyperenclave; the
# monitor hypercalls themselves stay in the driver.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror

ivm_launch: ivm_launch.c he_ivm.h
	$(CC) $(CFLAGS) -o $@ ivm_launch.c

clean:
	rm -f ivm_launch
