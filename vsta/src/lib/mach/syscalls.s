/*
 * syscalls.c
 *	Actual entry points into the VSTa operating system
 *
 * We keep the return address in a register so that tfork() will
 * return correctly even though the child thread returns on a new
 * stack.
 */
#include <sys/syscall.h>
#include <make/assym.h>

#define ENTRY(n, v)	.globl	_##n ; \
	_##n: popl %ebx ; movl $v,%eax ; int $0xFF ; jmp %ebx

ENTRY(msg_port, S_MSG_PORT)
ENTRY(msg_connect, S_MSG_CONNECT)
ENTRY(msg_accept, S_MSG_ACCEPT)
ENTRY(msg_send, S_MSG_SEND)
ENTRY(msg_receive, S_MSG_RECEIVE)
ENTRY(msg_reply, S_MSG_REPLY)
ENTRY(msg_disconnect, S_MSG_DISCONNECT)
ENTRY(_msg_err, S_MSG_ERR)
ENTRY(_exit, S_EXIT)
ENTRY(fork, S_FORK)
ENTRY(tfork, S_THREAD)
ENTRY(enable_io, S_ENABIO)
ENTRY(enable_isr, S_ISR)
ENTRY(mmap, S_MMAP)
ENTRY(munmap, S_MUNMAP)
ENTRY(_strerror, S_STRERROR)
ENTRY(_notify, S_NOTIFY)
ENTRY(clone, S_CLONE)
ENTRY(page_wire, S_PAGE_WIRE)
ENTRY(page_release, S_PAGE_RELEASE)
ENTRY(enable_dma, S_ENABLE_DMA)
ENTRY(time_get, S_TIME_GET)
ENTRY(time_sleep, S_TIME_SLEEP)

