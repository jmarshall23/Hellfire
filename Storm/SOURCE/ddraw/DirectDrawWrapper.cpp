#include "DirectDrawWrapper.h"
#include "resource.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

#ifndef SAFE_DELETE_ARRAY
#define SAFE_DELETE_ARRAY(x) do { if ((x) != NULL) { delete[] (x); (x) = NULL; } } while (0)
#endif

#ifndef SAFE_DELETE
#define SAFE_DELETE(x) do { if ((x) != NULL) { delete (x); (x) = NULL; } } while (0)
#endif

#ifndef DD_OK
#define DD_OK S_OK
#endif

static const char* g_BlitVS_HLSL = R"(
struct VS_IN
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(VS_IN input)
{
    VS_OUT o;
    o.pos = float4(input.pos, 1.0f);
    o.uv = input.uv;
    return o;
}
)";
static const char* g_BlitPS_HLSL = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

struct PS_IN
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float Luma(float3 c)
{
    return dot(c, float3(0.299f, 0.587f, 0.114f));
}

float3 SoftContrast(float3 c)
{
    return saturate(c * c * (3.0f - 2.0f * c));
}

float Vignette(float2 uv)
{
    float2 p = uv * 2.0f - 1.0f;
    float r2 = dot(p, p);
    return saturate(1.0f - r2 * 0.22f);
}

float GrainRand(float2 p)
{
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453123f);
}

float GrainValue(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);

    float a = GrainRand(i);
    float b = GrainRand(i + float2(1.0f, 0.0f));
    float c = GrainRand(i + float2(0.0f, 1.0f));
    float d = GrainRand(i + float2(1.0f, 1.0f));

    float2 u = f * f * (3.0f - 2.0f * f);

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float FilmGrain(float2 uv, float2 resolution)
{
    float2 p = uv * resolution;

    float g1 = GrainValue(p * 0.75f);
    float g2 = GrainValue(p * 1.50f);
    float g3 = GrainValue(p * 3.00f);

    float grain = g1 * 0.55f + g2 * 0.30f + g3 * 0.15f;
    return grain - 0.5f;
}

float3 BrightPass(float3 c, float lo, float hi)
{
    float lum = Luma(c);
    float mask = smoothstep(lo, hi, lum);
    return c * mask;
}

