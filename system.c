#include "system.h"
#include <stdio.h>

/* Memory Map
 *
 * 0000:0000	0000:03FF	Interrupt Vector Table
 * 0040:0000	0040:00FF	BIOS Data Area
 * 0050:0000	9000:FBFF	free conventional memory
 * 9000:FC00	9000:FFFF	extended BIOS Data Area
 * A000:0000	B000:FFFF	video card
 * C000:0000	C000:7FFF	EGA & VGA BIOS
 * C800:0000	E000:FFFF	not used
 * F000:0000	F000:FFFF	system BIOS
 *
 * I/O Map
 *
 * T.B.D.
 *
 */

struct cpu {
	unsigned errors, done;
	WORD ip;
	WORD segs[8]; /* ES CS SS DS */
	WORD regs[8]; /* AX    CX    DX    BX    SP    BP    SI    DI */
};

/* define ENDIAN as index to the high byte */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LITTLE16(x) (x)
#define LOW_BYTE 0
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define LOW_BYTE 1 /* causes certain accesses to by flipped */
#else
#error Must define __BYTE_ORDER__ as __ORDER_BIG_ENDIAN__ or __ORDER_LITTLE_ENDIAN__
#endif

/*
#define HIGH(x) ((BYTE*)&(x)[!LOW_BYTE])
#define LOW(x) ((BYTE*)&(x)[LOW_BYTE])
*/

/* AX    CX    DX    BX    SP    BP    SI    DI */
#define REG16(n) (cpu.regs[n])
#define AX REG16(0)
#define CX REG16(1)
#define DX REG16(2)
#define BX REG16(3)
#define SP REG16(4)
#define BP REG16(5)
#define SI REG16(6)
#define DI REG16(7)

/* AL    CL    DL    BL    AH    CH    DH    BH */
#define REG8(n) (((BYTE*)(void*)&cpu.regs[(n) & 3])[((n) >> 3) ^ LOW_BYTE])
#define AL REG8(0)
#define AH REG8(4)
#define CL REG8(1)
#define CH REG8(5)
#define DL REG8(2)
#define DH REG8(6)
#define BL REG8(3)
#define BH REG8(7)

#define IP cpu.ip
#define ES cpu.segs[0]
#define CS cpu.segs[1]
#define SS cpu.segs[2]
#define DS cpu.segs[3]

#define MODRM_MOD(b) (((BYTE)(b) & 0xC0) >> 6)
#define MODRM_RM(b) ((BYTE)(b) & 0x07)
#define MODRM_N(b) (((BYTE)(b) & 0x38) >> 3)

typedef size_t ADDR;

static BYTE sysmem[1 << 18]; /* 256K RAM */
static BYTE *basemem = sysmem + 0x500; /* conventional RAM at 0050:0000 */
static size_t topmem;
static struct cpu cpu;

static inline ADDR
segofs_to_addr(WORD seg, WORD ofs)
{
	return ((ADDR)seg << 4) | ofs;
}

static inline void
addr_to_segofs(ADDR a, WORD *seg, WORD *ofs)
{
	*seg = (a & 0xf0000u) >> 4;
	*ofs = a & 0xffffu;
}

static inline BYTE
readbyte(ADDR a)
{
	if (a >= topmem) {
		cpu.errors++;
		return 0xffu;
	}
	return sysmem[a];
}

static inline WORD
readword(ADDR a)
{
	if ((a | 1) >= topmem) {
		cpu.errors++;
		return 0xffffu;
	}
	return sysmem[a] | ((WORD)sysmem[a + 1] << 8);
}

/* read byte and increment IP */
static BYTE
fetchbyte(void)
{
	ADDR a = segofs_to_addr(CS, IP);

	IP++;

	return readbyte(a);
}

/* read opcode and increment IP */
static inline BYTE
fetchop(void)
{
	/* wrapping the fetchbyte() until we do something special for this W.R.T. decoding, prefixes, cycle count, etc */
	return fetchbyte();
}

/* read word and increment IP */
static WORD
fetchword(void)
{
	ADDR a = segofs_to_addr(CS, IP);

	IP += 2;

	return readword(a);
}

/* turns a ModRM into a pointer */
static BYTE *
modrm_byte(BYTE modrm)
{
	ADDR a;

	switch (MODRM_MOD(modrm)) {
	case 0: // Disp is 0
		a = 0;
		break;
	case 1: // Disp is 8-bit and sign extended
		a = (WORD)(int8_t)fetchbyte();
		break;
	case 2: // Disp is 16-bit
		a = fetchword();
		break;
	case 3: // R/M is REG
		return &REG8(MODRM_RM(modrm));
	}
	/* TODO: apply R/M
	switch (MODRM_RM(modrm)) {
	case 6:
		if (MODRM_MOD(modrm) == 0) ... TODO
	}
	*/

	if (a >= topmem) {
		cpu.errors++;
		return &sysmem[0];
	}
	return &sysmem[a];
}

static void
cpu_reset(void)
{
	cpu.done = 0;
	cpu.errors = 0;
	CS = 0xffffu;
	IP = 0x0000u;
}

