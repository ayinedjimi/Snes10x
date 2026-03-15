/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <immintrin.h>

#include "CDirect3D11.h"

// Stub logging when wsnes9x.h logging is unavailable
#ifndef INFOLOG_DEFINED
#define InfoLog(...) do { char _b[512]; _snprintf(_b,sizeof(_b),__VA_ARGS__); OutputDebugStringA(_b); OutputDebugStringA("\n"); } while(0)
#define WarnLog InfoLog
#define ErrLog InfoLog
#define PerfLog(e,a,b,c) (void)0
#define TraceLog InfoLog
#endif
#include "win32_display.h"
#include "../snes9x.h"
#include "../gfx.h"
#include "../ppu.h"
#include "../display.h"
#include "wsnes9x.h"
#include "dxerr.h"
#include <commctrl.h>
#include <vector>

#include "../filter/hq2x.h"
#include "../filter/2xsai.h"

#include "imgui_impl_dx11.h"
#include "snes9x_imgui.h"

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif


CDirect3D11::CDirect3D11()
{
    init_done = false;
    pDevice = nullptr;
    pContext = nullptr;
    pSwapChain = nullptr;
    pRenderTarget = nullptr;
    pRenderTargetView = nullptr;
    pDrawTexture = nullptr;
    pDrawTextureSRV = nullptr;
    pVertexShader = nullptr;
    pPixelShaderPoint = nullptr;
    pPixelShaderLinear = nullptr;
    pInputLayout = nullptr;
    pSamplerPoint = nullptr;
    pSamplerLinear = nullptr;
    pVertexBuffer = nullptr;
    pConstantBuffer = nullptr;
    pBlendState = nullptr;
    pRasterizerState = nullptr;
    pDepthStencilState = nullptr;
    afterRenderWidth = 0;
    afterRenderHeight = 0;
    quadTextureSize = 0;
    fullscreen = false;
    tearingEnabled = false;
    textureIs32bit = false;
    hWnd = nullptr;
    backBufferWidth = 0;
    backBufferHeight = 0;
    filterScale = 1;
}

CDirect3D11::~CDirect3D11()
{
    DeInitialize();
}

static bool CompileD3D11Shader(ID3D11Device* device, const char* hlsl, const char* entry, const char* profile,
    ID3D11VertexShader** ppVS, ID3D11PixelShader** ppPS, ID3D11InputLayout** ppLayout)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, profile, 0, 0, &blob, &errors)))
    {
        if (errors) { errors->Release(); }
        return false;
    }

    if (strstr(profile, "vs_"))
    {
        if (FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ppVS)))
        {
            blob->Release();
            return false;
        }
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        device->CreateInputLayout(layout, 3, blob->GetBufferPointer(), blob->GetBufferSize(), ppLayout);
    }
    else if (strstr(profile, "ps_"))
    {
        if (ppPS && FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ppPS)))
        {
            blob->Release();
            return false;
        }
    }
    blob->Release();
    return true;
}

