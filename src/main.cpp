#include "reader_writer_lock.hpp"

#include <posix_board_if.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <cstdint>

namespace {

struct SensorSample {
    uint32_t sequence;
    uint32_t temperature_milli_c;
    uint32_t pressure_pa;
    uint32_t timestamp_ms;
    uint32_t checksum;
};

uint32_t checksum(const SensorSample &sample)
{
    uint32_t hash = 2166136261u;
    const uint32_t words[] = {
        sample.sequence, sample.temperature_milli_c, sample.pressure_pa, sample.timestamp_ms,
    };
    for (uint32_t word : words) {
        hash = (hash ^ word) * 16777619u;
    }
    return hash;
}

SensorSample make_sample(uint32_t sequence, uint32_t timestamp_ms)
{
    SensorSample sample{
        sequence, 20000u + sequence % 10000u, 100000u + sequence % 3000u,
        timestamp_ms, 0u,
    };
    sample.checksum = checksum(sample);
    return sample;
}

// Individual atomic accesses keep the intentionally unlocked build free of data
// races while still allowing readers to see fields from different updates.
struct SensorStorage {
    atomic_t sequence;
    atomic_t temperature_milli_c;
    atomic_t pressure_pa;
    atomic_t timestamp_ms;
    atomic_t checksum;
};

struct Reader {
    const char *name;
    int delay_ms;
    unsigned int reads = 0;
    unsigned int inconsistent_reads = 0;
};

constexpr int worker_priority = 1;
constexpr int stack_size = 2048;
// Pack the writer flag and reader count into one atomic to observe access
// overlap at each entry without introducing a lock into the instrumentation.
constexpr atomic_val_t writer_active = 1 << 8;

Reader readers[] = {
    {"control", CONFIG_APP_CONTROL_DELAY_MS},
    {"diagnostics", CONFIG_APP_DIAGNOSTICS_DELAY_MS},
    {"telemetry", CONFIG_APP_TELEMETRY_DELAY_MS},
};
constexpr auto reader_count = sizeof(readers) / sizeof(readers[0]);

K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, reader_count, stack_size);
K_THREAD_STACK_DEFINE(writer_stack, stack_size);
k_thread reader_threads[reader_count];
k_thread writer_thread;

SensorStorage sensor{};
atomic_t stop_requested;
atomic_t active_accesses;
atomic_t max_active_readers;
atomic_t exclusion_violations;
unsigned int writes;
uint64_t max_writer_wait_ticks;

SensorSample load_sample()
{
    return {
        static_cast<uint32_t>(atomic_get(&sensor.sequence)),
        static_cast<uint32_t>(atomic_get(&sensor.temperature_milli_c)),
        static_cast<uint32_t>(atomic_get(&sensor.pressure_pa)),
        static_cast<uint32_t>(atomic_get(&sensor.timestamp_ms)),
        static_cast<uint32_t>(atomic_get(&sensor.checksum)),
    };
}

void read_sample(Reader &reader)
{
    const auto previous = atomic_inc(&active_accesses);
    if ((previous & writer_active) != 0) {
        atomic_inc(&exclusion_violations);
    }

    const auto concurrent = (previous & (writer_active - 1)) + 1;
    auto peak = atomic_get(&max_active_readers);
    while (concurrent > peak && !atomic_cas(&max_active_readers, peak, concurrent)) {
        peak = atomic_get(&max_active_readers);
    }

    k_msleep(CONFIG_APP_READER_HOLD_MS);
    const auto sample = load_sample();
    ++reader.reads;
    const auto expected = make_sample(sample.sequence, sample.timestamp_ms);
    if (sample.checksum != expected.checksum ||
        sample.temperature_milli_c != expected.temperature_milli_c ||
        sample.pressure_pa != expected.pressure_pa) {
        ++reader.inconsistent_reads;
    }
    atomic_dec(&active_accesses);
}

void write_sample(uint64_t wait_ticks)
{
    if (atomic_add(&active_accesses, writer_active) != 0) {
        atomic_inc(&exclusion_violations);
    }
    if (wait_ticks > max_writer_wait_ticks) {
        max_writer_wait_ticks = wait_ticks;
    }

    const auto sample = make_sample(writes + 1, static_cast<uint32_t>(k_uptime_get()));
    atomic_set(&sensor.sequence, sample.sequence);
    k_msleep(CONFIG_APP_WRITER_FIELD_DELAY_MS);
    atomic_set(&sensor.temperature_milli_c, sample.temperature_milli_c);
    k_msleep(CONFIG_APP_WRITER_FIELD_DELAY_MS);
    atomic_set(&sensor.pressure_pa, sample.pressure_pa);
    k_msleep(CONFIG_APP_WRITER_FIELD_DELAY_MS);
    atomic_set(&sensor.timestamp_ms, sample.timestamp_ms);
    k_msleep(CONFIG_APP_WRITER_FIELD_DELAY_MS);
    atomic_set(&sensor.checksum, sample.checksum);
    ++writes;
    atomic_sub(&active_accesses, writer_active);
}

