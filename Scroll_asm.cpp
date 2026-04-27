// scroll_asm.cpp
//

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef SCROLL_FASTCALL
# if defined(_MSC_VER)
#  define SCROLL_FASTCALL __fastcall
# elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(_M_IX86))
#  define SCROLL_FASTCALL __attribute__((fastcall))
# else
#  define SCROLL_FASTCALL
# endif
#endif

#ifndef SCROLL_EXTERN_C
# if defined(__cplusplus)
#  define SCROLL_EXTERN_C extern "C"
# else
#  define SCROLL_EXTERN_C
# endif
#endif

 /*
  * The original MASM module imported/exported C-linkage COFF symbols
  * (_nLVal, _pDungeonCels, @DrawMTileClipTop@4, ...).  Keep that linkage
  * even if this .c file is accidentally compiled as C++.
  */
#ifndef SCROLL_PORT_EXTERN_C_GLOBALS
# define SCROLL_PORT_EXTERN_C_GLOBALS 1
#endif

#ifndef SCROLL_BYTE_DEFINED
typedef uint8_t BYTE;
#define SCROLL_BYTE_DEFINED 1
#endif

#ifndef SCROLL_DWORD_DEFINED
typedef uint32_t DWORD;
#define SCROLL_DWORD_DEFINED 1
#endif

#define WTYPE_NONE        0
#define WTYPE_LEFT        1
#define WTYPE_RIGHT       2
#define WTYPE_ULC         3
#define WTYPE_LRC         4

#define PART_TRANS_NONE   0
#define PART_TRANS_LEFT   1
#define PART_TRANS_RIGHT  2

#define NBUFFW32          800
#define NBUFFW64          832

#ifndef SCROLL_UNUSED
# if defined(__GNUC__) || defined(__clang__)
#  define SCROLL_UNUSED __attribute__((unused))
# else
#  define SCROLL_UNUSED
# endif
#endif

/*
 * Externals from the original engine modules.
 *
 * scroll.asm used these as C-linkage imports, so these declarations must also
 * use C linkage when this file is compiled as C++.  Otherwise MSVC emits
 * unresolved symbols such as ?pSpeedCels@@3PAEA instead of the original
 * _pSpeedCels import.
 *
 * Leave SCROLL_DEFINE_GLOBALS undefined for the normal Diablo build, where
 * scrollrt.cpp, lighting.cpp, and gendung.cpp provide the storage.  Define it
 * in exactly one translation unit only for a standalone harness that does not
 * link those modules.
 */
#if defined(__cplusplus) && SCROLL_PORT_EXTERN_C_GLOBALS
extern "C" {
#endif
#ifndef SCROLL_DEFINE_GLOBALS
	extern int32_t nLVal;
	extern BYTE* glClipY;
	extern int32_t gnPieceNum;
	extern int32_t nTrans;
	extern DWORD gdwPNum;
	extern BYTE gbPartialTrans;

	extern BYTE lightmax;
	extern BYTE* pLightTbl;

	extern DWORD microoffset[];
	extern BYTE* pDungeonCels;
	extern BYTE* pSpeedCels;
	extern BYTE nWTypeTable[];
#else
	int32_t nLVal;
	BYTE* glClipY;
	int32_t gnPieceNum;
	int32_t nTrans;
	DWORD gdwPNum;
	BYTE gbPartialTrans;

	BYTE lightmax;
	BYTE* pLightTbl;

	DWORD microoffset[4096 * 16];
	BYTE* pDungeonCels;
	BYTE* pSpeedCels;
	BYTE nWTypeTable[4096];
#endif
#if defined(__cplusplus) && SCROLL_PORT_EXTERN_C_GLOBALS
}
#endif

#if defined(__cplusplus)
extern "C" {
#endif
	void SCROLL_FASTCALL DrawMTileClipTop(BYTE* pDecodeTo);
	void SCROLL_FASTCALL DrawMTileClipBottom(BYTE* pDecodeTo);
#if defined(__cplusplus)
}
#endif

/* Private data from scroll.asm. */
static DWORD sgLineVal;
static DWORD sgCM;
static BYTE sgWT;
static DWORD* sgT;
static const DWORD* sgMask;
static DWORD sgTimeLow;
static DWORD sgTimeHigh;
static DWORD sgLoopTime;
static DWORD sgUnrollTime;