bool CDirect3D11::Initialize(HWND hWnd)
{
    // RAW diagnostic via stdout (redirected to stdout.txt by WinMain)
    fprintf(stdout, "CDirect3D11::Initialize called init_done=%d\n", (int)init_done);
    fflush(stdout);
    InfoLog("CDirect3D11::Initialize ENTRY init_done=%d hWnd=%p", (int)init_done, (void*)hWnd);
    if (init_done) return true;

    this->hWnd = hWnd;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL gotLevel;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, 2, D3D11_SDK_VERSION, &pDevice, &gotLevel, &pContext);
    InfoLog("CDirect3D11::Initialize D3D11CreateDevice hr=0x%08X featureLevel=0x%x", (unsigned)hr, (unsigned)gotLevel);
    if (FAILED(hr))
    {
        DXTRACE_ERR_MSGBOX(TEXT("Error creating D3D11 device"), hr);
        return false;
    }

    IDXGIFactory1* pFactory = nullptr;
    IDXGIDevice* pDXGIDevice = nullptr;
    IDXGIAdapter* pAdapter = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice))) ||
        FAILED(pDXGIDevice->GetAdapter(&pAdapter)) ||
        FAILED(pAdapter->GetParent(IID_PPV_ARGS(&pFactory))))
    {
        if (pDXGIDevice) pDXGIDevice->Release();
        if (pAdapter) pAdapter->Release();
        return false;
    }

    RECT rc;
    GetClientRect(hWnd, &rc);
    unsigned int width = max(1, rc.right - rc.left);
    unsigned int height = max(1, rc.bottom - rc.top);

    // FLIP_DISCARD requires at least 2 buffers; use 3 for triple buffering when enabled
    unsigned int bufferCount = max(2u, GUI.DoubleBuffered ? 2u : 2u);

    IDXGIFactory2* pFactory2 = nullptr;
    if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&pFactory2))))
    {
        DXGI_SWAP_CHAIN_DESC1 scd1 = {};
        scd1.Width = width;
        scd1.Height = height;
        scd1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd1.SampleDesc.Count = 1;
        scd1.SampleDesc.Quality = 0;
        scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd1.BufferCount = bufferCount;
        scd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd1.Flags = GUI.Vsync ? 0 : DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        IDXGISwapChain1* pSwapChain1 = nullptr;
        hr = pFactory2->CreateSwapChainForHwnd(pDevice, hWnd, &scd1, nullptr, nullptr, &pSwapChain1);
        if (FAILED(hr) && !GUI.Vsync)
        {
            scd1.Flags = 0;
            hr = pFactory2->CreateSwapChainForHwnd(pDevice, hWnd, &scd1, nullptr, nullptr, &pSwapChain1);
        }
        if (SUCCEEDED(hr))
        {
            pSwapChain = pSwapChain1;
            tearingEnabled = (scd1.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
            InfoLog("D3D11 SwapChain FLIP_DISCARD %ux%u buffers=%u tearing=%d",
                scd1.Width, scd1.Height, scd1.BufferCount, (int)tearingEnabled);
            if (!GUI.Vsync)
                pFactory2->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        }
        else {
            WarnLog("D3D11 CreateSwapChainForHwnd(FLIP_DISCARD) FAILED hr=0x%08X", (unsigned)hr);
        }
        pFactory2->Release();
    }
    if (!pSwapChain)
    {
        // Fallback to legacy DISCARD swap chain
        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount = bufferCount;
        scd.BufferDesc.Width = width;
        scd.BufferDesc.Height = height;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferDesc.RefreshRate.Numerator = 0;
        scd.BufferDesc.RefreshRate.Denominator = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hWnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        scd.Flags = 0;
        hr = pFactory->CreateSwapChain(pDevice, &scd, &pSwapChain);
        WarnLog("D3D11 Fallback to DISCARD swap chain hr=0x%08X pSwapChain=%p", (unsigned)hr, pSwapChain);
    }

    pFactory->Release();
    pAdapter->Release();
    pDXGIDevice->Release();

    if (FAILED(hr) || !pSwapChain)
    {
        DXTRACE_ERR_MSGBOX(TEXT("Error creating swap chain"), hr);
        return false;
    }

    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hr)) return false;

    hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) return false;

    pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
    backBufferWidth = width;
    backBufferHeight = height;

    static const char* vsHLSL =
        "cbuffer constants : register(b0) { float4x4 proj; }\n"
        "struct VS_IN { float3 pos : POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };\n"
        "PS_IN main(VS_IN i) { PS_IN o; o.pos = mul(proj, float4(i.pos, 1)); o.uv0 = i.uv0; o.uv1 = i.uv1; return o; }\n";

    static const char* psHLSL =
        "Texture2D tex : register(t0); SamplerState samp : register(s0);\n"
        "struct PS_IN { float4 pos : SV_POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };\n"
        "float4 main(PS_IN i) : SV_Target { return tex.Sample(samp, i.uv0); }\n";

    if (!CompileD3D11Shader(pDevice, vsHLSL, "main", "vs_4_0", &pVertexShader, nullptr, &pInputLayout))
    {
        ErrLog("D3D11 Initialize: FAILED to compile vertex shader");
        DXTRACE_ERR_MSGBOX(TEXT("Failed to compile vertex shader"), 0);
        return false;
    }
    InfoLog("D3D11 Initialize: VS compiled OK pVertexShader=%p pInputLayout=%p", pVertexShader, pInputLayout);

    if (!CompileD3D11Shader(pDevice, psHLSL, "main", "ps_4_0", nullptr, &pPixelShaderPoint, nullptr))
    {
        ErrLog("D3D11 Initialize: FAILED to compile pixel shader");
        DXTRACE_ERR_MSGBOX(TEXT("Failed to compile pixel shader"), 0);
        return false;
    }
    InfoLog("D3D11 Initialize: PS compiled OK pPixelShaderPoint=%p", pPixelShaderPoint);
    pPixelShaderLinear = pPixelShaderPoint;

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    HRESULT hrSamp = pDevice->CreateSamplerState(&sampDesc, &pSamplerPoint);
    if (FAILED(hrSamp)) { ErrLog("D3D11 CreateSamplerPoint FAILED hr=0x%08X", (unsigned)hrSamp); return false; }

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hrSamp = pDevice->CreateSamplerState(&sampDesc, &pSamplerLinear);
    if (FAILED(hrSamp)) { ErrLog("D3D11 CreateSamplerLinear FAILED hr=0x%08X", (unsigned)hrSamp); return false; }
    InfoLog("D3D11 Initialize: samplers OK (CLAMP mode)");

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(VERTEX_D3D11) * 4;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hrVB = pDevice->CreateBuffer(&vbDesc, nullptr, &pVertexBuffer);
    if (FAILED(hrVB)) { ErrLog("D3D11 CreateBuffer(VB) FAILED hr=0x%08X", (unsigned)hrVB); return false; }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 64;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hrCB = pDevice->CreateBuffer(&cbDesc, nullptr, &pConstantBuffer);
    if (FAILED(hrCB)) { ErrLog("D3D11 CreateBuffer(CB) FAILED hr=0x%08X", (unsigned)hrCB); return false; }
    InfoLog("D3D11 Initialize: buffers OK pVB=%p pCB=%p sizeof(VERTEX)=%u", pVertexBuffer, pConstantBuffer, (unsigned)sizeof(VERTEX_D3D11));

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    HRESULT hrBlend = pDevice->CreateBlendState(&blendDesc, &pBlendState);
    if (FAILED(hrBlend)) { ErrLog("D3D11 CreateBlendState FAILED hr=0x%08X", (unsigned)hrBlend); return false; }

    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = FALSE;
    HRESULT hrRast = pDevice->CreateRasterizerState(&rastDesc, &pRasterizerState);
    if (FAILED(hrRast)) { ErrLog("D3D11 CreateRasterizerState FAILED hr=0x%08X", (unsigned)hrRast); return false; }

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    HRESULT hrDS = pDevice->CreateDepthStencilState(&dsDesc, &pDepthStencilState);
    if (FAILED(hrDS)) { ErrLog("D3D11 CreateDepthStencilState FAILED hr=0x%08X", (unsigned)hrDS); return false; }
    InfoLog("D3D11 Initialize: states OK blend=%p rast=%p ds=%p", pBlendState, pRasterizerState, pDepthStencilState);

    CreateDrawTexture();
    CreateGPUConvertResources();
    Clear();

    init_done = true;

    if (!Settings.AutoDisplayMessages)
    {
        auto defaults = S9xImGuiGetDefaults();
        defaults.font_size = GUI.OSDSize;
        defaults.spacing = (int)(defaults.font_size / 2.4);
        S9xImGuiInit(&defaults);
        ImGui_ImplDX11_Init(pDevice, pContext);
        Settings.DisplayIndicators = true;
    }

    ApplyDisplayChanges();

    // Start the render thread
    renderRunning.store(true, std::memory_order_relaxed);
    renderHasFrame = false;
    renderFrameDone = true;
    renderThread = std::thread(&CDirect3D11::RenderThreadFunc, this);
    InfoLog("D3D11 Render thread started");

    return true;
}

