#!/usr/bin/env python3
# fuzz/gen_corpus.py — generate seed corpus for the fuzzer.
#
# libFuzzer starts from these seeds and mutates them.  Well-formed inputs
# give the fuzzer a running start — it finds deeper code paths faster than
# starting from random bytes.

import struct
import os

MAGIC   = 0xFEED1234
VERSION = 1

MSG_NEW_ORDER = 1
MSG_CANCEL    = 2
MSG_MODIFY    = 3
MSG_HEARTBEAT = 99

def wire_header(msg_type, seq, payload_len):
    return struct.pack('<IHHQI', MAGIC, VERSION, msg_type, seq, payload_len)

def new_order(seq, order_id, price_fp, qty, symbol, side, otype):
    payload = struct.pack('<QQIHBBxx',
        order_id, price_fp, qty, symbol, side, otype)
    return wire_header(MSG_NEW_ORDER, seq, len(payload)) + payload

def cancel_order(seq, order_id, symbol):
    payload = struct.pack('<QH6x', order_id, symbol)
    return wire_header(MSG_CANCEL, seq, len(payload)) + payload

def modify_order(seq, order_id, new_qty, symbol):
    payload = struct.pack('<QIH2x', order_id, new_qty, symbol)
    return wire_header(MSG_MODIFY, seq, len(payload)) + payload

def heartbeat(seq):
    return wire_header(MSG_HEARTBEAT, seq, 0)

corpus_dir = os.path.join(os.path.dirname(__file__), 'corpus')
os.makedirs(corpus_dir, exist_ok=True)

samples = [
    # Heartbeat
    ('heartbeat',   heartbeat(0)),
    # Resting sell
    ('sell_limit',  new_order(1, 1001, 100_000_000, 100, 0, 1, 0)),
    # Aggressive buy (crosses)
    ('buy_limit',   new_order(2, 1002, 100_000_000, 100, 0, 0, 0)),
    # Market buy
    ('buy_market',  new_order(3, 1003, 0,           200, 0, 0, 1)),
    # IOC
    ('ioc',         new_order(4, 1004, 99_000_000,  50,  0, 0, 2)),
    # Cancel
    ('cancel',      cancel_order(5, 1001, 0)),
    # Modify
    ('modify',      modify_order(6, 1002, 50, 0)),
    # Truncated header (too short)
    ('truncated',   b'\x34\x12\xed\xfe'),
    # Wrong magic
    ('bad_magic',   b'\x00' * 20),
    # Max qty
    ('max_qty',     new_order(7, 9999, 50_000_000, 0xFFFFFFFF, 0, 0, 0)),
    # Negative price (as uint64)
    ('neg_price',   new_order(8, 8888, 0xFFFFFFFFFFFFFFFF, 100, 0, 1, 0)),
]

for name, data in samples:
    path = os.path.join(corpus_dir, name)
    with open(path, 'wb') as f:
        f.write(data)
    print(f'  {path}  ({len(data)} bytes)')

print(f'\nGenerated {len(samples)} corpus seeds in {corpus_dir}/')
print('\nTo fuzz:')
print('  clang++ -std=c++20 -O1 -fsanitize=fuzzer,address -Iinclude \\')
print('    fuzz/fuzz_market_data.cpp src/**/*.cpp -lpthread -o fuzz_market_data')
print('  ./fuzz_market_data fuzz/corpus/ -max_len=512 -jobs=4')
