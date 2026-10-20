# WinCraft shell - built with the MinGW-w64 cross compiler.
OUT    ?= .
CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -municode -mwindows -s
LIBS    = -lshell32 -lshlwapi -lole32 -luuid -lcomctl32 -lgdi32 -lmsimg32 -luser32 -ladvapi32 -lurlmon

all: $(OUT)/wcshell.exe $(OUT)/wclistwin.exe

$(OUT)/wcshell.res: wcshell.rc wcshell.manifest wincraft.ico
	@mkdir -p $(OUT)
	$(WINDRES) wcshell.rc -O coff -o $@

$(OUT)/wcshell.exe: wcshell.c $(OUT)/wcshell.res
	$(CC) $(CFLAGS) -o $@ wcshell.c $(OUT)/wcshell.res $(LIBS)

# the WinCraft Agent's sandbox: list the windows on its private screen
$(OUT)/wclistwin.exe: wclistwin.c
	@mkdir -p $(OUT)
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -municode -mconsole -s -o $@ wclistwin.c -luser32

wincraft.ico: mkicon.sh
	sh mkicon.sh $@

clean:
	rm -f $(OUT)/wcshell.exe $(OUT)/wcshell.res $(OUT)/wclistwin.exe
