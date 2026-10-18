// SPDX-License-Identifier: Apache-2.0
/*
 * cloud-hypervisor isolated-VM launcher.
 *
 * Builds the same 9-page guest hedev op=vcpu runs, but from ring 3:
 * donations and VCPU_RUN go through HE_IOC_IVM. The guest writes 0x42,
 * asks L1 to print "ivm\n", programs a timer, and the handler stores 0xa5
 * before HLT.
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

int main(void)
{
	struct he_ivm_call call;
	unsigned char *mem, *shared;
	uint64_t *pt;
	int fd, i, saw_puts = 0, pass;
	unsigned char code0;
	__s64 vm;

	_Static_assert(sizeof(struct he_ivm_call) == 200, "he_ivm_call ABI");

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
	if (cpu_is_intel()) {
		unsigned char *p = mem + 4 * PAGE;

		for (i = 0; i + 2 < (int)sizeof(guest_code); i++) {
			if (p[i] == 0x0f && p[i + 1] == 0x01 && p[i + 2] == 0xd9)
				p[i + 2] = 0xc1;
		}
	}
	memcpy(mem + 4 * PAGE + 0x100, guest_handler, sizeof(guest_handler));
	memcpy(mem + GUEST_SHARED * PAGE + 0x30, "ivm\n", 4);
	{
		unsigned char *gate = mem + 7 * PAGE + 0x30 * 16;

		gate[0] = 0x00;
		gate[1] = 0x41;
		gate[2] = 0x08;
		gate[5] = 0x8e;
	}
	pt = (uint64_t *)(mem + 8 * PAGE);
	pt[1] = 0x00af9b000000ffffULL;
	pt[2] = 0x00cf93000000ffffULL;
	pt[3] = 0x0000890000000067ULL;
	code0 = mem[4 * PAGE];

	fd = open("/dev/hyperenclave", O_RDWR);
	if (fd < 0) {
		perror("/dev/hyperenclave");
		return 1;
	}

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_CREATE;
	if (ivm_call(fd, &call) || call.ret <= 0) {
		fprintf(stderr, "vm_create -> %lld\n", (long long)call.ret);
		return 1;
	}
	vm = call.ret;
	printf("vm_create -> %lld\n", (long long)vm);

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

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_VCPU_CREATE;
	call.vm_id = vm;
	call.rip = 0x4000;
	call.rsp = 0x5ff0;
	call.gdtr_base = 0x8000;
	call.gdtr_limit = 31;
	call.idtr_base = 0x7000;
	call.idtr_limit = 0xfff;
	call.exit_gpa = 0x6000;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "vcpu_create -> %lld\n", (long long)call.ret);
		return 1;
	}
	printf("vcpu_create -> 0\n");

	memset(&call, 0, sizeof(call));
	call.op = HC_VM_FINALIZE;
	call.vm_id = vm;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "finalize -> %lld\n", (long long)call.ret);
		return 1;
	}
	printf("finalize -> 0\n");

	shared = mem + GUEST_SHARED * PAGE;
	for (i = 0; i < 8; i++) {
		memset(&call, 0, sizeof(call));
		call.op = HC_VM_VCPU_RUN;
		call.vm_id = vm;
		if (ivm_call(fd, &call) || call.ret != 0) {
			fprintf(stderr, "vcpu_run[%d] -> %lld\n", i,
				(long long)call.ret);
			return 1;
		}
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
	memset(&call, 0, sizeof(call));
	call.op = HC_VM_DESTROY;
	call.vm_id = vm;
	if (ivm_call(fd, &call) || call.ret != 0) {
		fprintf(stderr, "destroy -> %lld\n", (long long)call.ret);
		return 1;
	}
	printf("destroy -> 0\n");
	printf("%s\n", pass ? "PASS ivm_launch" : "FAIL ivm_launch");
	return pass ? 0 : 1;
}
