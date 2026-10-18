// SPDX-License-Identifier: Apache-2.0
/*
 * cloud-hypervisor isolated-VM launcher.
 *
 *   ivm_launch        same 9-page guest as hedev op=vcpu
 *   ivm_launch huge   one kernel 2MB leaf, then the hedev op=huge guest
 *   ivm_launch dev [bdf]
 *                     attach ivshmem (default 0x28), map BAR 1, expect
 *                     DeviceMapDma -EPERM and a 0xa5 readback
 *
 * Donations and VCPU_RUN go through HE_IOC_IVM. Ring 3 never issues the
 * hypercall itself.
 */
#include "he_ivm.h"

#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096
#define GUEST_PAGES 9
#define GUEST_SHARED 6
#define DEV_PAGES 11

/* Guest at GPA 0x4000. vmmcall (0f 01 d9) is patched to vmcall on Intel. */
static const unsigned char guest_code[] = {
	0xc6, 0x04, 0x25, 0x28, 0x60, 0x00, 0x00, 0x42, 0xc7, 0x04, 0x25, 0x04,
	0x60, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0xc7, 0xc0, 0x30, 0x60,
	0x00, 0x00, 0x48, 0x89, 0x04, 0x25, 0x10, 0x60, 0x00, 0x00, 0x48, 0xc7,
	0x04, 0x25, 0x18, 0x60, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x01,
	0xd9, 0xc7, 0x04, 0x25, 0x04, 0x60, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x48, 0xc7, 0xc0, 0x30, 0x60, 0x00, 0x00, 0x48, 0x89, 0x04, 0x25, 0x10,
	0x60, 0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x18, 0x60, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x0f, 0x01, 0xd9, 0x0f, 0x31, 0xc7, 0x04, 0x25, 0x04,
	0x60, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x48, 0x89, 0x04, 0x25, 0x10,
	0x60, 0x00, 0x00, 0x48, 0xc7, 0x04, 0x25, 0x18, 0x60, 0x00, 0x00, 0x30,
	0x00, 0x00, 0x00, 0x0f, 0x01, 0xd9, 0xf4
};
static const unsigned char guest_handler[] = {
	0xc6, 0x04, 0x25, 0x29, 0x60, 0x00, 0x00, 0xa5, 0xf4
};

/* Read both ends of the 2MB page at GPA 0x200000 into the shared page. */
static const unsigned char huge_guest[] = {
	0x8a, 0x04, 0x25, 0x00, 0x00, 0x20, 0x00, 0x88, 0x04, 0x25, 0x28, 0x60,
	0x00, 0x00, 0x8a, 0x04, 0x25, 0xff, 0xff, 0x3f, 0x00, 0x88, 0x04, 0x25,
	0x29, 0x60, 0x00, 0x00, 0xf4
};

/* mov byte [0x40000000], 0xa5; mov al, [0x40000000]; mov [0x6028], al; hlt */
static const unsigned char dev_guest[] = {
	0xc6, 0x04, 0x25, 0x00, 0x00, 0x00, 0x40, 0xa5, 0x8a, 0x04, 0x25, 0x00,
	0x00, 0x00, 0x40, 0x88, 0x04, 0x25, 0x28, 0x60, 0x00, 0x00, 0xf4
};

static int cpu_is_intel(void)
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

	__get_cpuid(0, &eax, &ebx, &ecx, &edx);
	return ebx == 0x756e6547 && edx == 0x49656e69 && ecx == 0x6c65746e;
}

static int ivm_call(int fd, struct he_ivm_call *call)
{
	if (ioctl(fd, HE_IOC_IVM, call) != 0) {
		perror("HE_IOC_IVM");
		return -1;
	}
	return 0;
}

static int vm_create(int fd, __s64 *vm)
{
	struct he_ivm_call call;

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_CREATE;
	if (ivm_call(fd, &call) || call.ret <= 0) {
		fprintf(stderr, "vm_create -> %lld\n", (long long)call.ret);
		return -1;
	}
	*vm = call.ret;
	printf("vm_create -> %lld\n", (long long)*vm);
	return 0;
}

