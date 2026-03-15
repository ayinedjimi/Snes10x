/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <immintrin.h>
#include "snes9x.h"
#include "memmap.h"
#include "cpuops.h"

// Include opcode implementations in this TU so the compiler can inline
// hot opcodes directly into the dispatch loop (equivalent to LTO)
#include "cpuops.cpp"
#include "dma.h"
#include "apu/apu.h"
#include "fxemu.h"
#include "snapshot.h"
#include "movie.h"
#ifdef DEBUGGER
#include "debug.h"
#include "missing.h"
#endif

static inline void S9xReschedule (void);

// flatten: tell clang to inline all called functions (especially opcode handlers)
// This turns indirect function pointer calls into direct inlined code
__attribute__((flatten))
void S9xMainLoop (void)
{
	#define CHECK_FOR_IRQ_CHANGE() \
	if (Timings.IRQFlagChanging) \
	{ \
		if (Timings.IRQFlagChanging & IRQ_TRIGGER_NMI) \
		{ \
			CPU.NMIPending = TRUE; \
			Timings.NMITriggerPos = CPU.Cycles + 6; \
		} \
		if (Timings.IRQFlagChanging & IRQ_CLEAR_FLAG) \
			ClearIRQ(); \
		else if (Timings.IRQFlagChanging & IRQ_SET_FLAG) \
			SetIRQ(); \
		Timings.IRQFlagChanging = IRQ_NONE; \
	}

	if (CPU.Flags & SCAN_KEYS_FLAG)
	{
		CPU.Flags &= ~SCAN_KEYS_FLAG;
	}

	for (;;)
	{
		// Fast path: skip NMI/IRQ/timer checks when nothing is pending
		// This avoids 3 unlikely branches per opcode in the common case
		if (S9X_UNLIKELY(CPU.NMIPending | CPU.IRQLine | CPU.IRQExternal |
		                 (CPU.Cycles >= Timings.NextIRQTimer)))
		{
			if (CPU.NMIPending)
			{
				#ifdef DEBUGGER
				if (Settings.TraceHCEvent)
				    S9xTraceFormattedMessage ("Comparing %d to %d\n", Timings.NMITriggerPos, CPU.Cycles);
				#endif
				if (Timings.NMITriggerPos <= CPU.Cycles)
				{
					CPU.NMIPending = FALSE;
					Timings.NMITriggerPos = 0xffff;
					if (CPU.WaitingForInterrupt)
					{
						CPU.WaitingForInterrupt = FALSE;
						Registers.PCw++;
						CPU.Cycles += TWO_CYCLES + ONE_DOT_CYCLE / 2;
						while (CPU.Cycles >= CPU.NextEvent)
							S9xDoHEventProcessing();
					}

					CHECK_FOR_IRQ_CHANGE();
					S9xOpcode_NMI();
				}
			}

			if (CPU.Cycles >= Timings.NextIRQTimer)
			{
				#ifdef DEBUGGER
				S9xTraceMessage ("Timer triggered\n");
				#endif

				S9xUpdateIRQPositions(false);
				CPU.IRQLine = TRUE;
			}

			if (CPU.IRQLine || CPU.IRQExternal)
			{
				if (CPU.WaitingForInterrupt)
				{
					CPU.WaitingForInterrupt = FALSE;
					Registers.PCw++;
					CPU.Cycles += TWO_CYCLES + ONE_DOT_CYCLE / 2;
					while (CPU.Cycles >= CPU.NextEvent)
						S9xDoHEventProcessing();
				}

				if (!CheckFlag(IRQ))
				{
					CHECK_FOR_IRQ_CHANGE();
					S9xOpcode_IRQ();
				}
			}
		}

		/* Change IRQ flag for instructions that set it only on last cycle */
		CHECK_FOR_IRQ_CHANGE();

	#ifdef DEBUGGER
		if ((CPU.Flags & BREAK_FLAG) && !(CPU.Flags & SINGLE_STEP_FLAG))
		{
			for (int Break = 0; Break != 6; Break++)
			{
				if (S9xBreakpoint[Break].Enabled &&
					S9xBreakpoint[Break].Bank == Registers.PB &&
					S9xBreakpoint[Break].Address == Registers.PCw)
				{
					if (S9xBreakpoint[Break].Enabled == 2)
						S9xBreakpoint[Break].Enabled = TRUE;
					else
						CPU.Flags |= DEBUG_MODE_FLAG;
				}
			}
		}

		if (CPU.Flags & DEBUG_MODE_FLAG)
			break;

		if (CPU.Flags & TRACE_FLAG)
			S9xTrace();

		if (CPU.Flags & SINGLE_STEP_FLAG)
		{
			CPU.Flags &= ~SINGLE_STEP_FLAG;
			CPU.Flags |= DEBUG_MODE_FLAG;
		}
	#endif

		if (S9X_UNLIKELY(CPU.Flags & SCAN_KEYS_FLAG))
		{
			break;
		}

		uint8				Op;
		struct	SOpcodes	*Opcodes;

		if (S9X_LIKELY(CPU.PCBase != NULL))
		{
			Op = CPU.PCBase[Registers.PCw];
			// Prefetch next cache line of opcodes (64 bytes ahead)
			_mm_prefetch((const char*)(CPU.PCBase + Registers.PCw + 64), _MM_HINT_T0);
			CPU.Cycles += CPU.MemSpeed;
			Opcodes = ICPU.S9xOpcodes;

			if (S9X_UNLIKELY(CPU.Cycles > 1000000))
			{
				Settings.StopEmulation = true;
				CPU.Flags |= HALTED_FLAG;
				S9xMessage(S9X_FATAL_ERROR, 0, "CPU is deadlocked");
				return;
			}
		}
		else
		{
			Op = S9xGetByte(Registers.PBPC);
			OpenBus = Op;
			Opcodes = S9xOpcodesSlow;
		}

		if (S9X_UNLIKELY((Registers.PCw & MEMMAP_MASK) + ICPU.S9xOpLengths[Op] >= MEMMAP_BLOCK_SIZE))
		{
			uint8	*oldPCBase = CPU.PCBase;

			CPU.PCBase = S9xGetBasePointer(ICPU.ShiftedPB + ((uint16) (Registers.PCw + 4)));
			if (oldPCBase != CPU.PCBase || (Registers.PCw & ~MEMMAP_MASK) == (0xffff & ~MEMMAP_MASK))
				Opcodes = S9xOpcodesSlow;
		}

		Registers.PCw++;

		// Direct dispatch for M0X0 mode (most common) — compiler generates jump table
		// Each case is a direct call (not indirect via function pointer), enabling
		// inlining by the compiler and better branch prediction per-opcode
		if (S9X_LIKELY(Opcodes == S9xOpcodesM0X0))
		{
			switch(Op) {
    case 0x00: Op00(); break;
    case 0x01: Op01E0M0(); break;
    case 0x02: Op02(); break;
    case 0x03: Op03M0(); break;
    case 0x04: Op04M0(); break;
    case 0x05: Op05M0(); break;
    case 0x06: Op06M0(); break;
    case 0x07: Op07M0(); break;
    case 0x08: Op08E0(); break;
    case 0x09: Op09M0(); break;
    case 0x0A: Op0AM0(); break;
    case 0x0B: Op0BE0(); break;
    case 0x0C: Op0CM0(); break;
    case 0x0D: Op0DM0(); break;
    case 0x0E: Op0EM0(); break;
    case 0x0F: Op0FM0(); break;
    case 0x10: Op10E0(); break;
    case 0x11: Op11E0M0X0(); break;
    case 0x12: Op12E0M0(); break;
    case 0x13: Op13M0(); break;
    case 0x14: Op14M0(); break;
    case 0x15: Op15E0M0(); break;
    case 0x16: Op16E0M0(); break;
    case 0x17: Op17M0(); break;
    case 0x18: Op18(); break;
    case 0x19: Op19M0X0(); break;
    case 0x1A: Op1AM0(); break;
    case 0x1B: Op1B(); break;
    case 0x1C: Op1CM0(); break;
    case 0x1D: Op1DM0X0(); break;
    case 0x1E: Op1EM0X0(); break;
    case 0x1F: Op1FM0(); break;
    case 0x20: Op20E0(); break;
    case 0x21: Op21E0M0(); break;
    case 0x22: Op22E0(); break;
    case 0x23: Op23M0(); break;
    case 0x24: Op24M0(); break;
    case 0x25: Op25M0(); break;
    case 0x26: Op26M0(); break;
    case 0x27: Op27M0(); break;
    case 0x28: Op28E0(); break;
    case 0x29: Op29M0(); break;
    case 0x2A: Op2AM0(); break;
    case 0x2B: Op2BE0(); break;
    case 0x2C: Op2CM0(); break;
    case 0x2D: Op2DM0(); break;
    case 0x2E: Op2EM0(); break;
    case 0x2F: Op2FM0(); break;
    case 0x30: Op30E0(); break;
    case 0x31: Op31E0M0X0(); break;
    case 0x32: Op32E0M0(); break;
    case 0x33: Op33M0(); break;
    case 0x34: Op34E0M0(); break;
    case 0x35: Op35E0M0(); break;
    case 0x36: Op36E0M0(); break;
    case 0x37: Op37M0(); break;
    case 0x38: Op38(); break;
    case 0x39: Op39M0X0(); break;
    case 0x3A: Op3AM0(); break;
    case 0x3B: Op3B(); break;
    case 0x3C: Op3CM0X0(); break;
    case 0x3D: Op3DM0X0(); break;
    case 0x3E: Op3EM0X0(); break;
    case 0x3F: Op3FM0(); break;
    case 0x40: Op40Slow(); break;
    case 0x41: Op41E0M0(); break;
    case 0x42: Op42(); break;
    case 0x43: Op43M0(); break;
    case 0x44: Op44X0(); break;
    case 0x45: Op45M0(); break;
    case 0x46: Op46M0(); break;
    case 0x47: Op47M0(); break;
    case 0x48: Op48E0M0(); break;
    case 0x49: Op49M0(); break;
    case 0x4A: Op4AM0(); break;
    case 0x4B: Op4BE0(); break;
    case 0x4C: Op4C(); break;
    case 0x4D: Op4DM0(); break;
    case 0x4E: Op4EM0(); break;
    case 0x4F: Op4FM0(); break;
    case 0x50: Op50E0(); break;
    case 0x51: Op51E0M0X0(); break;
    case 0x52: Op52E0M0(); break;
    case 0x53: Op53M0(); break;
    case 0x54: Op54X0(); break;
    case 0x55: Op55E0M0(); break;
    case 0x56: Op56E0M0(); break;
    case 0x57: Op57M0(); break;
    case 0x58: Op58(); break;
    case 0x59: Op59M0X0(); break;
    case 0x5A: Op5AE0X0(); break;
    case 0x5B: Op5B(); break;
    case 0x5C: Op5C(); break;
    case 0x5D: Op5DM0X0(); break;
    case 0x5E: Op5EM0X0(); break;
    case 0x5F: Op5FM0(); break;
    case 0x60: Op60E0(); break;
    case 0x61: Op61E0M0(); break;
    case 0x62: Op62E0(); break;
    case 0x63: Op63M0(); break;
    case 0x64: Op64M0(); break;
    case 0x65: Op65M0(); break;
    case 0x66: Op66M0(); break;
    case 0x67: Op67M0(); break;
    case 0x68: Op68E0M0(); break;
    case 0x69: Op69M0(); break;
    case 0x6A: Op6AM0(); break;
    case 0x6B: Op6BE0(); break;
    case 0x6C: Op6C(); break;
    case 0x6D: Op6DM0(); break;
    case 0x6E: Op6EM0(); break;
    case 0x6F: Op6FM0(); break;
    case 0x70: Op70E0(); break;
    case 0x71: Op71E0M0X0(); break;
    case 0x72: Op72E0M0(); break;
    case 0x73: Op73M0(); break;
    case 0x74: Op74E0M0(); break;
    case 0x75: Op75E0M0(); break;
    case 0x76: Op76E0M0(); break;
    case 0x77: Op77M0(); break;
    case 0x78: Op78(); break;
    case 0x79: Op79M0X0(); break;
    case 0x7A: Op7AE0X0(); break;
    case 0x7B: Op7B(); break;
    case 0x7C: Op7C(); break;
    case 0x7D: Op7DM0X0(); break;
    case 0x7E: Op7EM0X0(); break;
    case 0x7F: Op7FM0(); break;
    case 0x80: Op80E0(); break;
    case 0x81: Op81E0M0(); break;
    case 0x82: Op82(); break;
    case 0x83: Op83M0(); break;
    case 0x84: Op84X0(); break;
    case 0x85: Op85M0(); break;
    case 0x86: Op86X0(); break;
    case 0x87: Op87M0(); break;
    case 0x88: Op88X0(); break;
    case 0x89: Op89M0(); break;
    case 0x8A: Op8AM0(); break;
    case 0x8B: Op8BE0(); break;
    case 0x8C: Op8CX0(); break;
    case 0x8D: Op8DM0(); break;
    case 0x8E: Op8EX0(); break;
    case 0x8F: Op8FM0(); break;
    case 0x90: Op90E0(); break;
    case 0x91: Op91E0M0X0(); break;
    case 0x92: Op92E0M0(); break;
    case 0x93: Op93M0(); break;
    case 0x94: Op94E0X0(); break;
    case 0x95: Op95E0M0(); break;
    case 0x96: Op96E0X0(); break;
    case 0x97: Op97M0(); break;
    case 0x98: Op98M0(); break;
    case 0x99: Op99M0X0(); break;
    case 0x9A: Op9A(); break;
    case 0x9B: Op9BX0(); break;
    case 0x9C: Op9CM0(); break;
    case 0x9D: Op9DM0X0(); break;
    case 0x9E: Op9EM0X0(); break;
    case 0x9F: Op9FM0(); break;
    case 0xA0: OpA0X0(); break;
    case 0xA1: OpA1E0M0(); break;
    case 0xA2: OpA2X0(); break;
    case 0xA3: OpA3M0(); break;
    case 0xA4: OpA4X0(); break;
    case 0xA5: OpA5M0(); break;
    case 0xA6: OpA6X0(); break;
    case 0xA7: OpA7M0(); break;
    case 0xA8: OpA8X0(); break;
    case 0xA9: OpA9M0(); break;
    case 0xAA: OpAAX0(); break;
    case 0xAB: OpABE0(); break;
    case 0xAC: OpACX0(); break;
    case 0xAD: OpADM0(); break;
    case 0xAE: OpAEX0(); break;
    case 0xAF: OpAFM0(); break;
    case 0xB0: OpB0E0(); break;
    case 0xB1: OpB1E0M0X0(); break;
    case 0xB2: OpB2E0M0(); break;
    case 0xB3: OpB3M0(); break;
    case 0xB4: OpB4E0X0(); break;
    case 0xB5: OpB5E0M0(); break;
    case 0xB6: OpB6E0X0(); break;
    case 0xB7: OpB7M0(); break;
    case 0xB8: OpB8(); break;
    case 0xB9: OpB9M0X0(); break;
    case 0xBA: OpBAX0(); break;
    case 0xBB: OpBBX0(); break;
    case 0xBC: OpBCX0(); break;
    case 0xBD: OpBDM0X0(); break;
    case 0xBE: OpBEX0(); break;
    case 0xBF: OpBFM0(); break;
    case 0xC0: OpC0X0(); break;
    case 0xC1: OpC1E0M0(); break;
    case 0xC2: OpC2(); break;
    case 0xC3: OpC3M0(); break;
    case 0xC4: OpC4X0(); break;
    case 0xC5: OpC5M0(); break;
    case 0xC6: OpC6M0(); break;
    case 0xC7: OpC7M0(); break;
    case 0xC8: OpC8X0(); break;
    case 0xC9: OpC9M0(); break;
    case 0xCA: OpCAX0(); break;
    case 0xCB: OpCB(); break;
    case 0xCC: OpCCX0(); break;
    case 0xCD: OpCDM0(); break;
    case 0xCE: OpCEM0(); break;
    case 0xCF: OpCFM0(); break;
    case 0xD0: OpD0E0(); break;
    case 0xD1: OpD1E0M0X0(); break;
    case 0xD2: OpD2E0M0(); break;
    case 0xD3: OpD3M0(); break;
    case 0xD4: OpD4E0(); break;
    case 0xD5: OpD5E0M0(); break;
    case 0xD6: OpD6E0M0(); break;
    case 0xD7: OpD7M0(); break;
    case 0xD8: OpD8(); break;
    case 0xD9: OpD9M0X0(); break;
    case 0xDA: OpDAE0X0(); break;
    case 0xDB: OpDB(); break;
    case 0xDC: OpDC(); break;
    case 0xDD: OpDDM0X0(); break;
    case 0xDE: OpDEM0X0(); break;
    case 0xDF: OpDFM0(); break;
    case 0xE0: OpE0X0(); break;
    case 0xE1: OpE1E0M0(); break;
    case 0xE2: OpE2(); break;
    case 0xE3: OpE3M0(); break;
    case 0xE4: OpE4X0(); break;
    case 0xE5: OpE5M0(); break;
    case 0xE6: OpE6M0(); break;
    case 0xE7: OpE7M0(); break;
    case 0xE8: OpE8X0(); break;
    case 0xE9: OpE9M0(); break;
    case 0xEA: OpEA(); break;
    case 0xEB: OpEB(); break;
    case 0xEC: OpECX0(); break;
    case 0xED: OpEDM0(); break;
    case 0xEE: OpEEM0(); break;
    case 0xEF: OpEFM0(); break;
    case 0xF0: OpF0E0(); break;
    case 0xF1: OpF1E0M0X0(); break;
    case 0xF2: OpF2E0M0(); break;
    case 0xF3: OpF3M0(); break;
    case 0xF4: OpF4E0(); break;
    case 0xF5: OpF5E0M0(); break;
    case 0xF6: OpF6E0M0(); break;
    case 0xF7: OpF7M0(); break;
    case 0xF8: OpF8(); break;
    case 0xF9: OpF9M0X0(); break;
    case 0xFA: OpFAE0X0(); break;
    case 0xFB: OpFB(); break;
    case 0xFC: OpFCE0(); break;
    case 0xFD: OpFDM0X0(); break;
    case 0xFE: OpFEM0X0(); break;
    case 0xFF: OpFFM0(); break;
			}
		}
		else
		{
			(*Opcodes[Op].S9xOpcode)();
		}

		if (Settings.SA1)
			S9xSA1MainLoop();
	}

	S9xPackStatus();
}

