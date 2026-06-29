PROG = flow_raw_backup
SRCS = flow_raw_backup.c
OBJS = $(SRCS:.c=.o)

# libcurl: resolved against the cross sysroot via pkg-config (set up by the SDK).
# zlib:    linked directly with -lz (present in the SDK sysroot).
CFLAGS  += -Wall -O2 $(shell pkg-config --cflags libcurl)
LDLIBS  += $(shell pkg-config --libs libcurl) -lz

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(PROG) $(OBJS)