static int vm_destroy(int fd, __s64 vm)
{
	struct he_ivm_call call;

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_DESTROY;
	call.vm_id = vm;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "destroy -> %lld\n", (long long)call.ret);
		return -1;
	}
	printf("destroy -> 0\n");
	return 0;
}

static int donate(int fd, __s64 vm, int index, unsigned char *page, int shared)
{
	struct he_ivm_call call;

	memset(&call, 0, sizeof(call));
	call.op = shared ? HC_VM_ADD_SHARED : HC_VM_ADD_PAGE;
	call.vm_id = vm;
	call.gpa = (unsigned long long)index << 12;
	call.user_addr = (unsigned long long)page;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "map gpa=%#llx -> %lld\n",
			(unsigned long long)call.gpa, (long long)call.ret);
		return -1;
	}
	printf("map gpa=%#llx -> 0\n", (unsigned long long)call.gpa);
	return 0;
}

static void patch_vmmcall(unsigned char *code, int len)
{
	int i;

	if (!cpu_is_intel())
		return;
	for (i = 0; i + 2 < len; i++) {
		if (code[i] == 0x0f && code[i + 1] == 0x01 && code[i + 2] == 0xd9)
			code[i + 2] = 0xc1;
	}
}

static void write_gdt(unsigned char *page)
{
	uint64_t *gdt = (uint64_t *)page;

	gdt[1] = 0x00af9b000000ffffULL;
	gdt[2] = 0x00cf93000000ffffULL;
	gdt[3] = 0x0000890000000067ULL;
}

static int vcpu_setup(int fd, __s64 vm, uint64_t idtr_base, uint64_t idtr_limit)
{
	struct he_ivm_call call;

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_VCPU_CREATE;
	call.vm_id = vm;
	call.rip = 0x4000;
	call.rsp = 0x5ff0;
	call.gdtr_base = 0x8000;
	call.gdtr_limit = 31;
	call.idtr_base = idtr_base;
	call.idtr_limit = idtr_limit;
	call.exit_gpa = 0x6000;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "vcpu_create -> %lld\n", (long long)call.ret);
		return -1;
	}
	printf("vcpu_create -> 0\n");

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_FINALIZE;
	call.vm_id = vm;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "finalize -> %lld\n", (long long)call.ret);
		return -1;
	}
	printf("finalize -> 0\n");
	return 0;
}

static int run_once(int fd, __s64 vm, struct he_ivm_call *out)
{
	memset(out, 0, sizeof(*out));
	out->op = HC_VM_VCPU_RUN;
	out->vm_id = vm;
	if (ivm_call(fd, out) || out->ret != 0) {
		fprintf(stderr, "vcpu_run -> %lld\n", (long long)out->ret);
		return -1;
	}
	return 0;
}

