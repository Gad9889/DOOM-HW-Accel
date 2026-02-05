#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t *DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain(void);

void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
	myargc = argc;
	myargv = argv;

	M_FindResponseFile();

	// Only allocate if DG_ScreenBuffer wasn't already set by hardware accelerator
	if (DG_ScreenBuffer == NULL)
	{
		DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
	}

	DG_Init();

	D_DoomMain();
}
