CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -D_DEFAULT_SOURCE
BIN      := bin

all: $(BIN)/tftpd $(BIN)/tftp

$(BIN)/tftpd: src/tftpd.c src/tftp.c src/tftp.h | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/tftpd.c src/tftp.c

$(BIN)/tftp: src/tftp_client.c src/tftp.c src/tftp.h | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/tftp_client.c src/tftp.c

$(BIN):
	mkdir -p $@

test: all
	sh tests/run_tests.sh

clean:
	rm -rf $(BIN)

.PHONY: all test clean
