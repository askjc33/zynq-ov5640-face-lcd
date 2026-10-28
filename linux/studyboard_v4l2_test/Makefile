APPS := studyboard-v4l2-test studyboard-v4l2-latest \
	studyboard-v4l2-preprocess

CPPFLAGS +=
CFLAGS += -O2 -Wall -Wextra -std=gnu11 -pthread
LDLIBS += -pthread

.PHONY: all clean

all: $(APPS)

studyboard-v4l2-test: studyboard-v4l2-test.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

studyboard-v4l2-latest: studyboard-v4l2-latest.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

studyboard-v4l2-preprocess: studyboard-v4l2-preprocess.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(APPS) *.o
