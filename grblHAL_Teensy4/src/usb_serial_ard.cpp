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

// Hang watchdog: a companion to CrashReport for the case CrashReport doesn't cover - a hardware
// WDOG1 timeout with NO CPU fault, i.e. a single g-code line/command dispatch (protocol.c's
// watchdog_begin/_end) never returned. Reuses CrashReport's own technique - a small struct at a
// fixed address in the top of OCRAM2 (.bss.dma/RAM is NOLOAD in imxrt1062_t41.ld, so this region
// is never zero-initialized by the C runtime and survives a WDOG reset untouched), guarded by a
// magic number + CRC exactly like CrashReport's arm_fault_info_struct. Placed at 0x2027FF00,
// immediately below CrashReport's own 128-byte block at 0x2027FF80, so the two never collide.
struct hang_wd_record_struct {
    uint32_t magic;
    uint32_t len;
    char line[112];
    uint32_t crc;
};

#define HANG_WD_MAGIC 0x574C4E48UL // "HNLW"

static struct hang_wd_record_struct * const hang_wd = (struct hang_wd_record_struct *)0x2027FF00;

static uint32_t hang_wd_crc32 (const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    while(len--) {
        crc ^= *p++;
        for(int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) * 0xEDB88320UL);
    }

    return crc;
}

// Called once per (re)init in grbllib.c, on every reset - idempotent, safe to call more than once:
// WDOG1_WCR's control fields latch write-once until the next real chip reset, so later calls just
// re-feed. WT is in 0.5s units; 19 -> 10s (WATCHDOG_HARDWARE_PERIOD_MS in protocol.c) - protocol.c's
// grace-period-exhausted log fires right as this feed is withheld, so it has ~10s to land over the
// wire before this hardware timeout actually resets the board. SRS/WDA are active-LOW "write 0 to
// trigger immediately" bits - written 1 (their deasserted default) here, never 0.
extern "C" void hang_watchdog_init (void)
{
    CCM_CCGR3 |= CCM_CCGR3_WDOG1(CCM_CCGR_ON);

    WDOG1_WMCR = 0;
    WDOG1_WCR = WDOG_WCR_WDZST | WDOG_WCR_WDBG | WDOG_WCR_SRS | WDOG_WCR_WDA | WDOG_WCR_WT(19) | WDOG_WCR_WDE;

    WDOG1_WSR = 0x5555;
    WDOG1_WSR = 0xAAAA;
}

extern "C" void hang_watchdog_feed (void)
{
    WDOG1_WSR = 0x5555;
    WDOG1_WSR = 0xAAAA;
}

// Records the line/command about to be dispatched, so if THIS dispatch is the one that hangs, the
// record read back on the next boot names it. Also feeds - re-arming covers the gap since the last
// feed, same as every other feed point.
extern "C" void hang_watchdog_arm (const char *line)
{
    hang_wd->magic = 0;
    arm_dcache_flush((void *)hang_wd, sizeof(*hang_wd));

    size_t n = strlen(line);
    if(n >= sizeof(hang_wd->line))
        n = sizeof(hang_wd->line) - 1;
    memcpy(hang_wd->line, line, n);
    hang_wd->line[n] = '\0';
    hang_wd->len = (uint32_t)n;
    hang_wd->crc = hang_wd_crc32(hang_wd->line, sizeof(hang_wd->line)) ^ hang_wd->len;

    arm_dcache_flush((void *)hang_wd, sizeof(*hang_wd));
    hang_wd->magic = HANG_WD_MAGIC;
    arm_dcache_flush((void *)hang_wd, sizeof(*hang_wd));

    hang_watchdog_feed();
}

// Plain (non-retained) RAM copy of the last hang-watchdog record, populated once at boot below.
// Needed because hang_wd itself (the OCRAM record) gets overwritten by the very next dispatch's
// hang_watchdog_arm() - including the `$I` command used to query this - so by the time build_info()
// (system.c) runs to print it, hang_wd would already hold "$I", not the line that caused the
// reset. One-shot: report_hang_watchdog_summary() clears last_hang_valid after the first $I/$I+
// that reports it, so the notice doesn't keep reappearing on every later query this session.
static bool last_hang_valid = false;
static char last_hang_line[112];

extern "C" void report_hang_watchdog_summary (void)
{
    if(last_hang_valid) {
        hal.stream.write_all("[MSG:Restart after controller hang processing: ");
        hal.stream.write_all(last_hang_line);
        hal.stream.write_all("]" ASCII_EOL);
        last_hang_valid = false;   // one-shot: reported once (the first $I/$I+ after the hang-reset), then done
    }
}

extern "C" void report_crash_if_any (void)
{
    // Own the WDOG-reset flag ourselves, read BEFORE CrashReport.printTo() below (which write-1-
    // clears all of SRC_SRSR as a side effect, but only runs when a CPU fault ALSO happened) gets a
    // chance to consume it - a benign WDT timeout with no fault (the case this feature exists for)
    // would otherwise never be reported at all, since CrashReport's own "caused by watchdog" line
    // only prints alongside a real fault record.
    if(SRC_SRSR & SRC_SRSR_WDOG_RST_B) {
        uint32_t crc = hang_wd_crc32(hang_wd->line, sizeof(hang_wd->line)) ^ hang_wd->len;
        if(hang_wd->magic == HANG_WD_MAGIC && hang_wd->crc == crc) {
            strncpy(last_hang_line, hang_wd->line, sizeof(last_hang_line) - 1);
            last_hang_line[sizeof(last_hang_line) - 1] = '\0';
            last_hang_valid = true;
            hal.stream.write_all("[MSG:Restart after controller hang processing: ");
            hal.stream.write_all(hang_wd->line);
            hal.stream.write_all("]" ASCII_EOL);
        } else
            hal.stream.write_all("[MSG:Restart after controller hang - no valid record of the failing line was found]" ASCII_EOL);
        hang_wd->magic = 0;
        arm_dcache_flush((void *)hang_wd, sizeof(*hang_wd));
        SRC_SRSR = SRC_SRSR_WDOG_RST_B; // write-1-to-clear, only this bit
    }

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