void CDirect3D11::DeInitialize()
{
    // Stop render thread before releasing D3D11 resources
    if (renderRunning.load(std::memory_order_relaxed))
    {
        renderRunning.store(false, std::memory_order_relaxed);
        renderCV_work.notify_one();
        if (renderThread.joinable())
            renderThread.join();
        InfoLog("D3D11 Render thread stopped");
    }

    if (init_done && S9xImGuiRunning())
    {
        ImGui_ImplDX11_Shutdown();
        S9xImGuiDeinit();
    }

    DestroyGPUConvertResources();
    DestroyDrawTexture();

    if (pDepthStencilState) { pDepthStencilState->Release(); pDepthStencilState = nullptr; }
    if (pRasterizerState) { pRasterizerState->Release(); pRasterizerState = nullptr; }
    if (pBlendState) { pBlendState->Release(); pBlendState = nullptr; }
    if (pConstantBuffer) { pConstantBuffer->Release(); pConstantBuffer = nullptr; }
    if (pVertexBuffer) { pVertexBuffer->Release(); pVertexBuffer = nullptr; }
    if (pSamplerLinear) { pSamplerLinear->Release(); pSamplerLinear = nullptr; }
    if (pSamplerPoint) { pSamplerPoint->Release(); pSamplerPoint = nullptr; }
    if (pPixelShaderLinear && pPixelShaderLinear != pPixelShaderPoint) pPixelShaderLinear->Release();
    if (pPixelShaderPoint) { pPixelShaderPoint->Release(); pPixelShaderPoint = nullptr; }
    pPixelShaderLinear = nullptr;
    if (pVertexShader) { pVertexShader->Release(); pVertexShader = nullptr; }
    if (pInputLayout) { pInputLayout->Release(); pInputLayout = nullptr; }
    if (pRenderTargetView) { pRenderTargetView->Release(); pRenderTargetView = nullptr; }
    if (pRenderTarget) { pRenderTarget->Release(); pRenderTarget = nullptr; }
    if (pSwapChain) { pSwapChain->Release(); pSwapChain = nullptr; }
    if (pContext) { pContext->Release(); pContext = nullptr; }
    if (pDevice) { pDevice->Release(); pDevice = nullptr; }

    init_done = false;
    afterRenderWidth = afterRenderHeight = quadTextureSize = 0;
    fullscreen = false;
    hWnd = nullptr;
}

void CDirect3D11::CreateDrawTexture()
{
    unsigned int neededSize = SNES_WIDTH * filterScale;
    quadTextureSize = 512;
    while (quadTextureSize < neededSize) quadTextureSize *= 2;

    if (pDrawTexture) return;

    // Always use B8G8R8A8 — avoids R-B swap issues with B5G6R5 and is universally supported
    DXGI_FORMAT texFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureIs32bit = true;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = quadTextureSize;
    texDesc.Height = quadTextureSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = texFmt;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DYNAMIC;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hrTex = pDevice->CreateTexture2D(&texDesc, nullptr, &pDrawTexture);
    if (FAILED(hrTex)) {
        ErrLog("D3D11 CreateTexture2D FAILED hr=0x%08X fmt=%d size=%u", (unsigned)hrTex, (int)texFmt, quadTextureSize);
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    HRESULT hrSRV = pDevice->CreateShaderResourceView(pDrawTexture, &srvDesc, &pDrawTextureSRV);
    if (FAILED(hrSRV)) {
        ErrLog("D3D11 CreateShaderResourceView FAILED hr=0x%08X fmt=%d", (unsigned)hrSRV, (int)texFmt);
        pDrawTexture->Release(); pDrawTexture = nullptr;
        return;
    }
    InfoLog("D3D11 CreateDrawTexture OK: pDrawTexture=%p pDrawTextureSRV=%p", pDrawTexture, pDrawTextureSRV);

    // Note: even with B8G8R8A8 texture, snes9x core always renders 16-bit RGB565.
    // The Render() method converts 16-bit pixels to 32-bit when textureIs32bit is set.
}

void CDirect3D11::DestroyDrawTexture()
{
    if (pDrawTextureSRV) { pDrawTextureSRV->Release(); pDrawTextureSRV = nullptr; }
    if (pDrawTexture) { pDrawTexture->Release(); pDrawTexture = nullptr; }
}

void CDirect3D11::CreateGPUConvertResources()
{
    DestroyGPUConvertResources();

    // Create R16_UINT texture for raw RGB565 pixel upload
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = quadTextureSize;
    texDesc.Height = quadTextureSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16_UINT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DYNAMIC;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = pDevice->CreateTexture2D(&texDesc, nullptr, &pRGB565Texture);
    if (FAILED(hr)) {
        WarnLog("D3D11 CreateGPUConvert: R16_UINT texture FAILED hr=0x%08X", (unsigned)hr);
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = pDevice->CreateShaderResourceView(pRGB565Texture, &srvDesc, &pRGB565SRV);
    if (FAILED(hr)) {
        WarnLog("D3D11 CreateGPUConvert: SRV FAILED hr=0x%08X", (unsigned)hr);
        pRGB565Texture->Release(); pRGB565Texture = nullptr;
        return;
    }

    // Pixel shader that reads R16_UINT and converts RGB565 → float4 RGBA
    static const char* psConvertHLSL =
        "Texture2D<uint> texRGB565 : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "struct PS_IN { float4 pos : SV_POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };\n"
        "float4 main(PS_IN i) : SV_Target {\n"
        "  int2 coord = int2(i.uv0 * float2(%d, %d));\n"  // placeholder — we'll use Load instead
        "  return float4(1,0,1,1);\n"
        "}\n";

    // Better approach: use Load() with SV_POSITION for exact pixel coords
    static const char* psConvertHLSL2 =
        "Texture2D<uint> texIn : register(t0);\n"
        "struct PS_IN { float4 pos : SV_POSITION; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };\n"
        "float4 main(PS_IN i) : SV_Target {\n"
        "  uint2 dim;\n"
        "  texIn.GetDimensions(dim.x, dim.y);\n"
        "  uint2 coord = uint2(i.uv0 * float2(dim));\n"
        "  uint px = texIn.Load(int3(coord, 0));\n"
        "  float r = float((px >> 11) & 0x1F) / 31.0;\n"
        "  float g = float((px >> 6)  & 0x1F) / 31.0;\n"
        "  float b = float(px & 0x1F) / 31.0;\n"
        "  return float4(r, g, b, 1.0);\n"
        "}\n";

    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    hr = D3DCompile(psConvertHLSL2, strlen(psConvertHLSL2), nullptr, nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            WarnLog("D3D11 GPU Convert PS compile FAILED: %s", (char*)errors->GetBufferPointer());
            errors->Release();
        }
        DestroyGPUConvertResources();
        return;
    }
    hr = pDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &pPixelShaderConvert);
    blob->Release();
    if (FAILED(hr)) {
        WarnLog("D3D11 GPU Convert PS create FAILED hr=0x%08X", (unsigned)hr);
        DestroyGPUConvertResources();
        return;
    }

    gpuConvertEnabled = true;
    InfoLog("D3D11 GPU color conversion enabled (R16_UINT → RGBA pixel shader)");
}