void reader_entry(void *reader_arg, void *lock_arg, void *)
{
    auto &reader = *static_cast<Reader *>(reader_arg);
    auto &lock = *static_cast<ReaderWriterLock *>(lock_arg);
    while (!atomic_get(&stop_requested)) {
        if constexpr (IS_ENABLED(CONFIG_APP_DISABLE_SYNC)) {
            read_sample(reader);
        } else {
            ReaderWriterLock::SharedGuard guard(lock);
            read_sample(reader);
        }
        k_msleep(reader.delay_ms);
    }
}

void writer_entry(void *lock_arg, void *, void *)
{
    auto &lock = *static_cast<ReaderWriterLock *>(lock_arg);
    while (!atomic_get(&stop_requested)) {
        if constexpr (IS_ENABLED(CONFIG_APP_DISABLE_SYNC)) {
            write_sample(0);
        } else {
            const auto start = k_uptime_ticks();
            ReaderWriterLock::ExclusiveGuard guard(lock);
            write_sample(k_uptime_ticks() - start);
        }
        k_msleep(CONFIG_APP_WRITER_DELAY_MS);
    }
}

int report()
{
    unsigned int total_reads = 0;
    unsigned int inconsistent_reads = 0;
    bool all_readers_ran = true;
    printk("SUMMARY {\"sync_enabled\":%s,\"readers\":{",
           IS_ENABLED(CONFIG_APP_DISABLE_SYNC) ? "false" : "true");
    for (const auto &reader : readers) {
        printk("%s\"%s\":{\"reads\":%u,\"inconsistent_reads\":%u}",
               &reader == &readers[0] ? "" : ",",
               reader.name, reader.reads, reader.inconsistent_reads);
        total_reads += reader.reads;
        inconsistent_reads += reader.inconsistent_reads;
        all_readers_ran = all_readers_ran && reader.reads > 0;
    }

    const auto peak = static_cast<unsigned int>(atomic_get(&max_active_readers));
    const auto violations = static_cast<unsigned int>(atomic_get(&exclusion_violations));
    printk("},\"reads\":%u,\"writes\":%u,\"inconsistent_reads\":%u,"
           "\"max_concurrent_readers\":%u,\"exclusion_violations\":%u,"
           "\"max_writer_acquisition_us\":%llu}\n",
           total_reads, writes, inconsistent_reads, peak, violations,
           static_cast<unsigned long long>(k_ticks_to_us_floor64(max_writer_wait_ticks)));
    return all_readers_ran && writes > 0 && inconsistent_reads == 0 && violations == 0
               ? 0
               : 1;
}

} // namespace

int main()
{
    ReaderWriterLock lock;
    const auto initial = make_sample(0, 0);
    atomic_set(&sensor.temperature_milli_c, initial.temperature_milli_c);
    atomic_set(&sensor.pressure_pa, initial.pressure_pa);
    atomic_set(&sensor.checksum, initial.checksum);

    for (size_t i = 0; i < reader_count; ++i) {
        k_thread_create(&reader_threads[i], reader_stacks[i],
                        K_THREAD_STACK_SIZEOF(reader_stacks[i]), reader_entry,
                        &readers[i], &lock, nullptr, worker_priority, 0, K_NO_WAIT);
        k_thread_name_set(&reader_threads[i], readers[i].name);
    }
    k_thread_create(&writer_thread, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack),
                    writer_entry, &lock, nullptr, nullptr, worker_priority, 0, K_NO_WAIT);
    k_thread_name_set(&writer_thread, "writer");

    if constexpr (CONFIG_APP_TEST_DURATION_MS == 0) {
        k_sleep(K_FOREVER);
        return 0;
    }

    k_msleep(CONFIG_APP_TEST_DURATION_MS);
    atomic_set(&stop_requested, 1);
    k_thread_join(&writer_thread, K_FOREVER);
    for (auto &thread : reader_threads) {
        k_thread_join(&thread, K_FOREVER);
    }
    posix_exit(report());
}
