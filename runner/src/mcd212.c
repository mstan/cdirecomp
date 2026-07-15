/*
 * Philips MCD212 display-controller register and scan engine.
 *
 * The model follows the CD-i Full Functional Specification display chapters:
 * two memory-backed image planes, initial/dynamic control areas, live VSR/DCP
 * pointers, vertical timing, and the two CPU-visible status registers.  Pixel
 * decoding and composition are delegated to mcd212_video.c.
 */
#include "cdi_runtime.h"
#include "debug_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MCD212_REGISTER_BASE = 0x004FFFE0u };

enum {
    REG_CSR2W = 0x00,
    REG_DCR2  = 0x02,
    REG_VSR2  = 0x04,
    REG_DDR2  = 0x08,
    REG_DCP2  = 0x0A,
    REG_CSR1W = 0x10,
    REG_DCR1  = 0x12,
    REG_VSR1  = 0x14,
    REG_DDR1  = 0x18,
    REG_DCP1  = 0x1A
};

enum {
    DCR_DISPLAY_ENABLE = 1u << 15,
    DCR_CLOCK_FORMAT = 1u << 14,
    DCR_FRAME_DURATION = 1u << 13,
    DCR_SCAN_MODE = 1u << 12,
    DCR_COLOR_MODE = 1u << 11,
    DCR_ICA_ENABLE = 1u << 9,
    STATUS_DISPLAY_ACTIVE = 0x80,
    STATUS_PARITY = 0x20
};

typedef struct {
    uint16_t write_register[32]; /* indexed by byte offset in the MMIO window */
    uint8_t display_status;
    uint8_t interrupt_status;
    double line_phase_ns;
    uint32_t raster_line;
    uint16_t image_line;
} Mcd212;

static Mcd212 vdsc;

static unsigned plane_dcr(int plane) { return plane ? REG_DCR2 : REG_DCR1; }
static unsigned plane_vsr(int plane) { return plane ? REG_VSR2 : REG_VSR1; }
static unsigned plane_ddr(int plane) { return plane ? REG_DDR2 : REG_DDR1; }
static unsigned plane_dcp(int plane) { return plane ? REG_DCP2 : REG_DCP1; }

static uint16_t reg(unsigned offset) {
    return vdsc.write_register[offset];
}

static void set_reg(unsigned offset, uint16_t value) {
    vdsc.write_register[offset] = value;
}

static int ntsc_duration(void) {
    return (reg(REG_DCR1) & DCR_FRAME_DURATION) != 0;
}

static int interlaced(void) {
    return (reg(REG_DCR1) & DCR_SCAN_MODE) != 0;
}

static unsigned total_raster_lines(void) {
    return ntsc_duration() ? 262u : 312u;
}

static unsigned blanking_lines(void) {
    if (ntsc_duration()) return 22u;
    return (reg(REG_CSR1W) & 0x0002u) ? 72u : 32u;
}

static double raster_line_period_ns(void) {
    return (reg(REG_DCR1) & DCR_CLOCK_FORMAT) ? 63560.0 : 64000.0;
}

static unsigned raster_width(void) {
    return !(reg(REG_DCR1) & DCR_CLOCK_FORMAT) &&
           !(reg(REG_CSR1W) & 0x0002u) ? 360u : 384u;
}

static unsigned raster_height(void) {
    if (ntsc_duration()) return 240u;
    return !(reg(REG_DCR1) & DCR_CLOCK_FORMAT) &&
           !(reg(REG_CSR1W) & 0x0002u) ? 240u : 280u;
}

static uint8_t read_dram_byte(uint32_t address) {
    extern uint8_t g_ram0[CDI_RAM0_SIZE];
    extern uint8_t g_ram1[CDI_RAM1_SIZE];
    if (address < CDI_RAM0_SIZE) return g_ram0[address];
    if (address >= CDI_RAM1_BASE && address - CDI_RAM1_BASE < CDI_RAM1_SIZE)
        return g_ram1[address - CDI_RAM1_BASE];
    fprintf(stderr, "[mcd212] control-program fetch outside DRAM @ $%08X\n",
            address);
    abort();
}

static uint32_t fetch_control_instruction(uint32_t address) {
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < 4; i++)
        value = (value << 8) | read_dram_byte(address + i);
    return value;
}

static uint32_t read_pointer(unsigned upper_register, unsigned lower_register) {
    return ((uint32_t)(reg(upper_register) & 0x003Fu) << 16) |
           reg(lower_register);
}

static uint32_t video_start(int plane) {
    return read_pointer(plane_dcr(plane), plane_vsr(plane));
}

static uint32_t dynamic_control_pointer(int plane) {
    return read_pointer(plane_ddr(plane), plane_dcp(plane));
}

static void set_video_start(int plane, uint32_t address) {
    unsigned control = plane_dcr(plane);
    uint16_t preserved = (uint16_t)(reg(control) & (plane ? 0x0B00u : 0xFB00u));
    set_reg(control, (uint16_t)(preserved | ((address >> 16) & 0x3Fu)));
    set_reg(plane_vsr(plane), (uint16_t)address);
}

