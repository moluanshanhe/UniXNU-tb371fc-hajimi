// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB371FC Novatek NT36532 SPI touchscreen runtime integrated with J716fTouchState / NativeHidInput.
// Auto-selects Tianma (GPIO82=1, PID 0x6068) or BOE (GPIO82=0, PID 0x6067) firmware.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "Tb371fcTouchBoard.h"
#include "Tb371fcTouchReport.h"

extern "C" {
#include "GeniSpi.h"
#include "NvtProtocol.h"

extern const uint8_t tb371fc_touch_fw_tm[];
extern const uint32_t tb371fc_touch_fw_tm_size;
extern const uint8_t tb371fc_touch_fw_boe[];
extern const uint32_t tb371fc_touch_fw_boe_size;

extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
extern void native_touch_published(void);
extern volatile uint32_t native_touch_injecting;

volatile uint32_t tb371fc_touch_stage = 0, tb371fc_touch_reads = 0, tb371fc_touch_valid = 0;
volatile uint32_t tb371fc_touch_bad = 0, tb371fc_touch_recoveries = 0, tb371fc_touch_edges = 0;
volatile int tb371fc_touch_error = 0;
}

namespace {
struct Region {
    IOMemoryMap *mapping = nullptr;
    uintptr_t va = 0;
    bool map(uint64_t physical, uint64_t bytes) {
        IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
        if (!memory) return false;
        mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        va = mapping ? mapping->getVirtualAddress() : 0;
        return va != 0;
    }
};

uint32_t rd(uintptr_t base, unsigned reg) { return *reinterpret_cast<volatile uint32_t *>(base + reg); }
void wr(uintptr_t base, unsigned reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRead(void *p, uint32_t r) { return rd(reinterpret_cast<uintptr_t>(p), r); }
void ioWrite(void *p, uint32_t r, uint32_t v) { wr(reinterpret_cast<uintptr_t>(p), r, v); }
void ioDelay(void *, unsigned us) { IODelay(us); }

struct Session {
    Region gccQup, gccCore, gccVote, spiPins, resetPin, irqPin, panelPin, wrapper, engine;
    geni_spi spi = {};
    nvt_bus bus = {};
    nvt_chip chip = {};
    nvt_fw firmware = {};
    nvt_fw_info info = {};
    uint64_t clocks[8] = {};
    unsigned selectedLevel = 0, selectedHz = 0;
    unsigned panelId = 0;
    const uint8_t *fwBlob = nullptr;
    uint32_t fwSize = 0;
    uint16_t expectedPid = 0;
};

uint64_t clockRate(uint32_t cfg, uint32_t m, uint32_t n) {
    unsigned source = (cfg >> 8) & 7, hid = cfg & 31, mode = (cfg >> 12) & 3;
    uint64_t hz = source == 0 ? 19200000ULL : source == 1 ? 600000000ULL : source == 6 ? 300000000ULL : 0;
    if (hid) hz = hz * 2 / (hid + 1);
    if (mode) {
        m &= 0xffff;
        n = ((~n) & 0xffff) + m;
        if (!n) return 0;
        hz = hz * m / n;
    }
    return hz;
}

bool stage(unsigned n, int error = 0) {
    tb371fc_touch_stage = n;
    tb371fc_touch_error = error;
    IOLog("[TB371FC touch] stage=%u error=%d reads=%u valid=%u bad=%u edges=%u recover=%u\n", n, error,
          tb371fc_touch_reads, tb371fc_touch_valid, tb371fc_touch_bad, tb371fc_touch_edges, tb371fc_touch_recoveries);
    return error == 0;
}

int transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length) {
    Session *s = static_cast<Session *>(context);
    int rc = geni_spi_transfer(&s->spi, tx, rx, length, 0);
    if (rc && !tb371fc_touch_error) {
        tb371fc_touch_error = rc;
        IOLog("[TB371FC touch] SPI error=%d len=%lu irq=%x status=%x tx=%x rx=%x\n", rc,
              (unsigned long)length, s->spi.snap_irq, s->spi.snap_status, s->spi.snap_txfs, s->spi.snap_rxfs);
    }
    return rc;
}

void delayMs(void *, unsigned ms) { IOSleep(ms); }

void reset(void *context, int asserted) {
    Session *s = static_cast<Session *>(context);
    wr(s->resetPin.va, 4, asserted ? 0 : 2);
    wr(s->resetPin.va, 0, (TB371FC_TOUCH_DRIVE_6MA << 6) | (1u << 9));
}

void publishFrame(const Session &s, const Tb371fcTouchFrame &frame) {
    if (__atomic_load_n(&native_touch_injecting, __ATOMIC_ACQUIRE)) return;
    uint32_t seq = native_touch_seq;
    if (seq & 1u) ++seq;
    native_touch_seq = seq + 1u;
    __asm__ volatile("dmb ish" ::: "memory");

    unsigned count = frame.count;
    if (count > TB371FC_TOUCH_CONTACTS) count = TB371FC_TOUCH_CONTACTS;
    const bool landscapeDirect = (s.info.abs_x_max == 2944u && s.info.abs_y_max == 1840u);
    for (unsigned i = 0; i < count; ++i) {
        uint16_t x = 0, y = 0;
        if (landscapeDirect) {
            uint16_t rx = frame.contacts[i].x > 2944u ? 2944u : frame.contacts[i].x;
            uint16_t ry = frame.contacts[i].y > 1840u ? 1840u : frame.contacts[i].y;
            x = static_cast<uint16_t>(((uint32_t)rx * (TB371FC_HID_INPUT_X_MAX - 1u) + 1472u) / 2944u);
            y = static_cast<uint16_t>(((uint32_t)ry * (TB371FC_HID_INPUT_Y_MAX - 1u) + 920u) / 1840u);
        } else {
            tb371fc_touch_to_hid(frame.contacts[i].x, frame.contacts[i].y, &x, &y);
        }
        native_touch_ids[i] = frame.contacts[i].id;
        native_touch_xs[i] = x;
        native_touch_ys[i] = y;
    }
    native_touch_count = count;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = seq + 2u;
    native_touch_published();
}

bool startBus(Session &s) {
    stage(1);
    if (!s.gccQup.map(TB371FC_TOUCH_GCC_BASE + 0x1e000, 0x1000) ||
        !s.gccCore.map(TB371FC_TOUCH_GCC_BASE + 0x23000, 0x1000) ||
        !s.gccVote.map(TB371FC_TOUCH_GCC_BASE + 0x52000, 0x1000) ||
        !s.spiPins.map(TB371FC_TOUCH_TLMM_BASE + tb371fc_touch_tlmm_ctl(40), 0x4000) ||
        !s.resetPin.map(TB371FC_TOUCH_TLMM_BASE + tb371fc_touch_tlmm_ctl(56), 0x1000) ||
        !s.irqPin.map(TB371FC_TOUCH_TLMM_BASE + tb371fc_touch_tlmm_ctl(68), 0x1000) ||
        !s.panelPin.map(TB371FC_TOUCH_TLMM_BASE + tb371fc_touch_tlmm_ctl(82), 0x1000))
        return stage(1, -12);

    wr(s.panelPin.va, 0, rd(s.panelPin.va, 0) & ~((15u << 2) | (1u << 9)));
    s.panelId = rd(s.panelPin.va, 4) & 1u;
    if (s.panelId == 1u) {
        s.fwBlob = tb371fc_touch_fw_tm;
        s.fwSize = tb371fc_touch_fw_tm_size;
        s.expectedPid = 0x6068u;
    } else {
        s.fwBlob = tb371fc_touch_fw_boe;
        s.fwSize = tb371fc_touch_fw_boe_size;
        s.expectedPid = 0x6067u;
    }
    IOLog("[TB371FC touch] panel-id=%u (%s) fw-bytes=%u expected-pid=0x%04x\n",
          s.panelId, s.panelId ? "Tianma" : "BOE", s.fwSize, s.expectedPid);
    if (s.fwSize != 245760u) return stage(1, -19);

    wr(s.gccVote.va, 0x10, rd(s.gccVote.va, 0x10) | TB371FC_TOUCH_GCC_VOTE_MASK);
    auto running = [](uint32_t value) { return !(value & (1u << 31)) || (value & 2); };
    bool on = false;
    for (unsigned i = 0; i < 2000; ++i) {
        on = running(rd(s.gccQup.va, 4)) && running(rd(s.gccQup.va, 8)) &&
            !(rd(s.gccQup.va, 0xc) & (1u << 31)) && !(rd(s.gccCore.va, 0x270) & (1u << 31)) &&
            !(rd(s.gccCore.va, 0x278) & (1u << 31));
        if (on) break;
        IODelay(10);
    }
    if (!on) return stage(2, -110);

    const bool dfs = rd(s.gccQup.va, 0x24) & 1;
    for (unsigned i = 0; i < (dfs ? 8u : 1u); ++i) {
        unsigned cfg = dfs ? 0x2c + 4 * i : 0x14;
        unsigned m = dfs ? 0x6c + 4 * i : 0x18;
        unsigned n = dfs ? 0xac + 4 * i : 0x1c;
        s.clocks[i] = clockRate(rd(s.gccQup.va, cfg), rd(s.gccQup.va, m), rd(s.gccQup.va, n));
    }
    for (unsigned i = 0; i < 4; ++i)
        wr(s.spiPins.va, i * TB371FC_TOUCH_GPIO_STRIDE, (TB371FC_TOUCH_QUP14_FUNC << 2) | (TB371FC_TOUCH_DRIVE_6MA << 6));
    wr(s.irqPin.va, 0, (TB371FC_TOUCH_DRIVE_6MA << 6));

    if (!s.wrapper.map(TB371FC_TOUCH_QUP_BASE, TB371FC_TOUCH_QUP_SIZE) ||
        !s.engine.map(TB371FC_TOUCH_SE_BASE, TB371FC_TOUCH_SE_SIZE)) return stage(3, -12);

    s.spi.se = {reinterpret_cast<void *>(s.engine.va), ioRead, ioWrite, ioDelay};
    s.spi.wrapper = {reinterpret_cast<void *>(s.wrapper.va), ioRead, ioWrite, ioDelay};
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;
    s.spi.allow_fifo_disabled = 1;
    int rc = geni_spi_init(&s.spi);
    IOLog("[TB371FC touch] GENI proto=%u fifo-disabled=%u depth=%u width=%u dfs=%u\n", s.spi.info.protocol,
          s.spi.info.fifo_disabled, s.spi.info.tx_fifo_depth, s.spi.info.fifo_width_bits, dfs);
    if (rc) return stage(3, rc);

    unsigned best = 0;
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t hz = s.clocks[i], denominator = (uint64_t)s.spi.oversampling * TB371FC_TOUCH_SPI_MAX_HZ;
        if (!hz) continue;
        uint64_t div = (hz + denominator - 1) / denominator;
        if (!div || div > 0xfff) continue;
        unsigned actual = static_cast<unsigned>(hz / (s.spi.oversampling * div));
        if (actual > best && actual <= TB371FC_TOUCH_SPI_MAX_HZ) { best = actual; s.selectedLevel = i; }
    }
    if (!best) return stage(4, -22);
    s.selectedHz = best;
    rc = geni_spi_set_clock(&s.spi, TB371FC_TOUCH_SPI_MAX_HZ, static_cast<unsigned>(s.clocks[s.selectedLevel]), s.selectedLevel);
    if (!rc) rc = geni_spi_setup(&s.spi, 0, 0);
    IOLog("[TB371FC touch] clock-level=%u source=%llu spi=%u\n", s.selectedLevel,
          (unsigned long long)s.clocks[s.selectedLevel], s.selectedHz);
    return stage(4, rc);
}

