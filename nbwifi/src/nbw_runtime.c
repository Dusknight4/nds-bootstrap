// nbwifi runtime: delays, the log, and the packet buffer pools, standing in for
// what calico's system library provides.
#include <stdarg.h>
#include <string.h>
#include <calico/types.h>
#include <calico/dev/netbuf.h>
#include <calico/dev/ar6k.h>
#include "nbw_internal.h"
#include "nbwifi.h"

// ---- delays ----

void nbwWaitLoop(u32 count);    // nbw_asm.s: BIOS WaitByLoop, 4 ARM7 cycles a count

static u32 s_sleptUs;           // time spent waiting, for the log's timestamps

void nbwSleepUs(u32 us)
{
	// 33.51 MHz / 4 cycles a count = 8.38 counts a microsecond
	if (us) nbwWaitLoop(us * 8 + (us * 3) / 8);
	s_sleptUs += us;
}

// ---- the log: a ring of text, read with nbwifiLog ----

#define LOG_SIZE 2048
static char s_log[LOG_SIZE];
static unsigned s_logEnd = 0;   // total characters ever written

// where formatted text goes: the log, or a string (nbwSnprintf)
static char* s_str;
static unsigned s_strLeft, s_strLen;

static void _put(char c)
{
	if (s_str) {
		if (s_strLeft > 1) { s_str[s_strLen++] = c; s_strLeft--; }
		return;
	}
	s_log[s_logEnd % LOG_SIZE] = c;
	s_logEnd++;
}

static void _putNumber(unsigned long value, unsigned base, bool upper, int width, int precision, bool zero, bool negative)
{
	char digits[24];
	int n = 0;
	const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	do {
		digits[n++] = set[value % base];
		value /= base;
	} while (value && n < (int)sizeof(digits));
	while (n < precision && n < (int)sizeof(digits)) digits[n++] = '0';
	int len = n + (negative ? 1 : 0);
	if (!zero) for (; width > len; width--) _put(' ');
	if (negative) _put('-');
	if (zero) for (; width > len; width--) _put('0');
	while (n) _put(digits[--n]);
}

// a small printf: %d %i %u %x %X %p %c %s %%, with 0, width, .precision
static void _vformat(const char* fmt, va_list ap)
{
	for (const char* p = fmt; *p; p++) {
		if (*p != '%') { _put(*p); continue; }
		p++;
		bool zero = false;
		int width = 0, precision = -1;
		if (*p == '#') p++;
		if (*p == '0') { zero = true; p++; }
		while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
		if (*p == '.') {
			precision = 0;
			p++;
			while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
		}
		while (*p == 'l' || *p == 'z' || *p == 'h') p++;
		switch (*p) {
			case 'd': case 'i': {
				long v = va_arg(ap, int);
				_putNumber(v < 0 ? -v : v, 10, false, width, precision, zero, v < 0);
				break;
			}
			case 'u': _putNumber(va_arg(ap, unsigned), 10, false, width, precision, zero, false); break;
			case 'x': _putNumber(va_arg(ap, unsigned), 16, false, width, precision, zero, false); break;
			case 'X': _putNumber(va_arg(ap, unsigned), 16, true, width, precision, zero, false); break;
			case 'p': _putNumber((uptr)va_arg(ap, void*), 16, false, 8, 8, true, false); break;
			case 'c': _put((char)va_arg(ap, int)); break;
			case 's': { const char* s = va_arg(ap, const char*); while (s && *s) _put(*s++); break; }
			case '%': _put('%'); break;
			case 0: p--; break;
			default: _put('%'); _put(*p); break;
		}
	}
}

u32 nbwFramesForLog(void);      // nbwifi.c

void nbwLogf(const char* fmt, ...)
{
	// a timestamp in seconds: frames counted plus the driver's own waits (its
	// start-up is mostly waiting); about right, without a hardware timer
	const u32 ds = (nbwFramesForLog() * 16715 / 1000 + s_sleptUs / 1000) / 100;
	_putNumber(ds / 10, 10, false, 0, -1, false, false);
	_put('.');
	_putNumber(ds % 10, 10, false, 0, -1, false, false);
	_put(' ');
	va_list ap;
	va_start(ap, fmt);
	_vformat(fmt, ap);
	va_end(ap);
}

unsigned nbwSnprintf(char* out, unsigned size, const char* fmt, ...)
{
	if (!size) return 0;
	s_str = out;
	s_strLeft = size;
	s_strLen = 0;
	va_list ap;
	va_start(ap, fmt);
	_vformat(fmt, ap);
	va_end(ap);
	out[s_strLen] = 0;
	s_str = NULL;
	return s_strLen;
}

unsigned nbwifiLog(char* out, unsigned size, unsigned* cursor)
{
	// the text written since *cursor (as much of it as is still kept)
	unsigned from = *cursor;
	if (s_logEnd - from > LOG_SIZE) from = s_logEnd - LOG_SIZE;
	unsigned n = 0;
	while (from < s_logEnd && n + 1 < size) out[n++] = s_log[from++ % LOG_SIZE];
	if (size) out[n] = 0;
	*cursor = from;
	return n;
}

// ---- packet buffers: fixed pools carved out of memory nbwifi is given ----

#define NETBUF_DATA_SZ AR6K_HTC_MAX_PACKET_SZ   // the biggest packet the device sends
#define NETBUF_TX 4
#define NETBUF_RX 6

typedef struct _NbwBuf {
	NetBuf nb;
	alignas(4) u8 data[NETBUF_DATA_SZ];
} _NbwBuf;

static _NbwBuf* s_bufs;
static u8 s_bufUsed[NETBUF_TX + NETBUF_RX];

unsigned nbwNetbufMemSize(void)
{
	return sizeof(_NbwBuf) * (NETBUF_TX + NETBUF_RX);
}

void nbwNetbufInit(void* mem)
{
	s_bufs = (_NbwBuf*)mem;
	memset(s_bufUsed, 0, sizeof(s_bufUsed));
	for (unsigned i = 0; i < NETBUF_TX + NETBUF_RX; i++) {
		s_bufs[i].nb = (NetBuf){0};
		s_bufs[i].nb.capacity = NETBUF_DATA_SZ;
		s_bufs[i].nb.system[0] = i + 1;     // nonzero: ours (netbufFree ignores others)
	}
}

NetBuf* netbufAlloc(unsigned hdr_headroom_sz, unsigned data_sz, NetBufPool pool)
{
	if (!s_bufs || hdr_headroom_sz + data_sz > NETBUF_DATA_SZ) return NULL;
	unsigned first = pool == NetBufPool_Tx ? 0 : NETBUF_TX;
	unsigned last = pool == NetBufPool_Tx ? NETBUF_TX : NETBUF_TX + NETBUF_RX;
	for (unsigned i = first; i < last; i++) {
		if (s_bufUsed[i]) continue;
		s_bufUsed[i] = 1;
		NetBuf* nb = &s_bufs[i].nb;
		nb->pos = hdr_headroom_sz;
		nb->len = data_sz;
		return nb;
	}
	return NULL;
}

void netbufFree(NetBuf* nb)
{
	if (!nb || !nb->system[0]) return;
	s_bufUsed[nb->system[0] - 1] = 0;
}

void netbufFlush(NetBuf* nb)
{
	(void)nb;
}
