CC = gcc
CFLAGS = -I. -I../include -O2 -Wall
TARGET = vendor-reset
SRCS = vendor-reset.c ucommon.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