void armAttention(Session &s) {
    wr(s.irqPin.va, 8, (1u << 4) | (1u << 2) | (1u << 1));
    wr(s.irqPin.va, 12, 0);
}

bool download(Session &s) {
    int rc = nvt_download_firmware(&s.chip, &s.firmware);
    if (rc) {
        IOLog("[TB371FC touch] download=%d attempt=%u phase=%u reset=%02x crc=%02x\n", rc,
              s.chip.dl_attempt, s.chip.dl_stage, s.chip.dl_reset_state[0], s.chip.crc_flags);
        return stage(6, rc);
    }
    rc = nvt_start_reports(&s.chip, &s.info);
    IOLog("[TB371FC touch] firmware=%02x pid=%04x range=%ux%u cascade=%u start=%d\n", s.info.fw_ver,
          s.info.pid, s.info.abs_x_max, s.info.abs_y_max, s.chip.cascade, rc);
    const bool rangeOk =
        (s.info.abs_x_max == 2944u && s.info.abs_y_max == 1840u) ||
        (s.info.abs_x_max == 18400u && s.info.abs_y_max == 29440u);
    if (rc || (s.info.pid != 0x6068u && s.info.pid != 0x6067u) || !rangeOk)
        return stage(7, rc ? rc : -19);
    armAttention(s);
    return stage(8);
}

