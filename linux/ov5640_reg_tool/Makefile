APP := ov5640-reg
OBJECTS := ov5640-reg.o

.PHONY: all clean

all: $(APP)

$(APP): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

clean:
	$(RM) $(APP) $(OBJECTS)
