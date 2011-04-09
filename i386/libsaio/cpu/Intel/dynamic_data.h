/*
 * Several people in the Chameleon community worked on this, but <i>non</i> 
 * of them can claim copyright or ownership, simply because large parts  
 * are in fact copied straight over from the XNU source code.
 *
 * Updates:
 *
 *			- Refactorized for Revolution done by DHP in 2010/2011.
 *			- Core/thread count bug fixed by DHP in Februari 2011.
 *
 * Credits:
 *			- blackosx, DB1, dgsga, FKA, humph, scrax and STLVNUB (testers).
 */

#include "cpu/cpuid.h"
#include "cpu/proc_reg.h"
#include "pci.h"

#define DEFAULT_FSB				100000	// Hardcoded to 100MHz
#define BASE_NHM_CLOCK_SOURCE	133333333ULL

#define DEBUG_CPU_EXTREME		0

//==============================================================================
// DFE: Measures the TSC frequency in Hz (64-bit) using the ACPI PM timer

static uint64_t getTSCFrequency(void)
{
	// DFE: This constant comes from older xnu:
	#define CLKNUM					1193182	// formerly 1193167
	
	// DFE: These two constants come from Linux except CLOCK_TICK_RATE replaced with CLKNUM
	#define CALIBRATE_TIME_MSEC		30
	#define CALIBRATE_LATCH ((CLKNUM * CALIBRATE_TIME_MSEC + 1000/2)/1000)

    uint64_t tscStart;
    uint64_t tscEnd;
    uint64_t tscDelta = 0xffffffffffffffffULL;
    unsigned long pollCount;
    uint64_t retval = 0;
    int i;

    /* Time how many TSC ticks elapse in 30 msec using the 8254 PIT
     * counter 2.  We run this loop 3 times to make sure the cache
     * is hot and we take the minimum delta from all of the runs.
     * That is to say that we're biased towards measuring the minimum
     * number of TSC ticks that occur while waiting for the timer to
     * expire.  That theoretically helps avoid inconsistencies when
     * running under a VM if the TSC is not virtualized and the host
     * steals time.  The TSC is normally virtualized for VMware.
     */
    for (i = 0; i < 10; ++i)
    {
        enable_PIT2();
        set_PIT2_mode0(CALIBRATE_LATCH);
        tscStart = rdtsc64();
        pollCount = poll_PIT2_gate();
        tscEnd = rdtsc64();
        /* The poll loop must have run at least a few times for accuracy */

        if (pollCount <= 1)
		{
            continue;
		}
        /* The TSC must increment at LEAST once every millisecond.  We
         * should have waited exactly 30 msec so the TSC delta should
         * be >= 30.  Anything less and the processor is way too slow.
         */
        if ((tscEnd - tscStart) <= CALIBRATE_TIME_MSEC)
		{
            continue;
		}
        // tscDelta = min(tscDelta, (tscEnd - tscStart))
        if ((tscEnd - tscStart) < tscDelta)
		{
            tscDelta = tscEnd - tscStart;
		}
    }
    /* tscDelta is now the least number of TSC ticks the processor made in
     * a timespan of 0.03 s (e.g. 30 milliseconds)
     * Linux thus divides by 30 which gives the answer in kiloHertz because
     * 1 / ms = kHz.  But we're xnu and most of the rest of the code uses
     * Hz so we need to convert our milliseconds to seconds.  Since we're
     * dividing by the milliseconds, we simply multiply by 1000.
     */

    /* Unlike linux, we're not limited to 32-bit, but we do need to take care
     * that we're going to multiply by 1000 first so we do need at least some
     * arithmetic headroom.  For now, 32-bit should be enough.
     * Also unlike Linux, our compiler can do 64-bit integer arithmetic.
     */
    if (tscDelta > (1ULL << 32))
	{
        retval = 0;
	}
    else
    {
        retval = tscDelta * 1000 / 30;
    }
    disable_PIT2();
    return retval;
}