void CDirect3D11::DestroyGPUConvertResources()
{
    if (pPixelShaderConvert) { pPixelShaderConvert->Release(); pPixelShaderConvert = nullptr; }
    if (pRGB565SRV) { pRGB565SRV->Release(); pRGB565SRV = nullptr; }
    if (pRGB565Texture) { pRGB565Texture->Release(); pRGB565Texture = nullptr; }
    gpuConvertEnabled = false;
}

bool CDirect3D11::ChangeDrawTextureSize(unsigned int scale)
{
    filterScale = scale;
    if (pDevice)
    {
        DestroyDrawTexture();
        CreateDrawTexture();
        SetupVertices();
        return true;
    }
    return false;
}

void CDirect3D11::SetupVertices()
{
    float tX = (float)afterRenderWidth / (float)quadTextureSize;
    float tY = (float)afterRenderHeight / (float)quadTextureSize;

    static int vertexLogCount = 0;
    if (vertexLogCount < 5) {
        vertexLogCount++;
        TraceLog("D3D11 SetupVertices#%d: afterW=%u afterH=%u texSz=%u tX=%.4f tY=%.4f",
            vertexLogCount, afterRenderWidth, afterRenderHeight, quadTextureSize, tX, tY);
    }

    VERTEX_D3D11 vertices[4] = {
        { 0.0f, 0.0f, 0.0f, 0.0f, tY, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f, tX, tY, 0.0f, 0.0f },
        { 1.0f, 1.0f, 0.0f, tX, 0.0f, 0.0f, 0.0f },
    };

    for (int i = 0; i < 4; i++)
    {
        vertices[i].x -= 0.5f / (float)backBufferWidth;
        vertices[i].y += 0.5f / (float)backBufferHeight;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(pContext->Map(pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, vertices, sizeof(vertices));
        pContext->Unmap(pVertexBuffer, 0);
    }
}

void CDirect3D11::SetViewport()
{
    RECT drawRect = CalculateDisplayRect(afterRenderWidth, afterRenderHeight, backBufferWidth, backBufferHeight);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = (float)drawRect.left;
    vp.TopLeftY = (float)drawRect.top;
    vp.Width = (float)(drawRect.right - drawRect.left);
    vp.Height = (float)(drawRect.bottom - drawRect.top);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    static int vpLogCount = 0;
    if (vpLogCount < 5) {
        vpLogCount++;
        TraceLog("D3D11 SetViewport#%d: src=%ux%u backBuf=%ux%u drawRect=%d,%d-%d,%d vp=%.0fx%.0f",
            vpLogCount, afterRenderWidth, afterRenderHeight, backBufferWidth, backBufferHeight,
            drawRect.left, drawRect.top, drawRect.right, drawRect.bottom,
            vp.Width, vp.Height);
    }

    pContext->RSSetViewports(1, &vp);

    float proj[4][4] = {
        { 2.0f, 0, 0, 0 },
        { 0, 2.0f, 0, 0 },
        { 0, 0, -1.0f, 0 },
        { -1.0f, -1.0f, 0, 1.0f },
    };

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(pContext->Map(pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, proj, 64);
        pContext->Unmap(pConstantBuffer, 0);
    }

    SetupVertices();
}

void CDirect3D11::Clear()
{
    if (!init_done) return;

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)backBufferWidth;
    vp.Height = (float)backBufferHeight;
    vp.MaxDepth = 1.0f;
    pContext->RSSetViewports(1, &vp);
    float clearColor[] = { 0, 0, 0, 1 };
    pContext->ClearRenderTargetView(pRenderTargetView, clearColor);
}

