#include "cdi_runtime.h"
#include "cdi_board_jobs.h"

#include <stdint.h>
#include <stdio.h>

static int failures;
static int irq_count;
static uint8_t last_irq_level;
static uint16_t ciap_words[8];
static unsigned ciap_position;
static uint8_t dma_ram[32];
static int dma_complete;

void cdi_irq_raise_onchip_level(uint8_t level) {
    irq_count++;
    last_irq_level = level;
}

uint16_t cdic_dma_pull_word(void) {
    return ciap_words[ciap_position++];
}

void cdic_dma_push_word(uint16_t value) {
    ciap_words[ciap_position++] = value;
}

void cdic_dma_complete(void) {
    dma_complete++;
}

uint16_t m68k_read16(uint32_t address) {
    unsigned offset = address - 0x1000u;
    return (uint16_t)(((uint16_t)dma_ram[offset] << 8) |
                      dma_ram[offset + 1u]);
}

void m68k_write16(uint32_t address, uint16_t value) {
    unsigned offset = address - 0x1000u;
    dma_ram[offset] = (uint8_t)(value >> 8);
    dma_ram[offset + 1u] = (uint8_t)value;
}

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

int main(void) {
    periph_reset();
    CHECK(periph_read(0x80002013u, 1) == 0x04u);

    periph_write(0x80001001u, 0xFFu, 1);
    CHECK(periph_lir() == 0x77u);

    periph_write(0x80002019u, 0x41u, 1);
    CHECK(periph_read(0x80002013u, 1) == 0x0Cu);
    CHECK(periph_read(0x80002019u, 1) == 0x41u);

    periph_write(0x80008000u, 0xAAu, 1);
    CHECK(periph_read(0x80008000u, 1) == 0);

    periph_write(0x80002022u, 0x1234u, 2);
    periph_write(0x80002024u, 0xFFFFu, 2);
    periph_write(0x80002045u, 5, 1);
    periph_increment_timer(95);
    CHECK(periph_read(0x80002024u, 2) == 0xFFFFu);
    periph_increment_timer(1);
    CHECK(periph_read(0x80002024u, 2) == 0x1234u);
    CHECK((periph_read(0x80002020u, 1) & 0x80u) != 0);
    CHECK(irq_count == 1 && last_irq_level == 5);

    periph_write(0x80002020u, 0x80u, 1);
    CHECK((periph_read(0x80002020u, 1) & 0x80u) == 0);

    ciap_words[0] = 0x1234u;
    ciap_words[1] = 0xABCDu;
    periph_write(0x80004000u, 0xFFu, 1);
    periph_write(0x80004007u, 0x80u, 1);
    periph_write(0x8000400Au, 2u, 2);
    periph_write(0x8000400Cu, 0x1004u, 4);
    periph_ciap_dma_request(0x6000u);
    CHECK(dma_ram[4] == 0x12u && dma_ram[5] == 0x34u);
    CHECK(dma_ram[6] == 0xABu && dma_ram[7] == 0xCDu);
    CHECK(periph_read(0x8000400Au, 2) == 0);
    CHECK(periph_read(0x8000400Cu, 4) == 0x1008u);
    CHECK(periph_read(0x80004000u, 1) == 0x80u);
    CHECK(periph_read(0x80004007u, 1) == 0);
    CHECK(dma_complete == 1);
    periph_write(0x80004000u, 0xFFu, 1);
    CHECK(periph_read(0x80004000u, 1) == 0);
    periph_write(0x80002020u, 0x80u, 1);
    CHECK((periph_read(0x80002020u, 1) & 0x80u) == 0);

    /* Compare quiet-prefix arithmetic with the actual original per-tick
     * state machine, including clear modes, equality and near-wrap tails. */
    static const unsigned quiet_cases[][5]={
        {0,0,0,0,0},{0x10,123,123,400,95},{0x20,123,120,400,17},
        {0x30,123,123,400,33},{1,123,400,123,71},{2,123,400,120,3},
        {3,123,400,123,95},{0x33,0xfff0,0xfff8,0xfff0,47}
    };
    for(unsigned test=0;test<sizeof quiet_cases/sizeof quiet_cases[0];test++) {
        uint32_t reference[6]={0};
        for(unsigned native=0;native<2;native++) {
            const unsigned *input=quiet_cases[test];periph_reset();
            periph_write(0x80002021u,0x33,1);
            periph_write(0x80002024u,0xffff,2);periph_write(0x80002026u,0xffff,2);periph_write(0x80002028u,0xffff,2);
            periph_increment_timer(96); /* independently create all status conditions */
            periph_write(0x80002021u,input[0],1);periph_write(0x80002024u,input[1],2);
            periph_write(0x80002026u,input[2],2);periph_write(0x80002028u,input[3],2);
            periph_increment_timer(input[4]);periph_write(0x80002045u,5,1);irq_count=0;last_irq_level=0;
            unsigned count=periph_quiet_quanta(64);
            if(native)periph_advance_quiet(count*25u);
            else for(unsigned i=0;i<count;i++)periph_increment_timer(25);
            /* Continued original quanta expose a wrong hidden prescaler. */
            for(unsigned i=0;i<137;i++)periph_increment_timer(25);
            uint32_t actual[]={periph_read(0x80002024u,2),periph_read(0x80002026u,2),
                periph_read(0x80002028u,2),periph_read(0x80002020u,1),(uint32_t)irq_count,last_irq_level};
            for(unsigned i=0;i<6;i++){if(!native)reference[i]=actual[i];else CHECK(actual[i]==reference[i]);}
        }
    }
    if (failures) {
        fprintf(stderr, "SCC68070 peripheral tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("SCC68070 peripheral tests: PASS");
    return 0;
}
