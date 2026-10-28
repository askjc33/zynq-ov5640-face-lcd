APP := ov5640-init
OBJECTS := ov5640-init.o

.PHONY: all clean

all: $(APP)

$(APP): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

ov5640-init.o: ov5640-init.c ov5640-init-table.h

clean:
	$(RM) $(APP) $(OBJECTS)
