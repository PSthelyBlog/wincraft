# WinCraft Assistant - built with the MinGW-w64 cross compiler.
OUT    ?= .
CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -municode -mwindows -s
LIBS    = -lwinhttp -lshell32 -lshlwapi -lole32 -luuid -lcomctl32 -lgdi32 -luser32 -ladvapi32 \
          -lws2_32 -liphlpapi
SRCS    = wcagent.c llm.c tools.c json.c

all: $(OUT)/wcagent.exe

$(OUT)/wcagent.res: wcagent.rc wcagent.manifest assistant.ico
	@mkdir -p $(OUT)
	$(WINDRES) wcagent.rc -O coff -o $@

$(OUT)/wcagent.exe: $(SRCS) agent.h $(OUT)/wcagent.res
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(OUT)/wcagent.res $(LIBS)

assistant.ico: mkicon.sh
	sh mkicon.sh $@

clean:
	rm -f $(OUT)/wcagent.exe $(OUT)/wcagent.res