//==============================================================================
// Copyright by dgobe (i3/i5/i7 bus speed detection).

unsigned long getQPISpeed(uint64_t aFSBFrequency)
{
	int i, nhm_bus = 0;

	static long possible_nhm_bus[] = { 0xFF, 0x7F, 0x3F };

	unsigned long vendorID, deviceID, qpimult, qpiBusSpeed = 0;

	// Nehalem supports Scrubbing. First, locate the PCI bus where the MCH is located
	for (i = 0; i < 3; i++)
	{
		vendorID = (pciConfigRead16(PCIADDR(possible_nhm_bus[i], 3, 4), 0x00) & 0xFFFF);
		deviceID = (pciConfigRead16(PCIADDR(possible_nhm_bus[i], 3, 4), 0x02) & 0xFF00);

		if (vendorID == 0x8086 && deviceID >= 0x2C00)
		{
			nhm_bus = possible_nhm_bus[i];
		}
	}

	if (nhm_bus)
	{
		qpimult = (pciConfigRead32(PCIADDR(nhm_bus, 2, 1), 0x50) & 0x7F);
		qpiBusSpeed = (qpimult * 2 * (aFSBFrequency / 1000000));

		// Rekursor: Rounding decimals to match original Mac profile info.
		if (qpiBusSpeed % 100 != 0)
		{
			qpiBusSpeed = ((qpiBusSpeed + 50) / 100) * 100;
		}
	}

	return qpiBusSpeed;
}


/*==============================================================================
 * Calculates the FSB and CPU frequencies using specific MSRs for each CPU
 * - multi. is read from a specific MSR. In the case of Intel, there is:
 *     a max multi. (used to calculate the FSB freq.),
 *     and a current multi. (used to calculate the CPU freq.)
 * - fsbFrequency = tscFrequency / multi
 * - cpuFrequency = fsbFrequency * multi
 */