// ============================================================================
// Render Thread: async display pipeline
// Emulation thread calls Render() which copies pixels to staging buffer and
// signals the render thread. The render thread does filter + color convert +
// texture upload + Draw + Present(). Emulation continues in parallel.
// ============================================================================

void CDirect3D11::RenderThreadFunc()
{
    while (renderRunning.load(std::memory_order_relaxed))
    {
        {
            std::unique_lock<std::mutex> lock(renderMtx);
            renderCV_work.wait(lock, [this] {
                return renderHasFrame || !renderRunning.load(std::memory_order_relaxed);
            });
            if (!renderRunning.load(std::memory_order_relaxed))
                break;
        }

        // Do the actual D3D11 rendering from the staging buffer
        RenderFrame();

        {
            std::lock_guard<std::mutex> lock(renderMtx);
            renderHasFrame = false;
            renderFrameDone = true;
        }
        renderCV_done.notify_one();
    }
}

void CDirect3D11::WaitForRenderDone()
{
    std::unique_lock<std::mutex> lock(renderMtx);
    renderCV_done.wait(lock, [this] { return renderFrameDone; });
}

void CDirect3D11::Render(SSurface Src)
{
    if (!init_done) return;

    // Wait for render thread to finish previous frame (back-pressure)
    WaitForRenderDone();

    // Copy the SNES screen to our staging buffer
    // This decouples the emulation thread from the GPU thread
    size_t frameBytes = (size_t)Src.Pitch * Src.Height;
    if (stagingBuffer.size() < frameBytes)
        stagingBuffer.resize(frameBytes);

    memcpy(stagingBuffer.data(), Src.Surface, frameBytes);

    // Record frame metadata
    stagingSrc.Surface = stagingBuffer.data();
    stagingSrc.Width = Src.Width;
    stagingSrc.Height = Src.Height;
    stagingSrc.Pitch = Src.Pitch;

    // Signal render thread
    {
        std::lock_guard<std::mutex> lock(renderMtx);
        renderHasFrame = true;
        renderFrameDone = false;
    }
    renderCV_work.notify_one();

    // Emulation thread returns immediately — next frame starts in parallel
}

// RenderFrame: all D3D11 work happens here, on the render thread
void CDirect3D11::RenderFrame()
{
    SSurface Dst;
    RECT dstRect;
    unsigned int newFilterScale;
    D3D11_MAPPED_SUBRESOURCE mapped;

    newFilterScale = max(2, max(GetFilterScale(GUI.ScaleHiRes), GetFilterScale(GUI.Scale)));
    if (newFilterScale != filterScale)
    {
        ChangeDrawTextureSize(newFilterScale);
        if (gpuConvertEnabled)
            CreateGPUConvertResources();  // recreate at new size
    }

    // Apply software filter to temporary 16-bit buffer
    static std::vector<uint8_t> tempBuf;
    unsigned int tempPitch = quadTextureSize * 2;
    size_t needed = (size_t)tempPitch * quadTextureSize;
    if (tempBuf.size() < needed) tempBuf.resize(needed);

    Dst.Surface = tempBuf.data();
    Dst.Height = quadTextureSize;
    Dst.Width = quadTextureSize;
    Dst.Pitch = tempPitch;
    RenderMethod(stagingSrc, Dst, &dstRect);

    if (gpuConvertEnabled && pRGB565Texture)
    {
        // === GPU PATH: upload 16-bit pixels directly, pixel shader converts ===
        HRESULT hrMap = pContext->Map(pRGB565Texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hrMap))
        {
            for (int row = 0; row < dstRect.bottom; row++)
            {
                memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch,
                       tempBuf.data() + row * tempPitch,
                       dstRect.right * 2);
            }
            pContext->Unmap(pRGB565Texture, 0);
        }
    }
    else
    {
        // === CPU FALLBACK: AVX2 color conversion ===
        HRESULT hrMap = pContext->Map(pDrawTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hrMap))
            return;

        for (int row = 0; row < dstRect.bottom; row++)
        {
            const uint16_t* src16 = (const uint16_t*)(tempBuf.data() + row * tempPitch);
            uint32_t* dst32 = (uint32_t*)((uint8_t*)mapped.pData + row * mapped.RowPitch);
            int col = 0;
            int width = dstRect.right;

            const __m256i mask5  = _mm256_set1_epi32(0x1F);
            const __m256i alpha  = _mm256_set1_epi32(0xFF000000u);
            // Use non-temporal stores to avoid polluting L1/L2 cache
            // (framebuffer data is write-only, won't be read back by CPU)
            bool aligned32 = ((uintptr_t)(dst32) & 31) == 0;
            for (; col + 8 <= width; col += 8)
            {
                __m128i src = _mm_loadu_si128((const __m128i*)(src16 + col));
                __m256i px = _mm256_cvtepu16_epi32(src);
                __m256i r5 = _mm256_and_si256(_mm256_srli_epi32(px, 11), mask5);
                __m256i g5 = _mm256_and_si256(_mm256_srli_epi32(px, 6), mask5);
                __m256i b5 = _mm256_and_si256(px, mask5);
                __m256i r8 = _mm256_or_si256(_mm256_slli_epi32(r5, 3), _mm256_srli_epi32(r5, 2));
                __m256i g8 = _mm256_or_si256(_mm256_slli_epi32(g5, 3), _mm256_srli_epi32(g5, 2));
                __m256i b8 = _mm256_or_si256(_mm256_slli_epi32(b5, 3), _mm256_srli_epi32(b5, 2));
                __m256i result = _mm256_or_si256(alpha,
                    _mm256_or_si256(_mm256_slli_epi32(r8, 16),
                        _mm256_or_si256(_mm256_slli_epi32(g8, 8), b8)));
                if (aligned32)
                    _mm256_stream_si256((__m256i*)(dst32 + col), result);
                else
                    _mm256_storeu_si256((__m256i*)(dst32 + col), result);
            }
            // Fence after non-temporal stores
            if (aligned32) _mm_sfence();
            for (; col < width; col++)
            {
                uint16_t px = src16[col];
                unsigned int r5 = (px >> 11) & 0x1F;
                unsigned int g5 = (px >> 6) & 0x1F;
                unsigned int b5 = px & 0x1F;
                dst32[col] = 0xFF000000u | ((r5<<3|r5>>2)<<16) | ((g5<<3|g5>>2)<<8) | (b5<<3|b5>>2);
            }
        }
        pContext->Unmap(pDrawTexture, 0);
    }

    if (!GUI.Stretch || GUI.AspectRatio)
        Clear();

    if (afterRenderHeight != (unsigned)dstRect.bottom || afterRenderWidth != (unsigned)dstRect.right)
    {
        afterRenderHeight = dstRect.bottom;
        afterRenderWidth = dstRect.right;
        SetViewport();
    }

    if (!pDrawTextureSRV || !pRenderTargetView)
        return;

    pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);

    pContext->IASetInputLayout(pInputLayout);
    UINT stride = sizeof(VERTEX_D3D11), offset = 0;
    pContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &offset);
    pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pContext->VSSetShader(pVertexShader, nullptr, 0);
    pContext->VSSetConstantBuffers(0, 1, &pConstantBuffer);

    if (gpuConvertEnabled && pPixelShaderConvert)
    {
        // GPU path: R16_UINT texture + conversion pixel shader
        pContext->PSSetShader(pPixelShaderConvert, nullptr, 0);
        pContext->PSSetShaderResources(0, 1, &pRGB565SRV);
    }
    else
    {
        // CPU path: pre-converted BGRA texture + standard shader
        ID3D11PixelShader* ps = Settings.BilinearFilter ? pPixelShaderLinear : pPixelShaderPoint;
        pContext->PSSetShader(ps, nullptr, 0);
        pContext->PSSetShaderResources(0, 1, &pDrawTextureSRV);
    }
    ID3D11SamplerState* samp = Settings.BilinearFilter ? pSamplerLinear : pSamplerPoint;
    pContext->PSSetSamplers(0, 1, &samp);
    pContext->OMSetBlendState(pBlendState, nullptr, 0xffffffff);
    pContext->OMSetDepthStencilState(pDepthStencilState, 0);
    pContext->RSSetState(pRasterizerState);

    pContext->Draw(4, 0);

    if (S9xImGuiRunning())
    {
        ImGui_ImplDX11_NewFrame();
        if (S9xImGuiDraw(backBufferWidth, backBufferHeight))
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    UINT syncInterval = GUI.Vsync ? 1 : 0;
    UINT flags = (!GUI.Vsync && !fullscreen && tearingEnabled) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    pSwapChain->Present(syncInterval, flags);

    WinThrottleFramerate();

    if (GUI.ReduceInputLag)
        pContext->Flush();
}

