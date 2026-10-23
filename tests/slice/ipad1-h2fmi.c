/* S5L8930 H2FMI (hw/arm/s5l8930_h2fmi.c): the read transfer-start rule, O(1)
 * FIFO reads, and a write that waits for its page's data AND metadata.
 *
 * Recent iPad fixes this pins (driven through the MMIO handlers, IOP page store
 * stubbed, no emulator):
 *   - A read transfer starts only on entering read mode (control bits 0-1 -> 3)
 *     or, in read mode, raising bit 7; a repeated write of 3 is not a new page
 *     (the FPart phantom-transfer fix).
 *   - FIFO reads pop from an advancing offset (O(1)), returning page bytes in
 *     order without re-shifting the buffer.
 *   - A page program completes only once the FIFOs hold the page's data AND its
 *     metadata (the 3.2.2 IOP-panic fix).
 *   - Any command but READ ID ends ID mode: a program right after the ID reads
 *     (the IOP firmware's first op on a bus after a hibernate wake) has its
 *     status poll (go 0x50) read the status, ready and not write-protected, not
 *     an ID byte (the n88 "WMR_ASSERT failed:(bWPDisabled)" fix).
 *
 * Mutation (named, must fail this test): in h2fmi_write's FMI_CONTROL case,
 * weaken the transfer-start guard
 *     if ((v & 3) == 3 && (b->read_pending || unread || (prev & 3) != 3 ||
 *                          ((v & 0x80) && !(prev & 0x80)))) {
 * to
 *     if ((v & 3) == 3) {
 * so every write of 3 re-transfers the page (the phantom transfer).
 *
 * SLICE hw/arm/s5l8930_h2fmi.c range #define H2FMI_BUSES | #define HT(
 * SLICE hw/arm/s5l8930_h2fmi.c fn h2fmi_format h2fmi_status h2fmi_update_irq h2fmi_ce h2fmi_fmc_events h2fmi_ppn_load h2fmi_command h2fmi_go fifo_compact h2fmi_raw_read h2fmi_read_bytes h2fmi_room h2fmi_transfer h2fmi_meta_per_page h2fmi_write_check h2fmi_drain fifo_pop h2fmi_ecc_sector h2fmi_seq_pop h2fmi_seq_advance h2fmi_seq_run h2fmi_seq_command h2fmi_read h2fmi_write
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, qemu_irq;
typedef struct DeviceState DeviceState;
typedef struct S5L8930H2FMIState S5L8930H2FMIState;
#define S5L8930_H2FMI_BASE 0x81200000u
#define LOG_UNIMP 1
#define LOG_GUEST_ERROR 2
#define HWADDR_PRIx "llx"
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define qemu_log_mask(...) ((void)0)
#define qemu_log(...) ((void)0)
#define HT(...) ((void)0)
static void qemu_set_irq(int irq, int v) { (void)irq; (void)v; }
static int ctz32(uint32_t v) { int n = 0; if (!v) return 32; while (!(v & 1)) { v >>= 1; n++; } return n; }
static int ctpop8(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static void stl_le_p(void *p, uint32_t v) {
    uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
/* IOP page-store stub: a 16-byte page (bytes 0,1,2,...) plus 10 meta bytes. */
static uint32_t g_page_bytes = 16, g_id = 0xa14d4b48;
static uint8_t g_ce_mask = 0x01;
static struct { int called, bus; uint32_t ce, page, len; uint8_t data[64], meta[16]; } g_prog;
void s5l8930_iop_nand_info(DeviceState *d, uint32_t *id, uint8_t *m, uint32_t *pb) {
    (void)d; *id = g_id; *m = g_ce_mask; *pb = g_page_bytes;
}
bool s5l8930_iop_nand_read(DeviceState *d, int bus, uint32_t ce, uint32_t page,
                           uint8_t *buf, uint32_t *stride) {
    (void)d; (void)bus; (void)ce; (void)page;
    for (unsigned i = 0; i < g_page_bytes + 64; i++) buf[i] = (uint8_t)i;
    *stride = g_page_bytes + 16;
    return true;
}
uint32_t s5l8930_iop_nand_program(DeviceState *d, int bus, uint32_t ce, uint32_t page,
                                  const uint8_t *data, uint32_t len, const uint8_t *meta) {
    (void)d; g_prog.called++; g_prog.bus = bus; g_prog.ce = ce; g_prog.page = page;
    g_prog.len = len; memcpy(g_prog.data, data, MIN(len, 64u));
    if (meta) memcpy(g_prog.meta, meta, 10); return 1;
}
uint32_t s5l8930_iop_nand_erase(DeviceState *d, int bus, uint32_t ce, uint32_t page) {
    (void)d; (void)bus; (void)ce; (void)page; return 1;
}
void s5l8930_cdma_kick(DeviceState *d) { (void)d; }
void s5l8930_cdma_sink_done(DeviceState *d, uint32_t base, uint32_t size) { (void)d; (void)base; (void)size; }
typedef struct H2FMIBus H2FMIBus;
static void h2fmi_transfer(H2FMIBus *b, int ce, bool queued);
static void h2fmi_write(void *opaque, hwaddr off, uint64_t val, unsigned size);
#include "slice.h"

