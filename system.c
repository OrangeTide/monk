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
	WORD flags;
	enum segment_override {
		OVERRIDE_NONE,
		OVERRIDE_ES,
		OVERRIDE_CS,
		OVERRIDE_SS,
		OVERRIDE_DS,
	} segment_override;
	struct {
		void *p;
		BYTE n;
		BYTE modrm;
	} pending; // Pending/temporary memory access (kept in host byte order)
};

static const BYTE implied_seg[8] = {
	[0] = 3, // (BX) + (SI) + DISP
	[1] = 3, // (BX) + (DI) + DISP
	[2] = 2, // (BP) + (SI) + DISP
	[3] = 2, // (BP) + (DI) + DISP
	[4] = 3, // (SI) + DISP
	[5] = 0, // (DI) + DISP  ... might be DS or ES
	[6] = 2, // (BP) + DISP or disp-high:disp-low
	[7] = 3, // (BX) + DISP
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

#define FLAG_VALUE_CF (1)
#define FLAG_VALUE_AF (16)
#define FLAG_VALUE_ZF (64)
#define FLAG_VALUE_SF (128)

#define FLAG_CF (cpu.flags & FLAG_VALUE_CF) /* Carry Flag */
#define FLAG_PF (cpu.flags & 4) /* Parity Flag */
#define FLAG_AF (cpu.flags & FLAG_VALUE_AF) /* Aux Carry Flag */
#define FLAG_ZF (cpu.flags & FLAG_VALUE_ZF) /* Zero Flag */
#define FLAG_SF (cpu.flags & FLAG_VALUE_SF) /* Sign Flag */
#define FLAG_TF (cpu.flags & 256) /* Trap Flag */
#define FLAG_IF (cpu.flags & 512) /* Interrupt Enable */
#define FLAG_DF (cpu.flags & 1024) /* Direction */
#define FLAG_OF (cpu.flags & 2048) /* Overflow Flag */

typedef size_t ADDR;

#if 0 /// TODO: use this for readbyte()/writebyte() etc
enum peripherial_address {
	PERIPH_RAM0,	/* 0000:0000 to 9000:FFFF -- Main system RAM */
	PERIPH_VIDEO,	/* A000:0000 to B000:FFFF -- Video card */
	PERIPH_VBIOS,	/* C000:0000 to C000:7FFF -- Video BIOS */
	PERIPH_SYSBIOS,	/* F000:0000 to F000:FFFF -- System BIOS */
};
#endif

static BYTE sysmem[1 << 18]; /* 256K RAM */
static BYTE *basemem = sysmem + 0x500; /* conventional RAM at 0050:0000 */
static size_t topmem;
static struct cpu cpu;

/* sign extend 8-bits to 16-bits */
static inline WORD
signext(BYTE b)
{
	return (WORD)(int8_t)b;
}

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

static inline void
writebyte(ADDR a, BYTE b)
{
	if (a >= topmem) {
		cpu.errors++;
		return;
	}
	sysmem[a] = b;
}

static inline void
writeword(ADDR a, WORD w)
{
	if ((a | 1) >= topmem) {
		cpu.errors++;
		return;
	}
	sysmem[a] = w & 0xffu;
	sysmem[a + 1] = (w & 0xff00u) >> 8;
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

static void
pushword(WORD w)
{
	ADDR a = segofs_to_addr(SS, SP);
	SP -= 2;
	writeword(a, w);
}

static WORD
popword(void)
{
	ADDR a = segofs_to_addr(SS, SP);
	SS += 2;
	return readword(a);
}

static void
modrm_begin(int w)
{
	ADDR a;

	cpu.pending.modrm = fetchbyte();
	cpu.pending.n = MODRM_N(cpu.pending.modrm); // TODO: elminate this

	switch (MODRM_MOD(cpu.pending.modrm)) {
	case 0: // Disp is 0
		a = 0;
		break;
	case 1: // Disp is 8-bit and sign extended
		a = signext(fetchbyte());
		break;
	case 2: // Disp is 16-bit
		a = fetchword();
		break;
	case 3: // R/M is REG
		if (w)
			cpu.pending.p = &REG16(MODRM_RM(cpu.pending.modrm));
		else
			cpu.pending.p = &REG8(MODRM_RM(cpu.pending.modrm));
		return;
	}

	switch (MODRM_RM(cpu.pending.modrm)) {
	case 0: // (BX) + (SI) + DISP
		a += BX + SI;
		break;
	case 1: // (BX) + (DI) + DISP
		a += BX + DI;
		break;
	case 2: // (BP) + (SI) + DISP
		a += BP + SI;
		break;
	case 3: // (BP) + (DI) + DISP
		a += BP;
		break;
	case 4: // (SI) + DISP
		a += SI;
		break;
	case 5: // (DI) + DISP
		a += DI;
		break;
	case 6: // (BP) + DISP or disp-high:disp-low
		if (MODRM_MOD(cpu.pending.modrm) == 0)
			a = fetchword();
		else
			a += BP;
		break;
	case 7: // (BX) + DISP
		a += BX;
		break;
	}

	switch (cpu.segment_override) {
	case OVERRIDE_NONE:
		a = segofs_to_addr(cpu.segs[implied_seg[MODRM_RM(cpu.pending.modrm)]], a);
		break;
	case OVERRIDE_ES:
		a = segofs_to_addr(ES, a);
		break;
	case OVERRIDE_CS:
		a = segofs_to_addr(CS, a);
		break;
	case OVERRIDE_SS:
		a = segofs_to_addr(SS, a);
		break;
	case OVERRIDE_DS:
		a = segofs_to_addr(DS, a);
		break;
	}

	if (a >= topmem) {
		cpu.errors++;
		return;
	}

	cpu.pending.p = &sysmem[a];
}

static void
modrm_end(void)
{
	// useful for debugging code
}

static BYTE
modrm_readbyte(void)
{
	return *(BYTE*)cpu.pending.p;
}

static WORD
modrm_readword(void)
{
	if (MODRM_MOD(cpu.pending.modrm) == 3) {
		return *(WORD*)cpu.pending.p;
	} else {
		return readword((BYTE*)cpu.pending.p - sysmem);
	}
}

static void
modrm_writebyte(BYTE b)
{
	*(BYTE*)cpu.pending.p = b;
}

static void
modrm_writeword(WORD w)
{
	if (MODRM_MOD(cpu.pending.modrm) == 3) {
		*(WORD*)cpu.pending.p = w;
	} else {
		writeword((BYTE*)cpu.pending.p - sysmem, w);
	}
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

int
system_setargs(int argc, char *argv[])
{
	WORD psp_seg;
	ADDR a;
	int i, j;
	size_t total_len;

	// TODO: check for .COM vs .EXE
	psp_seg = (ADDR)(basemem - sysmem) >> 4;

	/* length of command line arguments */
	a = segofs_to_addr(psp_seg, 0x80);
	fprintf(stderr, "Command-line at @ %06hX\n", a);

	a++;
	for (i = 0, total_len = 0; i < argc; i++) {
		const char *s = argv[i];

		for (j = 0; s[j]; j++) {
			if (total_len < 126) {
				writebyte(a + total_len, s[j]);
				total_len++;
			}
		}
		if (total_len < 126 && i + 1 != argc) {
			writebyte(a + total_len, ' ');
			total_len++;
		}
	}
	if (total_len < 127) {
		writebyte(a + total_len, '\r');
		total_len++;
	}

	writebyte(a - 1, total_len);

	return 0; // TODO: return error on overflow
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
	if (b == '\r')
		return;
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
		case 0x40: { /* Write file handle */
			if (BX == 1) { /* stdout */
				BYTE b;
				WORD i;
				ADDR m;

				fprintf(stdout, "Console: \"");
				for (i = 0; i < CX; i++) {
					m = segofs_to_addr(DS, DX + i);
					b = readbyte(m);
					console_out(b);
					m++;
				}
				fprintf(stdout, "\"\n");
				AX = i;
			} else { /* error - handle not found or not value for writing */
				cpu.flags |= FLAG_VALUE_CF;
				AX = 0x05; // TODO: use the right error code here
			}
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

static void
unknown(BYTE a) {
	fprintf(stderr, "Unknown opcode %02hhX\n", a);
}

static void
unknown2(BYTE a, BYTE b) {
	fprintf(stderr, "Unknown opcode %02hhX %02hhX\n", a, b);
}

int
system_tick(int n)
{
	BYTE bt, wt; /* temp byte and temp word */

	while (!cpu.done && !cpu.errors && n > 0) {
		BYTE op = fetchop();

		/* reset some state at the start of each instruction */
		cpu.segment_override = OVERRIDE_NONE;

		switch (op) {
		// 00 /r      ADD eb,rb   2,mem=7    Add byte register into EA byte
		case 0x00:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt + REG8(cpu.pending.n));
			modrm_end();
			break;

		// 01 /r      ADD ew,rw   2,mem=7    Add word register into EA word
		case 0x01:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt + REG16(cpu.pending.n));
			modrm_end();
			break;

		// 02 /r      ADD rb,eb   2,mem=7    Add EA byte into byte register
		case 0x02:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt + REG8(cpu.pending.n);
			modrm_end();
			break;

		// 03 /r      ADD rw,ew   2,mem=7    Add EA word into word register
		case 0x03:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt + REG16(cpu.pending.n);
			modrm_end();
			break;

		// 04 db      ADD AL,db   3          Add immediate byte into AL
		case 0x04:
			AL += fetchbyte();
			break;

		// 05 dw      ADD AX,dw   3          Add immediate word into AX
		case 0x05:
			AX += fetchword();
			break;

		// 06         PUSH ES      3         Push ES
		case 0x06:
			pushword(ES);
			break;

		// 07          POP ES           5,pm=20    Pop top of stack into ES
		case 0x07:
			ES = popword();
			break;

		// 08 /r      OR eb,rb       2,mem=7   Logical-OR byte register into EA byte
		case 0x08:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt | REG8(cpu.pending.n));
			modrm_end();
			break;

		// 09 /r      OR ew,rw       2,mem=7   Logical-OR word register into EA word
		case 0x09:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt | REG16(cpu.pending.n));
			modrm_end();
			break;

		// 0A /r      OR rb,eb       2,mem=7   Logical-OR EA byte into byte register
		case 0x0A:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt | REG8(cpu.pending.n);
			modrm_end();
			break;

		// 0B /r      OR rw,ew       2,mem=7   Logical-OR EA word into word register
		case 0x0B:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt | REG16(cpu.pending.n);
			modrm_end();
			break;

		// 0C db      OR AL,db       3         Logical-OR immediate byte into AL
		case 0x0C:
			AL = AL | fetchbyte();
			break;

		// 0D dw      OR AX,dw       3         Logical-OR immediate word into AX
		case 0x0D:
			AX = AX | fetchword();
			break;

		// 0E         PUSH CS      3         Push CS
		case 0x0E:
			pushword(CS);
			break;

		// 10 /r      ADC eb,rb   2,mem=7    Add with carry byte register into EA byte
		case 0x10:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt + REG8(cpu.pending.n) + FLAG_CF);
			modrm_end();
			break;

		// 11 /r      ADC ew,rw   2,mem=7    Add with carry word register into EA word
		case 0x11:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt + REG16(cpu.pending.n) + FLAG_CF);
			modrm_end();
			break;

		// 12 /r      ADC rb,eb   2,mem=7    Add with carry EA byte into byte register
		case 0x12:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt + REG8(cpu.pending.n) + FLAG_CF;
			modrm_end();
			break;

		// 13 /r      ADC rw,ew   2,mem=7    Add with carry EA word into word register
		case 0x13:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt + REG16(cpu.pending.n) + FLAG_CF;
			modrm_end();
			break;

		// 14 db      ADC AL,db   3          Add with carry immediate byte into AL
		case 0x14:
			AL += fetchbyte() + FLAG_CF;
			break;

		// 15 dw      ADC AX,dw   3          Add with carry immediate word into AX
		case 0x15:
			AX += fetchword() + FLAG_CF;
			break;


		// 16         PUSH SS      3         Push SS
		case 0x16:
			pushword(SS);
			break;

		// 17          POP SS           5,pm=20    Pop top of stack into SS
		case 0x17:
			SS = popword();
			break;

		// 18 /r       SBB eb,rb    2,mem=7   Subtract with borrow byte register from EA byte
		case 0x18:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt - (REG8(cpu.pending.n) + FLAG_CF));
			modrm_end();
			break;

		// 19 /r       SBB ew,rw    2,mem=7   Subtract with borrow word register from EA word
		case 0x19:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt - (REG16(cpu.pending.n) + FLAG_CF));
			modrm_end();
			break;

		// 1A /r       SBB rb,eb    2,mem=7   Subtract with borrow EA byte from byte register
		case 0x1A:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt - (REG8(cpu.pending.n) + FLAG_CF);
			modrm_end();
			break;

		// 1B /r       SBB rw,ew    2,mem=7   Subtract with borrow EA word from word register
		case 0x1B:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt - (REG16(cpu.pending.n) - FLAG_CF);
			modrm_end();
			break;

		// 1C db       SBB AL,db    3         Subtract with borrow imm.  byte from AL
		case 0x1C:
			AL -= fetchbyte() + FLAG_CF;
			break;

		// 1D dw       SBB AX,dw    3         Subtract with borrow imm.  word from AX
		case 0x1D:
			AX -= fetchword() + FLAG_CF;
			break;

		// 1E         PUSH DS      3         Push DS
		case 0x1E:
			pushword(DS);
			break;

		// 1F          POP DS           5,pm=20    Pop top of stack into DS
		case 0x1F:
			DS = popword();
			break;

		// 20 /r      AND eb,rb     2,mem=7    Logical-AND byte register into EA byte
		case 0x20:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt & REG8(cpu.pending.n));
			modrm_end();
			break;

		// 21 /r      AND ew,rw     2,mem=7    Logical-AND word register into EA word
		case 0x21:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt & REG16(cpu.pending.n));
			modrm_end();
			break;

		// 22 /r      AND rb,eb     2,mem=7    Logical-AND EA byte into byte register
		case 0x22:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt & REG8(cpu.pending.n);
			modrm_end();
			break;

		// 23 /r      AND rw,ew     2,mem=7    Logical-AND EA word into word register
		case 0x23:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt & REG16(cpu.pending.n);
			modrm_end();
			break;

		// 24 db      AND AL,db     3          Logical-AND immediate byte into AL
		case 0x24:
			AL = AL & fetchbyte();
			break;

		// 25 dw      AND AX,dw     3          Logical-AND immediate word into AX
		case 0x25:
			AX = AX & fetchword();
			break;

		case 0x26:
			cpu.segment_override = OVERRIDE_ES;
			break;

		// 27      DAA            3         Decimal adjust AL after addition
		case 0x27:
			if (FLAG_AF || (AL & 15) > 9) {
				AL += 6;
				cpu.flags |= FLAG_VALUE_AF;
				// TODO: update CF
				if (FLAG_CF || AL > 0x9F) {
					AL += 0x60;
					cpu.flags |= FLAG_VALUE_CF;
				} else {
					// Reset AF
					cpu.flags &= ~FLAG_VALUE_AF;
				}
			} else {
				// Reset AF
				cpu.flags &= ~FLAG_VALUE_AF;
			}
			break;

		// 28 /r      SUB eb,rb      2,mem=7     Subtract byte register from EA byte
		case 0x28:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt - REG8(cpu.pending.n));
			modrm_end();
			break;

		// 29 /r      SUB ew,rw      2,mem=7     Subtract word register from EA word
		case 0x29:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt - REG16(cpu.pending.n));
			modrm_end();
			break;

		// 2A /r      SUB rb,eb      2,mem=7     Subtract EA byte from byte register
		case 0x2A:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt - REG8(cpu.pending.n);
			modrm_end();
			break;

		// 2B /r      SUB rw,ew      2,mem=7     Subtract EA word from word register
		case 0x2B:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt - REG16(cpu.pending.n);
			modrm_end();
			break;

		// 2C db      SUB AL,db      3           Subtract immediate byte from AL
		case 0x2C:
			AL -= fetchbyte();
			break;

		// 2D dw      SUB AX,dw      3           Subtract immediate word from AX
		case 0x2D:
			AX -= fetchword();
			break;

		case 0x2E:
			cpu.segment_override = OVERRIDE_CS;
			break;

		// 2F        DAS             3          Decimal adjust AL after subtraction
		case 0x2F:
			if (FLAG_AF || (AL & 15) > 9) {
				AL -= 6;
				cpu.flags |= FLAG_VALUE_AF;
				// TODO: update CF
				if (FLAG_CF || AL > 0x9F) {
					AL -= 0x60;
					cpu.flags |= FLAG_VALUE_CF;
				} else {
					// Reset AF
					cpu.flags &= ~FLAG_VALUE_AF;
				}
			} else {
				// Reset AF
				cpu.flags &= ~FLAG_VALUE_AF;
			}
			break;

		// 30 /r     XOR eb,rb   2,mem=7   Exclusive-OR byte register into EA byte
		case 0x30:
			modrm_begin(0);
			bt = modrm_readbyte();
			modrm_writebyte(bt ^ REG8(cpu.pending.n));
			modrm_end();
			break;

		// 31 /r     XOR ew,rw   2,mem=7   Exclusive-OR word register into EA word
		case 0x31:
			modrm_begin(1);
			wt = modrm_readword();
			modrm_writeword(wt ^ REG16(cpu.pending.n));
			modrm_end();
			break;

		// 32 /r     XOR rb,eb   2,mem=7   Exclusive-OR EA byte into byte register
		case 0x32:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt ^ REG8(cpu.pending.n);
			modrm_end();
			break;

		// 33 /r     XOR rw,ew   2,mem=7   Exclusive-OR EA word into word register
		case 0x33:
			modrm_begin(1);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt ^ REG16(cpu.pending.n);
			modrm_end();
			break;

		// 34 db     XOR AL,db   3         Exclusive-OR immediate byte into AL
		case 0x34:
			AL = AL ^ fetchbyte();
			break;

		// 35 dw     XOR AX,dw   3         Exclusive-OR immediate word into AX
		case 0x35:
			AX = AX ^ fetchword();
			break;

		// 50+ rw     PUSH rw      3         Push word register
		case 0x50: case 0x51: case 0x52: case 0x53:
		case 0x55: case 0x56: case 0x57:
			pushword(REG16(op - 0x50));
			break;
		case 0x54: // PUSH SP
