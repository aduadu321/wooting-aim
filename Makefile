CC = gcc
CFLAGS = -O2 -Wall -g -I./include
LDFLAGS = -L./lib -lwooting_analog_sdk -lhidapi -lsetupapi -lws2_32 -ladvapi32

SRC = src/main.c src/hid_writer.c
OUT = wooting-aim.exe

ENUM_SRC = src/hid_enum.c
ENUM_OUT = hid-enum.exe

all: $(OUT) $(ENUM_OUT)

$(OUT): $(SRC) src/hid_writer.h
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)

$(ENUM_OUT): $(ENUM_SRC)
	$(CC) $(CFLAGS) -o $(ENUM_OUT) $(ENUM_SRC) -L./lib -lhidapi -lsetupapi

clean:
	-del /Q $(OUT) $(ENUM_OUT) 2>nul

run: $(OUT)
	./$(OUT) --adaptive

.PHONY: all clean run