void run(void *, wait_result_t) {
    Session *s = new Session;
    if (!s) { stage(0, -12); return; }
    if (!startBus(*s)) return;
    s->bus = {s, transfer, delayMs, reset};
    s->chip.bus = &s->bus;
    s->chip.scratch = static_cast<uint8_t *>(IOMalloc(NVT_TRANSFER_LEN + 8));
    s->chip.rxbuf = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 2));
    if (!s->chip.scratch || !s->chip.rxbuf) { stage(5, -12); return; }
    int rc = nvt_identify(&s->chip);
    IOLog("[TB371FC touch] identify=%d trim=%02x%02x%02x%02x%02x%02x map=%s\n", rc, s->chip.id[0], s->chip.id[1],
          s->chip.id[2], s->chip.id[3], s->chip.id[4], s->chip.id[5], s->chip.map ? s->chip.map->name : "none");
    if (rc || !s->chip.cascade || !s->chip.map || s->chip.map->event_buf != 0x125800) { stage(5, rc ? rc : -19); return; }
    rc = nvt_fw_parse(&s->firmware, s->fwBlob, s->fwSize);
    if (rc) { stage(6, rc); return; }
    if (!download(*s)) return;

    unsigned idle = 0, watchdog = 0, errors = 0, published = 0, ticks = 0;
    unsigned badLogged = 0, goodLogged = 0;
    uint8_t report[TB371FC_TOUCH_REPORT_BYTES] = {};
    for (;;) {
        IOSleep(2);
        if (++ticks % 5000 == 0) stage(8);
        bool edge = (rd(s->irqPin.va, 12) & 1) != 0;
        if (edge) { wr(s->irqPin.va, 12, 0); tb371fc_touch_edges = tb371fc_touch_edges + 1; }
        if (!edge && ++idle < 250) continue;
        idle = 0;
        rc = nvt_read_points(&s->chip, report + 1);
        tb371fc_touch_reads = tb371fc_touch_reads + 1;
        if (rc) {
            Tb371fcTouchFrame empty = {};
            if (published) { publishFrame(*s, empty); published = 0; }
            if (++errors >= 3) { stage(9, rc); return; }
            continue;
        }
        errors = 0;
        int recovery = tb371fc_touch_recovery(report, sizeof report);
        if (recovery) {
            tb371fc_touch_recoveries = tb371fc_touch_recoveries + 1;
            Tb371fcTouchFrame empty = {};
            if (published) { publishFrame(*s, empty); published = 0; }
            if (recovery == 2 && ++watchdog >= 10) { watchdog = 0; if (!download(*s)) return; }
            continue;
        }
        watchdog = 0;
        Tb371fcTouchFrame frame = {};
        int dec = tb371fc_touch_decode_ex(report, sizeof report,
                                          s->info.abs_x_max, s->info.abs_y_max, &frame);
        if (dec != 0 && !tb371fc_touch_checksum_ok(report, sizeof report)) {
            /* Single immediate retry if SPI read collided with IC SRAM update. */
            IODelay(200);
            if (nvt_read_points(&s->chip, report + 1) == 0) {
                dec = tb371fc_touch_decode_ex(report, sizeof report,
                                              s->info.abs_x_max, s->info.abs_y_max, &frame);
            }
        }
        if (dec) {
            tb371fc_touch_bad = tb371fc_touch_bad + 1;
            if (badLogged < 5u) {
                ++badLogged;
                IOLog("[TB371FC touch] bad#%u chk=%d r[1..8]=%02x %02x %02x %02x %02x %02x %02x %02x r[65]=%02x\n",
                      badLogged, tb371fc_touch_checksum_ok(report, sizeof report),
                      report[1], report[2], report[3], report[4],
                      report[5], report[6], report[7], report[8], report[65]);
            }
            continue;
        }
        tb371fc_touch_valid = tb371fc_touch_valid + 1;
        if (frame.count && goodLogged < 5u) {
            ++goodLogged;
            IOLog("[TB371FC touch] ok#%u cnt=%u id=%u pos=(%u,%u) raw=%02x %02x %02x %02x %02x %02x\n",
                  goodLogged, frame.count, frame.contacts[0].id,
                  frame.contacts[0].x, frame.contacts[0].y,
                  report[1], report[2], report[3], report[4], report[5], report[6]);
        }
        if (frame.count || published) publishFrame(*s, frame);
        published = frame.count;
    }
}
}  // namespace

extern "C" void tb371fc_touch_start(void) {
    static bool started = false;
    unsigned enabled = 0;
    if (started || !PE_parse_boot_argn("tb371fc-touch", &enabled, sizeof enabled) || enabled != 1) return;
    started = true;
    thread_t worker = THREAD_NULL;
    kern_return_t rc = kernel_thread_start(run, nullptr, &worker);
    if (rc == KERN_SUCCESS) thread_deallocate(worker);
    else stage(0, rc);
}
