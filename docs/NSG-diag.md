# NSG DIAG over QRTR

How `diagtty` turns a modern Qualcomm phone into something NSG can read.

## Service discovery

Modern Snapdragon SoCs do not expose `/dev/diag`. The diag service registers
with the QRTR name service (the IPC router). Query it from a root shell:

```console
$ su -c '/data/local/tmp/gl/qrtr_probe'
local node=0 port=5
service=1    wds     instance=0x1         node=0   port=9
service=4097         instance=0x0         node=0   port=28
...
```

On the SM8550 (Xperia 1 V) the DIAG command service is **service 4097 (0x1001),
node 0, port 28**. A second DCI instance exists at **port 31**. `diagtty`
hardcodes node 0 / port 28 (pass `dci` as the second argument for port 31).

## Framing

Two layers meet in the bridge:

**PTY side (what NSG writes and reads).** Classic DIAG HDLC-ish framing: a
request is a payload delimited by the `0x7e` flag, e.g. the VERNO version query:

```text
7e 37 44 b5 7e
```

Responses start with `7e 01` (response opcode) followed by the payload.

**QRTR side (what the modem expects).** Each datagram is the HDLC frame wrapped
in a four-byte little-endian header — `ver = 7`, then `len`:

```text
07 00 <len_lo> <len_hi> <HDLC frame incl. 0x7e flags>
```

`diagtty` reassembles bytes between `0x7e` flags from the PTY into frames,
prepends the header, and sends. On the way back it strips the header when the
declared length matches `n − 4`, then writes the raw DIAG bytes to the PTY.

## Samsung node emulation

NSG only enables its full logging engine when Samsung-style device nodes exist.
Three symlinks (created per-boot, gone after reboot) convince it:

```text
/dev/umts_router  ->  /data/local/tmp/diag0   # the node NSG keys on
/dev/umts_dm0     ->  /data/local/tmp/diag0
/dev/diag         ->  /data/local/tmp/diag0
```

`/data/local/tmp/diag0` is itself a symlink to the PTY slave (`/dev/pts/N`).
NSG's `bridge` process opens it; you can confirm with:

```console
$ adb shell 'su -c "ls -l /proc/$(pidof bridge)/fd"' | grep pts
```

## Self-test

The restore script pushes a five-byte VERNO request and checks the answer:

```text
request:  7e 37 44 b5 7e
expect:   response beginning 7e 01
```

If the PTY exists but the modem never answers, re-run `qrtr_probe` and confirm
service 4097 is still node 0 / port 28 — the port assignment is not an ABI.

## DCI (port 31)

`diagtty /data/local/tmp/diag_dci dci` bridges the DCI instance. NSG 4.8.8 does
not consume it; it is there for other tooling and future experiments.
