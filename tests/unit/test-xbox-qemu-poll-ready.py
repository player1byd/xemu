#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check CPU-saving selection and ready-event interruption with API doubles.

Run directly with a C compiler. Pass --source to check an older qemu-timer.c:
the original busy-wait must fail the ready-event deadline assertion.
This checks control flow, not native Windows wake latency.
"""
import argparse
import ast
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=repo/'util/qemu-timer.c')
args = parser.parse_args()
fixture = (repo/'tests/unit/test-xbox-qemu-poll-faults.py').read_text()
# Share only the literal C doubles; do not execute the other test script.
assignments = ast.parse(fixture).body
preamble = next(
    ast.literal_eval(node.value) for node in assignments
    if isinstance(node, ast.Assign) and any(
        isinstance(target, ast.Name) and target.id == 'preamble'
        for target in node.targets))
assert 'return now;' in preamble
assert 'if (n) f[n-1].revents=G_IO_IN;' in preamble
preamble = preamble.replace('return now;', 'return now += 1000;')
preamble = preamble.replace('if (n) f[n-1].revents=G_IO_IN;',
                            'if (n) f[0].revents=G_IO_IN;')
source = args.source.read_text()
helper = ''
if 'typedef struct XboxHighResolutionPoll {' in source:
    start = source.index('typedef struct XboxHighResolutionPoll {')
    helper = source[start:source.index('\n#endif', start)]
state = ""
if 'static bool qemu_poll_cpu_saving;' in source:
    state_start = source.index('static bool qemu_poll_cpu_saving;')
    state = source[state_start:source.index('#if defined(XBOX)', state_start)]
else:
    state = ('static void qemu_poll_set_cpu_saving(bool enabled) {}\n'
             'static bool qemu_poll_get_cpu_saving(void) { return false; }\n')
start = source.index('int qemu_poll_ns(')
body = source[start:source.index('\nvoid timer_init_full(', start)]
main = r'''
int main(void)
{
    GPollFD fd = { 7, G_IO_IN, 0 };

    assert(qemu_poll_ns(&fd, 1, 500000) == 1);
    assert(now >= 500000); /* CPU saving is off by default. */
    now = 0;
    puts("PASS: default preserves legacy wait");
    assert(arms == 0);
    qemu_poll_set_cpu_saving(true);
    assert(qemu_poll_get_cpu_saving());

    assert(qemu_poll_ns(&fd, 1, 500000) == 1);
    assert(now < 500000);
    assert(arms == 1);
    puts("PASS: enabled wait interrupts before deadline");
    qemu_poll_set_cpu_saving(false);
    now = 0;
    assert(qemu_poll_ns(&fd, 1, 500000) == 1);
    assert(now >= 500000);
    assert(arms == 1);
    puts("PASS: disabling restores legacy wait without rearming timer");
}
'''
with tempfile.TemporaryDirectory(prefix='xemu-poll-ready-') as tmp:
    c = Path(tmp)/'test.c'
    exe = Path(tmp)/'test'
    c.write_text(preamble + '\n#define XBOX\n#define _WIN32\n' +
                 'static int qemu_timeout_ns_to_ms(int64_t t){return 0;}\n' +
                 '#define qatomic_set(p,v) (*(p)=(v))\n' +
                 '#define qatomic_read(p) (*(p))\n' +
                 state + helper + body + main)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(c),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
