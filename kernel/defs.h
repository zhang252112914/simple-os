// console.c
void consoleinit(void);
void consputc(int);

// printf.c
int printf(char *, ...) __attribute__((format(printf, 1, 2)));
void panic(char *) __attribute__((noreturn));
void printfinit(void);

// uart.c
void uartinit(void);
void uartputc(char);
void uartputs(char *);