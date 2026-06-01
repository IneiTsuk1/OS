# Toolchain — must produce 32-bit freestanding ELF32 executables.
CC      = gcc
AR      = ar
LD      = ld

GCC_INC := $(shell $(CC) -m32 -print-file-name=include)

CFLAGS  = -m32 -ffreestanding -nostdlib -nostdinc -Wall -Wextra \
          -I. -Icrt -I$(GCC_INC)
LDFLAGS = -m elf_i386 -T user.ld --nmagic

CRT_LIB = crt/crt.a
CRT_OBJ = crt/string.o crt/stdlib.o crt/stdio.o

PROGS = hello.elf echo.elf hello2.elf cat.elf wc.elf

all: $(PROGS)

$(CRT_LIB): $(CRT_OBJ)
	$(AR) rcs $@ $^
	@echo "Built $@"

crt/string.o: crt/string.c crt/string.h
	$(CC) $(CFLAGS) -c $< -o $@

crt/stdlib.o: crt/stdlib.c crt/stdlib.h crt/string.h
	$(CC) $(CFLAGS) -c $< -o $@

crt/stdio.o: crt/stdio.c crt/stdio.h crt/string.h syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

hello.o: hello.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

hello.elf: hello.o user.ld
	$(LD) $(LDFLAGS) hello.o -o $@
	@echo "Built $@ (standalone)"

echo.o: echo.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

echo.elf: echo.o $(CRT_LIB) user.ld
	$(LD) $(LDFLAGS) echo.o $(CRT_LIB) -o $@
	@echo "Built $@ (with CRT)"

hello2.o: hello2.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

hello2.elf: hello2.o $(CRT_LIB) user.ld
	$(LD) $(LDFLAGS) hello2.o $(CRT_LIB) -o $@
	@echo "Built $@ (with CRT)"

cat.o: cat.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

cat.elf: cat.o $(CRT_LIB) user.ld
	$(LD) $(LDFLAGS) cat.o $(CRT_LIB) -o $@
	@echo "Built $@ (with CRT)"

wc.o: wc.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

wc.elf: wc.o $(CRT_LIB) user.ld
	$(LD) $(LDFLAGS) wc.o $(CRT_LIB) -o $@
	@echo "Built $@ (with CRT)"

clean:
	rm -f *.o *.elf crt/*.o crt/*.a

.PHONY: all clean
