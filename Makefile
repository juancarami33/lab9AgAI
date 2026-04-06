CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2
TARGET  = departures_board

.PHONY: all clean run

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
