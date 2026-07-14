/*

  usb_serial_ard.cpp - driver code for IMXRT1062 processor (on Teensy 4.0 board) : USB serial port wrapper

  Part of grblHAL

  Copyright (c) 2018-2025 Terje Io


  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.

*/

#include <string.h>

#include "Arduino.h"
#include "CrashReport.h"

#include "driver.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "grbl/protocol.h"
#ifdef __cplusplus
}
#endif

// Bridges Teensyduino's CrashReport (a CRC-validated fault record stashed in OCRAM by the core's
// default handler for any unimplemented interrupt/exception vector - what a genuine Cortex-M7
// HardFault/BusFault/UsageFault lands in on this build) onto hal.stream, so a real crash's fault
// registers/address surface in ioSender's console on the next boot instead of being invisible.
// No-ops silently when there's nothing to report.
class CrashReportStreamPrint : public Print {
public:
    size_t write (uint8_t c) override
    {
        char buf[2] = { (char)c, '\0' };
        hal.stream.write_all(buf);
        return 1;
    }
};

extern "C" void report_crash_if_any (void)
{
    if(CrashReport) {
        hal.stream.write_all("[MSG:--- Teensy CrashReport follows (a fault occurred before this boot) ---]" ASCII_EOL);
        CrashReportStreamPrint sp;
        CrashReport.printTo(sp);
        hal.stream.write_all(ASCII_EOL "[MSG:--- end CrashReport ---]" ASCII_EOL);
        CrashReport.clear();
    }
}

#if USB_SERIAL_CDC == 1

