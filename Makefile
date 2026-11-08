CXX ?= g++
ifeq ($(OS),Windows_NT)
 FFMPEG ?= .deps/ffmpeg
 TARGET=Appeal_FFmpeg_Video.dll
 CPPFLAGS=-I$(FFMPEG)/include
 LIBS=-L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -lswresample
 SHARED=-shared
else
 TARGET=Appeal_FFmpeg_Video.so
 CPPFLAGS=$(shell pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample)
 LIBS=$(shell pkg-config --libs libavformat libavcodec libavutil libswscale libswresample)
 SHARED=-shared -fPIC
endif
all: $(TARGET)
$(TARGET): src/main.cpp ../src/Video_Plugin_API.h
	$(CXX) -std=c++17 -Wall -Wextra $(CPPFLAGS) $(SHARED) src/main.cpp -o $@ $(LIBS)
.PHONY: all
