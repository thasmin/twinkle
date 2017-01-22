# Install
BIN = main

# Flags
CFLAGS = -g -std=c++14

SRC = main.cpp common.cpp clip.cpp
OBJ = $(SRC:.cpp=.o)

ifeq ($(OS),Windows_NT)
BIN := $(BIN).exe
LIBS = -lmingw32 -lSDL2main -lSDL2 -lopengl32 -lm -lGLU32 -lGLEW32 -lavcodec -lavformat -lavutil -lswresample -lswscale -lavfilter
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		LIBS = -lSDL2 -framework OpenGL -lm -lGLEW -lavcodec -lavformat -lavutil -lswresample -lswscale -lavfilter
	else
		LIBS = -lSDL2 -lGL -lm -lGLU -lGLEW -lavcodec -lavformat -lavutil -lswresample -lswscale -lavfilter
	endif
endif

$(BIN): $(SRC)
	@$(CXX) $(CFLAGS) $^ -o $(BIN) $(LIBS)
