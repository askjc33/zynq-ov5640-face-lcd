APP := studyboard-ncnn-face
NCNN_INSTALL ?= /opt/ncnn-zynq
OPENMP ?= 0

CPPFLAGS += -I$(NCNN_INSTALL)/include/ncnn
CXXFLAGS += -O3 -Wall -Wextra -std=gnu++11 -pthread \
	-ffunction-sections -fdata-sections
LDFLAGS += -L$(NCNN_INSTALL)/lib -Wl,--gc-sections \
	-Wl,-rpath,'$$ORIGIN/lib' -static-libgcc
LDLIBS += -lncnn -pthread -lm -latomic

# Native GNU OpenMP needs -fopenmp during compilation and linking.  Use
# OPENMP=simple with an NCNN_SIMPLEOMP library: NCNN's static library then
# provides its own pthread-based OpenMP ABI and no libgomp link flag is used.
ifeq ($(OPENMP),1)
CXXFLAGS += -fopenmp
LDFLAGS += -fopenmp
endif

OBJECTS := studyboard-ncnn-face.o face_detector.o lcd_output.o video_stream_server.o

.PHONY: all clean

all: $(APP)

$(APP): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

studyboard-ncnn-face.o: studyboard-ncnn-face.cpp face_detector.h lcd_output.h video_stream_server.h
face_detector.o: face_detector.cpp face_detector.h
lcd_output.o: lcd_output.cpp lcd_output.h
video_stream_server.o: video_stream_server.cpp video_stream_server.h

clean:
	$(RM) $(APP) $(OBJECTS)