#ifdef __cplusplus
extern "C" {
#endif

#include "grbl/protocol.h"

#define BLOCK_RX_BUFFER_SIZE 20

DMAMEM static stream_block_tx_buffer_t txbuf;
DMAMEM static stream_rx_linebuffer_t rxbuf;
static on_execute_realtime_ptr on_execute_realtime;
static enqueue_realtime_command_ptr enqueue_realtime_command = protocol_enqueue_realtime_command;

static bool usb_isConnected (void)
{
    return sys.cold_start || SerialUSB;
}

//
// Returns number of characters in serial input buffer
//
static uint16_t usb_serialRxCount (void)
{
    return stream_rx_linebuffer_count(&rxbuf);
}

//
// Returns number of free characters in serial input buffer
//
static uint16_t usb_serialRxFree (void)
{
    return stream_rx_linebuffer_free(&rxbuf);
}

//
// Flushes the serial input buffer (including the USB buffer)
//
void usb_serialRxFlush (void)
{
    SerialUSB.flush();
    stream_rx_linebuffer_flush(&rxbuf);
}

//
// Flushes and adds a CAN character to the serial input buffer
//
static void usb_serialRxCancel (void)
{
    stream_rx_linebuffer_cancel(&rxbuf);
}

//
// Flushes the serial output buffer, discarding anything not yet handed to the USB hardware. Called
// from grbllib.c's reboot sequence (hal.stream.reset_write_buffer) - without this, a stale,
// partially-transmitted message left in txbuf across a reset could leak out merged with the
// reboot's own output with no separator, corrupting whatever followed.
static void usb_serialTxFlush (void)
{
    txbuf.length = 0;
    txbuf.s = txbuf.data;
}

//
// Writes current buffer to the USB output stream, swaps buffers
//
static inline bool _usb_write (void)
{
    size_t length, txfree;

    txbuf.s = txbuf.data;

    while(txbuf.length) {

        if((txfree = SerialUSB.availableForWrite()) > 10) {

            length = txfree < txbuf.length ? txfree : txbuf.length;

            SerialUSB.write((uint8_t *)txbuf.s, length); // doc is wrong - does not return bytes sent!

            txbuf.length -= length;
            txbuf.s += length;
        }

        if(txbuf.length && !hal.stream_blocking_callback()) {
            txbuf.length = 0;
            txbuf.s = txbuf.data;
            return false;
        }
    }

    txbuf.s = txbuf.data;

    return true;
}

//
// Writes a number of characters from string to the USB output stream, blocks if buffer full
//
static void usb_serialWrite (const uint8_t *s, uint16_t length)
{
    if(length == 0)
        return;

    if(txbuf.length && (txbuf.length + length) > txbuf.max_length) {
        if(!_usb_write())
            return;
    }

    while(length > txbuf.max_length) {
        txbuf.length = txbuf.max_length;
        memcpy(txbuf.s, s, txbuf.length);
        if(!_usb_write())
            return;
        length -= txbuf.max_length;
        s += txbuf.max_length;
    }

    if(length) {
        memcpy(txbuf.s, s, length);
        txbuf.length += length;
        txbuf.s += length;
        _usb_write();
    }
}

//
// Writes a null terminated string to the USB output stream, blocks if buffer full.
// Buffers locally up to 40 characters or until the string is terminated with a ASCII_LF character.
// NOTE: grbl always sends ASCII_LF terminated strings!
//
static void usb_serialWriteS (const char *s)
{
    if(*s == '\0')
        return;

    size_t length = strlen(s);

    if((length + txbuf.length) < BLOCK_TX_BUFFER_SIZE) {

        memcpy(txbuf.s, s, length);
        txbuf.length += length;
        txbuf.s += length;

        if(s[length - 1] == ASCII_LF || txbuf.length > txbuf.max_length) {
            if(!_usb_write())
                return;
        }
    } else
        usb_serialWrite((uint8_t *)s, (uint16_t)length);
}

//
// Writes a character to the serial output stream
//
static bool usb_serialPutC (const uint8_t c)
{
    if(txbuf.length) {
        char s[2];
        s[0] = c;
        s[1] = '\0';
        usb_serialWriteS(s);
    }
    else
        SerialUSB.write(c);

    return true;
}

//
// serialGetC - returns -1 if no data available
//
static int32_t usb_serialGetC (void)
{
    return stream_rx_linebuffer_get(&rxbuf);
}

static bool usb_serialSuspendInput (bool suspend)
{
    return stream_rx_linebuffer_suspend(&rxbuf, suspend);
}

static bool usb_serialEnqueueRtCommand (uint8_t c)
{
    return enqueue_realtime_command(c);
}

static enqueue_realtime_command_ptr usb_serialSetRtHandler (enqueue_realtime_command_ptr handler)
{
    enqueue_realtime_command_ptr prev = enqueue_realtime_command;

    if(handler)
        enqueue_realtime_command = handler;

    return prev;
}

//
// This function get called from the foregorund process,
// used here to get characters off the USB serial input stream and buffer
// them for processing by the core. Real time command characters are stripped out
// and submitted for realtime processing.
//
static void usb_execute_realtime (sys_state_t state)
{
    static volatile bool lock = false;
    static char tmpbuf[BLOCK_RX_BUFFER_SIZE];

    on_execute_realtime(state);

    if(lock)
        return;

    char c, *dp;
    int avail, free;

    lock = true;

    if((avail = SerialUSB.available())) {

        dp = tmpbuf;
        free = usb_serialRxFree();
        free = free > BLOCK_RX_BUFFER_SIZE ? BLOCK_RX_BUFFER_SIZE : free;

        avail = SerialUSB.readBytes(tmpbuf, avail > free ? free : avail);

        while(avail--) {
            c = *dp++;
            if(!enqueue_realtime_command(c)) {
                // USB has no per-byte retry path (unlike telnet, which can hold the byte in the TCP
                // receive window and redeliver it once there's room). A rejected terminator here means
                // the ring was full when this line tried to close - discard the unclosed line rather
                // than let it silently absorb every following byte forever (a permanent jam).
                if(!stream_rx_linebuffer_put(&rxbuf, c))
                    rxbuf.len[rxbuf.head] = 0;
            }
        }
    }

    lock = false;
}

FLASHMEM const io_stream_t *usb_serialInit (void)
{
    PROGMEM static const io_stream_t stream = {
        .type = StreamType_Serial,
        .instance = 0,
        .state = { .is_usb = On },
        .is_connected = usb_isConnected,
        .get_rx_buffer_free = usb_serialRxFree,
        .write = usb_serialWriteS,
        .write_all = NULL,
        .write_char = usb_serialPutC,
        .enqueue_rt_command = usb_serialEnqueueRtCommand,
        .read = usb_serialGetC,
        .reset_read_buffer = usb_serialRxFlush,
        .cancel_read_buffer = usb_serialRxCancel,
        .set_enqueue_rt_handler = usb_serialSetRtHandler,
        .suspend_read = usb_serialSuspendInput,
        .write_n = usb_serialWrite,
        .disable_rx = NULL,
        .get_rx_buffer_count = usb_serialRxCount,
        .get_tx_buffer_count = NULL,
        .reset_write_buffer = usb_serialTxFlush
    };


    memset(&rxbuf, 0, sizeof(stream_rx_linebuffer_t));
    memset(&txbuf, 0, sizeof(stream_block_tx_buffer_t));

    SerialUSB.begin(BAUD_RATE);

#if USB_SERIAL_WAIT
    while(!SerialUSB); // Wait for connection

    hal.stream.connected = true;
#endif

    txbuf.s = txbuf.data;
    txbuf.max_length = SerialUSB.availableForWrite(); // 6144 bytes
    txbuf.max_length = (txbuf.max_length > BLOCK_TX_BUFFER_SIZE ? BLOCK_TX_BUFFER_SIZE : txbuf.max_length) - 20;

    on_execute_realtime = grbl.on_execute_realtime;
    grbl.on_execute_realtime = usb_execute_realtime;

    return &stream;
}

int usb_serial_input (void)
{
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