static int
loadfile_com(const char *filename)
{
	unsigned char *out;
	size_t count, size;
	FILE *f;
	WORD psp_seg;

	f = fopen(filename, "rb");
	if (!f) {
		perror(filename);
		return -1;
	}

	psp_seg = (ADDR)(basemem - sysmem) >> 4;
	fprintf(stderr, "PSP @ %04hhX:0000\n", psp_seg);
	out = basemem + 0x100u; /* start writing .COM file after PSP */
	for (out = basemem, size = 0; size < topmem && !feof(f); out += count, size += count) {
		size_t rem = topmem - size;
		count = fread(out, 1, rem, f);
		if (!count)
			break;
	}

	fclose(f);

	/* .COM file register and memory layout
	 * CS:IP = PSP:0100
	 * DS,ES,SS = PSP
	 * SP = end of 64K segment
	 * TODO: AL,AH = drive letter status
	 */
	DS = ES = SS = CS = psp_seg;
	IP = 0x0100u;
	SP = 0xfffeu; // TODO: is this correct?

	return 0;
}

int
system_init(void)
{
	topmem = sizeof(sysmem) - (basemem - sysmem);

	cpu_reset();

	return 0;
}

void
system_done(void)
{
}

int
system_loadfile(const char *filename)
{
	// TODO: check for .COM vs .EXE
	if (loadfile_com(filename))
		return -1;
	return 0;
}

static void
print_cpu(const char *prefix)
{
	if (prefix)
		fprintf(stderr, "%s: ", prefix);
	fprintf(stderr, "CS: %04hX IP: %04hX\n", CS, IP);

	if (prefix)
		fprintf(stderr, "%s: ", prefix);
	fprintf(stderr, "AX: %04hX CX: %04hX DX: %04hX BX: %04hX\n", AX, CX, DX, BX);

	if (prefix)
		fprintf(stderr, "%s: ", prefix);
	fprintf(stderr, "AL: %04hhX AH: %04hhX\n", AL, AH);
}

static void
console_out(BYTE b)
{
	fputc(b, stdout);
}

static void
dosirq(void)
{
	BYTE service = AH;

	switch (service) {
		case 0x02: /* Write character to stdout */
			console_out(DL);
			AL = DL == '\t' ? ' ' : DL;
			break;
		case 0x09: { /* Write string to stdout */
			ADDR m = segofs_to_addr(DS, DX);
			BYTE b;
			fprintf(stdout, "Console: \"");
			for (; '$' != (b = readbyte(m)); m++) {
				console_out(b);
			}
			fprintf(stdout, "\"\n");
			AL = '$';
			break;
		}
		default:
			cpu.errors++;
			fprintf(stderr, "DOSIRQ: Unknown service %02hhX\n", service);
			print_cpu("DOSIRQ");
	}
}

static void
initiate_irq(BYTE irq)
{
	switch (irq) {
	case 0x20: // Terminate
		cpu.done = 1;
		fprintf(stderr, "Successful Termination\n");
		break;
	case 0x21: // DOS
		dosirq();
		break;
	default:
		cpu.errors++;
		fprintf(stderr, "IRQ: Unknown interrupt %02hhX\n", irq);
	}
}

int
system_tick(int n)
{
	while (!cpu.done && !cpu.errors && n > 0) {
		BYTE op = fetchop();

		switch (op) {
		// B0+ rb db  MOV rb,db   2             Move immediate byte into byte register
		case 0xB0: case 0xB1: case 0xB2: case 0xB3:
		case 0xB4: case 0xB5: case 0xB6: case 0xB7:
			REG8(op - 0xB0) = fetchbyte();
			// TODO: what side-effects?
			break;

		// B8+ rw dw  MOV rw,dw   2             Move immediate word into word register
		case 0xB8: case 0xB9: case 0xBA: case 0xBB:
		case 0xBC: case 0xBD: case 0xBE: case 0xBF:
			REG16(op - 0xB8) = fetchword();
			// TODO: what side-effects?
			break;

		case 0xCD: /* INT */
			initiate_irq(fetchbyte());
			break;

		case 0XE2: { /* LOOP cb */
			WORD disp = (DWORD)(int8_t)fetchbyte();
			CX--;
			if (CX != 0)
				IP += disp;
			break;
		}

		case 0xFE: { /* misc eb */
			BYTE *eb;
			WORD *ew;
			BYTE modrm = fetchbyte();

			switch (MODRM_N(modrm)) {
			case 0: /* INC eb */
				eb = modrm_byte(modrm);
				(*eb)++;
				// TODO: what side-effects?
				fprintf(stderr, "[%02hhX %02hhX] INC eb {mod=%d,n=%d,rm=%d}\n", op, modrm,
					MODRM_MOD(modrm), MODRM_N(modrm), MODRM_RM(modrm));
				break;
			case 1: /* DEC eb */
				eb = modrm_byte(modrm);
				(*eb)--;
				// TODO: what side-effects?
				fprintf(stderr, "[%02hhX %02hhX] DEC eb\n", op, modrm);
				break;
			default:
				cpu.errors++;
				fprintf(stderr, "Unknown opcode %02hhX %02hhX\n", op, modrm);
				goto out;
			}
			break;
		}

		default:
			cpu.errors++;
			fprintf(stderr, "Unknown opcode %02hhX\n", op);
			goto out;
		}
		n--;
	}
out:
	if (1 /*cpu.errors*/) {
		print_cpu(0);
	}

	return cpu.errors ? -1 : !!cpu.done;
}
