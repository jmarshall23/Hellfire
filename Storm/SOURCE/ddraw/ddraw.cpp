#include "DirectDrawWrapper.h"
#include "resource.h"

#include <stdio.h>
#include <math.h>

// Main thread ddrawwrapper object
IDirectDrawWrapper *lpDD = NULL;
// Original setcursorpos function pointer
static BOOL (WINAPI *TrueSetCursorPos)(int,int) = SetCursorPos;
static HMODULE (WINAPI *TrueLoadLibraryA)(LPCSTR) = GetModuleHandleA;

// Are we in the settings menu
BOOL inMenu;
// Dll start time
DWORD start_time;
// The level of debug to display
int debugLevel;
//debug display mode (-1 = none, 0 = console, 1 = file)
int debugDisplay;
//the debug file handle
FILE *debugFile;

// Dll hmodule
HMODULE hMod;

/* Helper function for throwing debug/error messages
 *
 * int level      - Debug level
 * char *location - Message location
 * char *message  - Message
 */
void debugMessage(int level, char *location, char *message)
{
	// If above the current level then skip totally
	if(level > debugLevel) return;

    // Calculate HMS
	DWORD cur_time = GetTickCount() - start_time;
	long hours = (long)floor((double)cur_time / (double)3600000.0);
	cur_time -= (hours * 3600000);
	int minutes = (int)floor((double)cur_time / (double)60000.0);
	cur_time -= (minutes * 60000);
	double seconds = (double)cur_time / (double)1000.0;

    // Build error message
	char text[4096] = "\0";
	if(level == 0)
	{
		sprintf_s(text, 4096, "%d:%d:%#.1f ERR %s %s\n", hours, minutes, seconds, location, message);
	}
	else if(level == 1)
	{
		sprintf_s(text, 4096, "%d:%d:%#.1f WRN %s %s\n", hours, minutes, seconds, location, message);
	}
	else if(level == 2)
	{
		sprintf_s(text, 4096, "%d:%d:%#.1f INF %s %s\n", hours, minutes, seconds, location, message);
	}
    // Output and flush
	printf_s(text);
	fflush(stdout);
}

// Override function for cursor position
BOOL WINAPI OverrideSetCursorPos(int X, int Y)
{
	// If ddraw object exists and windowed mode
	if(lpDD != NULL && lpDD->isWindowed)
	{
		// X,Y are relative to client area within the code
		// Get client area location
		POINT cpos;
		cpos.x = 0;
		cpos.y = 0;
		ClientToScreen(lpDD->hWnd, &cpos);

		// Calculate correct cursor offset and move
		BOOL res = TrueSetCursorPos(cpos.x + X, cpos.y + Y);
		return res;
	}
	return TrueSetCursorPos(X, Y);
}

// Override function for load library
/*HMODULE WINAPI OverrideLoadLibraryA(LPCSTR lpModuleName)
{
	printf_s("%s\n", lpModuleName);
	return TrueLoadLibraryA(lpModuleName);
}*/

// Emulated direct draw create
extern "C" BOOL APIENTRY SDirectDrawCreate(GUID FAR* lpGUID, LPDIRECTDRAW FAR* lplpDD, IUnknown FAR* pUnkOuter)
{
	// Create directdraw object
	lpDD = new IDirectDrawWrapper();
	if(lpDD == NULL) 
	{
		debugMessage(0, "DirectDrawCreate", "Failed to create IDirectDrawWrapper.");
		return DDERR_OUTOFMEMORY;  // Simulate OOM error
	}

	// Set return pointer to the newly created DirectDrawWrapper interface
	*lplpDD = (LPDIRECTDRAW)lpDD;
	lpDD->WrapperInitialize(NULL, GetModuleHandleA(NULL));
	
	// Return success
	return DD_OK;
}