static const DWORD sgRightMask[32] = {
	0xEAAAAAAAu, 0xF5555555u, 0xFEAAAAAAu, 0xFF555555u,
	0xFFEAAAAAu, 0xFFF55555u, 0xFFFEAAAAu, 0xFFFF5555u,
	0xFFFFEAAAu, 0xFFFFF555u, 0xFFFFFEAAu, 0xFFFFFF55u,
	0xFFFFFFEAu, 0xFFFFFFF5u, 0xFFFFFFFEu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
};

static const DWORD sgLeftMask[32] = {
	0xAAAAAAABu, 0x5555555Fu, 0xAAAAAABFu, 0x555555FFu,
	0xAAAAABFFu, 0x55555FFFu, 0xAAAABFFFu, 0x5555FFFFu,
	0xAAABFFFFu, 0x555FFFFFu, 0xAABFFFFFu, 0x55FFFFFFu,
	0xABFFFFFFu, 0x5FFFFFFFu, 0xBFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
};

static const DWORD sgFullMask[32] = {
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
	0xAAAAAAAAu, 0x55555555u, 0xAAAAAAAAu, 0x55555555u,
};

static const DWORD sgDivBy3MulBy4[48] = {
	 0, 0, 0, 4, 4, 4, 8, 8, 8,12,12,12,16,16,16,
	20,20,20,24,24,24,28,28,28,32,32,32,36,36,36,
	40,40,40,44,44,44,48,48,48,52,52,52,56,56,56,
	60,60,60
};

static const DWORD TotalDataPerLineBottom[17] = {
	  0,  4,  8, 16, 24, 36, 48, 64, 80,
	100,120,144,168,196,224,256,288
};

static const DWORD TotalDataPerLineTop[17] = {
	  0, 32, 60, 88,112,136,156,176,192,
	208,220,232,240,248,252,256,288
};

