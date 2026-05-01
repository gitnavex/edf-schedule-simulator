CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -ggdb3
LDFLAGS = -lreadline

TARGET  = scheduler
OBJS    = scheduler.o edf.o

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c edf.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJS) $(TARGET)