bool CDirect3D11::ChangeRenderSize(unsigned int newWidth, unsigned int newHeight)
{
    if (!init_done) return false;
    if (backBufferWidth == newWidth && backBufferHeight == newHeight) return true;
    if (fullscreen) return true;

    // Wait for render thread to finish before touching D3D11 resources
    WaitForRenderDone();

    TraceLog("D3D11 ChangeRenderSize: %ux%u → %ux%u tearingEnabled=%d",
        backBufferWidth, backBufferHeight, newWidth, newHeight, (int)tearingEnabled);

    if (pRenderTargetView) { pRenderTargetView->Release(); pRenderTargetView = nullptr; }

    HRESULT hr = pSwapChain->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_UNKNOWN,
        tearingEnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        ErrLog("D3D11 ResizeBuffers(%ux%u) FAILED hr=0x%08X", newWidth, newHeight, (unsigned)hr);
        return false;
    }

    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hr)) return false;

    hr = pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) return false;

    backBufferWidth = newWidth;
    backBufferHeight = newHeight;
    pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
    CreateDrawTexture();
    SetViewport();

    if (S9xImGuiRunning())
        ImGui_ImplDX11_CreateDeviceObjects();

    return true;
}

void CDirect3D11::SetSnes9xColorFormat()
{
    // Use the same color format as all other renderers (DX9, OpenGL, Vulkan):
    // RGB565 with Red in bits[15:11], Green in bits[10:5], Blue in bits[4:0]
    GUI.ScreenDepth = 16;
    GUI.BlueShift  = 0;
    GUI.GreenShift = 6;
    GUI.RedShift   = 11;
    S9xBlit2xSaIFilterInit();
    S9xBlitHQ2xFilterInit();
    GUI.NeedDepthConvert = FALSE;
    GUI.DepthConverted = TRUE;
}

bool CDirect3D11::SetFullscreen(bool fullscreen)
{
    if (!init_done) return false;
    if (this->fullscreen == fullscreen) return true;

    // Wait for render thread to finish before touching swap chain
    WaitForRenderDone();

    this->fullscreen = fullscreen;
    pSwapChain->SetFullscreenState(fullscreen ? TRUE : FALSE, nullptr);

    if (fullscreen)
    {
        DXGI_MODE_DESC md = {};
        md.Width = GUI.FullscreenMode.width;
        md.Height = GUI.FullscreenMode.height;
        md.RefreshRate.Numerator = GUI.FullscreenMode.rate;
        md.RefreshRate.Denominator = 1;
        md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        pSwapChain->ResizeTarget(&md);
    }
    else
    {
        RECT rc;
        GetClientRect(hWnd, &rc);
        backBufferWidth = rc.right - rc.left;
        backBufferHeight = rc.bottom - rc.top;
        if (backBufferWidth && backBufferHeight)
            ChangeRenderSize(backBufferWidth, backBufferHeight);
    }

    if (pRenderTargetView)
        pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);

    pSwapChain->Present(0, 0);
    return true;
}