static int run_vcpu(int fd)
{
	struct he_ivm_call call;
	unsigned char *mem, *shared;
	uint64_t *pt;
	int i, saw_puts = 0, pass;
	unsigned char code0;
	__s64 vm = 0;

	mem = mmap(NULL, GUEST_PAGES * PAGE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(mem, 0, GUEST_PAGES * PAGE);

	pt = (uint64_t *)mem;
	pt[0] = 0x1000 | 3;
	pt = (uint64_t *)(mem + PAGE);
	pt[0] = 0x2000 | 3;
	pt = (uint64_t *)(mem + 2 * PAGE);
	pt[0] = 0x3000 | 3;
	pt = (uint64_t *)(mem + 3 * PAGE);
	for (i = 0; i < GUEST_PAGES; i++)
		pt[i] = ((uint64_t)i << 12) | 3;

	memcpy(mem + 4 * PAGE, guest_code, sizeof(guest_code));
	patch_vmmcall(mem + 4 * PAGE, sizeof(guest_code));
	memcpy(mem + 4 * PAGE + 0x100, guest_handler, sizeof(guest_handler));
	memcpy(mem + GUEST_SHARED * PAGE + 0x30, "ivm\n", 4);
	{
		unsigned char *gate = mem + 7 * PAGE + 0x30 * 16;

		gate[0] = 0x00;
		gate[1] = 0x41;
		gate[2] = 0x08;
		gate[5] = 0x8e;
	}
	write_gdt(mem + 8 * PAGE);
	code0 = mem[4 * PAGE];

	if (vm_create(fd, &vm))
		return 1;
	for (i = 0; i < GUEST_PAGES; i++) {
		if (donate(fd, vm, i, mem + i * PAGE, i == GUEST_SHARED))
			return 1;
	}
	if (mem[4 * PAGE] != 0) {
		fprintf(stderr, "donated code page still reads %#x\n",
			mem[4 * PAGE]);
		return 1;
	}
	printf("donated code page reads 0 (was %#x)\n", code0);
	if (vcpu_setup(fd, vm, 0x7000, 0xfff))
		return 1;

	shared = mem + GUEST_SHARED * PAGE;
	for (i = 0; i < 8; i++) {
		if (run_once(fd, vm, &call))
			return 1;
		printf("vcpu_run[%d] reason %u req %u arg0 %#llx arg1 %#llx marker %#x irq %#x\n",
		       i, call.reason, call.request,
		       (unsigned long long)call.arg0,
		       (unsigned long long)call.arg1, shared[0x28],
		       shared[0x29]);
		if (call.reason == HE_EXIT_SERVICE && call.request == 1) {
			saw_puts = 1;
			printf("service msg \"%.*s\"\n", 4, shared + 0x30);
		}
		if (call.reason == HE_EXIT_HLT && shared[0x29] == 0xa5)
			break;
		if (call.reason == HE_EXIT_FAULT) {
			fprintf(stderr, "guest fault arg0 %#llx\n",
				(unsigned long long)call.arg0);
			return 1;
		}
	}

	pass = saw_puts && shared[0x28] == 0x42 && shared[0x29] == 0xa5;
	if (vm_destroy(fd, vm))
		return 1;
	printf("%s\n", pass ? "PASS ivm_launch" : "FAIL ivm_launch");
	return pass ? 0 : 1;
}

static int run_huge(int fd)
{
	struct he_ivm_call call;
	unsigned char *mem, *shared;
	uint64_t *pt;
	int i, pass;
	unsigned int scrub0, scrub1;
	__s64 vm = 0;
	long long unaligned;

	mem = mmap(NULL, GUEST_PAGES * PAGE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(mem, 0, GUEST_PAGES * PAGE);
	pt = (uint64_t *)mem;
	pt[0] = 0x1000 | 3;
	pt = (uint64_t *)(mem + PAGE);
	pt[0] = 0x2000 | 3;
	pt = (uint64_t *)(mem + 2 * PAGE);
	pt[0] = 0x3000 | 3;
	pt[1] = 0x200000 | 0x83;
	pt = (uint64_t *)(mem + 3 * PAGE);
	for (i = 0; i < GUEST_PAGES; i++)
		pt[i] = ((uint64_t)i << 12) | 3;
	memcpy(mem + 4 * PAGE, huge_guest, sizeof(huge_guest));
	write_gdt(mem + 8 * PAGE);

	if (vm_create(fd, &vm))
		return 1;

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_ADD_PAGE;
	call.vm_id = vm;
	call.gpa = 0x200000;
	call.flags = HE_IVM_PAGE_2M;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "huge add -> %lld\n", (long long)call.ret);
		return 1;
	}
	printf("huge add l1 %#x/%#x\n", call.digest[0], call.digest[1]);
	if (call.digest[0] != 0 || call.digest[1] != 0) {
		fprintf(stderr, "L1 still sees the 2MB block\n");
		return 1;
	}

	for (i = 0; i < GUEST_PAGES; i++) {
		if (donate(fd, vm, i, mem + i * PAGE, i == GUEST_SHARED))
			return 1;
	}
	if (vcpu_setup(fd, vm, 0, 0))
		return 1;
	if (run_once(fd, vm, &call))
		return 1;
	shared = mem + GUEST_SHARED * PAGE;
	printf("huge run reason %u guest %#x/%#x\n", call.reason, shared[0x28],
	       shared[0x29]);
	if (call.reason != HE_EXIT_HLT || shared[0x28] != 0xab ||
	    shared[0x29] != 0xcd) {
		fprintf(stderr, "huge guest mismatch\n");
		return 1;
	}

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_REMOVE_PAGE;
	call.vm_id = vm;
	call.gpa = 0x200000;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "huge remove -> %lld\n", (long long)call.ret);
		return 1;
	}
	scrub0 = call.digest[0];
	scrub1 = call.digest[1];
	printf("huge scrub %#x/%#x\n", scrub0, scrub1);

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_ADD_PAGE;
	call.vm_id = vm;
	call.gpa = 0x200000;
	call.flags = HE_IVM_PAGE_2M;
	call.aux = PAGE;
	if (ivm_call(fd, &call))
		return 1;
	unaligned = call.ret;
	printf("huge unaligned -> %lld\n", unaligned);

	pass = shared[0x28] == 0xab && shared[0x29] == 0xcd && scrub0 == 0 &&
	       scrub1 == 0 && unaligned < 0;
	if (vm_destroy(fd, vm))
		return 1;
	printf("%s\n", pass ? "PASS ivm_launch huge" : "FAIL ivm_launch huge");
	return pass ? 0 : 1;
}

