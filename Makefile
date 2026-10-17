LIMINE_CONFIG ?= boot/limine.conf
# CapMix — capability hybrid kernel
#   make test     hosted selftest (recommended)
#   make kernel   freestanding ELF
#   make iso      bootable ISO (needs third_party/limine)
#   ./scripts/qemu.sh   run ISO in QEMU

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wshadow -O2 -g
LINUX_ABI ?= 1
TCP_RX_BYTES ?= 16384
INCLUDES = -Iinclude -DCM_LINUX_ABI=$(LINUX_ABI) -DCM_TCP_RX_BYTES=$(TCP_RX_BYTES)

SRC = \
	console/fb.c console/ps2.c \
	arch/x86_64/serial.c arch/x86_64/power.c \
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
	obs/trace.c obs/static_trace.c \
	obs/etw.c \
	mm/buddy.c mm/numa.c \
	mm/slab.c \
	mm/commit.c \
	mm/pressure.c \
	mm/cache.c \
	mm/anon.c mm/vm.c \
	cap/object.c \
	cap/handle.c \
	sched/eevdf.c \
	ipc/channel.c \
	proc/syscall.c proc/control.c proc/uaccess.c abi/linux.c abi/linux_file.c abi/linux_process.c abi/linux_socket.c abi/linux_pipe.c \
	proc/spawn.c proc/elf.c proc/elf_args.c proc/init.c user/shell.c \
	fs/vfs.c \
	fs/cowfs.c \
	io/irq.c \
	io/blk.c io/disk_layout.c io/virtio_blk.c io/ahci.c \
	io/uring.c \
	io/driverkit.c \
	net/if.c net/e1000.c net/virtio_net.c net/virtio_ip.c \
	net/pkt.c net/udp.c net/runtime.c net/dhcp.c net/tcp.c \
	net/ebpf.c \
	net/filter.c \
	security/seatbelt.c \
	security/sip.c \
	security/authz.c \
	security/sign.c lib/sha256.c \
	security/gate.c \
	virt/job.c \
	virt/kvm.c virt/hardware.c virt/vmcs.c virt/ept.c arch/x86_64/virt_probe.S arch/x86_64/vmcs.S \
	boot/kernel.c boot/linux_ldso_test.c boot/linux_sh_test.c boot/linux_l5_test.c boot/linux_l4_test.c boot/linux_l3_test.c boot/linux_l2_test.c boot/linux_l1_test.c boot/linux_test.c boot/control_test.c boot/concurrent_test.c boot/argv_test.c boot/tcp_elf_test.c boot/p1d_test.c boot/p1c_test.c boot/p1b_test.c boot/p1a_test.c boot/buddy_smp_test.c boot/m3_test.c boot/m4_test.c boot/m8_test.c \
	boot/phase9_test.c boot/phase8_test.c boot/numa_test.c boot/phase0_test.c boot/phase1_test.c boot/phase2_test.c boot/phase4_test.c boot/phase5_test.c boot/phase6_test.c boot/phase7_test.c \
	boot/phase3_test.c \
 
     
        
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

