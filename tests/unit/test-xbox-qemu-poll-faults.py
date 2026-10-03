#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the actual Windows wait helper against deterministic API doubles.

Run with a C compiler: python3 tests/unit/test-xbox-qemu-poll-faults.py
This verifies error/ownership contracts, not native Windows wake latency.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'util/qemu-timer.c').read_text()
start = source.index('typedef struct XboxHighResolutionPoll {')
end = source.index('\n#endif', start)
helper = source[start:end]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
typedef void *HANDLE;
typedef unsigned DWORD;
typedef unsigned guint;
typedef intptr_t gintptr;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct Notifier { void (*notify)(struct Notifier *, void *); } Notifier;
typedef struct { gintptr fd; unsigned short events, revents; } GPollFD;
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#define TIMER_MODIFY_STATE 2
#define SYNCHRONIZE 16
#define G_IO_IN 1
#define QEMU_CLOCK_REALTIME 0
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define g_renew(t,p,n) ((t *)realloc(p,sizeof(t)*(n)))
#define g_free free
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NOT_SUPPORTED 50
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffff
static int creates, arms, cancels, polls, closes, warnings;
static bool create_fail, arm_fail, cancel_fail, poll_fail;
static DWORD error_code;
static int64_t now;
static DWORD GetLastError(void) { return error_code; }
static void SetLastError(DWORD e) { error_code=e; }
static HANDLE CreateWaitableTimerExW(void *a,void *b,int c,int d) {
    creates++; return create_fail ? NULL : (HANDLE)(uintptr_t)1;
}
static bool SetWaitableTimerEx(HANDLE h, LARGE_INTEGER *d, int p,
                              void *a, void *b, void *c, int t) {
    arms++; return !arm_fail;
}
static bool CancelWaitableTimer(HANDLE h) { cancels++; return !cancel_fail; }
static DWORD WaitForSingleObject(HANDLE h,int t) { return WAIT_TIMEOUT; }
static void CloseHandle(HANDLE h) { closes++; }
static void qemu_thread_atexit_add(Notifier *n) {}
static int64_t qemu_clock_get_ns(int c) { return now; }
#define warn_report(...) (warnings++)
static int g_poll(GPollFD *f,guint n,int t) {
    polls++;
    for (unsigned i=0;i<n;i++) f[i].revents=0;
    if (poll_fail) {errno=EIO;return -1;}
    if (n) f[n-1].revents=G_IO_IN;
    return n ? 1 : 0;
}
'''
tests = r'''
static void reset(void) {
    xbox_high_resolution_poll_cleanup(NULL,NULL);
    creates=arms=cancels=polls=closes=warnings=0;
    create_fail=arm_fail=cancel_fail=poll_fail=false;
    now=0; error_code=0;
}
int main(void) {
    GPollFD fd={.fd=7,.events=G_IO_IN,.revents=G_IO_IN};
    reset(); poll_fail=true;
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==-1);
    assert(fd.revents==0 && errno==EIO);
    reset(); xbox_high_resolution_poll.in_use=true;
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==INT_MIN);
    assert(creates==0 && polls==0);
    reset(); create_fail=true;error_code=ERROR_NOT_SUPPORTED;
    for (int i = 0; i < 1000; i++) {
        assert(xbox_high_resolution_poll_ns(&fd, 1, 500000) == INT_MIN);
    }
    assert(creates==1); /* Permanent unavailability must not retry per wait. */
    assert(warnings==1);
    reset(); create_fail=true;error_code=8;
    for (int i = 0; i < 1000; i++) {
        assert(xbox_high_resolution_poll_ns(&fd, 1, 500000) == INT_MIN);
    }
    assert(creates==1); /* Transient failures require a retry backoff. */
    now=1000000000;create_fail=false;
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==0);
    assert(creates==2 && arms==1);
    reset(); arm_fail=true;error_code=8;
    for (int i = 0; i < 1000; i++) {
        assert(xbox_high_resolution_poll_ns(&fd, 1, 500000) == INT_MIN);
    }
    assert(arms==1 && warnings==1);
    now=1000000000;arm_fail=false;
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==0);
    reset(); poll_fail=true;cancel_fail=true;error_code=8;
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==-1);
    assert(closes==1 && xbox_high_resolution_poll.timer==NULL);
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==INT_MIN);
    assert(creates==1);
    now=1000000000;poll_fail=false;cancel_fail=false;
    assert(xbox_high_resolution_poll_ns(&fd,1,500000)==0);
    assert(creates==2);
    reset();
    puts("PASS: readiness, ownership, failure backoff and recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='xemu-poll-faults-') as tmp:
    c = Path(tmp)/'test.c'
    c.write_text(preamble+helper+tests)
    exe = Path(tmp)/'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-g',
                    '-fsanitize=address,undefined', str(c), '-o', str(exe)],
                   check=True)
    subprocess.run([str(exe)],check=True)