static void set_dynamic_control_pointer(int plane, uint32_t address) {
    unsigned data_descriptor = plane_ddr(plane);
    set_reg(data_descriptor,
            (uint16_t)((reg(data_descriptor) & 0x0F00u) |
                       ((address >> 16) & 0x3Fu)));
    set_reg(plane_dcp(plane), (uint16_t)address);
}

static void signal_control_interrupt(int plane) {
    uint8_t flag = plane ? 0x02u : 0x04u;
    uint16_t control = reg(plane ? REG_CSR2W : REG_CSR1W);
    vdsc.interrupt_status &= plane ? 0x05u : 0x03u;
    vdsc.interrupt_status |= flag;
    if (!(control & 0x8000u)) cdi_irq_raise_onchip(1);
}

typedef enum {
    CONTROL_CONTINUE,
    CONTROL_STOP
} ControlResult;

static void apply_display_parameters(int plane, uint32_t instruction) {
    unsigned control = plane_dcr(plane);
    unsigned descriptor = plane_ddr(plane);
    uint16_t color_mode = (uint16_t)((instruction >> 4) & 1u);
    uint16_t mosaic_factor = (uint16_t)((instruction >> 2) & 3u);
    uint16_t file_type = (uint16_t)(instruction & 3u);
    uint16_t control_mask = plane ? 0x033Fu : 0xF33Fu;

    set_reg(control, (uint16_t)((reg(control) & control_mask) |
                                (color_mode << 11)));
    set_reg(descriptor, (uint16_t)((reg(descriptor) & 0x003Fu) |
                                   (mosaic_factor << 10) | (file_type << 8)));
}

static ControlResult execute_flow_command(int plane, uint32_t instruction,
                                          uint32_t *stream_address,
                                          int initial_area) {
    unsigned opcode = instruction >> 28;
    uint32_t aligned_pointer = instruction & 0x003FFFFCu;
    uint32_t byte_pointer = instruction & 0x003FFFFFu;

    if (opcode == 0) return CONTROL_STOP;
    if (opcode == 2 || opcode == 3)
        set_dynamic_control_pointer(plane, aligned_pointer);
    else if (opcode == 4) {
        if (initial_area) *stream_address = byte_pointer;
        else set_video_start(plane, byte_pointer);
    } else if (opcode == 5) {
        set_video_start(plane, byte_pointer);
    } else if (opcode == 6) {
        signal_control_interrupt(plane);
    } else if (opcode == 7 && (instruction >> 24) == 0x78u) {
        apply_display_parameters(plane, instruction);
    }

    return opcode == 3 || opcode == 5 ? CONTROL_STOP : CONTROL_CONTINUE;
}

static void run_initial_control_area(int plane) {
    unsigned words_per_blanking_line =
        (reg(REG_DCR1) & DCR_CLOCK_FORMAT) ? 120u : 112u;
    unsigned limit = words_per_blanking_line * blanking_lines();
    uint32_t cursor = interlaced() && !(vdsc.display_status & STATUS_PARITY)
                        ? 0x404u : 0x400u;
    unsigned i;

    if (plane) cursor += CDI_RAM1_BASE;
    for (i = 0; i < limit; i++) {
        uint32_t instruction = fetch_control_instruction(cursor);
        debug_trace_mcd_event(plane ? 2u : 0u, 0, instruction);
        cursor += 4;
        if (execute_flow_command(plane, instruction, &cursor, 1) == CONTROL_STOP)
            break;
        mcd212_video_control(plane, instruction);
    }
}

static void run_dynamic_control_area(int plane) {
    unsigned limit = (reg(REG_DCR1) & DCR_CLOCK_FORMAT) ? 16u : 8u;
    unsigned i;
    for (i = 0; i < limit; i++) {
        uint32_t cursor = dynamic_control_pointer(plane);
        uint32_t instruction;
        set_dynamic_control_pointer(plane, cursor + 4);
        instruction = fetch_control_instruction(cursor);
        debug_trace_mcd_event(plane ? 3u : 1u, vdsc.image_line, instruction);
        if (execute_flow_command(plane, instruction, &cursor, 0) == CONTROL_STOP)
            break;
        mcd212_video_control(plane, instruction);
    }
}

static void begin_active_line(void) {
    vdsc.display_status |= STATUS_DISPLAY_ACTIVE;
    if (!interlaced()) {
        vdsc.display_status |= STATUS_PARITY;
        return;
    }
    if (g_frame_count & 1u) {
        vdsc.display_status |= STATUS_PARITY;
    } else {
        vdsc.display_status &= (uint8_t)~STATUS_PARITY;
        if (vdsc.image_line == 0) vdsc.image_line = 1;
    }
}