void CDirect3D11::EnumModes(std::vector<dMode>* modeVector)
{
    if (!init_done) return;

    IDXGIFactory1* pFactory = nullptr;
    IDXGIDevice* pDXGIDevice = nullptr;
    IDXGIAdapter* pAdapter = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice))) ||
        FAILED(pDXGIDevice->GetAdapter(&pAdapter)))
    {
        if (pDXGIDevice) pDXGIDevice->Release();
        return;
    }

    IDXGIOutput* pOutput = nullptr;
    if (SUCCEEDED(pAdapter->EnumOutputs(0, &pOutput)))
    {
        UINT numModes = 0;
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
        pOutput->GetDisplayModeList(format, 0, &numModes, nullptr);
        if (numModes > 0)
        {
            DXGI_MODE_DESC* modes = new DXGI_MODE_DESC[numModes];
            pOutput->GetDisplayModeList(format, 0, &numModes, modes);
            for (UINT i = 0; i < numModes; i++)
            {
                dMode mode;
                mode.width = modes[i].Width;
                mode.height = modes[i].Height;
                mode.rate = modes[i].RefreshRate.Numerator / max(1, modes[i].RefreshRate.Denominator);
                mode.depth = 32;
                modeVector->push_back(mode);
            }
            delete[] modes;
        }
        pOutput->Release();
    }
    pAdapter->Release();
    pDXGIDevice->Release();
}

bool CDirect3D11::ApplyDisplayChanges(void)
{
    if (!init_done) return false;
    RECT rc;
    GetClientRect(hWnd, &rc);
    unsigned int w = max(1, rc.right - rc.left);
    unsigned int h = max(1, rc.bottom - rc.top);
    return ChangeRenderSize(w, h);
}