static inline void S9xReschedule (void)
{
	switch (CPU.WhichEvent)
	{
		case HC_HBLANK_START_EVENT:
			CPU.WhichEvent = HC_HDMA_START_EVENT;
			CPU.NextEvent  = Timings.HDMAStart;
			break;

		case HC_HDMA_START_EVENT:
			CPU.WhichEvent = HC_HCOUNTER_MAX_EVENT;
			CPU.NextEvent  = Timings.H_Max;
			break;

		case HC_HCOUNTER_MAX_EVENT:
			CPU.WhichEvent = HC_HDMA_INIT_EVENT;
			CPU.NextEvent  = Timings.HDMAInit;
			break;

		case HC_HDMA_INIT_EVENT:
			CPU.WhichEvent = HC_RENDER_EVENT;
			CPU.NextEvent  = Timings.RenderPos;
			break;

		case HC_RENDER_EVENT:
			CPU.WhichEvent = HC_WRAM_REFRESH_EVENT;
			CPU.NextEvent  = Timings.WRAMRefreshPos;
			break;

		case HC_WRAM_REFRESH_EVENT:
			CPU.WhichEvent = HC_HBLANK_START_EVENT;
			CPU.NextEvent  = Timings.HBlankStart;
			break;
	}
}

void S9xDoHEventProcessing (void)
{
#ifdef DEBUGGER
	static char	eventname[7][32] =
	{
		"",
		"HC_HBLANK_START_EVENT",
		"HC_HDMA_START_EVENT  ",
		"HC_HCOUNTER_MAX_EVENT",
		"HC_HDMA_INIT_EVENT   ",
		"HC_RENDER_EVENT      ",
		"HC_WRAM_REFRESH_EVENT"
	};
#endif

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event processing  (%s)  expected HC:%04d  executed HC:%04d VC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles, CPU.V_Counter);
#endif

	switch (CPU.WhichEvent)
	{
		case HC_HBLANK_START_EVENT:
			S9xReschedule();
			break;

		case HC_HDMA_START_EVENT:
			S9xReschedule();

			if (PPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Transfer HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				PPU.HDMA = S9xDoHDMA(PPU.HDMA);
			}

			break;

		case HC_HCOUNTER_MAX_EVENT:
			if (Settings.SuperFX)
			{
				if (!SuperFX.oneLineDone)
					S9xSuperFXExec();
				SuperFX.oneLineDone = FALSE;
			}

			S9xAPUEndScanline();
			CPU.Cycles -= Timings.H_Max;
			if (Timings.NMITriggerPos != 0xffff)
				Timings.NMITriggerPos -= Timings.H_Max;
			if (Timings.NextIRQTimer != 0x0fffffff)
				Timings.NextIRQTimer -= Timings.H_Max;
			S9xAPUSetReferenceTime(CPU.Cycles);

			if (Settings.SA1)
				SA1.Cycles -= Timings.H_Max * 3;

			CPU.V_Counter++;
			if (CPU.V_Counter >= Timings.V_Max)	// V ranges from 0 to Timings.V_Max - 1
			{
				CPU.V_Counter = 0;

				// From byuu:
				// [NTSC]
				// interlace mode has 525 scanlines: 263 on the even frame, and 262 on the odd.
				// non-interlace mode has 524 scanlines: 262 scanlines on both even and odd frames.
				// [PAL] <PAL info is unverified on hardware>
				// interlace mode has 625 scanlines: 313 on the even frame, and 312 on the odd.
				// non-interlace mode has 624 scanlines: 312 scanlines on both even and odd frames.
				if (IPPU.Interlace && S9xInterlaceField())
					Timings.V_Max = Timings.V_Max_Master + 1;	// 263 (NTSC), 313?(PAL)
				else
					Timings.V_Max = Timings.V_Max_Master;		// 262 (NTSC), 312?(PAL)

				Memory.FillRAM[0x213F] ^= 0x80;
				PPU.RangeTimeOver = 0;

				// FIXME: reading $4210 will wait 2 cycles, then perform reading, then wait 4 more cycles.
				Memory.FillRAM[0x4210] = Model->_5A22;

				ICPU.Frame++;
				PPU.HVBeamCounterLatched = 0;
			}

			// From byuu:
			// In non-interlace mode, there are 341 dots per scanline, and 262 scanlines per frame.
			// On odd frames, scanline 240 is one dot short.
			// In interlace mode, there are always 341 dots per scanline. Even frames have 263 scanlines,
			// and odd frames have 262 scanlines.
			// Interlace mode scanline 240 on odd frames is not missing a dot.
			if (CPU.V_Counter == 240 && !IPPU.Interlace && S9xInterlaceField())	// V=240
				Timings.H_Max = Timings.H_Max_Master - ONE_DOT_CYCLE;	// HC=1360
			else
				Timings.H_Max = Timings.H_Max_Master;					// HC=1364

			if (Model->_5A22 == 2)
			{
				if (CPU.V_Counter != 240 || IPPU.Interlace || !S9xInterlaceField())	// V=240
				{
					if (Timings.WRAMRefreshPos == SNES_WRAM_REFRESH_HC_v2 - ONE_DOT_CYCLE)	// HC=534
						Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v2;					// HC=538
					else
						Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v2 - ONE_DOT_CYCLE;	// HC=534
				}
			}
			else
				Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v1;

			if (CPU.V_Counter == PPU.ScreenHeight + FIRST_VISIBLE_LINE)	// VBlank starts from V=225(240).
			{
				S9xEndScreenRefresh();
				#ifdef DEBUGGER
					if (!(CPU.Flags & FRAME_ADVANCE_FLAG))
				#endif
				{
					S9xSyncSpeed();
				}

				CPU.Flags |= SCAN_KEYS_FLAG;

				PPU.HDMA = 0;
				// Bits 7 and 6 of $4212 are computed when read in S9xGetPPU.
			#ifdef DEBUGGER
				missing.dma_this_frame = 0;
			#endif
				IPPU.MaxBrightness = PPU.Brightness;
				PPU.ForcedBlanking = (Memory.FillRAM[0x2100] >> 7) & 1;

				if (!PPU.ForcedBlanking)
				{
					PPU.OAMAddr = PPU.SavedOAMAddr;

					uint8	tmp = 0;

					if (PPU.OAMPriorityRotation)
						tmp = (PPU.OAMAddr & 0xFE) >> 1;
					if ((PPU.OAMFlip & 1) || PPU.FirstSprite != tmp)
					{
						PPU.FirstSprite = tmp;
						IPPU.OBJChanged = TRUE;
					}

					PPU.OAMFlip = 0;
				}

				// FIXME: writing to $4210 will wait 6 cycles.
				Memory.FillRAM[0x4210] = 0x80 | Model->_5A22;
				if (Memory.FillRAM[0x4200] & 0x80)
				{
#ifdef DEBUGGER
					if (Settings.TraceHCEvent)
					    S9xTraceFormattedMessage ("NMI Scheduled for next scanline.");
#endif
					// FIXME: triggered at HC=6, checked just before the final CPU cycle,
					// then, when to call S9xOpcode_NMI()?
					CPU.NMIPending = TRUE;
					Timings.NMITriggerPos = 6 + 6;
				}

			}

			if (CPU.V_Counter == PPU.ScreenHeight + 3)	// FIXME: not true
			{
				if (Memory.FillRAM[0x4200] & 1)
					S9xDoAutoJoypad();
			}

			if (CPU.V_Counter == FIRST_VISIBLE_LINE)	// V=1
				S9xStartScreenRefresh();

			S9xReschedule();

			break;

		case HC_HDMA_INIT_EVENT:
			S9xReschedule();

			if (CPU.V_Counter == 0)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Init     HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				S9xStartHDMA();
			}

			break;

		case HC_RENDER_EVENT:
			if (CPU.V_Counter >= FIRST_VISIBLE_LINE && CPU.V_Counter <= PPU.ScreenHeight)
				RenderLine((uint8) (CPU.V_Counter - FIRST_VISIBLE_LINE));

			S9xReschedule();

			break;

		case HC_WRAM_REFRESH_EVENT:
		#ifdef DEBUGGER
			S9xTraceFormattedMessage("*** WRAM Refresh  HC:%04d", CPU.Cycles);
		#endif

			CPU.Cycles += SNES_WRAM_REFRESH_CYCLES;

			S9xReschedule();

			break;
	}

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event rescheduled (%s)  expected HC:%04d  current  HC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles);
#endif
}