static int enable_mse(unsigned int bdf)
{
	char path[80];
	unsigned int bus = (bdf >> 8) & 0xff;
	unsigned int dev = (bdf >> 3) & 0x1f;
	unsigned int fn = bdf & 7;
	unsigned char cmd[2];
	int cfg;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/0000:%02x:%02x.%u/config",
		 bus, dev, fn);
	cfg = open(path, O_RDWR);
	if (cfg < 0) {
		perror(path);
		return -1;
	}
	if (pread(cfg, cmd, 2, 4) != 2) {
		perror("pread command");
		close(cfg);
		return -1;
	}
	cmd[0] |= 0x2;
	if (pwrite(cfg, cmd, 2, 4) != 2) {
		perror("pwrite command");
		close(cfg);
		return -1;
	}
	printf("mse bdf %#x command %#x\n", bdf, cmd[0] | ((unsigned)cmd[1] << 8));
	close(cfg);
	return 0;
}

static int dma_expect_eperm(int fd, __s64 vm, unsigned int bdf, __u32 op)
{
	struct he_device_dma_map_req dma;
	struct he_ivm_call call;

	memset(&dma, 0, sizeof(dma));
	dma.bdf = bdf;
	dma.handle = vm;
	dma.nr_maps = 1;
	dma.maps[0].iova = 0x1000;
	dma.maps[0].size = PAGE;

	memset(&call, 0, sizeof(call));
	call.op = op;
	call.vm_id = vm;
	call.gpa = 0;
	call.user_addr = (unsigned long long)&dma;
	call.flags = HE_IVM_DMA_USE_GPA;
	call.bdf = bdf;
	if (ivm_call(fd, &call))
		return -1;
	printf("dma op %#x -> %lld\n", op, (long long)call.ret);
	return call.ret == -1 ? 0 : -1;
}

