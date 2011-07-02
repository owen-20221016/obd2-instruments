/* serial.c: UART queue support for small controllers.
 * Originally written for the Vvvvroom motor controller.
 */

#if defined(STM32)
#include <armduino.h>

#define F_CPU 24*1000*1000		/* 24MHz */
#define BAUD 9600
#define cli()
#define sei()

#define CONCAT1(n,m) USART ## n ## _ ## m
#define UCONCAT2(n,m) CONCAT1(n,m)
#define USARTNUM 2
#define USART_DR UCONCAT2(USARTNUM, DR)
#define USART_SR UCONCAT2(USARTNUM, SR)
#define USART_CR1 UCONCAT2(USARTNUM, CR1)
#define USART_CR2 UCONCAT2(USARTNUM, CR2)
#define USART_CR3 UCONCAT2(USARTNUM, CR3)
#define USART_BRR UCONCAT2(USARTNUM, BRR)
#define USART_Intr UCONCAT2(USARTNUM, Intr)

#else
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
/* Using setbaud.h requires us to pre-define our speed. */
#if ! defined(BAUD)
#define BAUD 9600
#endif
#define F_CPU 16000000		/* 16MHz */
#include <util/setbaud.h>

#if defined(IOM8)
#include <avr/iom8.h>
#elif defined(__AVR_ATmega168__)
#define MEGA168
#include <avr/iom168.h>
#elif defined(__AVR_ATmega1280__)
#define MEGA1280
#include <avr/iom1280.h>
#elif
#error No target AVR controller defined
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* Set the size of the receive and transmit buffers.
 * The transmit buffer should be able to buffer a whole line, but can
 * smaller at the cost of busy-waiting.
 * The receive buffer only needs to handle a simple command.
 * We have little RAM on an ATMega, more is not better.
 */
#define UART_RXBUF_SIZE 16
#define UART_TXBUF_SIZE 128

/* Hardware constants for the serial port settings.
 * See setup_uart() below where they are used. */
#define PARITY_NONE	0x00
#define PARITY_EVEN	0x20
#define PARITY_ODD	0x30

#define BITS_7_1	0x04
#define BITS_7_2	0x0C
#define BITS_8_1	0x06
#define BITS_8_2	0x0E

#if defined(IOM8)
/* The IOM8 has only a single UART, thus the symbolic names omit the UART
 * index.
 */
#define SIG_USART0_RECV SIG_USART_RECV
#define SIG_USART0_DATA SIG_USART_DATA
#define UCSR0B UCSRB
#define UDR0 UDR
#define UBRR0L UBRRL
#define UBRR0H UBRRH
#elif defined(MEGA168)
#define SIG_UART_RECV SIG_USART_RECV
#define SIG_UART_DATA SIG_USART_DATA
#define UCSRB UCSR0B
#define RXEN RXEN0
#define TXEN TXEN0
#define RXCIE RXCIE0
#define UDRIE UDRIE0
#define UBRRL UBRR0L
#define UBRRH UBRR0H
#elif defined(_AVR_IOMXX0_1_H_)
/* It's silly to repeat the register bit definitions for each channel. */
#define RXCIE RXCIE0
#define UDRIE UDRIE0
#define RXEN RXEN0
#define TXEN TXEN0
#endif

/* The queue state structure for a single UART. */
typedef struct {
	unsigned char rxbuf[UART_RXBUF_SIZE];
	unsigned char txbuf[UART_TXBUF_SIZE];
	volatile unsigned rxhead;
	volatile unsigned rxtail;
	volatile unsigned txhead;
	volatile unsigned txtail;
} uart_fifo_type;

uart_fifo_type uart0;

/* The output buffer for uart_putstr(). */
char uart_str[80];
/* Public statistics for the serial port - interrupt counts. */
volatile unsigned long serial_txbytes = 0;
volatile unsigned long serial_rxbytes = 0;

/* UART3 interrupt. */
ISR(USART2)
{
	unsigned status;
	unsigned char c;
	unsigned i;

	status = USART_SR;
	if (status & USART_RXNE) {
		c = USART_DR;
		i = uart0.rxhead + 1;
		if (i >= UART_RXBUF_SIZE)
			i = 0;
		if (i != uart0.rxtail) {		/* Check that the queue is not full. */
			uart0.rxbuf[uart0.rxhead] = c;
			uart0.rxhead = i;
		}
		serial_rxbytes++;
	}
	if (status & USART_TXE) {
		i = uart0.txtail;
		if (i != uart0.txhead) {
			USART_DR = uart0.txbuf[i++];
			if (i >= UART_TXBUF_SIZE) i = 0;
			uart0.txtail = i;
		} else {
			/* Leave only the Rx interrupt enabled. */
			USART_CR1 &= ~USART_TXEIE;
		}
		serial_txbytes++;
	}
}

/* Get the next character from the UART input FIFO.
 * Return -1 if the FIFO is empty.
 */
int uart_getch(void)
{
	unsigned char c;
	unsigned i, j;

	i = uart0.rxtail;
	j = uart0.rxhead;			/* Must be atomic. */
	if (i != j) {
		c = uart0.rxbuf[i++];
		if (i >= UART_RXBUF_SIZE) i = 0;
		uart0.rxtail = i;		/* Must be atomic. */
		return c;
	}
	return -1;
}

/*
 * Put character C on the UART transmit queue.
 * Return 0 on success, -1 if the queue is full.
 */
char uart_putch(char c)
{
	unsigned i, j;

	i = uart0.txhead + 1;
	if (i >= UART_TXBUF_SIZE) i = 0;
	j = uart0.txtail;			/* Must be atomic. */
	if (i == j)					/* Queue full, report failure. */
		return -1;
	uart0.txbuf[uart0.txhead] = c;
	uart0.txhead = i;			/* Must be atomic. */
	/* Enable TX buffer empty interrupt. */
	USART_CR1 |= USART_TXEIE;
	return 0;
}

/* Configure the USART registers.
 * We set the UART to 9600,N,8,1 to match the Arduino, with the baud
 * rate set in the define above or overridden in the compile environment.
 * Note that the pin mapping and direction have already been set up for us.
 */
void setup_uart(void)
{
	/* Re-initialize counts when called. */
	serial_txbytes = serial_rxbytes = 0;
	uart0.rxhead = uart0.txhead = uart0.rxtail = uart0.txtail = 0;

	/* Baud rate: Use the value from RM0041 table 122,
	   9600 8MHz 52.0625 */
#if F_CPU / BAUD > 0xFFFF
#error "Baud rate out of range"
#endif
	USART_BRR = F_CPU / BAUD;

	USART_SR = 0;
	USART_CR2 = 0;
	USART_CR3 = 0x000;

	/* Enable the USART and receive interrupts.
	 * Nothing will happen unless interrupts are globally enabled.
	 */
	INTR_SETENA(USART_Intr);

	USART_CR1 = USART_UE | USART_TE | USART_RE | USART_RXNEIE;

	return;
}

/*
 * Local variables:
 *  compile-command: "make serial.o"
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */