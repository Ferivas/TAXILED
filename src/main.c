#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <stdint.h>

#define GROUPS 8

#define LED_PERIOD_TICKS 2000
#define LED_ON_TICKS 200
#define DEBOUNCE_MS 150

/* Scroll de dígitos: desplazamiento vertical, 5 filas, una cada 200 ms,
   empezando 1 s antes del cambio. En cada paso todo el contenido sube una
   fila (la fila de arriba sale) y por abajo entra la siguiente fila del
   dígito nuevo (primero su fila 0). Timer1 corre a 200 ms (OCR1A=6249,
   prescaler 256) y el ISR divide entre TICKS_PER_SEC para el reloj de 1 Hz.
   La ventana tras k pasos es: fila j muestra stream[k+j], con
   stream[0..4] = glifo viejo y stream[5..9] = glifo nuevo. */
#define SCROLL_ROWS 5
#define TICKS_PER_SEC 5

/* HH:MM horizontal ocupa 23 columnas: 4 + 2 + 4 + 3 + 4 + 2 + 4 */
#define CLOCK_COL0 0
#define COL_H1 (CLOCK_COL0 + 0)
#define COL_H2 (CLOCK_COL0 + 6)
#define COL_COLON (CLOCK_COL0 + 9) /* pixel en col0+2 = 11 (centro del hueco) */
#define COL_M1 (CLOCK_COL0 + 13)
#define COL_M2 (CLOCK_COL0 + 19)

/* Vertical (leído de arriba a abajo con la pantalla rotada 90° CCW, las
   columnas altas arriba): H@19-23, h@13-17, dos puntos@11-12, M@6-10,
   m@0-4. Los dígitos de los minutos se pintan una fila más abajo
   (filas 1-4 = un píxel a la derecha de la vista vertical). */
#define VC_H1 19  /* hora, decenas */
#define VC_H2 13  /* hora, unidades */
#define VC_M1 6   /* minuto, decenas */
#define VC_M2 0   /* minuto, unidades */
#define VC_COL_A 11
#define VC_COL_B 12
#define VC_MROW 1

#define SETCLK_LEN 13

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
static uint8_t second = 50; /* arranque 12:00:50: primer cambio a los 10 s */
static uint8_t shown_minute;
static uint8_t colon_shown;
static uint8_t tick_div;

/* Scroll: glifos viejo/nuevo por posición (0=H dec, 1=H un, 2=M dec, 3=M un) */
static uint8_t anim_mask;
static uint8_t anim_k;
static uint8_t anim_old[4];
static uint8_t anim_new[4];

static volatile uint8_t orientation;
static volatile uint8_t eeprom_pending;
static volatile uint16_t ms_cnt;
static uint8_t ms_div;

static volatile uint8_t rx_active;
static volatile uint8_t rx_len;
static volatile uint8_t rx_buf[SETCLK_LEN];
static volatile uint8_t rx_ready;
static volatile uint8_t rx_overflow;

static uint8_t EEMEM ee_orientation;

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

/* Dibuja la fila `frow` del glifo en el hueco de fila `slot` del dígito
   (rot=1: 90° horario). frow y slot difieren mientras hay scroll. */
static void draw_glyph_slot(uint8_t col0, uint8_t row0, uint8_t glyph,
                            uint8_t frow, uint8_t slot, uint8_t rot)
{
    uint8_t bits = font[glyph][frow];

    for (uint8_t c = 0; c < 4; c++)
        if (bits & (1 << c))
            set_pixel(rot ? col0 + (4 - slot) : col0 + c,
                      row0 + (rot ? c : slot));
}

static void clear_glyph_slot(uint8_t col0, uint8_t row0, uint8_t glyph,
                             uint8_t frow, uint8_t slot, uint8_t rot)
{
    uint8_t bits = font[glyph][frow];

    for (uint8_t c = 0; c < 4; c++)
        if (bits & (1 << c))
            clear_pixel(rot ? col0 + (4 - slot) : col0 + c,
                        row0 + (rot ? c : slot));
}

static void slot_pos(uint8_t s, uint8_t *col0, uint8_t *row0)
{
    if (orientation) {
        *row0 = (s < 2) ? 0 : VC_MROW;
        *col0 = (s == 0) ? VC_H1 : (s == 1) ? VC_H2
                           : (s == 2) ? VC_M1 : VC_M2;
    } else {
        *row0 = 0;
        *col0 = (s == 0) ? COL_H1 : (s == 1) ? COL_H2
                           : (s == 2) ? COL_M1 : COL_M2;
    }
}