static int run_dev(int fd, unsigned int bdf)
{
	struct he_ivm_call call;
	unsigned char *mem, *shared;
	uint64_t *pt;
	int i, pass, dma_ok, detach_ok;
	unsigned int reason;
	__s64 vm = 0;

	mem = mmap(NULL, DEV_PAGES * PAGE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(mem, 0, DEV_PAGES * PAGE);
	pt = (uint64_t *)mem;
	pt[0] = 0x1000 | 3;
	pt = (uint64_t *)(mem + PAGE);
	pt[0] = 0x2000 | 3;
	pt[1] = 0x9000 | 3;
	pt = (uint64_t *)(mem + 2 * PAGE);
	pt[0] = 0x3000 | 3;
	pt = (uint64_t *)(mem + 3 * PAGE);
	for (i = 0; i < DEV_PAGES; i++)
		pt[i] = ((uint64_t)i << 12) | 3;
	pt = (uint64_t *)(mem + 9 * PAGE);
	pt[0] = 0xa000 | 3;
	pt = (uint64_t *)(mem + 10 * PAGE);
	pt[0] = 0x40000000ULL | 3;
	memcpy(mem + 4 * PAGE, dev_guest, sizeof(dev_guest));
	write_gdt(mem + 8 * PAGE);

	if (vm_create(fd, &vm))
		return 1;
	for (i = 0; i < DEV_PAGES; i++) {
		if (donate(fd, vm, i, mem + i * PAGE, i == GUEST_SHARED))
			return 1;
	}

	memset(&call, 0, sizeof(call));
	call.op = HC_DEV_ATTACH;
	call.vm_id = vm;
	call.bdf = bdf;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "dev attach -> %lld\n", (long long)call.ret);
		vm_destroy(fd, vm);
		return 1;
	}
	printf("dev attach %#x -> 0\n", bdf);
	if (enable_mse(bdf)) {
		vm_destroy(fd, vm);
		return 1;
	}

	memset(&call, 0, sizeof(call));
	call.op = HC_DEV_MAP_BAR;
	call.vm_id = vm;
	call.bdf = bdf;
	call.bar = 1;
	call.gpa = 0x40000000ULL;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "map_bar -> %lld\n", (long long)call.ret);
		vm_destroy(fd, vm);
		return 1;
	}
	printf("map_bar -> 0\n");

	dma_ok = dma_expect_eperm(fd, vm, bdf, HC_DEVICE_MAP_DMA) == 0 &&
		 dma_expect_eperm(fd, vm, bdf, HC_DEVICE_UNMAP_DMA) == 0;

	if (vcpu_setup(fd, vm, 0, 0)) {
		vm_destroy(fd, vm);
		return 1;
	}
	if (run_once(fd, vm, &call)) {
		vm_destroy(fd, vm);
		return 1;
	}
	shared = mem + GUEST_SHARED * PAGE;
	reason = call.reason;
	printf("dev run reason %u readback %#x\n", reason, shared[0x28]);

	memset(&call, 0, sizeof(call));
	call.op = HC_DEV_DETACH;
	call.vm_id = vm;
	call.bdf = bdf;
	detach_ok = ivm_call(fd, &call) == 0 && call.ret == 0;
	if (!detach_ok)
		fprintf(stderr, "dev detach -> %lld\n", (long long)call.ret);
	else
		printf("dev detach -> 0\n");

	pass = dma_ok && detach_ok && reason == HE_EXIT_HLT && shared[0x28] == 0xa5;
	/* `call` was reused for detach; the readback is already in shared. */
	if (vm_destroy(fd, vm))
		return 1;
	printf("%s\n", pass ? "PASS ivm_launch dev" : "FAIL ivm_launch dev");
	return pass ? 0 : 1;
}

static int usage(void)
{
	fprintf(stderr, "usage: ivm_launch [huge | dev [bdf]]\n");
	return 2;
}

int main(int argc, char **argv)
{
	int fd, rc;
	unsigned long bdf = 0x28;

	_Static_assert(sizeof(struct he_ivm_call) == 200, "he_ivm_call ABI");
	_Static_assert(sizeof(struct he_device_dma_map_req) == 1560,
		       "DeviceDmaMapReq ABI");

	if (argc == 2 && !strcmp(argv[1], "huge")) {
		/* selected below */
	} else if (argc == 2 && !strcmp(argv[1], "dev")) {
		bdf = 0x28;
	} else if (argc == 3 && !strcmp(argv[1], "dev")) {
		bdf = strtoul(argv[2], NULL, 0);
		if (bdf > 0xffff)
			return usage();
	} else if (argc != 1) {
		return usage();
	}

	fd = open("/dev/hyperenclave", O_RDWR);
	if (fd < 0) {
		perror("/dev/hyperenclave");
		return 1;
	}
	if (argc == 1)
		rc = run_vcpu(fd);
	else if (!strcmp(argv[1], "huge"))
		rc = run_huge(fd);
	else
		rc = run_dev(fd, (unsigned int)bdf);
	close(fd);
	return rc;
}