$(HOST_BIN): build/spin_elf.inc build/user_init.inc build/user_shell.inc build/concurrent_elf.inc build/argv_elf.inc build/tcp_elf.inc build/wire_elf.inc build/file_elf.inc build/hello_elf.inc build/blockd_elf.inc build/net_elf.inc build/trace_elf.inc build/blockd_wait_elf.inc $(HOST_SRC) $(wildcard include/capmix/*.h arch/x86_64/*_asm.h)
	$(CC) $(CFLAGS) $(INCLUDES) -DCAPMIX_HOSTED -o $@ $(HOST_SRC)

test: $(HOST_BIN)
	@chmod +x $(HOST_BIN) 2>/dev/null || true
	@if [ -x $(HOST_BIN) ]; then ./$(HOST_BIN); \
	 else cp $(HOST_BIN) /tmp/capmix-hosted && chmod +x /tmp/capmix-hosted && /tmp/capmix-hosted; fi

build/hello-elf.o: tests/hello_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<

build/hello.elf: build/hello-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<

build/hello_elf.inc: build/hello.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@

build/blockd-elf.o: tests/blockd_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<

build/blockd.elf: build/blockd-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<

build/blockd_elf.inc: build/blockd.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_blockd_elf

build/net-elf.o: tests/net_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<

build/net.elf: build/net-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<

build/net_elf.inc: build/net.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_net_elf

kernel: $(FREE_BIN)

$(FREE_BIN): build/spin_elf.inc build/user_init.inc build/user_shell.inc build/concurrent_elf.inc build/argv_elf.inc build/tcp_elf.inc build/wire_elf.inc build/file_elf.inc build/hello_elf.inc build/blockd_elf.inc build/net_elf.inc build/trace_elf.inc build/blockd_wait_elf.inc $(FREE_SRC) $(wildcard include/capmix/*.h arch/x86_64/*_asm.h) arch/x86_64/linker.ld
	$(CC) $(FREE_CFLAGS) $(INCLUDES) $(FREE_LDFLAGS) -o $@ $(FREE_SRC)

iso: $(FREE_BIN)
	@if [ ! -f third_party/limine/limine-bios-cd.bin ]; then \
		echo "run ./scripts/fetch-limine.sh first"; exit 1; fi
	rm -rf iso
	mkdir -p iso/boot/limine iso/EFI/BOOT
	cp $(FREE_BIN) iso/boot/capmix.elf
	cp $(LIMINE_CONFIG) iso/boot/limine/limine.conf
	cp third_party/limine/limine-bios.sys iso/boot/limine/ 2>/dev/null || true
	cp third_party/limine/limine-bios-cd.bin iso/boot/limine/
	cp third_party/limine/limine-uefi-cd.bin iso/EFI/BOOT/ 2>/dev/null || true
	cp third_party/limine/BOOTX64.EFI iso/EFI/BOOT/ 2>/dev/null || true
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table \
		-eltorito-alt-boot -e EFI/BOOT/limine-uefi-cd.bin -no-emul-boot \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso -o capmix.iso
	@if [ -x third_party/limine/limine ]; then \
		third_party/limine/limine bios-install capmix.iso; fi
	@echo "built capmix.iso — run ./scripts/qemu.sh"

clean:
	rm -f $(HOST_BIN) $(FREE_BIN) capmix.iso
	rm -rf iso build
	find . -name '*.o' -delete

.PHONY: numa-test
numa-test:
	mkdir -p build
	$(CC) $(CFLAGS) -Werror $(INCLUDES) -DCAPMIX_HOSTED -o build/numa-parser-test tests/numa_parser_test.c mm/numa.c
	./build/numa-parser-test


build/trace-elf.o: tests/trace_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<

build/trace.elf: build/trace-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<

build/trace_elf.inc: build/trace.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_trace_elf


build/blockd-wait-elf.o: tests/blockd_wait_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<

build/blockd-wait.elf: build/blockd-wait-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<

build/blockd_wait_elf.inc: build/blockd-wait.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_blockd_wait_elf

build/file-elf.o: tests/file_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<
build/file.elf: build/file-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<
build/file_elf.inc: build/file.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_file_elf

build/wire-elf.o: tests/wire_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<
build/wire.elf: build/wire-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<
build/wire_elf.inc: build/wire.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_wire_elf

build/tcp-elf.o: tests/tcp_elf.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<
build/tcp.elf: build/tcp-elf.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<
build/tcp_elf.inc: build/tcp.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_tcp_elf

build/argv-crt.o: user/crt0.S
	mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o $@ $<
build/argv-main.o: tests/argv_main.c
	mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -c -o $@ $<
build/argv.elf: build/argv-crt.o build/argv-main.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ build/argv-crt.o build/argv-main.o
build/argv_elf.inc: build/argv.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_argv_elf

build/concurrent.o: tests/concurrent_elf.S
	@mkdir -p build
	$(CC) -c -nostdlib -o $@ $<
build/concurrent.elf: build/concurrent.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<
build/concurrent_elf.inc: build/concurrent.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_concurrent_elf

build/spin.o: tests/spin_elf.S
	@mkdir -p build
	$(CC) -c -nostdlib -o $@ $<
build/spin.elf: build/spin.o tests/hello_elf.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,tests/hello_elf.ld -o $@ $<
build/spin_elf.inc: build/spin.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_spin_elf

USER_CFLAGS = -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -Iinclude -Iinclude/freestanding
USER_HEADERS = user/runtime.h include/capmix/control.h include/capmix/syscall.h include/capmix/fs.h include/capmix/types.h
build/user-runtime.o: user/runtime.c $(USER_HEADERS)
	$(CC) $(USER_CFLAGS) -c -o $@ $<
build/user-init.o: user/init_main.c $(USER_HEADERS)
	$(CC) $(USER_CFLAGS) -c -o $@ $<
build/user-shell.o: user/shell_main.c $(USER_HEADERS)
	$(CC) $(USER_CFLAGS) -c -o $@ $<
build/user-init.elf: build/argv-crt.o build/user-runtime.o build/user-init.o user/user.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,user/user.ld -o $@ build/argv-crt.o build/user-runtime.o build/user-init.o
build/user-shell.elf: build/argv-crt.o build/user-runtime.o build/user-shell.o user/user.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-T,user/user.ld -o $@ build/argv-crt.o build/user-runtime.o build/user-shell.o
build/user_init.inc: build/user-init.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_user_init_elf
build/user_shell.inc: build/user-shell.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_user_shell_elf

# Host image installer: never accepts raw devices or nonempty images.
.PHONY: install-disk
third_party/limine/limine: third_party/limine/limine.c third_party/limine/limine-bios-hdd.h
	$(MAKE) -C third_party/limine

install-disk: $(FREE_BIN) third_party/limine/limine
	@test -n "$(DISK_IMAGE)" || { echo 'usage: make install-disk DISK_IMAGE=/path/new.img'; exit 1; }
	@test "$(INSTALL_LINUX)" = "" -o "$(INSTALL_LINUX)" = "0" -o "$(INSTALL_LINUX)" = "1" || { echo 'INSTALL_LINUX must be 0 or 1'; exit 1; }
	python3 scripts/install-disk.py "$(DISK_IMAGE)" $(if $(filter 1,$(INSTALL_LINUX)),--linux,)

# External freestanding GCC SDK; these files are not embedded in the kernel.
.PHONY: external-hello
build/external-hello.elf: examples/hello.c sdk/crt0.S sdk/capmix.ld sdk/include/capmix.h
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -O2 -static -nostdlib -no-pie -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -Isdk/include -Wl,--build-id=none,-z,max-page-size=0x1000,-T,sdk/capmix.ld -o $@ sdk/crt0.S examples/hello.c
build/cow-put: tools/cow-put.c fs/cowfs.c fs/vfs.c io/disk_layout.c $(wildcard include/capmix/*.h)
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Wshadow -O2 -DCAPMIX_HOSTED -DCM_TP_BUILD_MASK=0 -Iinclude -o $@ tools/cow-put.c fs/cowfs.c fs/vfs.c io/disk_layout.c
external-hello: build/external-hello.elf build/cow-put
	@test -n "$(DISK_IMAGE)" || { echo 'usage: make external-hello DISK_IMAGE=/path/initialized-offline.img'; exit 1; }
	python3 scripts/copy-to-disk.py "$(DISK_IMAGE)" build/external-hello.elf /bin/external-hello.elf

# Product examples use the same standalone SDK as external host-built ELFs.
EXAMPLE_NAMES = hello nc cat ls http-get httpd
EXAMPLE_ELFS = $(addprefix build/example-,$(addsuffix .elf,$(EXAMPLE_NAMES)))
EXAMPLE_INCS = $(addprefix build/example_,$(addsuffix .inc,$(EXAMPLE_NAMES)))
.PHONY: examples
examples: $(EXAMPLE_ELFS)
build/example-%.elf: examples/%.c sdk/crt0.S sdk/capmix.ld sdk/include/capmix.h
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -O2 -static -nostdlib -no-pie -ffreestanding -fno-builtin -fno-pie -fno-stack-protector -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -Isdk/include -Wl,--build-id=none,-z,max-page-size=0x1000,-T,sdk/capmix.ld -o $@ sdk/crt0.S $<
build/example_%.inc: build/example-%.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_example_$(subst -,_,$*)_elf

$(HOST_BIN) $(FREE_BIN): $(EXAMPLE_INCS)

# Switching personas must rebuild both kernels even when only make flags change.
build/linux-abi-$(LINUX_ABI).stamp:
	@mkdir -p build
	@rm -f build/linux-abi-*.stamp
	@touch $@
$(HOST_BIN) $(FREE_BIN): build/linux-abi-$(LINUX_ABI).stamp build/linux_hello.inc

build/linux-hello.elf: tests/fixtures/linux-l0.elf tests/fixtures/linux-l0.sha256
	@mkdir -p build
	sha256sum -c tests/fixtures/linux-l0.sha256
	cp $< $@
.PHONY: linux-golden-verify
linux-golden-verify: tests/linux_hello.S tests/linux_hello.ld scripts/mark-linux.py
	@mkdir -p build
	$(CC) -c -nostdlib -fno-pie -o build/linux-golden.o tests/linux_hello.S
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_hello.ld -o build/linux-hello-rebuilt.elf build/linux-golden.o
	python3 scripts/mark-linux.py build/linux-hello-rebuilt.elf
	cmp tests/fixtures/linux-l0.elf build/linux-hello-rebuilt.elf
build/linux_hello.inc: build/linux-hello.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_hello_elf

build/linux-probe.elf: tests/linux_probe.S tests/linux_hello.ld scripts/mark-linux.py
	@mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_hello.ld -o $@ $<
	python3 scripts/mark-linux.py $@
build/linux_probe.inc: build/linux-probe.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_probe_elf
$(HOST_BIN) $(FREE_BIN): build/linux_probe.inc

build/busybox_l1.inc: tests/fixtures/busybox-l1.elf tests/fixtures/busybox-l1.sha256 scripts/embed-elf.py
	sha256sum -c tests/fixtures/busybox-l1.sha256
	python3 scripts/embed-elf.py $< $@ cm_busybox_l1_elf
$(HOST_BIN) $(FREE_BIN): build/busybox_l1.inc

.PHONY: linux-l1-verify
linux-l1-verify:
	mkdir -p build
	sha256sum -c third_party/linux-l1/SHA256SUMS
	sha256sum -c tests/fixtures/busybox-l1.sha256
	$(CC) -Os -Werror -fno-builtin -fno-stack-protector tests/linux_bss_alloc.c -o build/linux-l1-allocator-test
	build/linux-l1-allocator-test
	@echo "Linux L1 bounded BSS: exhaustion/coalesce/overflow verified"


build/linux-l2.elf: tests/linux_l2.S tests/linux_hello.ld scripts/mark-linux.py
	mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_hello.ld -o $@ $<
	python3 scripts/mark-linux.py $@
build/linux-l2-next.elf: tests/linux_l2_next.S tests/linux_hello.ld scripts/mark-linux.py
	mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_hello.ld -o $@ $<
	python3 scripts/mark-linux.py $@
build/linux_l2.inc: build/linux-l2.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l2_elf
build/linux_l2_next.inc: build/linux-l2-next.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l2_next_elf
$(HOST_BIN) $(FREE_BIN): build/linux_l2.inc build/linux_l2_next.inc

build/linux-l3.elf: tests/linux_l3_start.S tests/linux_l3.c tests/linux_l3.ld scripts/mark-linux.py
	mkdir -p build
	$(CC) -std=c11 -Os -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_l3.ld -o $@ tests/linux_l3_start.S tests/linux_l3.c
	python3 scripts/mark-linux.py $@
build/linux_l3.inc: build/linux-l3.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l3_elf
$(HOST_BIN) $(FREE_BIN): build/linux_l3.inc

build/linux-l4a.elf: tests/linux_l4_main.S tests/linux_l4_main.ld scripts/mark-linux.py
	mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_l4_main.ld -o $@ $<
	python3 scripts/mark-linux.py $@
build/linux-l4a-interp.so: tests/linux_l4_interp.S tests/linux_l4_interp.ld scripts/mark-linux-interp.py
	mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_l4_interp.ld -o $@ $<
	python3 scripts/mark-linux-interp.py $@
build/linux-l4a-exec.elf: tests/linux_l4_exec.S tests/linux_hello.ld scripts/mark-linux.py
	mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_hello.ld -o $@ $<
	python3 scripts/mark-linux.py $@
build/linux_l4_main.inc: build/linux-l4a.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l4_main
build/linux_l4_interp.inc: build/linux-l4a-interp.so scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l4_interp
build/linux_l4_exec.inc: build/linux-l4a-exec.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l4_exec
$(HOST_BIN) $(FREE_BIN): build/linux_l4_main.inc build/linux_l4_interp.inc build/linux_l4_exec.inc

build/linux-package: tools/linux-package.c fs/cowfs.c fs/vfs.c io/disk_layout.c $(wildcard include/capmix/*.h)
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Wshadow -O2 -DCAPMIX_HOSTED -DCM_TP_BUILD_MASK=0 -Iinclude -o $@ tools/linux-package.c fs/cowfs.c fs/vfs.c io/disk_layout.c

build/test-linux-package-fixture: tests/linux_package_fixture.c tools/linux-package.c fs/cowfs.c fs/vfs.c io/disk_layout.c $(wildcard include/capmix/*.h)
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Wshadow -O2 -DCAPMIX_HOSTED -DCM_TP_BUILD_MASK=0 -Iinclude -o $@ tests/linux_package_fixture.c fs/cowfs.c fs/vfs.c io/disk_layout.c

# Linux L5 fixed raw-syscall golden image (no libc dependency).
build/linux-mmap.elf: tests/linux_l5.S tests/linux_hello.ld scripts/mark-linux.py
	@mkdir -p build
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none,-z,max-page-size=0x1000,-T,tests/linux_hello.ld -o $@ $<
	python3 scripts/mark-linux.py $@
build/linux_l5.inc: build/linux-mmap.elf scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_l5_elf
$(HOST_BIN) $(FREE_BIN): build/linux_l5.inc

build/linux_l5_musl.inc: third_party/linux-l5/musl-mmap.elf third_party/linux-l5/SHA256SUMS scripts/embed-elf.py
	sha256sum -c third_party/linux-l5/SHA256SUMS
	python3 scripts/embed-elf.py $< $@ cm_linux_l5_musl
$(HOST_BIN) $(FREE_BIN): build/linux_l5_musl.inc

build/busybox_sh.inc: tests/fixtures/busybox-sh.elf tests/fixtures/busybox-sh.sha256 scripts/embed-elf.py
	sha256sum -c tests/fixtures/busybox-sh.sha256
	python3 scripts/embed-elf.py $< $@ cm_busybox_sh_elf
$(HOST_BIN) $(FREE_BIN): build/busybox_sh.inc

build/linux_sh_demo.inc: user/linux/sh-smoke.sh scripts/embed-elf.py
	python3 scripts/embed-elf.py $< $@ cm_linux_sh_demo
$(HOST_BIN) $(FREE_BIN): build/linux_sh_demo.inc

build/ldso_main.inc build/ldso_interp.inc build/ldso_lib.inc &: tests/fixtures/ldso/linux-dynamic.elf tests/fixtures/ldso/ld-capmix.so.1 tests/fixtures/ldso/libcmgold.so.1 tests/fixtures/ldso/SHA256SUMS scripts/check-ldso-fixtures.py scripts/embed-elf.py
	sha256sum -c tests/fixtures/ldso/SHA256SUMS
	python3 scripts/check-ldso-fixtures.py tests/fixtures/ldso
	python3 scripts/embed-elf.py tests/fixtures/ldso/linux-dynamic.elf build/ldso_main.inc cm_ldso_main
	python3 scripts/embed-elf.py tests/fixtures/ldso/ld-capmix.so.1 build/ldso_interp.inc cm_ldso_interp
	python3 scripts/embed-elf.py tests/fixtures/ldso/libcmgold.so.1 build/ldso_lib.inc cm_ldso_lib
$(HOST_BIN) $(FREE_BIN): build/ldso_main.inc build/ldso_interp.inc build/ldso_lib.inc

TLS_FIXTURES = linux-tls.elf libcm_tls_a.so.1 libcm_tls_b.so.1
build/tls_main.inc build/tls_a.inc build/tls_b.inc &: $(addprefix tests/fixtures/ldso/,$(TLS_FIXTURES)) tests/fixtures/ldso/SHA256SUMS scripts/check-tls-fixtures.py scripts/embed-elf.py
	sha256sum -c tests/fixtures/ldso/SHA256SUMS
	python3 scripts/check-tls-fixtures.py tests/fixtures/ldso
	python3 scripts/embed-elf.py tests/fixtures/ldso/linux-tls.elf build/tls_main.inc cm_tls_main
	python3 scripts/embed-elf.py tests/fixtures/ldso/libcm_tls_a.so.1 build/tls_a.inc cm_tls_a
	python3 scripts/embed-elf.py tests/fixtures/ldso/libcm_tls_b.so.1 build/tls_b.inc cm_tls_b
$(HOST_BIN) $(FREE_BIN): build/tls_main.inc build/tls_a.inc build/tls_b.inc

# RX capacity is part of struct connection; rebuild both kernels on changes.
build/tcp-rx-$(TCP_RX_BYTES).stamp:
	@mkdir -p build
	@rm -f build/tcp-rx-*.stamp
	@touch $@
$(HOST_BIN) $(FREE_BIN): build/tcp-rx-$(TCP_RX_BYTES).stamp