struct DeviceState { int unused; };
#define RD(a)   h2fmi_read(bp, (a), 4)
#define WR(a,v) h2fmi_write(bp, (a), (v), 4)
static void load_page(H2FMIBus *b, uint32_t row) {
    h2fmi_write(b, FMC_BASE + FMC_CE, 0x1, 4);         /* CE0 */
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x3000, 4);     /* read: 0x00 then 0x30 */
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, row << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR1, 0, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0xb, 4);         /* cmd1 | addr | cmd2 */
}
int main(void) {
    DeviceState iop = {0};
    /* The model's buffers are multi-MB; keep the buses off the stack. */
    S5L8930H2FMIState *sp = calloc(1, sizeof *sp);
    H2FMIBus *bp = calloc(1, sizeof *bp);
    sp->iop = &iop;
    sp->fmc_off = 0x40000; sp->ecc_off = 0x80000;   /* the A4 layout */
    bp->s = sp; bp->n = 0;
    H2FMIBus *b = bp;                        /* FMC_BASE reads b->s->fmc_off */

    /* --- transfer-start rule --- */
    load_page(bp, 0);
    assert(bp->data_len == 0);
    WR(FMI_CONTROL, 3);                      /* enter read mode -> one transfer */
    assert(bp->data_len == g_page_bytes);
    WR(FMI_CONTROL, 3);                      /* still mode 3, no bit 7: NOT a new page */
    assert(bp->data_len == g_page_bytes);    /* no phantom transfer */
    WR(FMI_CONTROL, 3);                      /* and again */
    assert(bp->data_len == g_page_bytes);

    /* Pipelined next page: raise bit 7 -> a real second transfer. */
    load_page(bp, 1);
    WR(FMI_CONTROL, 0x83);
    assert(bp->data_len == 2 * g_page_bytes);

    /* --- O(1) FIFO reads: bytes come back in order, offset advancing --- */
    assert((RD(FMI_LEVEL) & 0x18));          /* data ready */
    assert(RD(FMI_DATA) == 0x03020100);      /* page bytes 0..3 */
    assert(RD(FMI_DATA) == 0x07060504);      /* next word, not the same one */
    assert(bp->data_len == 2 * g_page_bytes - 8 && bp->data_off == 8);

    /* --- write waits for BOTH data and metadata --- */
    S5L8930H2FMIState *wsp = calloc(1, sizeof *wsp);
    H2FMIBus *wp = calloc(1, sizeof *wp);
    wsp->iop = &iop; wsp->fmc_off = 0x40000; wsp->ecc_off = 0x80000; wp->s = wsp; wp->n = 0;
    h2fmi_write(wp, FMC_BASE + FMC_CE, 0x1, 4);
    h2fmi_write(wp, FMC_BASE + FMC_CMD, 0x80, 4);       /* page program */
    h2fmi_write(wp, FMC_BASE + FMC_GO, 0x9, 4);         /* cmd1 | addr */
    h2fmi_write(wp, FMI_CONTROL, 5, 4);                 /* write transfer */
    for (unsigned i = 0; i < g_page_bytes; i += 4) {    /* all the data... */
        h2fmi_write(wp, FMI_DATA, 0xa0a1a2a3, 4);
    }
    assert(!(wp->fmi[FMI_STATUS / 4] & (1u << 1)));      /* ...but no DONE without meta */
    assert(wp->wpage_len[0] == 0 && g_prog.called == 0);
    for (unsigned i = 0; i < 10; i += 2) {              /* now the 10 meta bytes */
        h2fmi_write(wp, FMI_META, 0xbbbb, 4);
    }
    assert(wp->fmi[FMI_STATUS / 4] & (1u << 1));         /* DONE once the page is whole */
    assert(wp->wpage_len[0] == g_page_bytes);
    h2fmi_write(wp, FMC_BASE + FMC_CMD, 0x10, 4);        /* program confirm */
    h2fmi_write(wp, FMC_BASE + FMC_GO, 0x1, 4);
    assert(g_prog.called == 1 && g_prog.len == g_page_bytes);
    assert(g_prog.data[0] == 0xa3 && g_prog.data[1] == 0xa2);

    /* --- READ ID, then a program: the status poll reads the status --- */
    S5L8930H2FMIState *isp = calloc(1, sizeof *isp);
    H2FMIBus *ip = calloc(1, sizeof *ip);
    isp->iop = &iop; isp->fmc_off = 0x400; isp->ecc_off = 0x800; ip->s = isp; ip->n = 1;   /* the S5L8920 layout */
    h2fmi_write(ip, isp->fmc_off + FMC_CE, 0x1, 4);
    h2fmi_write(ip, isp->fmc_off + FMC_CMD, 0x90, 4);         /* READ ID */
    h2fmi_write(ip, isp->fmc_off + FMC_GO, 0x9, 4);
    h2fmi_write(ip, isp->fmc_off + FMC_GO, 0x50, 4);          /* first ID byte */
    assert(ip->fmc[FMC_NAND_STATUS / 4] == (g_id & 0xff));
    h2fmi_write(ip, isp->fmc_off + FMC_CMD, 0x80, 4);         /* program, as above */
    h2fmi_write(ip, isp->fmc_off + FMC_GO, 0x9, 4);
    h2fmi_write(ip, FMI_CONTROL, 5, 4);
    for (unsigned i = 0; i < g_page_bytes; i += 4) {
        h2fmi_write(ip, FMI_DATA, 0xa0a1a2a3, 4);
    }
    for (unsigned i = 0; i < 10; i += 2) {
        h2fmi_write(ip, FMI_META, 0xbbbb, 4);
    }
    h2fmi_write(ip, isp->fmc_off + FMC_CMD, 0x1000, 4);       /* confirm as the second command */
    h2fmi_write(ip, isp->fmc_off + FMC_GO, 0x2, 4);
    h2fmi_write(ip, isp->fmc_off + FMC_CMD, 0x70, 4);         /* status */
    h2fmi_write(ip, isp->fmc_off + FMC_GO, 0x1, 4);
    h2fmi_write(ip, isp->fmc_off + FMC_GO, 0x50, 4);          /* the status byte */
    assert(ip->fmc[FMC_NAND_STATUS / 4] & 0x80);              /* not write-protected */

    /* N94: the PPN controller owns ECC, queues reads, and transfers complete 8 KiB pages + 32 metadata bytes. */
    S5L8930H2FMIState *ps = calloc(1, sizeof *ps);
    H2FMIBus *pp = calloc(1, sizeof *pp);
    ps->ppn = true; ps->iop = &iop; ps->fmc_off = 0x40000; ps->ecc_off = 0x80000; pp->s = ps;
    g_page_bytes = 8192;
    h2fmi_write(pp, 0x4000c, 1, 4);
    h2fmi_write(pp, FMI_FORMAT, 0x20840004, 4);
    h2fmi_write(pp, FMI_CONTROL, 0x1302, 4);
    for (unsigned block = 0x80f; block >= 0x80e; block--) {
        h2fmi_write(pp, 0x40020, 6, 4);
        h2fmi_write(pp, 0x40018, 0x2020, 4);
        h2fmi_write(pp, 0x4001c, 0x200000 | block << 8, 4);
        h2fmi_write(pp, 0x40014, 0x370a, 4);
        h2fmi_write(pp, 0x40010, 0xb, 4);
    }
    assert(pp->ppn_read_n[0] == 2);
    h2fmi_write(pp, 0x40014, 0x7a, 4);
    h2fmi_write(pp, 0x40010, 1, 4);
    assert(pp->page_row[0] == PPN_BOOT_BASE + 0x80f00 && pp->ppn_read_n[0] == 1);
    assert(pp->data_len == 0); /* NAND read command latches; sequencer frames perform the DMA. */
    h2fmi_write(pp, 0xc0000, 0x0b000000, 4);
    assert(pp->data_len == 4096 && pp->meta_len == 16 && pp->ecc_n == 0);
    h2fmi_write(pp, 0xc0000, 0x0b000000, 4);
    assert(pp->page_row[0] == PPN_BOOT_BASE + 0x80f00 && pp->ppn_read_n[0] == 1);
    assert(pp->data_len == 8192 && pp->meta_len == 32 && pp->ecc_n == 0);
    h2fmi_write(pp, 0xc0000, 0x0b000000, 4);
    h2fmi_write(pp, 0xc0000, 0x0b000000, 4);
    assert(pp->page_row[0] == PPN_BOOT_BASE + 0x80e00 && pp->ppn_read_n[0] == 0);
    assert(pp->data_len == 16384 && pp->meta_len == 64 && pp->ecc_n == 0);

    /* Address-list preflight must not become the first page's payload. */
    pp->wdata_len = 16;
    memset(pp->wdata, 0x99, 16);
    h2fmi_command(pp, 0xb0);
    h2fmi_command(pp, 0xb8);
    assert(pp->wdata_len == 0 && pp->wmeta_len == 0);

    /* A queued burst programs one physical page per confirm, including intermediate 0x87 pages. */
    pp->wdata_len = 16384; pp->wmeta_len = 64;
    memset(pp->wdata, 0xaa, 8192); memset(pp->wdata + 8192, 0xbb, 8192);
    memset(pp->wmeta, 0x20, 64);
    for (unsigned page = 0; page < 2; page++) {
        h2fmi_write(pp, 0x40020, 2, 4);
        h2fmi_write(pp, 0x40018, 0x280f00 + page, 4);
        h2fmi_write(pp, 0x40014, page ? 0x8a : 0x87, 4);
        h2fmi_write(pp, 0x40010, 9, 4);
        h2fmi_write(pp, 0x40014, 0x17, 4);
        h2fmi_write(pp, 0x40010, 1, 4);
        assert(g_prog.len == 8192 && g_prog.page == PPN_BOOT_BASE + 0x80f00 + page);
        assert(g_prog.data[0] == (page ? 0xbb : 0xaa));
    }
    assert(pp->wdata_len == 0 && pp->wmeta_len == 0);

    /* Sequencer operand FIFO and completion status are independent. */
    h2fmi_write(pp, 0xc1000, 0x10040018, 4);
    h2fmi_write(pp, 0xc1004, 0x0d000000, 4);
    h2fmi_write(pp, 0xc0014, 0xdeadbeef, 4);
    h2fmi_write(pp, 0xc0000, 0x11000002, 4);
    assert(pp->fmc[FMC_ADDR0 / 4] == 0xdeadbeef && pp->seq_operand_n == 0);
    assert(h2fmi_read(pp, 0xc0020, 4) == 1);
    h2fmi_write(pp, 0xc0020, 1, 4);
    assert(h2fmi_read(pp, 0xc0020, 4) == 0);
    h2fmi_write(pp, 0xc0000, 0x10040018, 4);
    assert(pp->seq_command_n == 1); /* waits for the operand, rather than consuming zero */
    h2fmi_write(pp, 0xc0014, 0x12345678, 4);
    assert(pp->seq_command_n == 0 && pp->fmc[FMC_ADDR0 / 4] == 0x12345678);
    h2fmi_write(pp, 0xc0000, 0x0f000034, 4);
    assert(pp->seq_command_n == 1); /* immediate arrives in the command FIFO */
    h2fmi_write(pp, 0xc0000, 0x1001, 4);
    assert(pp->fmi[FMI_FORMAT / 4] == 0x1001 && pp->seq_command_n == 0);
    /* Restoring PPN power features after hibernate uses PIO, not page DMA. */
    h2fmi_write(pp, 4, 6, 4);
    h2fmi_write(pp, 0x40014, 0xef, 4);
    h2fmi_write(pp, 0x40010, 1, 4);
    h2fmi_write(pp, 0x34, 0x401, 4);
    h2fmi_write(pp, 4, 5, 4);
    assert(h2fmi_read(pp, 0x1c, 4) & 0x18);
    assert(!(h2fmi_read(pp, 0xc, 4) & FMI_ST_DONE));
    h2fmi_write(pp, 0x14, 0x12345678, 4);
    assert(h2fmi_read(pp, 0xc, 4) & FMI_ST_DONE);
    assert(!(h2fmi_read(pp, 0x1c, 4) & 0x18));
    assert(pp->wdata_len == 4 && pp->wdata[0] == 0x78);
    h2fmi_command(pp, 0xe7);
    assert(pp->mode == MODE_NONE && pp->fmc[FMC_NAND_STATUS / 4] == 0x40);
    free(ps); free(pp);
    free(sp); free(bp); free(wsp); free(wp); free(isp); free(ip);
    puts("PASS: H2FMI transfer-start rule, O(1) FIFO reads in order, write waits for data + metadata, "
         "status after READ ID");
}
