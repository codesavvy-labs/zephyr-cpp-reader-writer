# Zephyr C++ sensor reader–writer demo

One writer publishes a synthetic sensor sample to three readers representing
control, diagnostics, and telemetry. Each sample contains a sequence number,
temperature in millidegrees Celsius, pressure in pascals, a 32-bit uptime
timestamp in milliseconds, and a checksum. The writer sleeps between field
stores. Every reader validates the checksum and checks temperature and pressure
against the sample's sequence number.

## Build and run

From the application directory, activate the existing Zephyr Python environment
and select the Zephyr checkout and host toolchain.

```bash
source ~/projects/zephyr-venv/bin/activate
export ZEPHYR_BASE=~/projects/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=host

west build -b native_sim/native/64 -d build .
./build/zephyr/zephyr.exe
python -m unittest discover -s tests -v
```

The tests build both configurations in temporary directories. Each build has a
120-second host timeout, and each simulator run has a 10-second host timeout.
The synchronized test checks progress by all four workers, overlapping shared
access, absence of writer overlap and inconsistent reads, and observed writer
contention. The negative test bypasses both guards and requires the same
workload to detect writer overlap and inconsistent samples. The backing fields
use individual Zephyr atomic loads and stores so the negative run is free of
language-level data races while permitting inconsistent multi-field snapshots.

## Configuration and output

Set these application Kconfig options in `prj.conf` or pass a fragment with
`-DEXTRA_CONF_FILE=/path/to/demo.conf` after the `west build` command's `--`.

| Option | Default | Meaning |
| --- | ---: | --- |
| `CONFIG_APP_TEST_DURATION_MS` | 1000 | Observation window in simulated milliseconds. Use 0 for continuous operation. |
| `CONFIG_APP_READER_HOLD_MS` | 3 | Time spent inside each shared access before loading the sample. |
| `CONFIG_APP_CONTROL_DELAY_MS` | 5 | Control reader pause after releasing shared access. |
| `CONFIG_APP_DIAGNOSTICS_DELAY_MS` | 15 | Diagnostics reader pause after releasing shared access. |
| `CONFIG_APP_TELEMETRY_DELAY_MS` | 25 | Telemetry reader pause after releasing shared access. |
| `CONFIG_APP_WRITER_DELAY_MS` | 13 | Writer pause after releasing exclusive access. |
| `CONFIG_APP_WRITER_FIELD_DELAY_MS` | 2 | Delay between adjacent writer field stores. |
| `CONFIG_APP_DISABLE_SYNC` | n | Bypass the lock in finite mode for the negative test. |

At the end of the observation window, main requests shutdown and joins all
workers before printing one `SUMMARY ` line followed by a JSON object. In-flight
iterations finish, so counts can include work completed after the window.
The summary includes synchronization status, per-reader and total read and
inconsistency counts, completed writes, peak simultaneous readers, exclusion
violations, and the largest writer acquisition latency. An exclusion violation
is counted when a writer enters with readers active or a reader enters while
the writer is active. Writer latency measures elapsed Zephyr uptime ticks from
before requesting exclusive access to successful acquisition, expressed in
microseconds. The bypass build reports zero for this metric. The simulator exits
with status 0 when all workers made progress and both error counts are zero,
or status 1 otherwise. In continuous mode, stop the simulator with Ctrl+C.

## Synchronization and scheduling

- A reader acquires the gate mutex before touching the reader count. A separate
  mutex protects every count update. The first reader takes the binary resource
  semaphore before releasing the gate, and later readers share that ownership.
- A departing reader only needs the count mutex. The last reader gives the
  resource semaphore. The first and last reader can be different threads.
  A Zephyr mutex requires the owning thread to unlock it, whereas the semaphore
  permits that handoff.
- A writer holds the gate from before it takes the resource semaphore until
  after it gives the semaphore back. Once it owns the gate, new readers cannot
  enter. Existing readers can leave without the gate, allowing their group to
  drain. The resource semaphore excludes the writer from the whole group and
  also supports multiple exclusive callers, although the demo uses one.
- Use the lock in thread context. Each guard is scoped to the acquiring thread.
  The lock assumes balanced, nonrecursive acquisition and release and has no
  shared-to-exclusive upgrade operation. Keep the lock alive until all its
  users have finished.
- The demo uses equal-priority preemptible workers on native_sim. Sleeps inside
  the guards let other workers run and make overlapping readers and writer
  contention observable even on one simulated CPU. Before the writer acquires
  the gate, admission depends on Zephyr mutex scheduling. The algorithm provides
  no strict FIFO ordering or general starvation bound.
- Zephyr mutex priority inheritance can apply to the gate and count mutexes,
  subject to kernel configuration. The resource semaphore has no owning thread
  to boost. A higher-priority writer waiting on it can be delayed by
  lower-priority readers, including when unrelated runnable threads preempt them.
  The measured latency is an observation from simulated time at kernel tick
  resolution. Native_sim provides no hard real-time guarantees.
