#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

#define GROUPS 8

#define LED_PERIOD_TICKS 2000
#define LED_ON_TICKS 200

/* HH:MM ocupa 23 columnas: 4 + 2 + 4 + 3 + 4 + 2 + 4 */
#define CLOCK_COL0 0
#define COL_H1 (CLOCK_COL0 + 0)
#define COL_H2 (CLOCK_COL0 + 6)
#define COL_COLON (CLOCK_COL0 + 9) /* pixel en col0+2 = 11 (centro del hueco) */
#define COL_M1 (CLOCK_COL0 + 13)
#define COL_M2 (CLOCK_COL0 + 19)

#define OE_DISABLE() (PORTC |= (1 << PC4))
#define OE_ENABLE() (PORTC &= ~(1 << PC4))

#define LED_ON() (PORTB &= ~(1 << PB2))
#define LED_OFF() (PORTB |= (1 << PB2))

static const uint8_t font[12][5] = {
    { 0x06, 0x09, 0x09, 0x09, 0x06 },
    { 0x04, 0x06, 0x04, 0x04, 0x04 },
    { 0x07, 0x08, 0x06, 0x01, 0x0F },
    { 0x0F, 0x08, 0x06, 0x08, 0x07 },
    { 0x09, 0x09, 0x0F, 0x08, 0x08 },
    { 0x0F, 0x01, 0x07, 0x08, 0x07 },
    { 0x06, 0x01, 0x07, 0x09, 0x06 },
    { 0x0F, 0x08, 0x04, 0x02, 0x02 },
    { 0x06, 0x09, 0x06, 0x09, 0x06 },
    { 0x06, 0x09, 0x0E, 0x08, 0x04 },
    { 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x04, 0x04, 0x00, 0x00 },
};

static volatile uint16_t grp[GROUPS];

static uint8_t group;
static uint16_t led_cnt;
static uint8_t hour = 12;
static uint8_t minute;
static uint8_t second;
static uint8_t shown_minute;
static uint8_t colon_shown;

static void columns_off(void)
{
    PORTC |= (1 << PC0) | (1 << PC1) | (1 << PC2) | (1 << PC3);
    PORTD |= (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7);
}

static void column_on(uint8_t g)
{
    switch (g) {
    case 0: PORTC &= ~(1 << PC0); break;
    case 1: PORTC &= ~(1 << PC1); break;
    case 2: PORTC &= ~(1 << PC2); break;
    case 3: PORTC &= ~(1 << PC3); break;
    case 4: PORTD &= ~(1 << PD4); break;
    case 5: PORTD &= ~(1 << PD5); break;
    case 6: PORTD &= ~(1 << PD6); break;
    case 7: PORTD &= ~(1 << PD7); break;
    }
}

static void set_pixel(uint8_t col, uint8_t row)
{
    if (col < 24 && row < 5)
        grp[col / 3] |= (uint16_t)1 << (row + 5 * (col % 3));
}

static void clear_pixel(uint8_t col, uint8_t row)
{
    if (col < 24 && row < 5)
        grp[col / 3] &= ~((uint16_t)1 << (row + 5 * (col % 3)));
}

static void clear_frame(void)
{
    for (uint8_t i = 0; i < GROUPS; i++)
        grp[i] = 0;
}

static void draw_glyph(uint8_t col0, uint8_t glyph)
{
    for (uint8_t r = 0; r < 5; r++) {
        uint8_t bits = font[glyph][r];
        for (uint8_t c = 0; c < 4; c++)
            if (bits & (1 << c))
                set_pixel(col0 + c, r);
    }
}

static void colon_pixels(uint8_t on)
{
    for (uint8_t r = 2; r <= 3; r++) {
        if (on)
            set_pixel(COL_COLON + 2, r);
        else
            clear_pixel(COL_COLON + 2, r);
    }
}

static void render_clock(void)
{
    clear_frame();
    draw_glyph(COL_H1, hour / 10);
    draw_glyph(COL_H2, hour % 10);
    draw_glyph(COL_M1, minute / 10);
    draw_glyph(COL_M2, minute % 10);
    colon_shown = (second & 1) == 0;
    colon_pixels(colon_shown);
}

static void clock_tick(void)
{
    if (++second < 60)
        return;
    second = 0;
    if (++minute < 60)
        return;
    minute = 0;
    hour = (hour + 1) % 24;
}

static void shift_word(uint16_t w)
{
    for (uint8_t i = 0; i < 16; i++) {
        if (w & 0x8000)
            PORTD |= (1 << PD3);
        else
            PORTD &= ~(1 << PD3);
        w <<= 1;
        PORTB |= (1 << PB5);
        PORTB &= ~(1 << PB5);
    }
}

ISR(TIMER1_COMPA_vect)
{
    clock_tick();
    if (minute != shown_minute) {
        render_clock();
        shown_minute = minute;
    } else {
        uint8_t want = (second & 1) == 0;
        if (want != colon_shown) {
            colon_shown = want;
            colon_pixels(want);
        }
    }
}

ISR(TIMER2_COMP_vect)
{
    columns_off();
    OE_DISABLE();

    if (++led_cnt >= LED_PERIOD_TICKS)
        led_cnt = 0;
    if (led_cnt < LED_ON_TICKS)
        LED_ON();
    else
        LED_OFF();

    shift_word(grp[group]);

    PORTB |= (1 << PB0);
    _delay_us(1);
    PORTB &= ~(1 << PB0);

    OE_ENABLE();
    column_on(group);

    group++;
    if (group == GROUPS)
        group = 0;
}

static void gpio_init(void)
{
    DDRC |= (1 << PC0) | (1 << PC1) | (1 << PC2) | (1 << PC3) | (1 << PC4);
    DDRD |= (1 << PD3) | (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7);
    DDRB |= (1 << PB0) | (1 << PB2) | (1 << PB5);

    PORTC |= (1 << PC0) | (1 << PC1) | (1 << PC2) | (1 << PC3) | (1 << PC4);
    PORTD |= (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7);
    PORTD &= ~(1 << PD3);
    PORTB &= ~((1 << PB0) | (1 << PB5));
    LED_OFF();
}

static void timers_init(void)
{
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS12);
    OCR1A = 31249;
    TIMSK |= (1 << OCIE1A);

    TCCR2 = (1 << WGM21) | (1 << CS21) | (1 << CS20); /* /32 */
    OCR2 = 124; /* 8 MHz / 32 / 125 = 2 kHz (250 fps de barrido) */
    TIMSK |= (1 << OCIE2);
}

int main(void)
{
    gpio_init();
    render_clock();
    shown_minute = minute;
    timers_init();
    sei();

    for (;;)
        ;

    return 0;
}