void initCPUStruct(void)
{
	uint8_t		maxcoef, maxdiv, currcoef, currdiv;

	uint32_t	qpiSpeed = 0;

	uint64_t	tscFrequency, fsbFrequency, cpuFrequency;
	uint64_t	msr, flex_ratio;

	maxcoef = maxdiv = currcoef = currdiv = 0;

	// Get and cache CPUID data.
	do_cpuid( 0x00000000, gPlatform.CPU.ID[LEAF_0]);			// Vendor-ID and Largest Standard Function (Function 0h).
	do_cpuid( 0x00000001, gPlatform.CPU.ID[LEAF_1]);			// Feature Information (Function 01h).
	do_cpuid( 0x00000002, gPlatform.CPU.ID[LEAF_2]);			// Cache Descriptors (Function 02h).
	do_cpuid2(0x00000004, 0, gPlatform.CPU.ID[LEAF_4]);			// Deterministic Cache Parameters (Function 04h).

	do_cpuid(0x80000000, gPlatform.CPU.ID[LEAF_80]);			// Largest Extended Function # (Function 80000000h).

	if ((getCachedCPUID(LEAF_80, eax) & 0x0000000f) >= 1)
	{
		do_cpuid(0x80000001, gPlatform.CPU.ID[LEAF_81]);		// Extended Feature Bits (Function 80000001h).
	}
#if DEBUG_CPU_EXTREME
	{
		int		i;
		printf("ID Raw Values:\n");

		for (i = 0; i < MAX_CPUID_LEAVES; i++)
		{
			printf("%02d: %08x-%08x-%08x-%08x\n", i, getCachedCPUID(i, eax), getCachedCPUID(i, ebx), 
				   getCachedCPUID(i, ecx), getCachedCPUID(i, edx));
		}

		_CPU_DEBUG_SLEEP(5);

		printf("\n");
	}
#endif

	gPlatform.CPU.Vendor		= getCachedCPUID(LEAF_0, ebx);
	gPlatform.CPU.Signature		= getCachedCPUID(LEAF_1, eax);

	gPlatform.CPU.Stepping		= bitfield32(getCachedCPUID(LEAF_1, eax),  3,  0);
	gPlatform.CPU.Model			= bitfield32(getCachedCPUID(LEAF_1, eax),  7,  4);
	gPlatform.CPU.Family		= bitfield32(getCachedCPUID(LEAF_1, eax), 11,  8);
	gPlatform.CPU.ExtModel		= bitfield32(getCachedCPUID(LEAF_1, eax), 19, 16);
	gPlatform.CPU.ExtFamily		= bitfield32(getCachedCPUID(LEAF_1, eax), 27, 20);

	gPlatform.CPU.Model			+= (gPlatform.CPU.ExtModel << 4);

	//--------------------------------------------------------------------------
	// Intel Processor Brand String support (copied from XNU's cpuid.c)

	if (gPlatform.CPU.ID[LEAF_80][eax] >= 0x80000004)			// Processor Brand String (80000004h).
	{
		uint32_t    reg[4];
		char        str[128], *s;

		/*
		 * The brand/frequency string is defined to be 48 characters long, 47 bytes will contain
		 * characters and the 48th byte is defined to be NULL (0). Processors may return less
		 * than the 47 ASCII characters as long as the string is NULL terminated and the 
		 * processor returns valid data when do_cpuid() is executed with 8000000[2h/3h/4h].
		 */
		do_cpuid(0x80000002, reg);								// Processor Brand String (Function 80000002h) - first 16 bytes.
		bcopy((char *)reg, &str[0], 16);

		// Mobile CPU detection.
		if (strstr((char *)reg, "Atom") != NULL || strstr((char *)reg, "Mobile") != NULL)
		{
			gPlatform.CPU.Mobile = true;
		}

		do_cpuid(0x80000003, reg);								// Processor Brand String (Function 80000003h) - second 16 bytes.
		bcopy((char *)reg, &str[16], 16);

		do_cpuid(0x80000004, reg);								// Processor Brand String (Function 80000004h) - last 16 bytes.
		bcopy((char *)reg, &str[32], 16);

		for (s = str; *s != '\0'; s++)
		{
			if (*s != ' ')
			{
				break;
			}
		}

		strlcpy(gPlatform.CPU.BrandString, s, sizeof(gPlatform.CPU.BrandString));

		// Mobile CPU detection for Core i CPU's.
		if (!gPlatform.CPU.Mobile && strstr(gPlatform.CPU.BrandString, " M ") != NULL)
		{
			gPlatform.CPU.Mobile = true;
		}

		if (!strncmp(gPlatform.CPU.BrandString, CPU_STRING_UNKNOWN, min(sizeof(gPlatform.CPU.BrandString), strlen(CPU_STRING_UNKNOWN) + 1)))
		{
			/*
			 * This string means we have a firmware-programmable brand string,
			 * and the firmware couldn't figure out what sort of CPU we have.
			 */
			gPlatform.CPU.BrandString[0] = '\0';
		}
	}

	//--------------------------------------------------------------------------
	// Setup features.
	gPlatform.CPU.Features |= getCachedCPUID(LEAF_1, ecx);
	gPlatform.CPU.Features |= getCachedCPUID(LEAF_1, edx);
	// Add extended features.
	gPlatform.CPU.Features |= getCachedCPUID(LEAF_81, ecx);
	gPlatform.CPU.Features |= getCachedCPUID(LEAF_81, edx);

	fsbFrequency = 0;
	cpuFrequency = 0;
	tscFrequency = getTSCFrequency();

	// Select a default CPU type (Core 2 Duo / required for Lion).
	gPlatform.CPU.Type = 0x301;

	if (gPlatform.CPU.Vendor == CPU_VENDOR_INTEL)
	{
		if ((gPlatform.CPU.Family == 0x06 && gPlatform.CPU.Model >= 0x0c) || (gPlatform.CPU.Family == 0x0f && gPlatform.CPU.Model >= 0x03))
		{
			uint8_t hiBit = 0;
			
			switch (gPlatform.CPU.Model)
			{
				case CPU_MODEL_DALES_32NM:
				case CPU_MODEL_WESTMERE:
				case CPU_MODEL_WESTMERE_EX:
				case CPU_MODEL_SB_CORE:
					/*
					 * This should be the same as Nehalem but an A0 silicon bug returns
					 * invalid data in the top 12 bits. Hence, we use only bits [19..16]
					 * rather than [31..16] for core count - which actually can't exceed 8. 
					 */		
					hiBit = 19;
					break;
					
				case CPU_MODEL_NEHALEM:
				case CPU_MODEL_FIELDS:
				case CPU_MODEL_DALES:
				case CPU_MODEL_NEHALEM_EX:
					hiBit = 31;
					break;
			}

			// ---------------------------------------------------------------------------
			// When hiBit is set (to either 19 or 31) then we know that we are running on a 
			// CPU model that matched with one of the models in the above switch statement.

			if (hiBit)
			{
				//--------------------------------------------------------------------------
				// Get core and thread count - the new way (from: xnu/osfmk/i386/cpuid.c)

				msr = rdmsr64(MSR_CORE_THREAD_COUNT); // 0x035
				
				gPlatform.CPU.NumCores		= bitfield32(msr, hiBit, 16);
				gPlatform.CPU.NumThreads	= bitfield32(msr, 15,  0);

				// Getting 'cpu-type' for SMBIOS later on. 
				if (strstr(gPlatform.CPU.BrandString, "Core(TM) i7-2"))
				{
					gPlatform.CPU.Type =  0x307;	// Core i7-2xxx(X) for Sandy Bridge.
				}
				else if (strstr(gPlatform.CPU.BrandString, "Core(TM) i5"))
				{
					gPlatform.CPU.Type = 0x601;		// Core i5
				}
				else if (strstr(gPlatform.CPU.BrandString, "Core(TM) i3"))
				{
					gPlatform.CPU.Type =  0x901;	// Core i3
				}
				else
				{
					gPlatform.CPU.Type = 0x0701;	// Core i7
				}

#if DEBUG_CPU_TURBO_RATIO
				// Get turbo values of all cores.
				msr = rdmsr64(MSR_TURBO_RATIO_LIMIT);
				// Extends our CPU structure (defined in platform.h)
				gPlatform.CPU.CoreTurboRatio[gPlatform.CPU.NumCores] = 0;

				// All CPU's have at least two cores (think mobility CPU here).
				gPlatform.CPU.CoreTurboRatio[0] = bitfield32(msr, 7, 0);
				gPlatform.CPU.CoreTurboRatio[1] = bitfield32(msr, 15, 8);

				// Additionally for quad and six core CPU's.
				if (gPlatform.CPU.NumCores >= 4)
				{
					gPlatform.CPU.CoreTurboRatio[2] = bitfield32(msr, 23, 16);
					gPlatform.CPU.CoreTurboRatio[3] = bitfield32(msr, 31, 24);

					// For the lucky few with a six core Gulftown CPU.
					if (gPlatform.CPU.NumCores >= 6)
					{
						// bitfield32() supports 32 bit values only and thus we 
 						// have to do it a little different here (bit shifting).
						gPlatform.CPU.CoreTurboRatio[4] = ((msr >> 32) & 0xff);
						gPlatform.CPU.CoreTurboRatio[5] = ((msr >> 40) & 0xff);
					}
				}
#endif
				msr = rdmsr64(MSR_PLATFORM_INFO);

				_CPU_DEBUG_DUMP("msr(%d): platform_info %08x\n", __LINE__, (unsigned) msr & 0xffffffff);
 
				currcoef = (msr >> 8) & 0xff;
				msr = rdmsr64(MSR_FLEX_RATIO);

				_CPU_DEBUG_DUMP("msr(%d): flex_ratio %08x\n", __LINE__, (unsigned) msr & 0xffffffff);

				if ((msr >> 16) & 0x01)
				{
					flex_ratio = (msr >> 8) & 0xff;

					if (currcoef > flex_ratio)
					{
						currcoef = flex_ratio;
					}
				}

				if (currcoef)
				{
					fsbFrequency = (tscFrequency / currcoef);
				}

				cpuFrequency = tscFrequency;
				
				qpiSpeed = getQPISpeed(fsbFrequency);
			} 
			else // For all other (mostly older) Intel CPU models.
			{
				//--------------------------------------------------------------------------
				// Get core and thread count - the old way.

				/* Indicates the maximum number of addressable ID for logical processors in a physical package. 
				 * Within a physical package, there may be addressable IDs that are not occupied by any logical 
				 * processors. This parameter does not represents the hardware capability of the physical processor.
				 *
				 * Note: BIOS may reduce the number of logical processors to less than the number of physical 
				 *		 packages times the number of hardware-capable logical processors per package.
				 */
				gPlatform.CPU.NumThreads	= bitfield32(getCachedCPUID(LEAF_1, ebx), 23, 16);
				
				/* Addressable IDs for processor cores in the same Package
				 *
				 * Note: Software must check ID for its support of leaf 4 when implementing support 
				 *		 for multi-core. If ID leaf 4 is not available at runtime, software should 
				 *		 * handle the situation as if there is only one core per package.
				 */
				gPlatform.CPU.NumCores		= bitfield32(getCachedCPUID(LEAF_4, eax), 31, 26) + 1;

				msr = rdmsr64(MSR_IA32_PERF_STATUS);

				_CPU_DEBUG_DUMP("msr(%d): ia32_perf_stat 0x%08x\n", __LINE__, (unsigned) msr & 0xffffffff);

				currcoef = (msr >> 8) & 0x1f;
				// Non-integer bus ratio for the max-multi.
				maxdiv = (msr >> 46) & 0x01;
				// Non-integer bus ratio for the current-multi.
				currdiv = (msr >> 14) & 0x01;
				
				if ((gPlatform.CPU.Family == 0x06 && gPlatform.CPU.Model >= 0x0e) || (gPlatform.CPU.Family == 0x0f)) // This will always be model >= 3
				{
					// On these models, maxcoef defines TSC frequency.
					maxcoef = (msr >> 40) & 0x1f;
				}
				else
				{
					// On lower models, currcoef defines TSC frequency.
					maxcoef = currcoef;
				}

				if (maxcoef)
				{
					if (maxdiv)
					{
						fsbFrequency = ((tscFrequency * 2) / ((maxcoef * 2) + 1));
					}
					else
					{
						fsbFrequency = (tscFrequency / maxcoef);
					}

					if (currdiv)
					{
						cpuFrequency = (fsbFrequency * ((currcoef * 2) + 1) / 2);
					}
					else
					{
						cpuFrequency = (fsbFrequency * currcoef);
					}

					// _CPU_DEBUG_DUMP("max: %d%s current: %d%s\n", maxcoef, maxdiv ? ".5" : "", currcoef, currdiv ? ".5" : "");
				}
			}
		}
	}

	if (!fsbFrequency)
	{
		fsbFrequency = (DEFAULT_FSB * 1000);
		cpuFrequency = tscFrequency;

		_CPU_DEBUG_DUMP("0 ! using the default value for FSB !\n");
	}

	// Do we have a 'cpu-type' already?
	if (gPlatform.CPU.Type == 0x301)		// Intel Atom, Core 2 Solo and Core Duo processors.
	{
		if (gPlatform.CPU.NumCores >= 4)
		{
			gPlatform.CPU.Type = 0x0501;	// Intel Quad-Core Xeon (or similar).
		}
		else if (gPlatform.CPU.NumCores == 1 && gPlatform.CPU.NumThreads == 1)
		{
			gPlatform.CPU.Type = 0x0201;	// Intel Core Solo (old Mac Mini's).
		}
	}
	
	gPlatform.CPU.MaxCoef		= maxcoef;
	gPlatform.CPU.MaxDiv		= maxdiv;
	gPlatform.CPU.CurrCoef		= currcoef;
	gPlatform.CPU.CurrDiv		= currdiv;
	gPlatform.CPU.TSCFrequency	= tscFrequency;
	gPlatform.CPU.FSBFrequency	= fsbFrequency;
	gPlatform.CPU.CPUFrequency	= cpuFrequency;
	gPlatform.CPU.QPISpeed		= qpiSpeed;

	_CPU_DEBUG_DUMP("                            123456789 123456789 123456789 123456789 12345678\n");
	_CPU_DEBUG_DUMP("CPU: Brandstring          : %s\n",				gPlatform.CPU.BrandString);
	_CPU_DEBUG_DUMP("CPU: Vendor/Model/ExtModel: 0x%x/0x%x/0x%x\n",	gPlatform.CPU.Vendor, gPlatform.CPU.Model, gPlatform.CPU.ExtModel);
	_CPU_DEBUG_DUMP("CPU: Stepping / Signature : 0x%x/0x%x\n",		gPlatform.CPU.Stepping, gPlatform.CPU.Signature);
	_CPU_DEBUG_DUMP("CPU: Family/ExtFamily     : 0x%x/0x%x\n",		gPlatform.CPU.Family, gPlatform.CPU.ExtFamily);
	_CPU_DEBUG_DUMP("CPU: Type                 : 0x%x\n",			gPlatform.CPU.Type);
	_CPU_DEBUG_DUMP("CPU: Mobile CPU           : %s\n",				gPlatform.CPU.Mobile ? "true" : "false");
	_CPU_DEBUG_DUMP("CPU: NumCores/NumThreads  : %d/%d\n",			gPlatform.CPU.NumCores, gPlatform.CPU.NumThreads);

#if DEBUG_CPU_TURBO_RATIO
	int core = 0;
	char div[] = "-------------------------------------\n";

	_CPU_DEBUG_DUMP("%s", div);

	for (; core < gPlatform.CPU.NumCores; core++)
	{
		_CPU_DEBUG_DUMP("CPU: Max Turbo with %d core%s: %d00MHz\n", (core + 1), core > 1 ? "s" : " ", gPlatform.CPU.CoreTurboRatio[core]);
	}

	_CPU_DEBUG_DUMP("%s", div);
#endif

	_CPU_DEBUG_DUMP("CPU: Features             : 0x%08x\n",			gPlatform.CPU.Features);
	_CPU_DEBUG_DUMP("CPU: MaxCoef/CurrCoef     : %d%s/%d%s\n",		gPlatform.CPU.MaxCoef, gPlatform.CPU.MaxDiv ? ".5" : "",
																	gPlatform.CPU.CurrCoef, gPlatform.CPU.CurrDiv ? ".5" : "");
	_CPU_DEBUG_DUMP("CPU: MaxDiv/CurrDiv       : 0x%x/0x%x\n",		gPlatform.CPU.MaxDiv, gPlatform.CPU.CurrDiv);
	_CPU_DEBUG_DUMP("CPU: TSCFreq              : %dMHz\n",			gPlatform.CPU.TSCFrequency / 1000000);
	_CPU_DEBUG_DUMP("CPU: FSBFreq              : %dMHz\n",			gPlatform.CPU.FSBFrequency / 1000000);
	_CPU_DEBUG_DUMP("CPU: CPUFreq              : %dMHz\n",			gPlatform.CPU.CPUFrequency / 1000000);
	_CPU_DEBUG_DUMP("CPU: QPISpeed             : %x\n",				gPlatform.CPU.QPISpeed);
	_CPU_DEBUG_SLEEP(15);
}