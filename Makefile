CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = ports

all: $(TARGET)

$(TARGET): ports.c
	$(CC) $(CFLAGS) -o $(TARGET) ports.c

# Synology NAS build target with static linking for better portability
synology: ports.c
	$(CC) $(CFLAGS) -static -o $(TARGET) ports.c

# Synology cross-compilation (customize CC_SYNOLOGY for your toolchain)
# Example: CC_SYNOLOGY = arm-linux-gnueabihf-gcc
synology-cross:
	@if [ -z "$(CC_SYNOLOGY)" ]; then \
		echo "Error: Set CC_SYNOLOGY to your Synology cross-compiler"; \
		echo "Example: make synology-cross CC_SYNOLOGY=arm-linux-gnueabihf-gcc"; \
		exit 1; \
	fi
	$(CC_SYNOLOGY) $(CFLAGS) -static -o $(TARGET) ports.c

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
