# File-backed mmap input plugin

## Introduction

This sample-source plugin drains a producer-owned circular buffer from a
memory-mapped file. The producer supplies interleaved signed 16-bit I/Q
samples and advances a monotonic write sequence. SDRangel converts each
component to its internal sample width and writes the result to the standard
sample-source FIFO.

The mmap header is 64 bytes, little-endian, with this layout:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | Magic `UDMAIQ1` followed by NUL |
| 8 | 4 | Header version, currently 1 |
| 12 | 4 | Header size, currently 64 |
| 16 | 4 | Sample rate in samples/s |
| 20 | 4 | Component size, currently 16 bits |
| 24 | 8 | Center frequency in Hz |
| 32 | 8 | Ring capacity in complex samples |
| 40 | 8 | Producer write sequence in complex samples |
| 48 | 4 | Test pattern identifier |
| 52 | 4 | Reserved |
| 56 | 8 | Producer-reported dropped samples |

The ring begins at byte 64. Each complex sample is four bytes: signed
little-endian `int16` I followed by signed little-endian `int16` Q. The write
sequence is published with release ordering after the producer has written
the corresponding ring entries. The consumer reads it with acquire ordering.

If the producer laps the consumer, the plugin resumes at the oldest sample
still present and reports the upstream drop count. A ramp-pattern producer
also enables continuity checking in the worker log.

## Interface

Select the producer's mmap file, start the device engine, and use Play to
start or stop draining. Sample rate and center frequency come from the mmap
header. File looping, seeking, and playback acceleration do not apply to a
live ring and their controls are disabled.

The MAP indicator is green when the header and mapped-file size are valid,
red when validation fails, and grey before a file has been checked.