std::string CDirect3D11::GetDebugInfo()
{
    char buf[8192];
    int len = 0;

    len += sprintf(buf + len, "=== D3D11 Renderer Debug Info ===\r\n\r\n");
    len += sprintf(buf + len, "init_done: %s\r\n", init_done ? "YES" : "NO");
    len += sprintf(buf + len, "pDevice: %p\r\n", (void*)pDevice);
    len += sprintf(buf + len, "pContext: %p\r\n", (void*)pContext);
    len += sprintf(buf + len, "pSwapChain: %p\r\n", (void*)pSwapChain);
    len += sprintf(buf + len, "pRenderTargetView: %p\r\n", (void*)pRenderTargetView);
    len += sprintf(buf + len, "pDrawTexture: %p\r\n", (void*)pDrawTexture);
    len += sprintf(buf + len, "pDrawTextureSRV: %p\r\n", (void*)pDrawTextureSRV);
    len += sprintf(buf + len, "pVertexShader: %p\r\n", (void*)pVertexShader);
    len += sprintf(buf + len, "pPixelShaderPoint: %p\r\n", (void*)pPixelShaderPoint);
    len += sprintf(buf + len, "pPixelShaderLinear: %p\r\n", (void*)pPixelShaderLinear);
    len += sprintf(buf + len, "pInputLayout: %p\r\n", (void*)pInputLayout);
    len += sprintf(buf + len, "pVertexBuffer: %p\r\n", (void*)pVertexBuffer);
    len += sprintf(buf + len, "pConstantBuffer: %p\r\n", (void*)pConstantBuffer);
    len += sprintf(buf + len, "pBlendState: %p\r\n", (void*)pBlendState);
    len += sprintf(buf + len, "pRasterizerState: %p\r\n", (void*)pRasterizerState);
    len += sprintf(buf + len, "pDepthStencilState: %p\r\n", (void*)pDepthStencilState);
    len += sprintf(buf + len, "\r\n--- Texture / Viewport ---\r\n");
    len += sprintf(buf + len, "textureIs32bit: %s\r\n", textureIs32bit ? "YES (B8G8R8A8)" : "NO (B5G6R5)");
    len += sprintf(buf + len, "quadTextureSize: %u\r\n", quadTextureSize);
    len += sprintf(buf + len, "filterScale: %u\r\n", filterScale);
    len += sprintf(buf + len, "afterRenderWidth: %u\r\n", afterRenderWidth);
    len += sprintf(buf + len, "afterRenderHeight: %u\r\n", afterRenderHeight);
    len += sprintf(buf + len, "backBufferWidth: %u\r\n", backBufferWidth);
    len += sprintf(buf + len, "backBufferHeight: %u\r\n", backBufferHeight);
    len += sprintf(buf + len, "fullscreen: %s\r\n", fullscreen ? "YES" : "NO");
    len += sprintf(buf + len, "tearingEnabled: %s\r\n", tearingEnabled ? "YES" : "NO");

    if (pDrawTexture) {
        D3D11_TEXTURE2D_DESC td;
        pDrawTexture->GetDesc(&td);
        len += sprintf(buf + len, "\r\n--- Draw Texture Desc ---\r\n");
        len += sprintf(buf + len, "Width: %u  Height: %u\r\n", td.Width, td.Height);
        len += sprintf(buf + len, "Format: %d\r\n", (int)td.Format);
        len += sprintf(buf + len, "Usage: %d  BindFlags: 0x%X\r\n", (int)td.Usage, td.BindFlags);
    }

    len += sprintf(buf + len, "\r\n--- GUI Settings ---\r\n");
    len += sprintf(buf + len, "outputMethod: %d (0=DDraw,1=D3D,2=OGL,3=Vulkan)\r\n", GUI.outputMethod);
    len += sprintf(buf + len, "Scale: %d  ScaleHiRes: %d\r\n", GUI.Scale, GUI.ScaleHiRes);
    len += sprintf(buf + len, "Stretch: %d  AspectRatio: %d\r\n", GUI.Stretch, GUI.AspectRatio);
    len += sprintf(buf + len, "ScreenDepth: %d\r\n", GUI.ScreenDepth);
    len += sprintf(buf + len, "BilinearFilter: %d\r\n", Settings.BilinearFilter);
    len += sprintf(buf + len, "Vsync: %d  DoubleBuffered: %d\r\n", GUI.Vsync, GUI.DoubleBuffered);
    len += sprintf(buf + len, "FullScreen: %d\r\n", GUI.FullScreen);
    len += sprintf(buf + len, "FlipCounter: %u\r\n", GUI.FlipCounter);

    len += sprintf(buf + len, "\r\n--- Emulation ---\r\n");
    len += sprintf(buf + len, "StopEmulation: %d\r\n", Settings.StopEmulation);
    len += sprintf(buf + len, "Paused: %d\r\n", Settings.Paused);
    len += sprintf(buf + len, "ForcedPause: %d\r\n", Settings.ForcedPause);
    len += sprintf(buf + len, "FrameAdvance: %d\r\n", Settings.FrameAdvance);

    len += sprintf(buf + len, "\r\n--- Swap Chain ---\r\n");
    if (pSwapChain) {
        DXGI_SWAP_CHAIN_DESC scdesc;
        if (SUCCEEDED(pSwapChain->GetDesc(&scdesc))) {
            len += sprintf(buf + len, "BufferCount: %u\r\n", scdesc.BufferCount);
            len += sprintf(buf + len, "BufferDesc: %ux%u fmt=%d\r\n", scdesc.BufferDesc.Width, scdesc.BufferDesc.Height, (int)scdesc.BufferDesc.Format);
            len += sprintf(buf + len, "SwapEffect: %d (0=DISCARD,1=SEQ,2=FLIP_SEQ,3=FLIP_DISC)\r\n", (int)scdesc.SwapEffect);
            len += sprintf(buf + len, "Flags: 0x%X\r\n", scdesc.Flags);
            len += sprintf(buf + len, "Windowed: %d\r\n", scdesc.Windowed);
            len += sprintf(buf + len, "SampleDesc: Count=%u Quality=%u\r\n", scdesc.SampleDesc.Count, scdesc.SampleDesc.Quality);
        }
    }

    len += sprintf(buf + len, "\r\n--- Render State ---\r\n");
    len += sprintf(buf + len, "pDrawTexture: %p\r\n", (void*)pDrawTexture);
    len += sprintf(buf + len, "textureIs32bit: %d\r\n", (int)textureIs32bit);
    len += sprintf(buf + len, "tearingEnabled: %d\r\n", (int)tearingEnabled);

    len += sprintf(buf + len, "\r\n--- GFX / PPU Core ---\r\n");
    len += sprintf(buf + len, "GFX.Screen: %p\r\n", (void*)GFX.Screen);
    len += sprintf(buf + len, "GFX.SubScreen: %p\r\n", (void*)GFX.SubScreen);
    len += sprintf(buf + len, "GFX.RealPPL: %u\r\n", (unsigned)GFX.RealPPL);
    len += sprintf(buf + len, "GFX.Pitch: %u\r\n", (unsigned)GFX.Pitch);
    len += sprintf(buf + len, "GFX.StartY: %u  EndY: %u\r\n", (unsigned)GFX.StartY, (unsigned)GFX.EndY);
    len += sprintf(buf + len, "IPPU.RenderThisFrame: %d\r\n", (int)IPPU.RenderThisFrame);
    len += sprintf(buf + len, "IPPU.RenderedFramesCount: %u\r\n", IPPU.RenderedFramesCount);
    len += sprintf(buf + len, "IPPU.SkippedFrames: %u\r\n", IPPU.SkippedFrames);
    len += sprintf(buf + len, "IPPU.ColorsChanged: %d\r\n", (int)IPPU.ColorsChanged);
    // Sample GFX.Screen directly (first non-zero scan)
    if (GFX.Screen) {
        int firstNZRow = -1;
        for (int r = 0; r < 240 && firstNZRow < 0; r++) {
            const uint16_t* row = GFX.Screen + r * GFX.RealPPL;
            for (int c = 0; c < 256; c++) {
                if (row[c] != 0) { firstNZRow = r; break; }
            }
        }
        len += sprintf(buf + len, "GFX.Screen first non-zero row: %d\r\n", firstNZRow);
        if (firstNZRow >= 0) {
            const uint16_t* row = GFX.Screen + firstNZRow * GFX.RealPPL;
            len += sprintf(buf + len, "  Samples: ");
            for (int c = 0; c < 256; c += 32)
                len += sprintf(buf + len, "[%d]=0x%04X ", c, row[c]);
            len += sprintf(buf + len, "\r\n");
        }
    }
    // Check pixel format settings
    len += sprintf(buf + len, "GUI.RedShift: %d  GreenShift: %d  BlueShift: %d\r\n",
        GUI.RedShift, GUI.GreenShift, GUI.BlueShift);
    len += sprintf(buf + len, "GUI.NeedDepthConvert: %d  DepthConverted: %d\r\n",
        (int)GUI.NeedDepthConvert, (int)GUI.DepthConverted);

    RECT rc;
    GetClientRect(hWnd, &rc);
    len += sprintf(buf + len, "\r\n--- Window ---\r\n");
    len += sprintf(buf + len, "hWnd: %p\r\n", (void*)hWnd);
    len += sprintf(buf + len, "ClientRect: %dx%d\r\n", (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));

    return std::string(buf, len);
}
