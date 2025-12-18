CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = ports

all: $(TARGET)

$(TARGET): ports.c
	$(CC) $(CFLAGS) -o $(TARGET) ports.c

check:
	@echo "Build check passed - no tests defined yet"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
