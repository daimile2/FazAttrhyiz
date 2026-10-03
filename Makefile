# CapMix — capability hybrid kernel
#   make test     hosted selftest (recommended)
#   make kernel   freestanding ELF
#   make iso      bootable ISO (needs third_party/limine)
#   ./scripts/qemu.sh   run ISO in QEMU

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wshadow -O2 -g
INCLUDES = -Iinclude

SRC = \
	arch/x86_64/serial.c \
	arch/x86_64/paging.c \
	arch/x86_64/idt.c \
	arch/x86_64/lapic.c \
	arch/x86_64/fault.c arch/x86_64/ctx.c \
	arch/x86_64/switch.S \
	arch/x86_64/syscall.S \
	arch/x86_64/usermode.c \
 	arch/x86_64/ioapic.c \
 	arch/x86_64/ioapic_stub.S \
	boot/limine.c \
	mm/wkdm.c \
	obs/trace.c \
	obs/etw.c \
	mm/buddy.c \
	mm/slab.c \
	mm/commit.c \
	mm/pressure.c \
	mm/cache.c \
	cap/object.c \
	cap/handle.c \
	sched/eevdf.c \
	ipc/channel.c \
	proc/syscall.c \
	proc/spawn.c \
	fs/vfs.c \
	fs/cowvol.c \
	io/irq.c \
	io/blk.c \
	io/uring.c \
	io/driverkit.c \
	net/if.c \
	net/pkt.c \
	net/ebpf.c \
	net/filter.c \
	security/seatbelt.c \
	security/sip.c \
	security/authz.c \
	security/sign.c \
	security/gate.c \
	virt/job.c \
	virt/kvm.c \
	boot/kernel.c \
 
     
        
HOST_SRC = $(SRC) boot/hosted.c
HOST_BIN = capmix-hosted

FREE_SRC = $(SRC) boot/kmain.c lib/kstring.c
FREE_BIN = capmix.elf
FREE_CFLAGS = $(CFLAGS) -ffreestanding -fno-stack-protector -fno-pie \
	-fno-pic -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx \
	-mgeneral-regs-only -fno-asynchronous-unwind-tables \
	-Iinclude/freestanding
FREE_LDFLAGS = -nostdlib -static -no-pie -Wl,--build-id=none \
	-Wl,-z,max-page-size=0x1000 -Wl,-T,arch/x86_64/linker.ld

.PHONY: all test kernel iso clean dump

all: test

$(HOST_BIN): $(HOST_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -DCAPMIX_HOSTED -o $@ $(HOST_SRC)

test: $(HOST_BIN)
	@chmod +x $(HOST_BIN) 2>/dev/null || true
	@if [ -x $(HOST_BIN) ]; then ./$(HOST_BIN); \
	 else cp $(HOST_BIN) /tmp/capmix-hosted && chmod +x /tmp/capmix-hosted && /tmp/capmix-hosted; fi

kernel: $(FREE_BIN)

$(FREE_BIN): $(FREE_SRC) arch/x86_64/linker.ld
	$(CC) $(FREE_CFLAGS) $(INCLUDES) $(FREE_LDFLAGS) -o $@ $(FREE_SRC)

iso: $(FREE_BIN)
	@if [ ! -f third_party/limine/limine-bios-cd.bin ]; then \
		echo "run ./scripts/fetch-limine.sh first"; exit 1; fi
	rm -rf iso
	mkdir -p iso/boot/limine iso/EFI/BOOT
	cp $(FREE_BIN) iso/boot/capmix.elf
	cp boot/limine.conf iso/boot/limine/limine.conf
	cp third_party/limine/limine-bios.sys iso/boot/limine/ 2>/dev/null || true
	cp third_party/limine/limine-bios-cd.bin iso/boot/limine/
	cp third_party/limine/limine-uefi-cd.bin iso/EFI/BOOT/ 2>/dev/null || true
	cp third_party/limine/BOOTX64.EFI iso/EFI/BOOT/ 2>/dev/null || true
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table \
		--protective-msdos-label \
		iso -o capmix.iso
	@if [ -x third_party/limine/limine ]; then \
		third_party/limine/limine bios-install capmix.iso; fi
	@echo "built capmix.iso — run ./scripts/qemu.sh"

clean:
	rm -f $(HOST_BIN) $(FREE_BIN) capmix.iso
	rm -rf iso
	find . -name '*.o' -delete