static void draw_active_line(void) {
    unsigned width = raster_width();
    uint32_t start_a;
    uint32_t start_b;
    uint16_t consumed_a = 0;
    uint16_t consumed_b = 0;

    if (vdsc.image_line <= 1)
        mcd212_video_begin_frame((uint16_t)width, (uint16_t)raster_height(),
                                 interlaced());
    if (!(reg(REG_DCR1) & DCR_DISPLAY_ENABLE)) return;

    start_a = video_start(0);
    start_b = video_start(1);
    mcd212_video_draw_line(
        start_a, start_b, vdsc.image_line,
        (uint16_t)(width * ((reg(REG_DCR1) & DCR_COLOR_MODE) ? 2u : 1u)),
        (uint16_t)(width * ((reg(REG_DCR2) & DCR_COLOR_MODE) ? 2u : 1u)),
        &consumed_a, &consumed_b);
    set_video_start(0, start_a + consumed_a);
    set_video_start(1, start_b + consumed_b);

    if ((reg(REG_DCR1) & 0x0300u) == 0x0300u) run_dynamic_control_area(0);
    if ((reg(REG_DCR2) & 0x0300u) == 0x0300u) run_dynamic_control_area(1);
}

static void finish_frame(void) {
    vdsc.display_status &= (uint8_t)~STATUS_DISPLAY_ACTIVE;
    vdsc.raster_line = 0;
    vdsc.image_line = 0;
    mcd212_video_end_frame();
    g_frame_count++;
    cdi_input_schedule_advance(g_frame_count);
    debug_ring_capture_frame();
    if (g_stop_frame && g_frame_count >= g_stop_frame) cdi_fault_hold();
}

static void advance_one_raster_line(void) {
    vdsc.raster_line++;
    if (vdsc.raster_line <= blanking_lines()) {
        if (vdsc.raster_line == 1 && (reg(REG_DCR1) & DCR_DISPLAY_ENABLE)) {
            if (reg(REG_DCR1) & DCR_ICA_ENABLE) run_initial_control_area(0);
            if (reg(REG_DCR2) & DCR_ICA_ENABLE) run_initial_control_area(1);
        }
        return;
    }

    begin_active_line();
    draw_active_line();
    vdsc.image_line += interlaced() ? 2u : 1u;
    if (vdsc.raster_line >= total_raster_lines()) finish_frame();
}

void mcd212_tick(uint32_t cycles) {
    const double elapsed_ns = (double)cycles * (1.0e9 / 15104900.0);
    double line_period;

    /* Board devices share the same SCC68070 execution quantum. */
    periph_increment_timer(cycles);
    slave_increment_time(elapsed_ns);
    nvram_increment_clock(elapsed_ns);
    g_total_cycles += cycles;

    vdsc.line_phase_ns += elapsed_ns;
    line_period = raster_line_period_ns();
    while (vdsc.line_phase_ns >= line_period) {
        vdsc.line_phase_ns -= line_period;
        advance_one_raster_line();
        line_period = raster_line_period_ns();
    }
    runtime_pace_cycles(cycles);
}

uint32_t mcd212_read(uint32_t address, int size) {
    if (address == 0x004FFFF0u || address == 0x004FFFF1u)
        return vdsc.display_status;
    if (address == 0x004FFFE0u || address == 0x004FFFE1u) {
        uint8_t result = vdsc.interrupt_status;
        vdsc.interrupt_status = 0;
        return result;
    }
    fprintf(stderr,
            "[mcd212] read%d @ $%08X: write-only or unmapped register\n",
            size * 8, address);
    abort();
}

void mcd212_write(uint32_t address, uint32_t value, int size) {
    uint32_t offset = address - MCD212_REGISTER_BASE;
    uint16_t old;
    if (offset >= 32) {
        fprintf(stderr, "[mcd212] write%d @ $%08X = $%X outside registers\n",
                size * 8, address, value);
        abort();
    }
    if (size >= 2) {
        set_reg(offset, (uint16_t)value);
        return;
    }
    old = reg(offset);
    if (!(address & 1u))
        set_reg(offset, (uint16_t)((old & 0x00FFu) |
                                   ((uint16_t)(uint8_t)value << 8)));
    else
        set_reg(offset, (uint16_t)((old & 0xFF00u) | (uint8_t)value));
}

void mcd212_render_frame(uint32_t *framebuffer) {
    mcd212_video_copy_frame(framebuffer,
                            MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT,
                            NULL, NULL, NULL);
}

uint32_t mcd212_framebuffer_info(uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    return mcd212_video_copy_frame(NULL, 0, width, height, generation);
}

uint64_t mcd212_framebuffer_hash(uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    return mcd212_video_frame_hash(width, height, generation);
}

void mcd212_debug_state(uint16_t registers[32], uint8_t *display_status,
                        uint8_t *interrupt_status, uint32_t *raster_line,
                        uint16_t *active_line) {
    if (registers) memcpy(registers, vdsc.write_register,
                          sizeof vdsc.write_register);
    if (display_status) *display_status = vdsc.display_status;
    if (interrupt_status) *interrupt_status = vdsc.interrupt_status;
    if (raster_line) *raster_line = vdsc.raster_line;
    if (active_line) *active_line = vdsc.image_line;
}