float4 main(PS_IN input) : SV_Target
{
    uint texWidth, texHeight;
    tex0.GetDimensions(texWidth, texHeight);

    float2 px = float2(1.0f / (float)texWidth, 1.0f / (float)texHeight);
    float2 uv = input.uv;

    // baked constants
    const float sharpenStrength   = 0.95f;
    const float contrastStrength  = 0.62f;

    // denoise tuning
    const float denoiseStrength   = 0.22f;
    const float denoiseSigma      = 24.0f;

    // dark-area tuning
    const float darkStart         = 0.42f;
    const float darkEnd           = 0.10f;
    const float darkStrength      = 0.12f;

    // smart brightness tuning
    const float brightnessLift    = 0.20f;
    const float brightnessLowMax  = 0.85f;

    // bloom tuning
    const float bloomStrength     = 0.78f;
    const float bloomThresholdLo  = 0.18f;
    const float bloomThresholdHi  = 0.96f;

    // pseudo-AO tuning
    const float aoStrength        = 0.58f;
    const float aoRadius1         = 1.5f;
    const float aoRadius2         = 3.0f;
    const float aoDarkBoost       = 0.65f;

    // 3x3 neighborhood
    float3 c00 = tex0.Sample(samp0, uv + float2(-px.x, -px.y)).rgb;
    float3 c10 = tex0.Sample(samp0, uv + float2( 0.0f, -px.y)).rgb;
    float3 c20 = tex0.Sample(samp0, uv + float2( px.x, -px.y)).rgb;

    float3 c01 = tex0.Sample(samp0, uv + float2(-px.x,  0.0f)).rgb;
    float3 c11 = tex0.Sample(samp0, uv).rgb;
    float3 c21 = tex0.Sample(samp0, uv + float2( px.x,  0.0f)).rgb;

    float3 c02 = tex0.Sample(samp0, uv + float2(-px.x,  px.y)).rgb;
    float3 c12 = tex0.Sample(samp0, uv + float2( 0.0f,  px.y)).rgb;
    float3 c22 = tex0.Sample(samp0, uv + float2( px.x,  px.y)).rgb;

    // Luma values
    float l00 = Luma(c00);
    float l10 = Luma(c10);
    float l20 = Luma(c20);
    float l01 = Luma(c01);
    float l11 = Luma(c11);
    float l21 = Luma(c21);
    float l02 = Luma(c02);
    float l12 = Luma(c12);
    float l22 = Luma(c22);

    // -------------------------------------------------------------------------
    // Edge-aware denoise
    // -------------------------------------------------------------------------
    float invSigma = 1.0f / denoiseSigma;

    float w00 = 1.0f / (1.0f + abs(l00 - l11) * invSigma);
    float w10 = 1.0f / (1.0f + abs(l10 - l11) * invSigma);
    float w20 = 1.0f / (1.0f + abs(l20 - l11) * invSigma);
    float w01 = 1.0f / (1.0f + abs(l01 - l11) * invSigma);
    float w21 = 1.0f / (1.0f + abs(l21 - l11) * invSigma);
    float w02 = 1.0f / (1.0f + abs(l02 - l11) * invSigma);
    float w12 = 1.0f / (1.0f + abs(l12 - l11) * invSigma);
    float w22 = 1.0f / (1.0f + abs(l22 - l11) * invSigma);

    float wCenter = 4.0f;

    float3 denoised =
        c00 * w00 + c10 * w10 + c20 * w20 +
        c01 * w01 + c11 * wCenter + c21 * w21 +
        c02 * w02 + c12 * w12 + c22 * w22;

    float wSum =
        w00 + w10 + w20 +
        w01 + wCenter + w21 +
        w02 + w12 + w22;

    denoised /= max(wSum, 1e-5f);

    float3 baseColor = lerp(c11, denoised, denoiseStrength);
    float baseLuma = Luma(baseColor);

    // -------------------------------------------------------------------------
    // Blur / sharpen pipeline
    // -------------------------------------------------------------------------
    float3 blur =
        (c00 + 2.0f * c10 + c20 +
         2.0f * c01 + 4.0f * baseColor + 2.0f * c21 +
         c02 + 2.0f * c12 + c22) / 16.0f;

    float lBlur =
        (l00 + 2.0f * l10 + l20 +
         2.0f * l01 + 4.0f * baseLuma + 2.0f * l21 +
         l02 + 2.0f * l12 + l22) / 16.0f;

    float detail = baseLuma - lBlur;

    float edge =
        abs(l10 - l12) +
        abs(l01 - l21) +
        0.5f * abs(l00 - l22) +
        0.5f * abs(l20 - l02);

    float adapt = 1.0f - saturate(edge * 4.0f);
    adapt = adapt * adapt;

    float amount = sharpenStrength * adapt;

    float3 color = baseColor + detail.xxx * amount;

    // Mild contrast boost
    float3 contrasted = SoftContrast(saturate(color));
    color = lerp(color, contrasted, contrastStrength);

    // -------------------------------------------------------------------------
    // Pseudo AO from neighborhood enclosure / crevices
    // -------------------------------------------------------------------------
    {
        float2 r1 = px * aoRadius1;
        float2 r2 = px * aoRadius2;

        float3 a0  = tex0.Sample(samp0, uv + float2(-r1.x,  0.0f)).rgb;
        float3 a1  = tex0.Sample(samp0, uv + float2( r1.x,  0.0f)).rgb;
        float3 a2  = tex0.Sample(samp0, uv + float2( 0.0f, -r1.y)).rgb;
        float3 a3  = tex0.Sample(samp0, uv + float2( 0.0f,  r1.y)).rgb;
        float3 a4  = tex0.Sample(samp0, uv + float2(-r1.x, -r1.y)).rgb;
        float3 a5  = tex0.Sample(samp0, uv + float2( r1.x, -r1.y)).rgb;
        float3 a6  = tex0.Sample(samp0, uv + float2(-r1.x,  r1.y)).rgb;
        float3 a7  = tex0.Sample(samp0, uv + float2( r1.x,  r1.y)).rgb;

        float3 b0  = tex0.Sample(samp0, uv + float2(-r2.x,  0.0f)).rgb;
        float3 b1  = tex0.Sample(samp0, uv + float2( r2.x,  0.0f)).rgb;
        float3 b2  = tex0.Sample(samp0, uv + float2( 0.0f, -r2.y)).rgb;
        float3 b3  = tex0.Sample(samp0, uv + float2( 0.0f,  r2.y)).rgb;

        float la0 = Luma(a0);
        float la1 = Luma(a1);
        float la2 = Luma(a2);
        float la3 = Luma(a3);
        float la4 = Luma(a4);
        float la5 = Luma(a5);
        float la6 = Luma(a6);
        float la7 = Luma(a7);

        float lb0 = Luma(b0);
        float lb1 = Luma(b1);
        float lb2 = Luma(b2);
        float lb3 = Luma(b3);

        float nearAvg = (la0 + la1 + la2 + la3 + la4 + la5 + la6 + la7) * (1.0f / 8.0f);
        float farAvg  = (lb0 + lb1 + lb2 + lb3) * 0.25f;

        // if center is darker than nearby samples, treat as enclosed/occluded
        float enclosedNear = saturate((nearAvg - baseLuma) * 2.4f);
        float enclosedFar  = saturate((farAvg  - baseLuma) * 1.8f);

        // strengthen inside little crevices and cracks
        float creviceAO = saturate((-detail) * 7.0f);

        // stronger in darker regions, weaker in highlights
        float darkAO = 1.0f - smoothstep(0.18f, 0.80f, baseLuma);

        float ao = enclosedNear * 0.50f +
                   enclosedFar  * 0.25f +
                   creviceAO    * 0.25f;

        ao *= lerp(1.0f, darkAO, aoDarkBoost);
        ao = saturate(ao);
        ao *= ao; // softer rolloff

        color *= (1.0f - ao * aoStrength);
    }

    // Dark-region deepening
    float lum = Luma(saturate(color));
    float darkMask = 1.0f - smoothstep(darkEnd, darkStart, lum);
    darkMask *= darkMask;

    float creviceMask = saturate((-detail) * 6.0f);
    darkMask *= lerp(1.0f, creviceMask, 0.65f);

    color *= (1.0f - darkMask * darkStrength);

    // Slight desaturation for grimier Diablo-like look
    color = lerp(color, Luma(color).xxx, 0.12f);

    // Cool shadows / warm mids
    {
        float gradedLum = Luma(color);
        float shadowMask = 1.0f - smoothstep(0.05f, 0.35f, gradedLum);
        float midMask = smoothstep(0.18f, 0.55f, gradedLum) *
                        (1.0f - smoothstep(0.55f, 0.85f, gradedLum));

        color = lerp(color, color * float3(0.92f, 0.95f, 1.04f), shadowMask * 0.18f);
        color = lerp(color, color * float3(1.04f, 1.00f, 0.94f), midMask * 0.10f);
    }

    // Slight highlight softening so bright areas don't feel too modern/clean
    {
        float hl = smoothstep(0.65f, 1.0f, Luma(color));
        color = lerp(color, blur, hl * 0.08f);
    }

    // Smart brightness lift
    {
        float brightLum = Luma(saturate(color));
        float brightMask = 1.0f - smoothstep(0.0f, brightnessLowMax, brightLum);
        brightMask = 0.35f + brightMask * 0.65f;

        color *= (1.0f + brightnessLift * brightMask);
    }

    // -------------------------------------------------------------------------
    // Advanced multi-radius bloom
    // -------------------------------------------------------------------------
    {
        float2 off1 = px * 4.0f;
        float2 off2 = px * 8.0f;
        float2 off3 = px * 10.0f;

        float3 bloomAccum = 0.0f.xxx;
        float bloomWeight = 0.0f;

        {
            float3 s = BrightPass(c11, bloomThresholdLo, bloomThresholdHi);
            bloomAccum += s * 1.20f;
            bloomWeight += 1.20f;
        }

        {
            float3 s0 = BrightPass(tex0.Sample(samp0, uv + float2(-off1.x, 0.0f)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s1 = BrightPass(tex0.Sample(samp0, uv + float2( off1.x, 0.0f)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s2 = BrightPass(tex0.Sample(samp0, uv + float2(0.0f, -off1.y)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s3 = BrightPass(tex0.Sample(samp0, uv + float2(0.0f,  off1.y)).rgb, bloomThresholdLo, bloomThresholdHi);

            bloomAccum += (s0 + s1 + s2 + s3) * 0.90f;
            bloomWeight += 4.0f * 0.90f;
        }

        {
            float3 s0 = BrightPass(tex0.Sample(samp0, uv + float2(-off1.x, -off1.y)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s1 = BrightPass(tex0.Sample(samp0, uv + float2( off1.x, -off1.y)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s2 = BrightPass(tex0.Sample(samp0, uv + float2(-off1.x,  off1.y)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s3 = BrightPass(tex0.Sample(samp0, uv + float2( off1.x,  off1.y)).rgb, bloomThresholdLo, bloomThresholdHi);

            bloomAccum += (s0 + s1 + s2 + s3) * 0.72f;
            bloomWeight += 4.0f * 0.72f;
        }

        {
            float3 s0 = BrightPass(tex0.Sample(samp0, uv + float2(-off2.x, 0.0f)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s1 = BrightPass(tex0.Sample(samp0, uv + float2( off2.x, 0.0f)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s2 = BrightPass(tex0.Sample(samp0, uv + float2(0.0f, -off2.y)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s3 = BrightPass(tex0.Sample(samp0, uv + float2(0.0f,  off2.y)).rgb, bloomThresholdLo, bloomThresholdHi);

            bloomAccum += (s0 + s1 + s2 + s3) * 0.52f;
            bloomWeight += 4.0f * 0.52f;
        }

        {
            float3 s0 = BrightPass(tex0.Sample(samp0, uv + float2(-off3.x, 0.0f)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s1 = BrightPass(tex0.Sample(samp0, uv + float2( off3.x, 0.0f)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s2 = BrightPass(tex0.Sample(samp0, uv + float2(0.0f, -off3.y)).rgb, bloomThresholdLo, bloomThresholdHi);
            float3 s3 = BrightPass(tex0.Sample(samp0, uv + float2(0.0f,  off3.y)).rgb, bloomThresholdLo, bloomThresholdHi);

            bloomAccum += (s0 + s1 + s2 + s3) * 0.30f;
            bloomWeight += 4.0f * 0.30f;
        }

        float3 bloom = bloomAccum / max(bloomWeight, 1e-5f);

        bloom *= float3(1.10f, 1.02f, 0.90f);

        float bloomLum = Luma(bloom);
        float bloomMask = smoothstep(0.02f, 0.75f, bloomLum);
        bloom *= (0.65f + bloomMask * 0.35f);

        color += bloom * bloomStrength;
    }

    // Film-style grain
    {
        float2 resolution = float2((float)texWidth, (float)texHeight);
        float grain = FilmGrain(uv, resolution);

        float lumNow = Luma(saturate(color));
        float grainMask = 1.0f - smoothstep(0.35f, 0.90f, lumNow);
        grainMask = 0.35f + grainMask * 0.65f;

        color += grain.xxx * (0.052f * grainMask);
    }

    // Subtle vignette for enclosed dungeon feel
    color *= Vignette(uv);

    return float4(saturate(color), 1.0f);
}
)";

static bool CompileShaderSource(const char* source, const char* entry, const char* target, ID3DBlob** blobOut)
{
	if (!source || !entry || !target || !blobOut)
		return false;

	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	ComPtr<ID3DBlob> shaderBlob;
	ComPtr<ID3DBlob> errorBlob;

	HRESULT hr = D3DCompile(
		source,
		strlen(source),
		nullptr,
		nullptr,
		nullptr,
		entry,
		target,
		flags,
		0,
		&shaderBlob,
		&errorBlob);

	if (FAILED(hr))
	{
		if (errorBlob)
			OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
		return false;
	}

	*blobOut = shaderBlob.Detach();
	return true;
}

static inline float PixelToNdcX(float x, float width)
{
	return (x / width) * 2.0f - 1.0f;
}

static inline float PixelToNdcY(float y, float height)
{
	return 1.0f - (y / height) * 2.0f;
}

static bool SaveBGRA8TextureToPng(
	ID3D11Device* device,
	ID3D11DeviceContext* context,
	ID3D11Texture2D* texture,
	const wchar_t* filename)
{
	if (!device || !context || !texture || !filename)
		return false;

	D3D11_TEXTURE2D_DESC srcDesc = {};
	texture->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;

	ComPtr<ID3D11Texture2D> staging;
	HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
	if (FAILED(hr))
		return false;

	context->CopyResource(staging.Get(), texture);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr))
		return false;

	ComPtr<IWICImagingFactory> factory;
	hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	ComPtr<IWICStream> stream;
	hr = factory->CreateStream(&stream);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	hr = stream->InitializeFromFilename(filename, GENERIC_WRITE);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	ComPtr<IWICBitmapEncoder> encoder;
	hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> props;
	hr = encoder->CreateNewFrame(&frame, &props);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	hr = frame->Initialize(props.Get());
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	hr = frame->SetSize(srcDesc.Width, srcDesc.Height);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
	hr = frame->SetPixelFormat(&format);
	if (FAILED(hr))
	{
		context->Unmap(staging.Get(), 0);
		return false;
	}

	hr = frame->WritePixels(
		srcDesc.Height,
		mapped.RowPitch,
		mapped.RowPitch * srcDesc.Height,
		reinterpret_cast<BYTE*>(mapped.pData));

	context->Unmap(staging.Get(), 0);

	if (FAILED(hr))
		return false;

	hr = frame->Commit();
	if (FAILED(hr))
		return false;

	hr = encoder->Commit();
	if (FAILED(hr))
		return false;

	return true;
}

/*******************
**IUnknown methods**
********************/

HRESULT __stdcall IDirectDrawWrapper::QueryInterface(REFIID riid, LPVOID FAR* ppvObj)
{
	debugMessage(1, "IDirectDrawWrapper::QueryInterface", "Partially Implemented");

	if (ppvObj == NULL)
		return E_POINTER;

	*ppvObj = NULL;

	if (riid == IID_IUnknown ||
		riid == IID_IDirectDraw ||
		riid == IID_IDirectDraw2 ||
		riid == IID_IDirectDraw4 ||
		riid == IID_IDirectDraw7)
	{
		*ppvObj = this;
		AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG __stdcall IDirectDrawWrapper::AddRef()
{
	debugMessage(1, "IDirectDrawWrapper::AddRef", "Partially Implemented");
	ReferenceCount++;
	return ReferenceCount;
}

ULONG __stdcall IDirectDrawWrapper::Release()
{
	debugMessage(1, "IDirectDrawWrapper::Release", "Partially Implemented");

	if (ReferenceCount > 0)
		ReferenceCount--;

	if (ReferenceCount == 0)
	{
		delete this;
		return 0;
	}

	return ReferenceCount;
}

/**********************
**IDirectDraw methods**
***********************/

HRESULT __stdcall IDirectDrawWrapper::Compact()
{
	debugMessage(0, "IDirectDrawWrapper::Compact", "Unsupported in DirectDraw");
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::CreateClipper(DWORD dwFlags, LPDIRECTDRAWCLIPPER FAR* lplpDDClipper, IUnknown FAR* pUnkOuter)
{
	UNREFERENCED_PARAMETER(pUnkOuter);

	char message[2048] = "\0";
	sprintf_s(message, 2048, "Partially Supported dwFlags: %u", dwFlags);
	debugMessage(1, "IDirectDrawWrapper::CreateClipper", message);

	if (lplpDDClipper == NULL)
		return DDERR_INVALIDPARAMS;

	IDirectDrawClipperWrapper* lpDDClipper = new IDirectDrawClipperWrapper();
	if (lpDDClipper == NULL)
		return DDERR_OUTOFMEMORY;

	HRESULT hr = lpDDClipper->WrapperInitialize(dwFlags);
	if (hr != DD_OK)
	{
		delete lpDDClipper;
		return hr;
	}

	*lplpDDClipper = (LPDIRECTDRAWCLIPPER)lpDDClipper;
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::CreatePalette(DWORD dwFlags, LPPALETTEENTRY lpDDColorArray, LPDIRECTDRAWPALETTE FAR* lplpDDPalette, IUnknown FAR* pUnkOuter)
{
	UNREFERENCED_PARAMETER(pUnkOuter);

	char message[2048] = "\0";
	sprintf_s(message, 2048, "Created dwFlags: %u", dwFlags);

	if (lplpDDPalette == NULL)
		return DDERR_INVALIDPARAMS;

	IDirectDrawPaletteWrapper* lpDDPalette = new IDirectDrawPaletteWrapper();
	if (lpDDPalette == NULL)
		return DDERR_OUTOFMEMORY;

	HRESULT hr = lpDDPalette->WrapperInitialize(dwFlags, lpDDColorArray, lplpDDPalette);
	if (hr != DD_OK)
	{
		delete lpDDPalette;
		return hr;
	}

	*lplpDDPalette = (LPDIRECTDRAWPALETTE)lpDDPalette;

	debugMessage(2, "IDirectDrawWrapper::CreatePalette", message);
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::CreateSurface(LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE FAR* lplpDDSurface, IUnknown FAR* pUnkOuter)
{
	UNREFERENCED_PARAMETER(pUnkOuter);

	if (lpDDSurfaceDesc == NULL || lplpDDSurface == NULL)
		return DDERR_INVALIDPARAMS;

	char message[2048] = "\0";
	sprintf_s(message, 2048, "lpDDSurfaceDesc->dwFlags:: %u", lpDDSurfaceDesc->dwFlags);

	if (lpAttachedSurface != NULL)
	{
		delete lpAttachedSurface;
		lpAttachedSurface = NULL;
	}

	lpAttachedSurface = new IDirectDrawSurfaceWrapper(this);
	if (lpAttachedSurface == NULL)
		return DDERR_OUTOFMEMORY;

	HRESULT hr = lpAttachedSurface->WrapperInitialize(lpDDSurfaceDesc, displayModeWidth, displayModeHeight, displayWidth, displayHeight);
	if (hr != DD_OK)
		return hr;

	*lplpDDSurface = (LPDIRECTDRAWSURFACE)lpAttachedSurface;

	debugMessage(2, "IDirectDrawWrapper::CreateSurface", message);
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::DuplicateSurface(LPDIRECTDRAWSURFACE lpDDSurface, LPDIRECTDRAWSURFACE FAR* lplpDupDDSurface)
{
	UNREFERENCED_PARAMETER(lpDDSurface);
	UNREFERENCED_PARAMETER(lplpDupDDSurface);
	debugMessage(0, "DirectDrawWrapper::DuplicateSurface", "Not Implemented");
	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::EnumDisplayModes(DWORD dwFlags, LPDDSURFACEDESC lpDDSurfaceDesc, LPVOID lpContext, LPDDENUMMODESCALLBACK lpEnumModesCallback)
{
	UNREFERENCED_PARAMETER(dwFlags);
	UNREFERENCED_PARAMETER(lpDDSurfaceDesc);
	UNREFERENCED_PARAMETER(lpContext);
	UNREFERENCED_PARAMETER(lpEnumModesCallback);

	debugMessage(0, "IDirectDrawWrapper::EnumDisplayModes", "Not Implemented");
	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::EnumSurfaces(DWORD dwFlags, LPDDSURFACEDESC lpDDSD, LPVOID lpContext, LPDDENUMSURFACESCALLBACK lpEnumSurfacesCallback)
{
	UNREFERENCED_PARAMETER(dwFlags);
	UNREFERENCED_PARAMETER(lpDDSD);
	UNREFERENCED_PARAMETER(lpContext);
	UNREFERENCED_PARAMETER(lpEnumSurfacesCallback);

	debugMessage(0, "IDirectDrawWrapper::EnumSurfaces", "Not Implemented");
	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::FlipToGDISurface()
{
	debugMessage(0, "IDirectDrawWrapper::FlipToGDISurface", "Not Implemented");
	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::GetCaps(LPDDCAPS lpDDDriverCaps, LPDDCAPS lpDDHELCaps)
{
	debugMessage(0, "IDirectDrawWrapper::GetCaps", "Partially Implemented");

	if (lpDDDriverCaps == NULL && lpDDHELCaps == NULL)
		return DDERR_INVALIDPARAMS;

	if (lpDDDriverCaps != NULL)
	{
		ZeroMemory(lpDDDriverCaps, lpDDDriverCaps->dwSize);
		lpDDDriverCaps->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_GDI;
		lpDDDriverCaps->dwVidMemTotal = 64 * 1024 * 1024;
		lpDDDriverCaps->dwVidMemFree = 64 * 1024 * 1024;
	}

	if (lpDDHELCaps != NULL)
	{
		ZeroMemory(lpDDHELCaps, lpDDHELCaps->dwSize);
		lpDDHELCaps->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_GDI;
		lpDDHELCaps->dwVidMemTotal = 64 * 1024 * 1024;
		lpDDHELCaps->dwVidMemFree = 64 * 1024 * 1024;
	}

	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::GetDisplayMode(LPDDSURFACEDESC lpDDSurfaceDesc)
{
	debugMessage(0, "IDirectDrawWrapper::GetDisplayMode", "Partially Implemented");

	if (lpDDSurfaceDesc == NULL)
		return DDERR_INVALIDPARAMS;

	ZeroMemory(lpDDSurfaceDesc, sizeof(DDSURFACEDESC));
	lpDDSurfaceDesc->dwSize = sizeof(DDSURFACEDESC);
	lpDDSurfaceDesc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
	lpDDSurfaceDesc->dwWidth = displayModeWidth;
	lpDDSurfaceDesc->dwHeight = displayModeHeight;
	lpDDSurfaceDesc->lPitch = displayModeWidth * 4;

	lpDDSurfaceDesc->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	lpDDSurfaceDesc->ddpfPixelFormat.dwFlags = DDPF_RGB;
	lpDDSurfaceDesc->ddpfPixelFormat.dwRGBBitCount = 32;
	lpDDSurfaceDesc->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
	lpDDSurfaceDesc->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
	lpDDSurfaceDesc->ddpfPixelFormat.dwBBitMask = 0x000000FF;

	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::GetFourCCCodes(LPDWORD lpNumCodes, LPDWORD lpCodes)
{
	debugMessage(0, "IDirectDrawWrapper::GetFourCCCodes", "Partially Implemented");

	if (lpNumCodes == NULL)
		return DDERR_INVALIDPARAMS;

	if (lpCodes == NULL)
	{
		*lpNumCodes = 0;
	}
	else
	{
		*lpNumCodes = 0;
	}

	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::GetGDISurface(LPDIRECTDRAWSURFACE FAR* lplpGDIDDSSurface)
{
	debugMessage(0, "IDirectDrawWrapper::GetGDISurface", "Partially Implemented");

	if (lplpGDIDDSSurface == NULL)
		return DDERR_INVALIDPARAMS;

	if (lpAttachedSurface == NULL)
		return DDERR_NOTFOUND;

	*lplpGDIDDSSurface = (LPDIRECTDRAWSURFACE)lpAttachedSurface;
	lpAttachedSurface->AddRef();
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::GetMonitorFrequency(LPDWORD lpdwFrequency)
{
	debugMessage(0, "IDirectDrawWrapper::GetMonitorFrequency", "Partially Implemented");

	if (lpdwFrequency == NULL)
		return DDERR_INVALIDPARAMS;

	*lpdwFrequency = refreshRate;
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::GetScanLine(LPDWORD lpdwScanLine)
{
	debugMessage(0, "IDirectDrawWrapper::GetScanLine", "Not Implemented");

	if (lpdwScanLine == NULL)
		return DDERR_INVALIDPARAMS;

	*lpdwScanLine = 0;
	return DDERR_UNSUPPORTED;
}

HRESULT __stdcall IDirectDrawWrapper::GetVerticalBlankStatus(LPBOOL lpbIsInVB)
{
	if (lpbIsInVB == NULL)
		return DDERR_INVALIDPARAMS;

	*lpbIsInVB = FALSE;
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::Initialize(GUID FAR* lpGUID)
{
	UNREFERENCED_PARAMETER(lpGUID);
	debugMessage(1, "IDirectDrawWrapper::Initialize", "Partially Implemented");
	return DDERR_ALREADYINITIALIZED;
}

HRESULT __stdcall IDirectDrawWrapper::RestoreDisplayMode()
{
	debugMessage(0, "IDirectDrawWrapper::RestoreDisplayMode", "Partially Implemented");

	isWindowed = true;
	displayWidth = displayWidthWindowed;
	displayHeight = displayHeightWindowed;
	AdjustWindow();
	ReinitDevice();

	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::SetCooperativeLevel(HWND in_hWnd, DWORD dwFlags)
{
	char message[2048] = "\0";
	sprintf_s(message, 2048, "Completed in_hWnd: 0x%p, dwFlags: %u", in_hWnd, dwFlags);

	if (in_hWnd == NULL)
	{
		debugMessage(0, "IDirectDrawWrapper::SetCooperativeLevel", "Unimplemented for NULL window handle");
		return DDERR_GENERIC;
	}

	cooperativeFlags = dwFlags;
	hWnd = in_hWnd;

#ifdef _WIN64
	lpPrevWndFunc = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_WNDPROC);
	if (SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc) == 0)
#else
	lpPrevWndFunc = (WNDPROC)GetWindowLong(hWnd, GWL_WNDPROC);
	if (SetWindowLong(hWnd, GWL_WNDPROC, (LONG)WndProc) == 0)
#endif
	{
		debugMessage(0, "IDirectDrawWrapper::SetCooperativeLevel", "Failed to overload WNDPROC");
	}

	AdjustWindow();

	if (!CreateD3DDevice())
	{
		MessageBox(NULL, TEXT("Error creating Direct3D11 Device"), TEXT("Error"), MB_OK | MB_ICONERROR);
		return DDERR_GENERIC;
	}

	debugMessage(2, "IDirectDrawWrapper::SetCooperativeLevel", message);
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::SetDisplayMode(DWORD dwWidth, DWORD dwHeight, DWORD dwBPP)
{
	char message[2048] = "\0";
	sprintf_s(message, "Complete dwWidth: %u, dwHeight: %u, dwBPP: %u", dwWidth, dwHeight, dwBPP);

	displayModeWidth = dwWidth;
	displayModeHeight = dwHeight;

	if (!CreateSurfaceTexture())
	{
		MessageBox(NULL, TEXT("Error creating Direct3D11 surface texture"), TEXT("Error"), MB_OK | MB_ICONERROR);
		return DDERR_GENERIC;
	}

	debugMessage(2, "IDirectDrawWrapper::SetDisplayMode", message);
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::WaitForVerticalBlank(DWORD dwFlags, HANDLE hEvent)
{
	UNREFERENCED_PARAMETER(hEvent);

	if (dwFlags & DDWAITVB_BLOCKBEGINEVENT)
		return DDERR_UNSUPPORTED;

	return DD_OK;
}

/****************************
**Added in the V2 interface**
*****************************/

HRESULT __stdcall IDirectDrawWrapper::GetAvailableVideoMem(LPDDSCAPS2 lpDDSCaps2, LPDWORD lpdwTotal, LPDWORD lpdwFree)
{
	UNREFERENCED_PARAMETER(lpDDSCaps2);

	debugMessage(0, "IDirectDrawWrapper::GetAvailableVideoMem", "Partially Implemented");

	if (lpDDSCaps2 == NULL || lpdwTotal == NULL || lpdwFree == NULL)
		return DDERR_INVALIDPARAMS;

	*lpdwTotal = 64 * 1024 * 1024;
	*lpdwFree = 64 * 1024 * 1024;
	return DD_OK;
}

/****************************
**Added in the V4 interface**
*****************************/

HRESULT __stdcall IDirectDrawWrapper::EvaluateMode(DWORD dwFlags, DWORD* pSecondsUntilTimeout)
{
	UNREFERENCED_PARAMETER(dwFlags);
	debugMessage(0, "IDirectDrawWrapper::EvaluateMode", "Not Implemented");

	if (pSecondsUntilTimeout == NULL)
		return DDERR_INVALIDPARAMS;

	*pSecondsUntilTimeout = 0;
	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 lpdddi, DWORD dwFlags)
{
	UNREFERENCED_PARAMETER(dwFlags);

	debugMessage(0, "IDirectDrawWrapper::GetDeviceIdentifier", "Partially Implemented");

	if (lpdddi == NULL)
		return DDERR_INVALIDPARAMS;

	ZeroMemory(lpdddi, sizeof(DDDEVICEIDENTIFIER2));
	strcpy_s(lpdddi->szDriver, "D3D11Wrapper");
	strcpy_s(lpdddi->szDescription, "DirectDraw to Direct3D11 Wrapper");
	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::GetSurfaceFromDC(HDC hdc, LPDIRECTDRAWSURFACE7* lpDDS)
{
	UNREFERENCED_PARAMETER(hdc);

	debugMessage(0, "IDirectDrawWrapper::GetSurfaceFromDC", "Not Implemented");

	if (lpDDS == NULL)
		return DDERR_INVALIDPARAMS;

	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::RestoreAllSurfaces()
{
	debugMessage(0, "IDirectDrawWrapper::RestoreAllSurfaces", "Partially Implemented");

	if (lpAttachedSurface != NULL)
		lpAttachedSurface->Restore();

	return DD_OK;
}

HRESULT __stdcall IDirectDrawWrapper::StartModeTest(LPSIZE lpModesToTest, DWORD dwNumEntries, DWORD dwFlags)
{
	UNREFERENCED_PARAMETER(lpModesToTest);
	UNREFERENCED_PARAMETER(dwNumEntries);
	UNREFERENCED_PARAMETER(dwFlags);

	debugMessage(0, "IDirectDrawWrapper::StartModeTest", "Not Implemented");
	return DDERR_GENERIC;
}

HRESULT __stdcall IDirectDrawWrapper::TestCooperativeLevel()
{
	debugMessage(0, "IDirectDrawWrapper::TestCooperativeLevel", "Partially Implemented");

	if (!d3d11Device || !swapChain)
		return DDERR_GENERIC;

	return DD_OK;
}

// Default constructor
IDirectDrawWrapper::IDirectDrawWrapper()
{
	hWnd = NULL;
	WndProc = NULL;
	lpPrevWndFunc = NULL;
	hModule = NULL;

	cooperativeFlags = 0;
	ReferenceCount = 0;

	lpAttachedSurface = NULL;

	inMenu = false;
	curMenu = 0;

	curMenuFrame = 0;
	menuTextureWidth = 512;
	menuTextureHeight = 512;

	menuLocations[0] = 117;
	menuLocations[1] = 162;
	menuLocations[2] = 208;
	menuLocations[3] = 253;
	menuLocations[4] = 298;

	menuSprites[0].left = 0;
	menuSprites[0].top = 0;
	menuSprites[0].right = 30;
	menuSprites[0].bottom = 32;

	menuSprites[1].left = menuSprites[0].right;
	menuSprites[1].top = 0;
	menuSprites[1].right = menuSprites[1].left + 14;
	menuSprites[1].bottom = 32;

	menuSprites[2].left = menuSprites[1].right;
	menuSprites[2].top = 0;
	menuSprites[2].right = menuSprites[2].left + 22;
	menuSprites[2].bottom = 32;

	menuSprites[3].left = menuSprites[2].right;
	menuSprites[3].top = 0;
	menuSprites[3].right = menuSprites[3].left + 21;
	menuSprites[3].bottom = 32;

	menuSprites[4].left = menuSprites[3].right;
	menuSprites[4].top = 0;
	menuSprites[4].right = menuSprites[4].left + 23;
	menuSprites[4].bottom = 32;

	menuSprites[5].left = menuSprites[4].right;
	menuSprites[5].top = 0;
	menuSprites[5].right = menuSprites[5].left + 21;
	menuSprites[5].bottom = 32;

	menuSprites[6].left = menuSprites[5].right;
	menuSprites[6].top = 0;
	menuSprites[6].right = menuSprites[6].left + 20;
	menuSprites[6].bottom = 32;

	menuSprites[7].left = menuSprites[6].right;
	menuSprites[7].top = 0;
	menuSprites[7].right = menuSprites[7].left + 21;
	menuSprites[7].bottom = 32;

	menuSprites[8].left = menuSprites[7].right;
	menuSprites[8].top = 0;
	menuSprites[8].right = menuSprites[8].left + 20;
	menuSprites[8].bottom = 32;

	menuSprites[9].left = menuSprites[8].right;
	menuSprites[9].top = 0;
	menuSprites[9].right = menuSprites[9].left + 21;
	menuSprites[9].bottom = 32;

	menuSprites[10].left = menuSprites[9].right;
	menuSprites[10].top = 0;
	menuSprites[10].right = menuSprites[10].left + 25;
	menuSprites[10].bottom = 32;

	menuSprites[11].left = 0;
	menuSprites[11].top = 32;
	menuSprites[11].right = 246;
	menuSprites[11].bottom = 64;

	menuSprites[12].left = 0;
	menuSprites[12].top = 64;
	menuSprites[12].right = 243;
	menuSprites[12].bottom = 96;

	menuSprites[13].left = 0;
	menuSprites[13].top = 96;
	menuSprites[13].right = 144;
	menuSprites[13].bottom = 128;

	menuSprites[14].left = 0;
	menuSprites[14].top = 128;
	menuSprites[14].right = 151;
	menuSprites[14].bottom = 160;

	menuSprites[15].left = 160;
	menuSprites[15].top = 96;
	menuSprites[15].right = menuSprites[15].left + 55;
	menuSprites[15].bottom = 128;

	menuSprites[16].left = 160;
	menuSprites[16].top = 128;
	menuSprites[16].right = menuSprites[15].left + 69;
	menuSprites[16].bottom = 160;

	menuSprites[17].left = 0;
	menuSprites[17].top = 256;
	menuSprites[17].right = 295;
	menuSprites[17].bottom = 356;

	windowedResolutions = new POINT[10];
	windowedResolutionCount = 10;
	windowedResolutions[0].x = 640;  windowedResolutions[0].y = 480;
	windowedResolutions[1].x = 800;  windowedResolutions[1].y = 600;
	windowedResolutions[2].x = 960;  windowedResolutions[2].y = 720;
	windowedResolutions[3].x = 1024; windowedResolutions[3].y = 768;
	windowedResolutions[4].x = 1152; windowedResolutions[4].y = 864;
	windowedResolutions[5].x = 1280; windowedResolutions[5].y = 960;
	windowedResolutions[6].x = 1400; windowedResolutions[6].y = 1050;
	windowedResolutions[7].x = 1440; windowedResolutions[7].y = 1080;
	windowedResolutions[8].x = 1600; windowedResolutions[8].y = 1200;
	windowedResolutions[9].x = 1920; windowedResolutions[9].y = 1440;

	fullscreenResolutionCount = 0;
	fullscreenResolutions = NULL;
	fullscreenRefreshes = NULL;

	displayModeWidth = 640;
	displayModeHeight = 480;

	displayWidthWindowed = 640;
	displayHeightWindowed = 480;
	displayWidthFullscreen = 640;
	displayHeightFullscreen = 480;

	displayWidth = 640;
	displayHeight = 480;

	refreshRate = 60;
	isWindowed = true;
	vSync = true;

	menuWindowed = true;
	menuvSync = true;
	menuWindowedResolution = 0;
	menuFullscreenResolution = 0;

	lastPosition.x = 100;
	lastPosition.y = 100;

	ZeroMemory(&swapDesc, sizeof(swapDesc));

	CoInitialize(NULL);

	wchar_t curPath[MAX_PATH];
	wchar_t filename[MAX_PATH];
	wchar_t temp[1024];

	GetCurrentDirectory(MAX_PATH, curPath);
	wsprintf(filename, TEXT("%s\\hellfire_settings.ini"), curPath);

	GetPrivateProfileString(TEXT("video"), TEXT("windowedResolution"), TEXT("640x480"), temp, 1024, filename);
	for (int i = 0; i < 1024 && temp[i] != TEXT('\0'); i++)
	{
		if (temp[i] == TEXT('x') || temp[i] == TEXT('X'))
		{
			temp[i] = TEXT('\0');
			displayWidthWindowed = _wtoi(temp);
			displayHeightWindowed = _wtoi(&(temp[i + 1]));
			if (displayWidthWindowed == 0 || displayHeightWindowed == 0)
			{
				displayWidthWindowed = 640;
				displayHeightWindowed = 480;
			}
			break;
		}
	}

	GetPrivateProfileString(TEXT("video"), TEXT("fullscreenResolution"), TEXT("640x480"), temp, 1024, filename);
	for (int i = 0; i < 1024 && temp[i] != TEXT('\0'); i++)
	{
		if (temp[i] == TEXT('x') || temp[i] == TEXT('X'))
		{
			temp[i] = TEXT('\0');
			displayWidthFullscreen = _wtoi(temp);
			displayHeightFullscreen = _wtoi(&(temp[i + 1]));
			if (displayWidthFullscreen == 0 || displayHeightFullscreen == 0)
			{
				displayWidthFullscreen = 640;
				displayHeightFullscreen = 480;
			}
			break;
		}
	}

	GetPrivateProfileString(TEXT("video"), TEXT("fullscreen"), TEXT("0"), temp, 1024, filename);
	if (temp[0] == TEXT('1'))
		isWindowed = false;
	else
		isWindowed = true;

	if (isWindowed)
	{
		displayWidth = displayWidthWindowed;
		displayHeight = displayHeightWindowed;
	}
	else
	{
		displayWidth = displayWidthFullscreen;
		displayHeight = displayHeightFullscreen;
	}

	GetPrivateProfileString(TEXT("video"), TEXT("vsync"), TEXT("1"), temp, 1024, filename);
	if (temp[0] == TEXT('1'))
		vSync = true;
	else
		vSync = false;

	GetPrivateProfileString(TEXT("video"), TEXT("refresh"), TEXT("60"), temp, 1024, filename);
	refreshRate = _wtoi(temp);
	if (refreshRate == 0)
		refreshRate = 60;

	AddRef();

	debugMessage(2, "IDirectDrawWrapper::IDirectDrawWrapper", "Created");
}

IDirectDrawWrapper::~IDirectDrawWrapper()
{
	if (d3d11Context)
	{
		d3d11Context->ClearState();
		d3d11Context->Flush();
	}

	if (lpAttachedSurface != NULL)
	{
		delete lpAttachedSurface;
		lpAttachedSurface = NULL;
	}

	menuSRV.Reset();
	menuTexture.Reset();
	menuVertexBuffer.Reset();

	surfaceSRV.Reset();
	surfaceTexture.Reset();

	vertexBuffer.Reset();
	inputLayout.Reset();
	blitVS.Reset();
	blitPS.Reset();
	samplerLinear.Reset();
	alphaBlendState.Reset();
	rasterState.Reset();
	renderTargetView.Reset();
	swapChain.Reset();
	d3d11Context.Reset();
	d3d11Device.Reset();

	SAFE_DELETE_ARRAY(windowedResolutions);
	windowedResolutionCount = 0;

	SAFE_DELETE_ARRAY(fullscreenResolutions);
	fullscreenResolutionCount = 0;

	SAFE_DELETE_ARRAY(fullscreenRefreshes);

	CoUninitialize();

	debugMessage(2, "IDirectDrawWrapper::~IDirectDrawWrapper", "Destroyed");
}

HRESULT IDirectDrawWrapper::WrapperInitialize(WNDPROC wp, HMODULE hMod)
{
	WndProc = wp;
	hModule = hMod;

	ZeroMemory(&swapDesc, sizeof(swapDesc));

	debugMessage(2, "IDirectDrawWrapper::WrapperInitialize", "Initialized");
	return DD_OK;
}

bool IDirectDrawWrapper::CreateRenderTarget()
{
	renderTargetView.Reset();

	ComPtr<ID3D11Texture2D> backBuffer;
	HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backBuffer.GetAddressOf());
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateRenderTarget", "GetBuffer failed");
		return false;
	}

	hr = d3d11Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateRenderTarget", "CreateRenderTargetView failed");
		return false;
	}

	return true;
}

bool IDirectDrawWrapper::CreateShaders()
{
	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;

	if (!CompileShaderSource(g_BlitVS_HLSL, "main", "vs_4_0", &vsBlob))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateShaders", "Vertex shader compile failed");
		return false;
	}

	if (!CompileShaderSource(g_BlitPS_HLSL, "main", "ps_4_0", &psBlob))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateShaders", "Pixel shader compile failed");
		return false;
	}

	HRESULT hr = d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &blitVS);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateShaders", "CreateVertexShader failed");
		return false;
	}

	hr = d3d11Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &blitPS);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateShaders", "CreatePixelShader failed");
		return false;
	}

	D3D11_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(TLVERTEX11, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(TLVERTEX11, u), D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};

	hr = d3d11Device->CreateInputLayout(
		layout,
		_countof(layout),
		vsBlob->GetBufferPointer(),
		vsBlob->GetBufferSize(),
		&inputLayout);

	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateShaders", "CreateInputLayout failed");
		return false;
	}

	return true;
}

bool IDirectDrawWrapper::CreateMenuVertexBuffer()
{
	menuVertexBuffer.Reset();

	D3D11_BUFFER_DESC vbDesc = {};
	vbDesc.ByteWidth = sizeof(SpriteVertex11) * 4;
	vbDesc.Usage = D3D11_USAGE_DYNAMIC;
	vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = d3d11Device->CreateBuffer(&vbDesc, nullptr, &menuVertexBuffer);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateMenuVertexBuffer", "CreateBuffer failed");
		return false;
	}

	return true;
}

bool IDirectDrawWrapper::CreateD3DDevice()
{
	if (d3d11Context)
	{
		d3d11Context->ClearState();
		d3d11Context->Flush();
	}

	menuSRV.Reset();
	menuTexture.Reset();
	menuVertexBuffer.Reset();

	surfaceSRV.Reset();
	surfaceTexture.Reset();
	vertexBuffer.Reset();

	inputLayout.Reset();
	blitVS.Reset();
	blitPS.Reset();
	samplerLinear.Reset();
	alphaBlendState.Reset();
	rasterState.Reset();
	renderTargetView.Reset();
	swapChain.Reset();
	d3d11Context.Reset();
	d3d11Device.Reset();

	SAFE_DELETE_ARRAY(fullscreenResolutions);
	SAFE_DELETE_ARRAY(fullscreenRefreshes);
	fullscreenResolutionCount = 0;

	ComPtr<IDXGIFactory> dxgiFactory;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIOutput> output;

	HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)dxgiFactory.GetAddressOf());
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "CreateDXGIFactory failed");
		return false;
	}

	hr = dxgiFactory->EnumAdapters(0, &adapter);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "EnumAdapters failed");
		return false;
	}

	hr = adapter->EnumOutputs(0, &output);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "EnumOutputs failed");
		return false;
	}

	UINT modeCount = 0;
	hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modeCount, nullptr);
	if (FAILED(hr) || modeCount == 0)
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "GetDisplayModeList count failed");
		return false;
	}

	DXGI_MODE_DESC* modes = new DXGI_MODE_DESC[modeCount];
	hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modeCount, modes);
	if (FAILED(hr))
	{
		SAFE_DELETE_ARRAY(modes);
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "GetDisplayModeList failed");
		return false;
	}

	fullscreenResolutions = new POINT[modeCount];
	fullscreenRefreshes = new UINT[modeCount];

	bool modeFound = false;
	for (UINT i = 0; i < modeCount; i++)
	{
		const DXGI_MODE_DESC& m = modes[i];
		UINT refresh = 60;
		if (m.RefreshRate.Denominator != 0)
			refresh = m.RefreshRate.Numerator / m.RefreshRate.Denominator;

		if (m.Width == (UINT)displayWidth && m.Height == (UINT)displayHeight && refresh == refreshRate)
		{
			modeFound = true;
			menuFullscreenResolution = fullscreenResolutionCount;
		}

		if (m.Width < 1920 && m.Height < 1440)
		{
			fullscreenResolutions[fullscreenResolutionCount].x = (LONG)m.Width;
			fullscreenResolutions[fullscreenResolutionCount].y = (LONG)m.Height;
			fullscreenRefreshes[fullscreenResolutionCount] = refresh;
			fullscreenResolutionCount++;
		}
	}

	SAFE_DELETE_ARRAY(modes);

	ZeroMemory(&swapDesc, sizeof(swapDesc));
	swapDesc.BufferCount = 1;
	swapDesc.BufferDesc.Width = isWindowed ? 0 : displayWidth;
	swapDesc.BufferDesc.Height = isWindowed ? 0 : displayHeight;
	swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapDesc.BufferDesc.RefreshRate.Numerator = vSync ? refreshRate : 0;
	swapDesc.BufferDesc.RefreshRate.Denominator = vSync ? 1 : 0;
	swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapDesc.OutputWindow = hWnd;
	swapDesc.SampleDesc.Count = 1;
	swapDesc.SampleDesc.Quality = 0;
	swapDesc.Windowed = isWindowed ? TRUE : FALSE;
	swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	UINT createFlags = 0;
#ifdef _DEBUG
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL featureLevelOut = D3D_FEATURE_LEVEL_11_0;
	D3D_FEATURE_LEVEL levels[] =
	{
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0
	};

	hr = D3D11CreateDeviceAndSwapChain(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		createFlags,
		levels,
		_countof(levels),
		D3D11_SDK_VERSION,
		&swapDesc,
		&swapChain,
		&d3d11Device,
		&featureLevelOut,
		&d3d11Context);

	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "D3D11CreateDeviceAndSwapChain failed");
		MessageBox(NULL, TEXT("Failed to create Direct3D11 device."), TEXT("Direct3D11 Device Error"), MB_OK);
		return false;
	}

	if (!isWindowed && modeFound)
		swapChain->SetFullscreenState(TRUE, nullptr);
	else
		swapChain->SetFullscreenState(FALSE, nullptr);

	if (!CreateRenderTarget())
		return false;

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (FLOAT)displayWidth;
	vp.Height = (FLOAT)displayHeight;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	d3d11Context->RSSetViewports(1, &vp);

	if (!CreateShaders())
		return false;

	D3D11_SAMPLER_DESC samp = {};
	samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp.MinLOD = 0.0f;
	samp.MaxLOD = D3D11_FLOAT32_MAX;

	hr = d3d11Device->CreateSamplerState(&samp, &samplerLinear);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "CreateSamplerState failed");
		return false;
	}

	D3D11_BLEND_DESC blend = {};
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	hr = d3d11Device->CreateBlendState(&blend, &alphaBlendState);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "CreateBlendState failed");
		return false;
	}

	D3D11_RASTERIZER_DESC rs = {};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.ScissorEnable = FALSE;
	rs.DepthClipEnable = TRUE;

	hr = d3d11Device->CreateRasterizerState(&rs, &rasterState);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateD3DDevice", "CreateRasterizerState failed");
		return false;
	}

	d3d11Context->RSSetState(rasterState.Get());

	if (!CreateMenuVertexBuffer())
		return false;

	debugMessage(2, "IDirectDrawWrapper::CreateD3DDevice", "Create D3D11 Object");
	return true;
}

bool IDirectDrawWrapper::CreateSurfaceTexture()
{
	surfaceSRV.Reset();
	surfaceTexture.Reset();
	vertexBuffer.Reset();

	if (!d3d11Device)
	{
		debugMessage(0, "IDirectDrawWrapper::CreateSurfaceTexture", "No D3D11 device");
		return false;
	}

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = displayModeWidth;
	texDesc.Height = displayModeHeight;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DYNAMIC;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, nullptr, &surfaceTexture);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateSurfaceTexture", "Unable to create surface texture");
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	hr = d3d11Device->CreateShaderResourceView(surfaceTexture.Get(), &srvDesc, &surfaceSRV);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateSurfaceTexture", "Unable to create SRV");
		return false;
	}

	TLVERTEX11 vertices[4];
	vertices[0] = { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f };
	vertices[1] = { 1.0f,  1.0f, 0.0f, 1.0f, 0.0f };
	vertices[2] = { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f };
	vertices[3] = { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f };

	D3D11_BUFFER_DESC vbDesc = {};
	vbDesc.ByteWidth = sizeof(vertices);
	vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
	vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

	D3D11_SUBRESOURCE_DATA initData = {};
	initData.pSysMem = vertices;

	hr = d3d11Device->CreateBuffer(&vbDesc, &initData, &vertexBuffer);
	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::CreateSurfaceTexture", "Unable to create vertex buffer");
		return false;
	}

	debugMessage(2, "IDirectDrawWrapper::CreateSurfaceTexture", "D3D11 Texture Created");
	return true;
}

HRESULT IDirectDrawWrapper::Present()
{
	if (!d3d11Device || !d3d11Context || !swapChain)
	{
		debugMessage(0, "IDirectDrawWrapper::Present", "Present called when D3D11 device doesn't exist");
		return false;
	}

	if (!surfaceTexture || !surfaceSRV)
	{
		debugMessage(0, "IDirectDrawWrapper::Present", "Present called when texture doesn't exist");
		return false;
	}

	if (lpAttachedSurface != NULL)
	{
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = d3d11Context->Map(surfaceTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr))
		{
			debugMessage(0, "IDirectDrawWrapper::Present", "Failed to map texture memory");
			return false;
		}

		for (DWORD y = 0; y < displayModeHeight; y++)
		{
			memcpy(
				(BYTE*)mapped.pData + (y * mapped.RowPitch),
				&lpAttachedSurface->rgbVideoMem[y * displayModeWidth],
				displayModeWidth * sizeof(UINT32));
		}

		d3d11Context->Unmap(surfaceTexture.Get(), 0);
	}
	else
	{
		debugMessage(1, "IDirectDrawWrapper::Present", "Attempt to Present with no attached surface");
	}

	float clearColor[4] = { 0, 0, 0, 1 };
	d3d11Context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);
	d3d11Context->ClearRenderTargetView(renderTargetView.Get(), clearColor);

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (FLOAT)displayWidth;
	vp.Height = (FLOAT)displayHeight;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	d3d11Context->RSSetViewports(1, &vp);

	UINT stride = sizeof(TLVERTEX11);
	UINT offset = 0;

	d3d11Context->IASetInputLayout(inputLayout.Get());
	d3d11Context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
	d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	d3d11Context->VSSetShader(blitVS.Get(), nullptr, 0);
	d3d11Context->PSSetShader(blitPS.Get(), nullptr, 0);
	d3d11Context->PSSetShaderResources(0, 1, surfaceSRV.GetAddressOf());
	d3d11Context->PSSetSamplers(0, 1, samplerLinear.GetAddressOf());

	float blendFactor[4] = { 0, 0, 0, 0 };
	d3d11Context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

	d3d11Context->Draw(4, 0);

	if (inMenu)
		RenderMenuD3D11();

	HRESULT hr = swapChain->Present(vSync ? 1 : 0, 0);
	if (FAILED(hr))
	{
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			debugMessage(1, "IDirectDrawWrapper::Present", "Device removed/reset");
			return ReinitDevice();
		}

		debugMessage(0, "IDirectDrawWrapper::Present", "Failed to present scene");
		return false;
	}

	return true;
}

BOOL IDirectDrawWrapper::MenuKey(WPARAM vKey)
{
	if (vKey == VK_OEM_3)
	{
		if (inMenu == TRUE)
		{
			inMenu = FALSE;
		}
		else
		{
			curMenu = 0;
			menuWindowed = isWindowed;
			menuvSync = vSync;
			menuWindowedResolution = 0;
			menuFullscreenResolution = 0;

			for (int i = 0; i < windowedResolutionCount; i++)
			{
				if (windowedResolutions[i].x == displayWidthWindowed && windowedResolutions[i].y == displayHeightWindowed)
				{
					menuWindowedResolution = i;
					break;
				}
			}

			for (int i = 0; i < fullscreenResolutionCount; i++)
			{
				if (fullscreenResolutions[i].x == displayWidthFullscreen &&
					fullscreenResolutions[i].y == displayHeightFullscreen &&
					fullscreenRefreshes[i] == refreshRate)
				{
					menuFullscreenResolution = i;
					break;
				}
			}

			inMenu = TRUE;
		}

		Present();
	}
	else if (vKey == VK_ESCAPE)
	{
		inMenu = FALSE;
		Present();
	}
	else if (vKey == VK_DOWN)
	{
		curMenu++;
		if (curMenu > 3) curMenu = 0;
		Present();
	}
	else if (vKey == VK_UP)
	{
		curMenu--;
		if (curMenu < 0) curMenu = 3;
		Present();
	}
	else if (vKey == VK_RIGHT)
	{
		if (curMenu == 0)
		{
			if (menuWindowed)
			{
				menuWindowedResolution++;
				if (menuWindowedResolution >= windowedResolutionCount)
					menuWindowedResolution = 0;
			}
			else
			{
				menuFullscreenResolution++;
				if (menuFullscreenResolution >= fullscreenResolutionCount)
					menuFullscreenResolution = 0;
			}
		}
		else if (curMenu == 1)
		{
			menuWindowed = !menuWindowed;
		}
		else if (curMenu == 2)
		{
			menuvSync = !menuvSync;
		}
		Present();
	}
	else if (vKey == VK_LEFT)
	{
		if (curMenu == 0)
		{
			if (menuWindowed)
			{
				menuWindowedResolution--;
				if (menuWindowedResolution < 0)
					menuWindowedResolution = windowedResolutionCount - 1;
			}
			else
			{
				menuFullscreenResolution--;
				if (menuFullscreenResolution < 0)
					menuFullscreenResolution = fullscreenResolutionCount - 1;
			}
		}
		else if (curMenu == 1)
		{
			menuWindowed = !menuWindowed;
		}
		else if (curMenu == 2)
		{
			menuvSync = !menuvSync;
		}
		Present();
	}
	else if (vKey == VK_RETURN)
	{
		inMenu = false;

		isWindowed = menuWindowed;

		if (menuWindowed)
		{
			displayWidth = windowedResolutions[menuWindowedResolution].x;
			displayHeight = windowedResolutions[menuWindowedResolution].y;
			displayWidthWindowed = displayWidth;
			displayHeightWindowed = displayHeight;
		}
		else
		{
			if (fullscreenResolutionCount > 0)
			{
				displayWidth = fullscreenResolutions[menuFullscreenResolution].x;
				displayHeight = fullscreenResolutions[menuFullscreenResolution].y;
				refreshRate = fullscreenRefreshes[menuFullscreenResolution];
				displayWidthFullscreen = displayWidth;
				displayHeightFullscreen = displayHeight;
			}
		}

		vSync = menuvSync;

		AdjustWindow();

		if (swapChain)
			swapChain->SetFullscreenState(isWindowed ? FALSE : TRUE, nullptr);

		ReinitDevice();
		Present();

		wchar_t curPath[MAX_PATH];
		wchar_t filename[MAX_PATH];
		wchar_t temp[1024];

		GetCurrentDirectory(MAX_PATH, curPath);
		wsprintf(filename, TEXT("%s\\ddraw_settings.ini"), curPath);

		wsprintf(temp, TEXT("%dx%d"), displayWidthWindowed, displayHeightWindowed);
		WritePrivateProfileString(TEXT("video"), TEXT("windowedResolution"), temp, filename);

		wsprintf(temp, TEXT("%dx%d"), displayWidthFullscreen, displayHeightFullscreen);
		WritePrivateProfileString(TEXT("video"), TEXT("fullscreenResolution"), temp, filename);

		wsprintf(temp, TEXT("%d"), refreshRate);
		WritePrivateProfileString(TEXT("video"), TEXT("refresh"), temp, filename);

		wsprintf(temp, TEXT("%d"), isWindowed ? 0 : 1);
		WritePrivateProfileString(TEXT("video"), TEXT("fullscreen"), temp, filename);

		wsprintf(temp, TEXT("%d"), vSync ? 1 : 0);
		WritePrivateProfileString(TEXT("video"), TEXT("vsync"), temp, filename);
	}

	return inMenu;
}

void IDirectDrawWrapper::ToggleFullscreen()
{
	if (isWindowed)
	{
		isWindowed = false;
		displayWidth = displayWidthFullscreen;
		displayHeight = displayHeightFullscreen;
	}
	else
	{
		isWindowed = true;
		displayWidth = displayWidthWindowed;
		displayHeight = displayHeightWindowed;
	}

	AdjustWindow();

	if (swapChain)
		swapChain->SetFullscreenState(isWindowed ? FALSE : TRUE, nullptr);

	ReinitDevice();
	Present();
}

void IDirectDrawWrapper::DoSnapshot()
{
	if (!surfaceTexture || !d3d11Device || !d3d11Context)
	{
		debugMessage(0, "IDirectDrawWrapper::DoSnapshot", "No surface texture to save.");
		return;
	}

	wchar_t curPath[MAX_PATH];
	GetCurrentDirectory(MAX_PATH, curPath);

	wchar_t filename[MAX_PATH];
	SYSTEMTIME sysTime;
	GetSystemTime(&sysTime);

	wchar_t title[1024];
	GetWindowText(hWnd, title, 1024);

	wsprintf(
		filename,
		TEXT("%s\\%s_%.4d%.2d%.2d_%.2d%.2d%.2d.png"),
		curPath,
		title,
		sysTime.wYear, sysTime.wMonth, sysTime.wDay,
		sysTime.wHour, sysTime.wMinute, sysTime.wSecond);

	WIN32_FIND_DATA findData;
	HANDLE findHandle = FindFirstFile(filename, &findData);
	int curFileNum = 1;
	while (findHandle != INVALID_HANDLE_VALUE)
	{
		FindClose(findHandle);
		wsprintf(
			filename,
			TEXT("%s\\Diablo_%.4d%.2d%.2d_%.2d%.2d%.2d(%d).png"),
			curPath,
			sysTime.wYear, sysTime.wMonth, sysTime.wDay,
			sysTime.wHour, sysTime.wMinute, sysTime.wSecond,
			curFileNum);
		curFileNum++;
		findHandle = FindFirstFile(filename, &findData);
	}

	if (!SaveBGRA8TextureToPng(d3d11Device.Get(), d3d11Context.Get(), surfaceTexture.Get(), filename))
	{
		debugMessage(0, "IDirectDrawWrapper::DoSnapshot", "Error saving texture to file.");
		return;
	}

	debugMessage(2, "IDirectDrawWrapper::DoSnapshot", "Saved Screenshot");
}

void IDirectDrawWrapper::AdjustWindow()
{
	if (hWnd == NULL)
		return;

	if (isWindowed)
	{
#ifdef _WIN64
		SetWindowLongPtr(hWnd, GWL_STYLE, WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
#else
		SetWindowLong(hWnd, GWL_STYLE, WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
#endif

		RECT rc = { 0, 0, displayWidth, displayHeight };
		AdjustWindowRect(&rc, GetWindowLong(hWnd, GWL_STYLE), FALSE);

		SetWindowPos(
			hWnd,
			NULL,
			lastPosition.x,
			lastPosition.y,
			rc.right - rc.left,
			rc.bottom - rc.top,
			SWP_NOZORDER | SWP_FRAMECHANGED);
	}
	else
	{
#ifdef _WIN64
		SetWindowLongPtr(hWnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
#else
		SetWindowLong(hWnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
#endif

		SetWindowPos(
			hWnd,
			NULL,
			0,
			0,
			displayWidth,
			displayHeight,
			SWP_NOZORDER | SWP_FRAMECHANGED);
	}

	debugMessage(2, "IDirectDrawWrapper::AdjustWindow", "Complete");
}

bool IDirectDrawWrapper::DrawMenuSprite(const RECT& srcRect, float dstX, float dstY, float dstW, float dstH)
{
	if (!menuVertexBuffer || !menuSRV)
		return false;

	float screenW = (float)displayWidth;
	float screenH = (float)displayHeight;

	float x0 = PixelToNdcX(dstX, screenW);
	float y0 = PixelToNdcY(dstY, screenH);
	float x1 = PixelToNdcX(dstX + dstW, screenW);
	float y1 = PixelToNdcY(dstY + dstH, screenH);

	float u0 = (float)srcRect.left / (float)menuTextureWidth;
	float v0 = (float)srcRect.top / (float)menuTextureHeight;
	float u1 = (float)srcRect.right / (float)menuTextureWidth;
	float v1 = (float)srcRect.bottom / (float)menuTextureHeight;

	SpriteVertex11 verts[4] =
	{
		{ x0, y0, 0.0f, u0, v0 },
		{ x1, y0, 0.0f, u1, v0 },
		{ x0, y1, 0.0f, u0, v1 },
		{ x1, y1, 0.0f, u1, v1 }
	};

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = d3d11Context->Map(menuVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr))
		return false;

	memcpy(mapped.pData, verts, sizeof(verts));
	d3d11Context->Unmap(menuVertexBuffer.Get(), 0);

	UINT stride = sizeof(SpriteVertex11);
	UINT offset = 0;

	d3d11Context->IASetInputLayout(inputLayout.Get());
	d3d11Context->IASetVertexBuffers(0, 1, menuVertexBuffer.GetAddressOf(), &stride, &offset);
	d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	d3d11Context->VSSetShader(blitVS.Get(), nullptr, 0);
	d3d11Context->PSSetShader(blitPS.Get(), nullptr, 0);
	d3d11Context->PSSetShaderResources(0, 1, menuSRV.GetAddressOf());
	d3d11Context->PSSetSamplers(0, 1, samplerLinear.GetAddressOf());

	float blendFactor[4] = { 0, 0, 0, 0 };
	d3d11Context->OMSetBlendState(alphaBlendState.Get(), blendFactor, 0xFFFFFFFF);

	d3d11Context->Draw(4, 0);
	return true;
}

void IDirectDrawWrapper::RenderMenuD3D11()
{
	if (!menuSRV)
		return;

	float sx = (float)displayWidth / 640.0f;
	float sy = (float)displayHeight / 480.0f;

	int selectionLeft = 0;
	int selectionRight = 0;

	{
		RECT r = menuSprites[17];
		float x = (640.0f - (float)(r.right - r.left)) / 2.0f;
		float y = 3.0f;
		DrawMenuSprite(r, x * sx, y * sy, (r.right - r.left) * sx, (r.bottom - r.top) * sy);
	}

	{
		RECT r = menuSprites[11];
		float x = (640.0f - (float)(r.right - r.left)) / 2.0f;
		float y = (float)menuLocations[0];
		DrawMenuSprite(r, x * sx, y * sy, (r.right - r.left) * sx, (r.bottom - r.top) * sy);
	}

	{
		char temp[1024];
		if (menuWindowed)
			sprintf_s(temp, 1024, "%dx%d", windowedResolutions[menuWindowedResolution].x, windowedResolutions[menuWindowedResolution].y);
		else if (fullscreenResolutionCount > 0)
			sprintf_s(temp, 1024, "%dx%dx%d", fullscreenResolutions[menuFullscreenResolution].x, fullscreenResolutions[menuFullscreenResolution].y, fullscreenRefreshes[menuFullscreenResolution]);
		else
			sprintf_s(temp, 1024, "%dx%d", displayWidth, displayHeight);

		int resolutionStringWidth = 0;
		for (size_t i = 0; i < strlen(temp); i++)
		{
			if (temp[i] != 'x')
				resolutionStringWidth += menuSprites[temp[i] - '0'].right - menuSprites[temp[i] - '0'].left;
			else
				resolutionStringWidth += menuSprites[10].right - menuSprites[10].left;
		}

		float x = (640.0f - (float)resolutionStringWidth) / 2.0f;
		float y = (float)menuLocations[1];

		if (curMenu == 0)
		{
			selectionLeft = (int)x - 52;
			selectionRight = (int)x + resolutionStringWidth + 10;
		}

		for (size_t i = 0; i < strlen(temp); i++)
		{
			RECT r = (temp[i] != 'x') ? menuSprites[temp[i] - '0'] : menuSprites[10];
			float w = (float)(r.right - r.left);
			float h = (float)(r.bottom - r.top);
			DrawMenuSprite(r, x * sx, y * sy, w * sx, h * sy);
			x += w;
		}
	}

	{
		RECT label = menuSprites[12];
		float labelX = (640.0f - ((float)(menuSprites[12].right - menuSprites[12].left) + (float)(menuSprites[16].right - menuSprites[16].left) + 10.0f)) / 2.0f;
		float y = (float)menuLocations[2];

		if (curMenu == 1)
		{
			selectionLeft = (int)labelX - 52;
			selectionRight = (int)labelX + (menuSprites[12].right - menuSprites[12].left) + (menuSprites[16].right - menuSprites[16].left) + 20;
		}

		DrawMenuSprite(label, labelX * sx, y * sy, (label.right - label.left) * sx, (label.bottom - label.top) * sy);

		if (menuWindowed)
		{
			RECT offRect = menuSprites[16];
			float x = labelX + (menuSprites[12].right - menuSprites[12].left) + 10.0f;
			DrawMenuSprite(offRect, x * sx, y * sy, (offRect.right - offRect.left) * sx, (offRect.bottom - offRect.top) * sy);
		}
		else
		{
			RECT onRect = menuSprites[15];
			float x = labelX + (menuSprites[12].right - menuSprites[12].left) + 10.0f + 14.0f;
			DrawMenuSprite(onRect, x * sx, y * sy, (onRect.right - onRect.left) * sx, (onRect.bottom - onRect.top) * sy);
		}
	}

	{
		RECT label = menuSprites[13];
		float labelX = (640.0f - ((float)(menuSprites[13].right - menuSprites[13].left) + (float)(menuSprites[16].right - menuSprites[16].left) + 10.0f)) / 2.0f;
		float y = (float)menuLocations[3];

		if (curMenu == 2)
		{
			selectionLeft = (int)labelX - 52;
			selectionRight = (int)labelX + (menuSprites[13].right - menuSprites[13].left) + (menuSprites[16].right - menuSprites[16].left) + 20;
		}

		DrawMenuSprite(label, labelX * sx, y * sy, (label.right - label.left) * sx, (label.bottom - label.top) * sy);

		if (!menuvSync)
		{
			RECT offRect = menuSprites[16];
			float x = labelX + (menuSprites[13].right - menuSprites[13].left) + 10.0f;
			DrawMenuSprite(offRect, x * sx, y * sy, (offRect.right - offRect.left) * sx, (offRect.bottom - offRect.top) * sy);
		}
		else
		{
			RECT onRect = menuSprites[15];
			float x = labelX + (menuSprites[13].right - menuSprites[13].left) + 10.0f + 14.0f;
			DrawMenuSprite(onRect, x * sx, y * sy, (onRect.right - onRect.left) * sx, (onRect.bottom - onRect.top) * sy);
		}
	}

	{
		RECT r = menuSprites[14];
		float x = (640.0f - (float)(r.right - r.left)) / 2.0f;
		float y = (float)menuLocations[4];

		if (curMenu == 3)
		{
			selectionLeft = (int)x - 52;
			selectionRight = (int)x + (r.right - r.left) + 10;
		}

		DrawMenuSprite(r, x * sx, y * sy, (r.right - r.left) * sx, (r.bottom - r.top) * sy);
	}

	{
		RECT sLoc;
		sLoc.left = 42 * (curMenuFrame % 4);
		sLoc.top = 160 + (42 * (int)(curMenuFrame / 4));
		sLoc.right = sLoc.left + 42;
		sLoc.bottom = sLoc.top + 42;

		curMenuFrame++;
		if (curMenuFrame > 7)
			curMenuFrame = 0;

		float x = (float)selectionLeft;
		float y = (float)menuLocations[curMenu + 1] - 5.0f;
		DrawMenuSprite(sLoc, x * sx, y * sy, 42.0f * sx, 42.0f * sy);

		x = (float)selectionRight;
		DrawMenuSprite(sLoc, x * sx, y * sy, 42.0f * sx, 42.0f * sy);
	}
}

bool IDirectDrawWrapper::ReinitDevice()
{
	if (!swapChain || !d3d11Context)
		return CreateD3DDevice();

	d3d11Context->ClearState();
	d3d11Context->Flush();

	renderTargetView.Reset();

	HRESULT hr = swapChain->ResizeBuffers(
		1,
		isWindowed ? 0 : displayWidth,
		isWindowed ? 0 : displayHeight,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

	if (FAILED(hr))
	{
		debugMessage(0, "IDirectDrawWrapper::ReinitDevice", "ResizeBuffers failed");
		return false;
	}

	if (!CreateRenderTarget())
		return false;

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (FLOAT)displayWidth;
	vp.Height = (FLOAT)displayHeight;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	d3d11Context->RSSetViewports(1, &vp);

	debugMessage(2, "IDirectDrawWrapper::ReinitDevice", "Reset device, now create texture");

	return CreateSurfaceTexture();
}