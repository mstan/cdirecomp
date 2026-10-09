#include "cdi_runtime.h"
#include "debug_server.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static uint8_t fake_sector[2340];
static int irq_raises;
static int irq_clears;
static int audio_decodes;

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
    audio_decodes++;
    return 0;
}
uint32_t cdi_audio_decode_groups(const uint8_t sound_groups[2304], uint8_t coding) {
    (void)sound_groups;
    audio_decodes++;
    return (coding & 1u) ? 2016u : 4032u;
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

    /* Selection changes cannot invent DATA before a sector arrives. A real
     * selected delivery carries its header, payload and ownership together. */
    (void)cdic_read(CDI_CDIC_BASE + 0x2586, 2);    /* clear ISR */
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2); /* ack data buffers */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0008, 2); /* ASEL mid-stream */
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) == 0);
    make_sector(15, 0x62);                         /* selected video */
    cdic_increment_time(14000000.0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) != 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);

    /* STOPD parks the decoder without flushing an owned data buffer. */
    uint16_t stopped_bman = cdic_read(CDI_CDIC_BASE + 0x2594, 2);
    selection_state(&selected, &drive_lba);
    uint32_t stopped_lba = drive_lba;
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0040, 2);
    cdic_increment_time(28000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba == stopped_lba);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == stopped_bman);
    cdic_transport_resume();
    cdic_increment_time(14000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba > stopped_lba);

    /* Q-buffer ownership does not impersonate trigger/EOF. Locator IRQs
     * obey IER bit 2; the ordinary record mask $060B keeps them masked. */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_write(CDI_CDIC_BASE + 0x2584, 0x0608, 2); /* isolate Q from header DATA */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0044, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0008, 2);
    cdic_write(CDI_CDIC_BASE + 0x2588, 0x8000, 2);
    make_sector(3, 0x62);
    cdic_increment_time(14000000.0);
    (void)cdic_read(CDI_CDIC_BASE + 0x2586, 2); /* real header notification */
    irq_raises = 0;
    cdic_increment_time(14000000.0);
    CHECK(irq_raises == 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0600) == 0);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x0030, 2);
    cdic_write(CDI_CDIC_BASE + 0x2584, 0x0004, 2);
    irq_raises = 0;
    cdic_increment_time(14000000.0);
    CHECK(irq_raises == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0004) != 0);
    cdic_increment_time(28000000.0); /* leave both locator reports unconsumed */
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0800) == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x1B24, 2) == 0x4100);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x24E6, 2) == 0x4100);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x1B2E, 2) == 0); /* Q CRC must not overwrite R-W */
    CHECK(cdic_read(CDI_CDIC_BASE + 0x24F0, 2) == 0);

    /* A file-wide EOF on an unselected channel interrupts without filling
     * a host payload buffer. It must not be lost behind channel filtering. */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_write(CDI_CDIC_BASE + 0x2584, 0x0400, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x00C4, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0008, 2);
    /* The first real header confirms selection to the firmware discard
     * phase, even when a boundary sector belongs to another file. */
    make_sector(0, 0x89);
    fake_sector[4] = fake_sector[8] = 0;
    fake_sector[12] = 0xA5;
    cdic_increment_time(14000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0x0001);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x120C, 1) == 0);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);
    cdic_increment_time(14000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) == 0);
    make_sector(3, 0xE1);
    irq_raises = 0;
    cdic_increment_time(14000000.0);
    CHECK(irq_raises > 0); /* asserting an already-high IRQ is idempotent */
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0400) != 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) == 0);

    /* Scene triggers on an unselected channel signal the selected file.
     * They do not hand a payload to the host or impersonate PCL refill. */
    cdic_write(CDI_CDIC_BASE + 0x2584, 0x0200, 2);
    make_sector(0, 0x70);
    irq_raises = 0;
    cdic_increment_time(14000000.0);
    CHECK(irq_raises == 1);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0x0200);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) == 0);
    fake_sector[4] = fake_sector[8] = 2; /* different file */
    irq_raises = 0;
    cdic_increment_time(14000000.0);
    CHECK(irq_raises == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0);
    make_sector(0, 0x70);
    fake_sector[10] = 0x60; /* inconsistent duplicate subheader */
    cdic_increment_time(14000000.0);
    CHECK(irq_raises == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0);

    /* An unselected regular sector still advances host position through its
     * real header. Its payload cannot leak into a caller's DMA destination.
     * Re-selection alone must not announce stale data. */
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2); /* ack data buffers */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0008, 2); /* ASEL mid-stream */
    make_sector(3, 0x62);                          /* unselected channel */
    fake_sector[0] = 0x01; /* physical MSF header */
    fake_sector[12] = 0xA5;
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) == 0);
    cdic_increment_time(14000000.0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) != 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);
    unsigned header_buffer = (cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 4u)
                               ? 0x1200u : 0x1BC2u;
    CHECK(cdic_read(CDI_CDIC_BASE + header_buffer, 1) == 0x01);
    CHECK(cdic_read(CDI_CDIC_BASE + header_buffer + 5u, 1) == 3);
    CHECK(cdic_read(CDI_CDIC_BASE + header_buffer + 12u, 1) == 0);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);

    /* Every regular header can advance position, including with locators
     * enabled. Unselected record ends must never terminate another channel.
     * The next selected EOR remains an ordinary complete sector. */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0044, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0008, 2);
    make_sector(3, 0x62);
    cdic_increment_time(14000000.0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) != 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);
    cdic_increment_time(14000000.0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) != 0);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);
    make_sector(3, 0x65); /* EOR of an unselected audio channel */
    cdic_increment_time(14000000.0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0601) == 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) == 0);
    make_sector(15, 0x65); /* selected audio end-of-record */
    cdic_increment_time(14000000.0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 0x0001) != 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);

    /* AUDIO submode alone must not swallow host records. With TACS clear,
     * normal audio payloads arrive in DATA buffers; with its channel bit
     * set, decoding is direct but the header still advances host position.
     * The directly decoded payload is not delivered to host RAM.
     * Channel 16 cannot alias bit 0. */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x00C4, 2);
    cdic_write(CDI_CDIC_BASE + 0x2588, 0x0000, 2);
    audio_decodes = 0;
    make_sector(14, 0x64);
    fake_sector[12] = 0xA5;
    cdic_increment_time(14000000.0);
    CHECK(audio_decodes == 0);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x120C, 1) == 0xA5);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);
    cdic_write(CDI_CDIC_BASE + 0x2588, 0x4000, 2);
    cdic_increment_time(14000000.0);
    CHECK(audio_decodes == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x1BC6, 1) == 1); /* file */
    CHECK(cdic_read(CDI_CDIC_BASE + 0x1BCE, 1) == 0); /* no ADPCM payload */
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);
    cdic_write(CDI_CDIC_BASE + 0x2588, 0x0001, 2);
    make_sector(16, 0x64);
    cdic_increment_time(14000000.0);
    CHECK(audio_decodes == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);

    /* Backpressure cannot overwrite owned bytes or stop the physical head.
     * After release, delivery resumes with the current sector, not a replay. */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x00C4, 2);
    make_sector(15, 0x62);
    fake_sector[12] = 0xA1;
    cdic_increment_time(14000000.0);
    fake_sector[12] = 0xB2;
    cdic_increment_time(14000000.0);
    (void)cdic_read(CDI_CDIC_BASE + 0x2586, 2);
    selection_state(&selected, &drive_lba);
    uint32_t full_lba = drive_lba;
    fake_sector[12] = 0xC3;
    cdic_increment_time(28000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba > full_lba);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x120C, 1) == 0xA1);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x1BCE, 1) == 0xB2);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2586, 2) & 1) == 0);
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x000C, 2);
    cdic_increment_time(14000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x120C, 1) == 0xC3);

    /* A transport pause retains an armed decoder, but a decoder RESET is
     * final until another CCR start. A deferred C4 reply after completion
     * teardown must not refill the just-flushed buffers. Firmware idle $9
     * belongs to core startup, not to every completed stream. */
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x003C, 2);
    cdic_transport_pause();
    selection_state(&selected, &drive_lba);
    uint32_t paused_lba = drive_lba;
    cdic_increment_time(14000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba == paused_lba);
    cdic_transport_resume();
    cdic_increment_time(14000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba > paused_lba);

    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    selection_state(&selected, &drive_lba);
    uint32_t reset_lba = drive_lba;
    cdic_transport_resume();
    cdic_increment_time(28000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba == reset_lba);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x00C4, 2);
    make_sector(15, 0x62);
    cdic_increment_time(14000000.0);
    selection_state(&selected, &drive_lba);
    CHECK(drive_lba > reset_lba);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x2594, 2) & 0x000C) != 0);

    /* PLAY0 consumes two owned buffers at their sample duration, then
     * INTDONE completes after the last buffer. It never produces the
     * setup/ack storm that used to restart the ROM's PCL repeatedly. */
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_write(CDI_CDIC_BASE + 0x2584, 0x000A, 2);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0006, 2);
    cdic_write(CDI_CDIC_BASE + 0x259A, 0x0800, 2); /* stereo 37.8 kHz */
    cdic_write(CDI_CDIC_BASE + 0x2594, 0x0003, 2);
    audio_decodes = 0;
    irq_raises = 0;
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0140, 2);
    CHECK(audio_decodes == 1);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0020, 2);
    cdic_increment_time(25000.0);
    CHECK(irq_raises == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 3);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x00C0) == 0x0040);
    cdic_increment_time(54000000.0);
    CHECK(audio_decodes == 2);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 2);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0x0002);
    cdic_increment_time(54000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 0x0002);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x00C0) == 0);
    cdic_increment_time(25000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 8);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) != 0);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0200, 2);

    /* Record-engine RESET/seek must not cancel an independent memory sound.
     * SS_Sound stop sleeps until the next consumed-buffer/PCL notification;
     * cancelling that consumer strands the guest and overruns its CILs. */
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0006, 2);
    (void)cdic_read(CDI_CDIC_BASE + 0x2586, 2);
    cdic_write(CDI_CDIC_BASE + 0x259A, 0x0800, 2);
    cdic_write(CDI_CDIC_BASE + 0x2594, 1, 2);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0140, 2);
    cdic_increment_time(20000000.0);
    uint16_t audio_pointer = cdic_read(CDI_CDIC_BASE + 0x25B4, 2);
    CHECK(audio_pointer > 0 && audio_pointer < 0x480);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 1);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0040) != 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x25B4, 2) == audio_pointer);
    cdic_increment_time(34000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 2);

    /* Plain PLAY0 ($40) is the live SS_Sound replacement path, not a no-op.
     * The ROM uses the AP pointer to hand off the other buffer. Neither a
     * decoder RESET nor a second PLAY0 may replay an already active buffer. */
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0040, 2);
    cdic_write(CDI_CDIC_BASE + 0x2594, 2, 2);
    cdic_increment_time(20000000.0);
    audio_pointer = cdic_read(CDI_CDIC_BASE + 0x25B4, 2);
    CHECK(audio_pointer > 0x480 && audio_pointer < 0x900);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0040, 2);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x25B4, 2) == audio_pointer);
    cdic_increment_time(34000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 2);

    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x00A0, 2);
    cdic_write(CDI_CDIC_BASE + 0x2596, 0x0100, 2);
    cdic_increment_time(25000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2586, 2) == 8);
    CHECK((cdic_read(CDI_CDIC_BASE + 0x25AA, 2) & 0x0080) != 0);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0200, 2);

    /* A delayed refill resumes the next buffer without replaying the old
     * one. Its coding is captured at handoff, independent of A_SHDW later. */
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0006, 2);
    cdic_write(CDI_CDIC_BASE + 0x259A, 0x0800, 2);
    cdic_write(CDI_CDIC_BASE + 0x2594, 1, 2);
    cdic_write(CDI_CDIC_BASE + 0x25A6, 0x0140, 2);
    cdic_increment_time(54000000.0);
    (void)cdic_read(CDI_CDIC_BASE + 0x2586, 2);
    cdic_write(CDI_CDIC_BASE + 0x259A, 0, 2); /* mono lasts twice as long */
    cdic_write(CDI_CDIC_BASE + 0x2594, 2, 2);
    cdic_write(CDI_CDIC_BASE + 0x259A, 0x0800, 2);
    cdic_increment_time(54000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 2);
    cdic_increment_time(54000000.0);
    CHECK(cdic_read(CDI_CDIC_BASE + 0x2594, 2) == 0);

    if (failures) return 1;
    puts("CIAP channel-selection and AP command tests passed");
    return 0;
}