/* Giro 90 horario: (c,r) -> (col0 + 4 - r, row0 + c). Para giro antihorario
   usar set_pixel(col0 + r, row0 + 3 - c) en su lugar. */
static void draw_glyph_rot(uint8_t col0, uint8_t row0, uint8_t glyph)
{
    for (uint8_t r = 0; r < 5; r++) {
        uint8_t bits = font[glyph][r];
        for (uint8_t c = 0; c < 4; c++)
            if (bits & (1 << c))
                set_pixel(col0 + (4 - r), row0 + c);
    }
}

static void colon_pixels(uint8_t on)
{
    uint8_t c0, c1, r0, r1;

    if (orientation) {
        c0 = VC_COL_A;
        c1 = VC_COL_B;
        r0 = r1 = 2;
    } else {
        c0 = c1 = COL_COLON + 2;
        r0 = 2;
        r1 = 3;
    }

    if (on) {
        set_pixel(c0, r0);
        set_pixel(c1, r1);
    } else {
        clear_pixel(c0, r0);
        clear_pixel(c1, r1);
    }
}

static void render_clock(void)
{
    clear_frame();
    if (orientation) {
        draw_glyph_rot(VC_H1, 0, hour / 10);
        draw_glyph_rot(VC_H2, 0, hour % 10);
        draw_glyph_rot(VC_M1, VC_MROW, minute / 10);
        draw_glyph_rot(VC_M2, VC_MROW, minute % 10);
    } else {
        draw_glyph(COL_H1, hour / 10);
        draw_glyph(COL_H2, hour % 10);
        draw_glyph(COL_M1, minute / 10);
        draw_glyph(COL_M2, minute % 10);
    }
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

/* Prepara el scroll: compara cada dígito actual con el valor en +1 s. */
static void prepare_scroll(void)
{
    uint8_t nhour = hour;
    uint8_t nmin = minute + 1;

    if (nmin == 60) {
        nmin = 0;
        nhour = (hour + 1) % 24;
    }

    anim_old[0] = hour / 10;
    anim_old[1] = hour % 10;
    anim_old[2] = minute / 10;
    anim_old[3] = minute % 10;
    anim_new[0] = nhour / 10;
    anim_new[1] = nhour % 10;
    anim_new[2] = nmin / 10;
    anim_new[3] = nmin % 10;

    anim_mask = 0;
    for (uint8_t s = 0; s < 4; s++)
        if (anim_new[s] != anim_old[s])
            anim_mask |= (1 << s);
    anim_k = 0;
}

/* Un paso de scroll (anim_k ya incrementado, 1..5): cada hueco de fila j
   pasa de mostrar stream[k-1+j] a stream[k+j]; lo que se va se borra y lo
   que llega se dibuja. */
static void scroll_redraw(void)
{
    uint8_t k = anim_k;

    for (uint8_t s = 0; s < 4; s++) {
        uint8_t col0, row0;

        if (!(anim_mask & (1 << s)))
            continue;
        slot_pos(s, &col0, &row0);
        for (uint8_t j = 0; j < SCROLL_ROWS; j++) {
            uint8_t pi = (k - 1) + j;
            uint8_t ni = k + j;

            clear_glyph_slot(col0, row0,
                             pi < 5 ? anim_old[s] : anim_new[s],
                             pi < 5 ? pi : pi - 5, j, orientation);
            draw_glyph_slot(col0, row0,
                            ni < 5 ? anim_old[s] : anim_new[s],
                            ni < 5 ? ni : ni - 5, j, orientation);
        }
    }
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

static void uart_putc(uint8_t c)
{
    while (!(UCSRA & (1 << UDRE)))
        ;
    UDR = c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc((uint8_t)*s++);
}

static void setclk_apply(void)
{
    static const uint8_t tag[7] = "SETCLK,";
    uint8_t line[SETCLK_LEN];
    uint8_t len, i;
    uint8_t hh, mm, ss;

    cli();
    len = rx_len;
    for (i = 0; i < len; i++)
        line[i] = rx_buf[i];
    rx_ready = 0;
    sei();

    uart_puts("RX ");
    for (i = 0; i < len; i++)
        uart_putc(line[i] >= 32 && line[i] < 127 ? line[i] : '.');
    uart_puts("\r\n");

    if (len != SETCLK_LEN) {
        uart_puts("ERR LEN\r\n");
        return;
    }
    for (i = 0; i < 7; i++)
        if (line[i] != tag[i]) {
            uart_puts("ERR CMD\r\n");
            return;
        }
    for (i = 7; i < SETCLK_LEN; i++)
        if (line[i] < '0' || line[i] > '9') {
            uart_puts("ERR DIGIT\r\n");
            return;
        }
    hh = (line[7] - '0') * 10 + (line[8] - '0');
    mm = (line[9] - '0') * 10 + (line[10] - '0');
    ss = (line[11] - '0') * 10 + (line[12] - '0');
    if (hh > 23 || mm > 59 || ss > 59) {
        uart_puts("ERR RANGE\r\n");
        return;
    }

    cli();
    hour = hh;
    minute = mm;
    second = ss;
    anim_mask = 0;
    render_clock();
    shown_minute = minute;
    sei();

    uart_puts("OK\r\n");
}

ISR(TIMER1_COMPA_vect)
{
    if (++tick_div == TICKS_PER_SEC) {
        tick_div = 0;
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
        if (second == 59 && !anim_mask)
            prepare_scroll();
    }

    if (anim_mask && anim_k < SCROLL_ROWS) {
        anim_k++;
        scroll_redraw();
        if (anim_k == SCROLL_ROWS)
            anim_mask = 0;
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

    if (++ms_div == 2) {
        ms_div = 0;
        ms_cnt++;
    }

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

ISR(INT0_vect)
{
    static uint16_t last_press;
    uint16_t now = ms_cnt;

    if ((uint16_t)(now - last_press) < DEBOUNCE_MS)
        return;
    last_press = now;

    orientation ^= 1;
    anim_mask = 0;
    render_clock();
    shown_minute = minute;
    eeprom_pending = 1;
}

ISR(USART_RXC_vect)
{
    uint8_t c = UDR;

    if (rx_ready)
        return;
    if (!rx_active) {
        if (c == '$') {
            rx_len = 0;
            rx_active = 1;
        }
        return;
    }
    if (c == '\r' || c == '\n') {
        if (rx_len == 0)
            return;
        rx_active = 0;
        rx_ready = 1;
        return;
    }
    if (rx_len >= SETCLK_LEN) {
        rx_active = 0;
        rx_overflow = 1;
    } else {
        rx_buf[rx_len++] = c;
    }
}

static void gpio_init(void)
{
    DDRC |= (1 << PC0) | (1 << PC1) | (1 << PC2) | (1 << PC3) | (1 << PC4);
    DDRD |= (1 << PD3) | (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7);
    DDRB |= (1 << PB0) | (1 << PB2) | (1 << PB5);

    PORTC |= (1 << PC0) | (1 << PC1) | (1 << PC2) | (1 << PC3) | (1 << PC4);
    PORTD |= (1 << PD2); /* pull-up en INT0 (pulsador a GND) */
    PORTD |= (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7);
    PORTD &= ~(1 << PD3);
    PORTB &= ~((1 << PB0) | (1 << PB5));
    LED_OFF();
}

static void timers_init(void)
{
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS12);
    OCR1A = 6249; /* 200 ms: el ISR divide entre TICKS_PER_SEC (1 Hz) */
    TIMSK |= (1 << OCIE1A);

    TCCR2 = (1 << WGM21) | (1 << CS21) | (1 << CS20); /* /32 */
    OCR2 = 124; /* 8 MHz / 32 / 125 = 2 kHz (250 fps de barrido) */
    TIMSK |= (1 << OCIE2);
}

static void extint_init(void)
{
    MCUCR = (MCUCR & ~((1 << ISC01) | (1 << ISC00))) | (1 << ISC01);
    GICR |= (1 << INT0);
}

static void uart_init(void)
{
    PORTD |= (1 << PD0); /* pull-up en RX */
    DDRD |= (1 << PD1);  /* TX */
    UBRRH = 0;
    UBRRL = 51; /* 9600 bps con 8 MHz */
    UCSRB = (1 << RXEN) | (1 << TXEN) | (1 << RXCIE);
    UCSRC = (1 << URSEL) | (1 << UCSZ1) | (1 << UCSZ0); /* 8N1 */
}

int main(void)
{
    gpio_init();

    orientation = eeprom_read_byte(&ee_orientation);
    if (orientation > 1)
        orientation = 0;

    render_clock();
    shown_minute = minute;
    timers_init();
    extint_init();
    uart_init();
    sei();

    uart_puts("TAXILED 9600 READY\r\n");

    for (;;) {
        if (rx_overflow) {
            rx_overflow = 0;
            uart_puts("ERR LONG\r\n");
        }
        if (rx_ready)
            setclk_apply();
        if (eeprom_pending) {
            eeprom_pending = 0;
            eeprom_update_byte(&ee_orientation, orientation);
        }
    }

    return 0;
}