#if 1
			/* behavior on 8088/8086 */
			pushword(SP - 2);
#else
			/* behavior on 286+ */
			pushword(SP);
#endif
			break;

		// 58+rw       POP rw           5          Pop top of stack into word register
		case 0x58: case 0x59: case 0x5A: case 0x5B:
		case 0x5C: case 0x5D: case 0x5E: case 0x5F:
			REG16(op - 0x58) = popword();
			break;

		// 68  dw     PUSH dw      3         Push immediate word
		case 0x68:
			pushword(fetchword());
			break;

		// 6A  db     PUSH db      3         Push immediate sign-extended byte
		case 0x6A:
			pushword(signext(fetchbyte()));
			break;

		// 70  cb     JO cb      7,noj=3   Jump short if overflow (OF=1)
		case 0x70: {
			int a = signext(fetchbyte());
			if (FLAG_OF)
				IP += a;
			break;
		}

		// 71  cb     JNO cb     7,noj=3   Jump short if notoverflow (OF=0)
		case 0x71: {
			int a = signext(fetchbyte());
			if (!FLAG_PF)
				IP += a;
			break;
		}

		// 72  cb     JB cb      7,noj=3   Jump short if below (CF=1)
		// 72  cb     JC cb      7,noj=3   Jump short if carry (CF=1)
		case 0x72: {
			int a = signext(fetchbyte());
			if (FLAG_CF)
				IP += a;
			break;
		}

		// 73  cb     JNB cb     7,noj=3   Jump short if not below (CF=0)
		// 73  cb     JNC cb     7,noj=3   Jump short if not carry (CF=0)
		case 0x73: {
			int a = signext(fetchbyte());
			if (!FLAG_CF)
				IP += a;
			break;
		}

		// 74  cb     JE cb      7,noj=3   Jump short if equal (ZF=1)
		// 74  cb     JZ cb      7,noj=3   Jump short if zero (ZF=1)
		case 0x74: {
			int a = signext(fetchbyte());
			if (FLAG_ZF)
				IP += a;
			break;
		}

		// 75  cb     JNE cb     7,noj=3   Jump short if not equal (ZF=0)
		// 75  cb     JNZ cb     7,noj=3   Jump short if not zero (ZF=0)
		case 0x75: {
			int a = signext(fetchbyte());
			if (!FLAG_ZF)
				IP += a;
			break;
		}

		// 76  cb     JBE cb     7,noj=3   Jump short if below or equal (CF=1 or ZF=1)
		// 76  cb     JNA cb     7,noj=3   Jump short if not above (CF=1 or ZF=1)
		case 0x76: {
			int a = signext(fetchbyte());
			if (FLAG_CF | FLAG_ZF)
				IP += a;
			break;
		}

		// 77  cb     JA cb      7,noj=3   Jump short if above (CF=0 and ZF=0)
		// 77  cb     JNBE cb    7,noj=3   Jump short if not below/equal (CF=0 and ZF=0)
		case 0x77: {
			int a = signext(fetchbyte());
			if (FLAG_CF & FLAG_ZF)
				IP += a;
			break;
		}

		// 78  cb     JS cb      7,noj=3   Jump short if sign (SF=1)
		case 0x78: {
			int a = signext(fetchbyte());
			if (FLAG_SF)
				IP += a;
			break;
		}

		// 79  cb     JNS cb     7,noj=3   Jump short if not sign (SF=0)
		case 0x79: {
			int a = signext(fetchbyte());
			if (!FLAG_SF)
				IP += a;
			break;
		}

		// 7A  cb     JP cb      7,noj=3   Jump short if parity (PF=1)
		// 7A  cb     JPE cb     7,noj=3   Jump short if parity even (PF=1)
		case 0x7A: {
			int a = signext(fetchbyte());
			if (FLAG_PF)
				IP += a;
			break;
		}

		// 7B  cb     JPO cb     7,noj=3   Jump short if parity odd (PF=0)
		// 7B  cb     JNP cb     7,noj=3   Jump short if not parity (PF=0)
		case 0x7B: {
			int a = signext(fetchbyte());
			if (!FLAG_PF)
				IP += a;
			break;
		}

		// 7C  cb     JL cb      7,noj=3   Jump short if less (SF/=OF)
		// 7C  cb     JNGE cb    7,noj=3   Jump short if not greater/equal (SF/=OF)
		case 0x7C: {
			int a = signext(fetchbyte());
			if (!FLAG_SF != !FLAG_OF)
				IP += a;
			break;
		}

		// 7D  cb     JGE cb     7,noj=3   Jump short if greater or equal (SF=OF)
		// 7D  cb     JNL cb     7,noj=3   Jump short if not less (SF=OF)
		case 0x7D: {
			int a = signext(fetchbyte());
			if (!FLAG_SF == !FLAG_OF)
				IP += a;
			break;
		}

		// 7E  cb     JLE cb     7,noj=3   Jump short if less or equal (ZF=1 or SF/=OF)
		// 7E  cb     JNG cb     7,noj=3   Jump short if not greater (ZF=1 or SF/=OF)
		case 0x7E: {
			int a = signext(fetchbyte());
			if (FLAG_ZF || (!FLAG_SF != !FLAG_OF))
				IP += a;
			break;
		}

		// 7F  cb     JG cb      7,noj=3   Jump short if greater (ZF=0 and SF=OF)
		// 7F  cb     JNLE cb    7,noj=3   Jump short if not less/equal (ZF=0 and SF=OF)
		case 0x7F: {
			int a = signext(fetchbyte());
			if (!FLAG_ZF && (!FLAG_SF == !FLAG_OF))
				IP += a;
			break;
		}

		// 86 /r     XCHG eb,rb     3,mem=5     Exchange byte register with EA byte
		// 86 /r     XCHG rb,eb     3,mem=5     Exchange EA byte with byte register
		// 87 /r     XCHG ew,rw     3,mem=5     Exchange word register with EA word
		// 87 /r     XCHG rw,ew     3,mem=5     Exchange EA word with word register
		case 0x86:
		case 0x87:
			cpu.errors++;
			unknown(op); // TODO: implement this
			goto out;
			break;

		// 88 /r      MOV eb,rb   2,mem=3       Move byte register into EA byte
		case 0x88:
			modrm_begin(0);
			modrm_writebyte(REG8(cpu.pending.n));
			modrm_end();
			break;

		// 89 /r      MOV ew,rw   2,mem=3       Move word register into EA word
		case 0x89:
			modrm_begin(0);
			wt = modrm_readword();
			REG16(cpu.pending.n) = wt;
			modrm_end();
			break;

		// 8A /r      MOV rb,eb   2,mem=5       Move EA byte into byte register
		case 0x8A:
			modrm_begin(0);
			bt = modrm_readbyte();
			REG8(cpu.pending.n) = bt;
			modrm_end();
			break;

		// 8B /r      MOV rw,ew   2,mem=5       Move EA word into word register
		case 0x8B:
			modrm_begin(0);
			modrm_writeword(REG16(cpu.pending.n));
			modrm_end();
			break;

		case 0x8C: /* 8C MOV ew, ES/CS/SS/DS */
		// 8C /0      MOV ew,ES   2,mem=3       Move ES into EA word
		// 8C /1      MOV ew,CS   2,mem=3       Move CS into EA word
		// 8C /2      MOV ew,SS   2,mem=3       Move SS into EA word
		// 8C /3      MOV ew,DS   2,mem=3       Move DS into EA word
			cpu.errors++;
			unknown(op); // TODO: implement this
			goto out;
			break;

		case 0x8E: /* 8E MOV ES/SS/DS, mw/rw */
		// 8E /0      MOV ES,mw   5,pm=19       Move memory word into ES
		// 8E /0      MOV ES,rw   2,pm=17       Move word register into ES
		// 8E /2      MOV SS,mw   5,pm=19       Move memory word into SS
		// 8E /2      MOV SS,rw   2,pm=17       Move word register into SS
		// 8E /3      MOV DS,mw   5,pm=19       Move memory word into DS
		// 8E /3      MOV DS,rw   2,pm=17       Move word register into DS
			cpu.errors++;
			unknown(op); // TODO: implement this
			goto out;
			break;


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
			WORD disp = signext(fetchbyte());
			CX--;
			if (CX != 0)
				IP += disp;
			break;
		}

		case 0xFE: /* misc eb */
			modrm_begin(0);
			switch (cpu.pending.n) {
			case 0: /* INC eb */
				bt = modrm_readbyte();
				modrm_writebyte(bt + 1);
				// TODO: what side-effects?
				break;
			case 1: /* DEC eb */
				bt = modrm_readbyte();
				modrm_writebyte(bt - 1);
				// TODO: what side-effects?
				break;
			default:
				cpu.errors++;
				unknown2(op, cpu.pending.modrm);
				goto out;
			}
			modrm_end();

		case 0xFF: { /* misc eb */
			modrm_begin(1);
			switch (cpu.pending.n) {
			case 0: /* INC ew */
				wt = modrm_readword();
				modrm_writebyte(wt + 1);
				// TODO: what side-effects?
				break;
			case 1: /* DEC ew */
				wt = modrm_readword();
				modrm_writebyte(wt - 1);
				// TODO: what side-effects?
				break;
			case 2: /* CALL r/m16 */
			case 3: /* CALL m32 */
			case 4: /* JMP r/m16 */
			case 5: /* JMP m32 */
				// TODO: implement this
				cpu.errors++;
				unknown2(op, cpu.pending.modrm);
				goto out;
			case 6: /* PUSH r/m16 */
				pushword(modrm_readword());
				break;
			case 7: /* invalid ... */
			default:
				cpu.errors++;
				unknown2(op, cpu.pending.modrm);
				goto out;
			}
			modrm_end();
			break;

		}

		case 0x0F: /* undefined on 8086/8088 */
		default:
			cpu.errors++;
			unknown(op);
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
