# Install
BIN = main

# Flags
CFLAGS = -g -std=c++14 -O0

SRC = main.cpp common.cpp clip.cpp logger.cpp
OBJ = $(SRC:.cpp=.o)

LIBS = -lSDL2 -lm -lavcodec -lavformat -lavutil -lswresample -lswscale -lavfilter

ifeq ($(OS),Windows_NT)
BIN := $(BIN).exe
LIBS += -lmingw32 -lSDL2main -lopengl32 -lGLU32 -lGLEW32
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		SRC += mac.mm
		LIBS += -framework OpenGL -lGLEW -framework Cocoa
	else
		LIBS += -lGL -lGLU -lGLEW
	endif
endif

# local ffmpeg flags
#CFLAGS += -I../ffmpeg
#LIBS += -L../ffmpeg/libavcodec -L../ffmpeg/libavformat -L../ffmpeg/libavutil -L../ffmpeg/libswresample -L../ffmpeg/libswscale -L../ffmpeg/libavfilter
#LIBS += -framework Cocoa -framework VideoToolbox -framework VideoDecodeAcceleration -framework AudioToolbox -framework Security
#LIBS += -framework CoreAudio -framework CoreImage -framework CoreVideo -framework CoreFoundation -framework CoreMedia
#LIBS += -lz -llzma -liconv -lbz2

$(BIN): $(SRC)
	@$(CXX) $(CFLAGS) $^ -o $(BIN) $(LIBS)
