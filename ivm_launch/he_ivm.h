/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Userspace copy of hyperenclave-driver/driver/ioctl.h's isolated-VM ABI.
 * sizeof(struct he_ivm_call) is 200. Keep the two headers in lockstep.
 */
#ifndef HE_IVM_H
#define HE_IVM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define HC_DEVICE_MAP_DMA 0x53
#define HC_DEVICE_UNMAP_DMA 0x54

#define HC_VM_CREATE 0x60
#define HC_VM_DESTROY 0x61
#define HC_VM_FINALIZE 0x62
#define HC_VM_ADD_PAGE 0x63
#define HC_VM_REMOVE_PAGE 0x64
#define HC_VM_ADD_SHARED 0x65
#define HC_VM_VCPU_CREATE 0x66
#define HC_VM_VCPU_RUN 0x67
#define HC_VM_VCPU_INJECT 0x68
#define HC_VM_VCPU_SET_TIMER 0x69
#define HC_DEV_ATTACH 0x6a
#define HC_DEV_DETACH 0x6b
#define HC_DEV_MAP_BAR 0x6c
#define HC_VM_MEASURE 0x6d

#define HE_IVM_PAGE_MEASURE 1u
#define HE_IVM_PAGE_2M 2u
/* MAP_DMA / UNMAP_DMA only: fill maps[0].paddr from the donated page at gpa. */
#define HE_IVM_DMA_USE_GPA 1u

#define HE_DMA_MAP_MAX 64

struct he_dma_mapping {
	__u64 iova;
	__u64 paddr;
	__u64 size;
} __attribute__((packed));

struct he_device_dma_map_req {
	__u16 bdf;
	__u16 _reserved[3];
	__u64 handle;
	__u32 nr_maps;
	__u32 _reserved2;
	struct he_dma_mapping maps[HE_DMA_MAP_MAX];
} __attribute__((packed));

#define HE_EXIT_HLT 1u
#define HE_EXIT_EXT_INTR 2u
#define HE_EXIT_NEED_PAGE 3u
#define HE_EXIT_SERVICE 4u
#define HE_EXIT_FAULT 5u
#define HE_EXIT_TIMER 6u

struct he_ivm_call {
	__u32 op;
	__u32 idx;
	__s64 ret;
	__u64 vm_id;
	__u64 gpa;
	__u64 user_addr;
	__u32 flags;
	__u32 vector;
	__u64 aux;
	__u64 rip;
	__u64 rsp;
	__u64 cr3;
	__u64 gdtr_base;
	__u64 gdtr_limit;
	__u64 idtr_base;
	__u64 idtr_limit;
	__u64 exit_gpa;
	__u32 reason;
	__u32 request;
	__u64 arg0;
	__u64 arg1;
	__u64 arg2;
	__u64 bdf;
	__u32 bar;
	__u32 _pad;
	__u8 digest[32];
} __attribute__((packed));

#define HE_IOC_IVM _IOWR(0, 0x30, struct he_ivm_call)

#endif
