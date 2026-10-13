# Builds ControlInfiniteRespec.dll with a MinGW-w64 GCC.
#   Linux (Debian/Ubuntu: mingw-w64, Fedora: mingw64-gcc, Arch: mingw-w64-gcc):  make
#   Windows (MSYS2 UCRT64 shell, package mingw-w64-ucrt-x86_64-gcc):             make CC=gcc
CC = x86_64-w64-mingw32-gcc

CFLAGS  = -O2 -Wall -Wextra -Werror \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
          -ffile-prefix-map=$(CURDIR)=.
LDFLAGS = -nostdlib -shared -s \
          -Wl,--entry,DllMain -Wl,--no-insert-timestamp -Wl,--build-id=none

ControlInfiniteRespec.dll: src/respec.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lkernel32

clean:
	rm -f ControlInfiniteRespec.dll

.PHONY: clean
