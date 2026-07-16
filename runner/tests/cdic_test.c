#include "cdi_runtime.h"
#include "debug_server.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static uint8_t fake_sector[2340];
static int irq_raises;
static int irq_clears;

M68KState g_cpu;
uint64_t g_total_cycles;
uint64_t g_frame_count;
int g_hold_on_fault;

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
        failures++; \
    } \
} while (0)

uint64_t debug_trace_sequence(void) { return 0; }
void debug_dump_fault_trail(const char *reason) { (void)reason; }
void cdi_fault_hold(void) {}
void cdi_irq_raise_vector(uint8_t level, uint8_t vector) {
    (void)level;
    (void)vector;
    irq_raises++;
}
void cdi_irq_clear(uint8_t level) {
    (void)level;
    irq_clears++;
}
void periph_ciap_dma_request(uint16_t control) { (void)control; }
void cdi_audio_reset(void) {}
int cdi_audio_decode_sector(const uint8_t sector[2340]) {
    (void)sector;
    return 0;
}
int cdi_media_present(void) { return 1; }
int cdi_media_read_sector_body(uint32_t lba, uint8_t dst[2340]) {
    (void)lba;
    memcpy(dst, fake_sector, sizeof fake_sector);
    return 1;
}

static void make_sector(uint8_t channel, uint8_t submode) {
    memset(fake_sector, 0, sizeof fake_sector);
    fake_sector[3] = 2;
    fake_sector[4] = fake_sector[8] = 1;
    fake_sector[5] = fake_sector[9] = channel;
    fake_sector[6] = fake_sector[10] = submode;
    fake_sector[7] = fake_sector[11] = 4;
}

static void selection_state(int *selected, uint32_t *drive_lba) {
    uint32_t last_lba;
    uint8_t file, channel, submode, coding;
    int running, waiting_ack;
    cdic_debug_state(drive_lba, &last_lba, &file, &channel, &submode,
                     &coding, selected, &running, &waiting_ack);
}

int main(void) {
    int selected;
    uint32_t drive_lba;

    cdic_set_drive_position(100, 0);
    cdic_write(CDI_CDIC_BASE + 0x258C, 0x8000, 2); /* channel 15 */
    cdic_write(CDI_CDIC_BASE + 0x2590, 0x0000, 2);
    cdic_write(CDI_CDIC_BASE + 0x2592, 0x0001, 2); /* file 1 */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0008, 2); /* select */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x00C4, 2); /* start data */

    /* EOF/EOR/trigger bits do not override file/channel selection. */
    make_sector(0, 0xF1);
    cdic_increment_time(14000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(selected == 0);
    CHECK(drive_lba == 101);

    make_sector(15, 0xF1);
    cdic_increment_time(14000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(selected == 1);
    CHECK(drive_lba == 102);

    /* AP setup must not complete synchronously.  CD-RTOS publishes the new
     * driver state after this write and requests completion separately. */
    cdic_write(CDI_CDIC_BASE + 0x25C0, 0x0553, 2); /* level 3, vector $AA */
    cdic_write(CDI_CDIC_BASE + 0x2584, 0x0008, 2); /* enable AP interrupt */
    irq_raises = 0;
    irq_clears = 0;
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0142, 2); /* AP setup */
    CHECK(irq_raises == 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) == 0);

    /* INTNOW/FINISH complete ASYNCHRONOUSLY: the AP executes the command
     * and interrupts later. CD-RTOS arms its notify flag right after the
     * APCR write ($428836/$42883A); a synchronous interrupt preempts that
     * arm and strands the client wake. */
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x00A0, 2); /* INTNOW */
    CHECK(irq_raises == 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) == 0);
    cdic_increment_time(25000.0); /* > the 20 us AP command latency */
    CHECK(irq_raises == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) != 0);

    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0200, 2); /* acknowledge */
    CHECK(irq_clears == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) == 0);

    /* Bit-8 (interrupt-enable) transport commands complete asynchronously
     * too: the CD driver writes $142 at SS_Play submit ($428674) and parks
     * in status 8 until ISR bit 3 arrives; a $142 that never completes
     * gates the PCL processor off forever. */
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0142, 2); /* play-continue + IE */
    CHECK(irq_raises == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) == 0);
    cdic_increment_time(25000.0);
    CHECK(irq_raises == 2);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) != 0);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0200, 2); /* acknowledge */
    CHECK(irq_clears == 2);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) == 0);

    if (failures) return 1;
    puts("CIAP channel-selection and AP command tests passed");
    return 0;
}
