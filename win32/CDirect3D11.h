/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef W9XDIRECT3D11_H
#define W9XDIRECT3D11_H

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include "render.h"
#include "wsnes9x.h"
#include "IS9xDisplayOutput.h"

struct VERTEX_D3D11 {
    float x, y, z;
    float tx, ty;
    float lutx, luty;
};

class CDirect3D11 : public IS9xDisplayOutput
{
private:
    bool                    init_done;
    ID3D11Device*           pDevice;
    ID3D11DeviceContext*    pContext;
    IDXGISwapChain*         pSwapChain;
    ID3D11Texture2D*        pRenderTarget;
    ID3D11RenderTargetView* pRenderTargetView;
    ID3D11Texture2D*        pDrawTexture;
    ID3D11ShaderResourceView* pDrawTextureSRV;
    ID3D11VertexShader*     pVertexShader;
    ID3D11PixelShader*      pPixelShaderPoint;
    ID3D11PixelShader*      pPixelShaderLinear;
    ID3D11PixelShader*      pPixelShaderFX = nullptr;  // effects shader
    ID3D11InputLayout*      pInputLayout;
    ID3D11SamplerState*     pSamplerPoint;
    ID3D11SamplerState*     pSamplerLinear;
    ID3D11Buffer*           pVertexBuffer;
    ID3D11Buffer*           pConstantBuffer;
    ID3D11Buffer*           pFXConstBuffer = nullptr;  // effects parameters
    ID3D11BlendState*       pBlendState;
    ID3D11RasterizerState*  pRasterizerState;
    ID3D11DepthStencilState* pDepthStencilState;

    unsigned int filterScale;
    unsigned int afterRenderWidth, afterRenderHeight;
    unsigned int quadTextureSize;
    bool fullscreen;
    bool tearingEnabled;  // true if swap chain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
    bool textureIs32bit;  // true if draw texture is B8G8R8A8 (fallback from B5G6R5)
    HWND hWnd;
    unsigned int backBufferWidth;
    unsigned int backBufferHeight;

    void CreateDrawTexture();
    void DestroyDrawTexture();
    bool ChangeDrawTextureSize(unsigned int scale);
    void SetViewport();
    void SetupVertices();
    bool CreateSwapChain(bool fullscreenMode);
    void Clear();

    // GPU color conversion: upload RGB565 as R16_UINT, pixel shader converts
    ID3D11Texture2D*            pRGB565Texture = nullptr;   // 16-bit raw pixel input
    ID3D11ShaderResourceView*   pRGB565SRV = nullptr;       // SRV for pixel shader read
    ID3D11PixelShader*          pPixelShaderConvert = nullptr; // RGB565→BGRA conversion PS
    bool                        gpuConvertEnabled = false;
    void CreateGPUConvertResources();
    void DestroyGPUConvertResources();

    // Render thread infrastructure
    std::thread             renderThread;
    std::mutex              renderMtx;
    std::condition_variable renderCV_work;    // signals: new frame available
    std::condition_variable renderCV_done;    // signals: render complete
    std::atomic<bool>       renderRunning{false};
    bool                    renderHasFrame = false;
    bool                    renderFrameDone = true;

    // Staging buffer: emulation thread copies GFX.Screen here
    std::vector<uint8_t>    stagingBuffer;
    SSurface                stagingSrc;       // describes the staged frame

    void RenderThreadFunc();
    void RenderFrame();      // actual D3D11 rendering (called on render thread)

public:
    CDirect3D11();
    ~CDirect3D11();
    bool Initialize(HWND hWnd) override;
    void DeInitialize() override;
    void Render(SSurface Src) override;
    void WaitForRenderDone();  // blocks until render thread finishes current frame
    bool ChangeRenderSize(unsigned int newWidth, unsigned int newHeight) override;
    bool ApplyDisplayChanges(void) override;
    bool SetFullscreen(bool fullscreen) override;
    void SetSnes9xColorFormat() override;
    void EnumModes(std::vector<dMode> *modeVector) override;
    std::string GetDebugInfo();
};

#endif
