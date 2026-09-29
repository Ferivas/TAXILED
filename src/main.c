#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

#define COLS 24
#define ROWS 5
#define GROUPS 8

#define VSWEEP_HOLD_FRAMES 4
#define HSWEEP_HOLD_FRAMES 30
#define PAUSE_FRAMES 60

#define LED_PERIOD_TICKS 500
#define LED_ON_TICKS 50

#define OE_DISABLE() (PORTC |= (1 << PC4))
#define OE_ENABLE() (PORTC &= ~(1 << PC4))

#define LED_ON() (PORTB &= ~(1 << PB2))
#define LED_OFF() (PORTB |= (1 << PB2))

static volatile uint16_t grp[GROUPS];

static uint8_t group;
static uint16_t tick;
static uint8_t anim_state;
static uint8_t anim_pos;
static uint16_t anim_hold;

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
    grp[col / 3] |= (uint16_t)1 << (row + 5 * (col % 3));
}

static void clear_frame(void)
{
    for (uint8_t i = 0; i < GROUPS; i++)
        grp[i] = 0;
}

static void build_vertical_line(uint8_t col)
{
    clear_frame();
    for (uint8_t r = 0; r < ROWS; r++)
        set_pixel(col, r);
}

static void build_horizontal_line(uint8_t row)
{
    clear_frame();
    for (uint8_t c = 0; c < COLS; c++)
        set_pixel(c, row);
}

static void animation_step(void)
{
    if (anim_hold > 0) {
        anim_hold--;
        return;
    }

    switch (anim_state) {
    case 0:
        if (anim_pos < COLS - 1) {
            anim_pos++;
            build_vertical_line(anim_pos);
            anim_hold = VSWEEP_HOLD_FRAMES - 1;
        } else {
            anim_state = 1;
            anim_pos = 0;
            anim_hold = PAUSE_FRAMES - 1;
            clear_frame();
        }
        break;
    case 1:
        anim_state = 2;
        build_horizontal_line(0);
        anim_hold = HSWEEP_HOLD_FRAMES - 1;
        break;
    case 2:
        if (anim_pos < ROWS - 1) {
            anim_pos++;
            build_horizontal_line(anim_pos);
            anim_hold = HSWEEP_HOLD_FRAMES - 1;
        } else {
            anim_state = 3;
            anim_pos = 0;
            anim_hold = PAUSE_FRAMES - 1;
            clear_frame();
        }
        break;
    default:
        anim_state = 0;
        build_vertical_line(0);
        anim_hold = VSWEEP_HOLD_FRAMES - 1;
        break;
    }
}

static void shift_word(uint16_t w)
{
    for (uint8_t i = 0; i < 16; i++) {
        if ((w >> (15 - i)) & 1)
            PORTD |= (1 << PD3);
        else
            PORTD &= ~(1 << PD3);
        PORTB |= (1 << PB5);
        PORTB &= ~(1 << PB5);
    }
}

ISR(TIMER1_COMPA_vect)
{
    columns_off();
    OE_DISABLE();

    if (tick % LED_PERIOD_TICKS < LED_ON_TICKS)
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
    if (group == GROUPS) {
        group = 0;
        animation_step();
    }
    tick++;
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

static void timer_init(void)
{
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS11);
    OCR1A = 1999;
    TIMSK |= (1 << OCIE1A);
}

int main(void)
{
    gpio_init();
    build_vertical_line(0);
    anim_hold = VSWEEP_HOLD_FRAMES - 1;
    timer_init();
    sei();

    for (;;)
        ;

    return 0;
}
