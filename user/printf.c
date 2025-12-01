#include "kernel/types.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

static void putc(int fd, char c) { write(fd, &c, 1); }

static void bufputc(char **buf, char *end, int *count, char c) {
  if (*buf < end) {
    **buf = c;
    (*buf)++;
  }
  (*count)++;
}

static void bufprintint(char **out, char *end, int *count, long long xx,
                        int base, int sgn) {
  char buf[32];
  int i = 0;
  int neg = 0;
  unsigned long long x;

  if (sgn && xx < 0) {
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);
  if (neg)
    buf[i++] = '-';

  while (--i >= 0)
    bufputc(out, end, count, buf[i]);
}

static void bufprintptr(char **out, char *end, int *count, uint64 x) {
  bufputc(out, end, count, '0');
  bufputc(out, end, count, 'x');
  for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    bufputc(out, end, count, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

static void printint(int fd, long long xx, int base, int sgn) {
  char buf[20];
  int i, neg;
  unsigned long long x;

  neg = 0;
  if (sgn && xx < 0) {
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);
  if (neg)
    buf[i++] = '-';

  while (--i >= 0)
    putc(fd, buf[i]);
}

static void printptr(int fd, uint64 x) {
  int i;
  putc(fd, '0');
  putc(fd, 'x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    putc(fd, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the given fd. Only understands %d, %x, %p, %c, %s.
void vprintf(int fd, const char *fmt, va_list ap) {
  char *s;
  int c0, c1, c2, i, state;

  state = 0;
  for (i = 0; fmt[i]; i++) {
    c0 = fmt[i] & 0xff;
    if (state == 0) {
      if (c0 == '%') {
        state = '%';
      } else {
        putc(fd, c0);
      }
    } else if (state == '%') {
      c1 = c2 = 0;
      if (c0)
        c1 = fmt[i + 1] & 0xff;
      if (c1)
        c2 = fmt[i + 2] & 0xff;
      if (c0 == 'd') {
        printint(fd, va_arg(ap, int), 10, 1);
      } else if (c0 == 'l' && c1 == 'd') {
        printint(fd, va_arg(ap, uint64), 10, 1);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
        printint(fd, va_arg(ap, uint64), 10, 1);
        i += 2;
      } else if (c0 == 'u') {
        printint(fd, va_arg(ap, uint32), 10, 0);
      } else if (c0 == 'l' && c1 == 'u') {
        printint(fd, va_arg(ap, uint64), 10, 0);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
        printint(fd, va_arg(ap, uint64), 10, 0);
        i += 2;
      } else if (c0 == 'x') {
        printint(fd, va_arg(ap, uint32), 16, 0);
      } else if (c0 == 'l' && c1 == 'x') {
        printint(fd, va_arg(ap, uint64), 16, 0);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
        printint(fd, va_arg(ap, uint64), 16, 0);
        i += 2;
      } else if (c0 == 'p') {
        printptr(fd, va_arg(ap, uint64));
      } else if (c0 == 'c') {
        putc(fd, va_arg(ap, uint32));
      } else if (c0 == 's') {
        if ((s = va_arg(ap, char *)) == 0)
          s = "(null)";
        for (; *s; s++)
          putc(fd, *s);
      } else if (c0 == '%') {
        putc(fd, '%');
      } else {
        // Unknown % sequence.  Print it to draw attention.
        putc(fd, '%');
        putc(fd, c0);
      }

      state = 0;
    }
  }
}

void fprintf(int fd, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
}

void printf(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vprintf(1, fmt, ap);
}

static int vsnprintf_internal(char *buf, int n, const char *fmt, va_list ap) {
  char *out = buf;
  char *end = (n > 0) ? buf + (n - 1) : buf; // leave room for NUL
  int count = 0;
  int c0, c1, c2, i, state = 0;

  for (i = 0; fmt[i]; i++) {
    c0 = fmt[i] & 0xff;
    if (state == 0) {
      if (c0 == '%') {
        state = '%';
      } else {
        bufputc(&out, end, &count, c0);
      }
    } else if (state == '%') {
      c1 = c2 = 0;
      if (c0)
        c1 = fmt[i + 1] & 0xff;
      if (c1)
        c2 = fmt[i + 2] & 0xff;
      if (c0 == 'd') {
        bufprintint(&out, end, &count, va_arg(ap, int), 10, 1);
      } else if (c0 == 'l' && c1 == 'd') {
        bufprintint(&out, end, &count, va_arg(ap, long), 10, 1);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
        bufprintint(&out, end, &count, va_arg(ap, long long), 10, 1);
        i += 2;
      } else if (c0 == 'u') {
        bufprintint(&out, end, &count, va_arg(ap, unsigned int), 10, 0);
      } else if (c0 == 'l' && c1 == 'u') {
        bufprintint(&out, end, &count, va_arg(ap, unsigned long), 10, 0);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
        bufprintint(&out, end, &count, va_arg(ap, unsigned long long), 10, 0);
        i += 2;
      } else if (c0 == 'x') {
        bufprintint(&out, end, &count, va_arg(ap, unsigned int), 16, 0);
      } else if (c0 == 'l' && c1 == 'x') {
        bufprintint(&out, end, &count, va_arg(ap, unsigned long), 16, 0);
        i += 1;
      } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
        bufprintint(&out, end, &count, va_arg(ap, unsigned long long), 16, 0);
        i += 2;
      } else if (c0 == 'p') {
        bufprintptr(&out, end, &count, va_arg(ap, uint64));
      } else if (c0 == 'c') {
        bufputc(&out, end, &count, va_arg(ap, uint32));
      } else if (c0 == 's') {
        char *s = va_arg(ap, char *);
        if (!s)
          s = "(null)";
        for (; *s; s++)
          bufputc(&out, end, &count, *s);
      } else if (c0 == '%') {
        bufputc(&out, end, &count, '%');
      } else {
        bufputc(&out, end, &count, '%');
        bufputc(&out, end, &count, c0);
      }
      state = 0;
    }
  }

  if (n > 0)
    *out = '\0';
  return count;
}

int snprintf(char *buf, int n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int written = vsnprintf_internal(buf, n, fmt, ap);
  va_end(ap);
  return written;
}