static SCROLL_UNUSED uint16_t LoadLE16(const BYTE* p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t LoadLE32(const BYTE* p)
{
	return (uint32_t)p[0]
		| ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}

static SCROLL_UNUSED void StoreLE16(BYTE* p, uint16_t v)
{
	p[0] = (BYTE)(v & 0xFFu);
	p[1] = (BYTE)((v >> 8) & 0xFFu);
}

static void StoreLE32(BYTE* p, uint32_t v)
{
	p[0] = (BYTE)(v & 0xFFu);
	p[1] = (BYTE)((v >> 8) & 0xFFu);
	p[2] = (BYTE)((v >> 16) & 0xFFu);
	p[3] = (BYTE)((v >> 24) & 0xFFu);
}

static uintptr_t ClipAddr(void)
{
	return (uintptr_t)glClipY;
}

static int PtrBelowClip(const BYTE* p)
{
	return (uintptr_t)p < ClipAddr();
}

static int BeginRowTop(BYTE* dst)
{
	return !PtrBelowClip(dst);
}

static int BeginRowBottomDraw(BYTE* dst)
{
	return PtrBelowClip(dst);
}

typedef enum PixelMode {
	PIXELS_LIGHT,
	PIXELS_PRETRANS,
	PIXELS_BLACK
} PixelMode;

typedef enum TransMode {
	TRANS_OPAQUE,
	TRANS_DITHER,
	TRANS_HALF_MASK
} TransMode;

typedef enum ClipMode {
	CLIP_TOP_MODE,
	CLIP_BOTTOM_MODE
} ClipMode;

typedef enum PadMode {
	PAD_NONE,
	PAD_PRE,
	PAD_POST
} PadMode;

typedef struct RenderSetup {
	const BYTE* src;
	const BYTE* lightTbl;
	PixelMode pixelMode;
	int shape;
} RenderSetup;

typedef struct RenderCtx {
	const BYTE* src;
	BYTE* dst;
	const BYTE* lightTbl;
	PixelMode pixelMode;
	TransMode transMode;
	ClipMode clipMode;
	const DWORD* maskPtr;
	DWORD maskWord;
	int linePhase;
} RenderCtx;

static const BYTE* GetDungeonCelSource(DWORD pnum)
{
	DWORD cel = pnum & 0x0FFFu;
	DWORD offs = LoadLE32(pDungeonCels + cel * 4u);
	return pDungeonCels + offs;
}

static const BYTE* GetSpeedCelSource(DWORD pnum)
{
	DWORD cel = pnum & 0x0FFFu;
	DWORD idx = (cel << 4) + (DWORD)nLVal;
	return pSpeedCels + microoffset[idx];
}

static void ResolveSpeedCelToNormalPNum(void)
{
	DWORD pnum = gdwPNum;
	DWORD cel = pnum & 0x0FFFu;
	gdwPNum = (pnum & 0xF000u) + microoffset[cel << 4];
}

static int ShapeFromNormalType(DWORD type)
{
	return (type <= 4u) ? (int)type : 5;
}

static int ShapeFromPretransType(DWORD type)
{
	switch (type) {
	case 8:  return 0;
	case 9:  return 1;
	case 10: return 2;
	case 11: return 3;
	case 12: return 4;
	default: return 5;
	}
}

static int ShapeFromBlackType(DWORD type)
{
	return (type <= 4u) ? (int)type : 5;
}

static RenderSetup GetRenderSetup(void)
{
	RenderSetup setup;
	DWORD pnum = gdwPNum;
	BYTE lval = (BYTE)nLVal;

	setup.src = NULL;
	setup.lightTbl = NULL;
	setup.pixelMode = PIXELS_PRETRANS;
	setup.shape = 0;

	sgT = microoffset;

	if (lval != 0) {
		if (lval == lightmax) {
			if ((pnum & 0x8000u) != 0) {
				ResolveSpeedCelToNormalPNum();
				pnum = gdwPNum;
			}
			setup.src = GetDungeonCelSource(pnum);
			setup.pixelMode = PIXELS_BLACK;
			setup.shape = ShapeFromBlackType((pnum >> 12) & 0x07u);
			return setup;
		}

		if ((pnum & 0x8000u) != 0) {
			setup.src = GetSpeedCelSource(pnum);
			setup.pixelMode = PIXELS_PRETRANS;
			setup.shape = ShapeFromPretransType((pnum >> 12) & 0x0Fu);
			return setup;
		}

		setup.src = GetDungeonCelSource(pnum);
		setup.lightTbl = pLightTbl + ((DWORD)nLVal << 8);
		setup.pixelMode = PIXELS_LIGHT;
		setup.shape = ShapeFromNormalType((pnum >> 12) & 0x0Fu);
		return setup;
	}

	/* No light: resolve speed CELs back to the unlighted dungeon CEL. */
	if ((pnum & 0x8000u) != 0) {
		ResolveSpeedCelToNormalPNum();
		pnum = gdwPNum;
	}
	setup.src = GetDungeonCelSource(pnum);
	setup.pixelMode = PIXELS_PRETRANS;
	setup.shape = ShapeFromPretransType(((pnum >> 12) & 0x07u) + 8u);
	return setup;
}

static int PixelSelected(const RenderCtx* ctx, int x)
{
	if (ctx->transMode == TRANS_DITHER) {
		return ((x & 1) != ctx->linePhase);
	}
	if (ctx->transMode == TRANS_HALF_MASK) {
		return (ctx->maskWord & (0x80000000u >> (unsigned)x)) != 0;
	}
	return 1;
}

static void DrawSpan(RenderCtx* ctx, int count, int xStart, PadMode pad, int draw)
{
	int i;
	const BYTE* src;
	BYTE* dst;

	if (count <= 0) {
		return;
	}

	if (pad == PAD_PRE) {
		ctx->src += (count & 2);
	}

	src = ctx->src;
	dst = ctx->dst;

	if (draw) {
		for (i = 0; i < count; i++) {
			if (PixelSelected(ctx, xStart + i)) {
				switch (ctx->pixelMode) {
				case PIXELS_LIGHT:
					dst[i] = ctx->lightTbl[src[i]];
					break;
				case PIXELS_PRETRANS:
					dst[i] = src[i];
					break;
				case PIXELS_BLACK:
					dst[i] = 0;
					break;
				}
			}
		}
	}

	ctx->src = src + count;
	ctx->dst = dst + count;

	if (pad == PAD_POST) {
		ctx->src += ((uintptr_t)ctx->src & 2u);
	}
}

static void FinishRow(RenderCtx* ctx)
{
	if (ctx->transMode == TRANS_DITHER) {
		ctx->linePhase ^= 1;
	}
	else if (ctx->transMode == TRANS_HALF_MASK && ctx->maskPtr != NULL) {
		ctx->maskPtr--;
	}
}

static int StartRow(RenderCtx* ctx, int* draw)
{
	if (ctx->clipMode == CLIP_TOP_MODE) {
		if (!BeginRowTop(ctx->dst)) {
			return 0;
		}
		*draw = 1;
	}
	else {
		*draw = BeginRowBottomDraw(ctx->dst);
	}

	if (ctx->transMode == TRANS_HALF_MASK && ctx->maskPtr != NULL) {
		ctx->maskWord = *ctx->maskPtr;
	}
	else {
		ctx->maskWord = 0xFFFFFFFFu;
	}
	return 1;
}

static int DrawFullRow(RenderCtx* ctx)
{
	int draw;
	if (!StartRow(ctx, &draw)) {
		return 0;
	}
	DrawSpan(ctx, 32, 0, PAD_NONE, draw);
	ctx->dst -= NBUFFW32;
	FinishRow(ctx);
	return 1;
}

static int DrawLeftTriangleRow(RenderCtx* ctx, int leftSkip)
{
	int draw;
	if (!StartRow(ctx, &draw)) {
		return 0;
	}
	ctx->dst += leftSkip;
	DrawSpan(ctx, 32 - leftSkip, leftSkip, PAD_PRE, draw);
	ctx->dst -= NBUFFW32;
	FinishRow(ctx);
	return 1;
}

static int DrawRightTriangleRow(RenderCtx* ctx, int rightSkip)
{
	int draw;
	if (!StartRow(ctx, &draw)) {
		return 0;
	}
	DrawSpan(ctx, 32 - rightSkip, 0, PAD_POST, draw);
	ctx->dst -= NBUFFW32;
	ctx->dst += rightSkip;
	FinishRow(ctx);
	return 1;
}

static int DrawSolidRows(RenderCtx* ctx, int rows)
{
	int row;
	for (row = 0; row < rows; row++) {
		if (!DrawFullRow(ctx)) {
			return 0;
		}
	}
	return 1;
}

static int DrawLeftBottomHalf(RenderCtx* ctx)
{
	int skip;
	for (skip = 30; skip >= 0; skip -= 2) {
		if (!DrawLeftTriangleRow(ctx, skip)) {
			return 0;
		}
	}
	return 1;
}

static int DrawLeftTopHalf(RenderCtx* ctx)
{
	int skip;
	for (skip = 2; skip != 32; skip += 2) {
		if (!DrawLeftTriangleRow(ctx, skip)) {
			return 0;
		}
	}
	return 1;
}

static int DrawRightBottomHalf(RenderCtx* ctx)
{
	int skip;
	for (skip = 30; skip >= 0; skip -= 2) {
		if (!DrawRightTriangleRow(ctx, skip)) {
			return 0;
		}
	}
	return 1;
}

static int DrawRightTopHalf(RenderCtx* ctx)
{
	int skip;
	for (skip = 2; skip != 32; skip += 2) {
		if (!DrawRightTriangleRow(ctx, skip)) {
			return 0;
		}
	}
	return 1;
}

static int DrawRleRows(RenderCtx* ctx)
{
	int row;
	for (row = 0; row < 32; row++) {
		int remaining = 32;
		int x = 0;

		if (ctx->transMode == TRANS_HALF_MASK && ctx->maskPtr != NULL) {
			ctx->maskWord = *ctx->maskPtr;
		}
		else {
			ctx->maskWord = 0xFFFFFFFFu;
		}

		while (remaining != 0) {
			BYTE raw = *ctx->src++;
			if ((raw & 0x80u) != 0) {
				int jump = (int)((BYTE)(0u - raw));
				ctx->dst += jump;
				x += jump;
				remaining -= jump;
			}
			else {
				int count = (int)raw;
				int draw;
				remaining -= count;

				if (ctx->clipMode == CLIP_TOP_MODE) {
					if (!BeginRowTop(ctx->dst)) {
						return 0;
					}
					draw = 1;
				}
				else {
					draw = BeginRowBottomDraw(ctx->dst);
				}

				DrawSpan(ctx, count, x, PAD_NONE, draw);
				x += count;
			}
		}

		ctx->dst -= NBUFFW32;
		FinishRow(ctx);
	}
	return 1;
}

static void RenderShape(BYTE* dst, const RenderSetup* setup, TransMode transMode, ClipMode clipMode, const DWORD* maskStart)
{
	RenderCtx ctx;
	ctx.src = setup->src;
	ctx.dst = dst;
	ctx.lightTbl = setup->lightTbl;
	ctx.pixelMode = setup->pixelMode;
	ctx.transMode = transMode;
	ctx.clipMode = clipMode;
	ctx.maskPtr = maskStart;
	ctx.maskWord = 0xFFFFFFFFu;
	ctx.linePhase = 0;

	switch (setup->shape) {
	case 0: /* Solid 32x32 block. */
		(void)DrawSolidRows(&ctx, 32);
		break;

	case 1: /* 32x32 block with transparent holes. */
		(void)DrawRleRows(&ctx);
		break;

	case 2: /* Left triangle / diamond. */
		if (transMode == TRANS_HALF_MASK) {
			ctx.transMode = TRANS_OPAQUE;
			ctx.maskPtr = NULL;
		}
		if (!DrawLeftBottomHalf(&ctx)) {
			break;
		}
		(void)DrawLeftTopHalf(&ctx);
		break;

	case 3: /* Right triangle / diamond. */
		if (transMode == TRANS_HALF_MASK) {
			ctx.transMode = TRANS_OPAQUE;
			ctx.maskPtr = NULL;
		}
		if (!DrawRightBottomHalf(&ctx)) {
			break;
		}
		(void)DrawRightTopHalf(&ctx);
		break;

	case 4: /* Left triangle to wall. */
		if (transMode == TRANS_HALF_MASK) {
			ctx.transMode = TRANS_OPAQUE;
			ctx.maskPtr = NULL;
		}
		if (!DrawLeftBottomHalf(&ctx)) {
			break;
		}
		if (transMode == TRANS_HALF_MASK) {
			ctx.transMode = TRANS_HALF_MASK;
			ctx.maskPtr = maskStart - 16;
		}
		else {
			ctx.transMode = transMode;
		}
		(void)DrawSolidRows(&ctx, 16);
		break;

	case 5: /* Right triangle to wall. */
	default:
		if (transMode == TRANS_HALF_MASK) {
			ctx.transMode = TRANS_OPAQUE;
			ctx.maskPtr = NULL;
		}
		if (!DrawRightBottomHalf(&ctx)) {
			break;
		}
		if (transMode == TRANS_HALF_MASK) {
			ctx.transMode = TRANS_HALF_MASK;
			ctx.maskPtr = maskStart - 16;
		}
		else {
			ctx.transMode = transMode;
		}
		(void)DrawSolidRows(&ctx, 16);
		break;
	}
}

static void DrawMTileDitherClipTop(BYTE* pDecodeTo)
{
	RenderSetup setup = GetRenderSetup();
	RenderShape(pDecodeTo, &setup, TRANS_DITHER, CLIP_TOP_MODE, NULL);
}

static void DrawMTileHalfDitherClipTop(BYTE* pDecodeTo, const DWORD* mask)
{
	RenderSetup setup = GetRenderSetup();
	RenderShape(pDecodeTo, &setup, TRANS_HALF_MASK, CLIP_TOP_MODE, mask);
}

static void DrawMTileDitherClipBottom(BYTE* pDecodeTo)
{
	RenderSetup setup = GetRenderSetup();
	RenderShape(pDecodeTo, &setup, TRANS_DITHER, CLIP_BOTTOM_MODE, NULL);
}

static void DrawMTileHalfDitherClipBottom(BYTE* pDecodeTo, const DWORD* mask)
{
	RenderSetup setup = GetRenderSetup();
	RenderShape(pDecodeTo, &setup, TRANS_HALF_MASK, CLIP_BOTTOM_MODE, mask);
}

static void DrawMTileNoTransClipTop(BYTE* pDecodeTo)
{
	RenderSetup setup = GetRenderSetup();
	RenderShape(pDecodeTo, &setup, TRANS_OPAQUE, CLIP_TOP_MODE, NULL);
}

static void DrawMTileNoTransClipBottom(BYTE* pDecodeTo)
{
	RenderSetup setup = GetRenderSetup();
	RenderShape(pDecodeTo, &setup, TRANS_OPAQUE, CLIP_BOTTOM_MODE, NULL);
}

SCROLL_EXTERN_C void SCROLL_FASTCALL DrawMTileClipTop(BYTE* pDecodeTo)
{
	BYTE partial;
	BYTE wt;

	if (nTrans != 0) {
		partial = gbPartialTrans;
		if (partial == PART_TRANS_NONE) {
			DrawMTileDitherClipTop(pDecodeTo);
			return;
		}

		if (partial == PART_TRANS_LEFT) {
			wt = nWTypeTable[gnPieceNum];
			if (wt == WTYPE_LEFT || wt == WTYPE_ULC) {
				DrawMTileHalfDitherClipTop(pDecodeTo, &sgLeftMask[31]);
				return;
			}
			/* The original assembly jumps to the PART_TRANS_RIGHT test here;
			 * its following WTYPE_LRC check is unreachable, so it is preserved
			 * as unreachable behavior by falling through to no-trans rendering.
			 */
		}

		if (partial == PART_TRANS_RIGHT) {
			wt = nWTypeTable[gnPieceNum];
			if (wt == WTYPE_RIGHT || wt == WTYPE_ULC) {
				DrawMTileHalfDitherClipTop(pDecodeTo, &sgRightMask[31]);
				return;
			}
			/* Same unreachable WTYPE_LRC path as in scroll.asm. */
		}
	}

	DrawMTileNoTransClipTop(pDecodeTo);
}

SCROLL_EXTERN_C void SCROLL_FASTCALL DrawMTileClipBottom(BYTE* pDecodeTo)
{
	BYTE partial;
	BYTE wt;

	if (nTrans != 0) {
		partial = gbPartialTrans;
		if (partial == PART_TRANS_NONE) {
			DrawMTileDitherClipBottom(pDecodeTo);
			return;
		}

		if (partial == PART_TRANS_LEFT) {
			wt = nWTypeTable[gnPieceNum];
			if (wt == WTYPE_LEFT || wt == WTYPE_ULC) {
				DrawMTileHalfDitherClipBottom(pDecodeTo, &sgLeftMask[31]);
				return;
			}
			/* Preserve the assembly's unreachable WTYPE_LRC check. */
		}

		if (partial == PART_TRANS_RIGHT) {
			wt = nWTypeTable[gnPieceNum];
			if (wt == WTYPE_RIGHT || wt == WTYPE_ULC) {
				DrawMTileHalfDitherClipBottom(pDecodeTo, &sgRightMask[31]);
				return;
			}
			/* Preserve the assembly's unreachable WTYPE_LRC check. */
		}
	}

	DrawMTileNoTransClipBottom(pDecodeTo);
}

 void  DrawBlankMTile(BYTE* pDecodeTo)
{
	BYTE* dst = pDecodeTo;
	int edx = 30;
	int ebx = 1;
	uint32_t zero = 0;

	for (;;) {
		int ecx;
		dst += edx;
		for (ecx = ebx; ecx != 0; ecx--) {
			StoreLE32(dst, zero);
			dst += 4;
		}
		dst += edx;
		dst -= NBUFFW64;
		if (edx == 0) {
			break;
		}
		edx -= 2;
		ebx++;
	}

	edx = 2;
	ebx = 15;
	while (edx != 32) {
		int ecx;
		dst += edx;
		for (ecx = ebx; ecx != 0; ecx--) {
			StoreLE32(dst, zero);
			dst += 4;
		}
		dst += edx;
		dst -= NBUFFW64;
		ebx--;
		edx += 2;
	}
}

/* Keep private symbols referenced so aggressive compilers do not drop the
 * data tables when this file is compiled into projects that inspect them while
 * debugging or compare against the original assembly data segment.
 */
static SCROLL_UNUSED void ScrollPortKeepPrivateDataReferenced(void)
{
	volatile DWORD sink = 0;
	sink ^= sgLineVal;
	sink ^= sgCM;
	sink ^= sgWT;
	sink ^= (DWORD)(uintptr_t)sgT;
	sink ^= (DWORD)(uintptr_t)sgMask;
	sink ^= sgTimeLow ^ sgTimeHigh ^ sgLoopTime ^ sgUnrollTime;
	sink ^= sgRightMask[0] ^ sgLeftMask[0] ^ sgFullMask[0];
	sink ^= sgDivBy3MulBy4[0] ^ TotalDataPerLineBottom[0] ^ TotalDataPerLineTop[0];
	(void)sink;
}
