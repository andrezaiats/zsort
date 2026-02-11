CC = gcc
CFLAGS = -O3 -march=native -flto -pthread -Wall -Wextra
TARGET = zsort

all: $(TARGET)

$(TARGET): zsort.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
