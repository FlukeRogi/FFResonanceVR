// xrtest: a minimal UEVR-like OpenXR D3D12 app, for testing the window layer without the game.
//
//   xrtest.exe <openxr_loader.dll> [frames]
//
// Uses the loader given (UEVR's), so XR_RUNTIME_JSON / XR_API_LAYER_PATH / XR_ENABLE_API_LAYERS apply as in the game.
// Like UEVR: LOCAL space as its world, VIEW space, a double-wide swapchain, views located in LOCAL and submitted
// as a projection layer, plus a tiny dummy projection layer. Each eye image: dark grey background, and a "world
// object" (a 1 m square, 6 m in front of the world origin) drawn where that eye's located pose/FOV puts it, in
// red (left eye) / blue (right eye). Exit code 0 when every call succeeded.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cmath>
#include <cstdio>
#include <vector>

static PFN_xrGetInstanceProcAddr gipa;
static XrInstance inst;
template <class F> static F Fn(const char* n) { PFN_xrVoidFunction f = nullptr; gipa(inst, n, &f); return (F)f; }
#define CHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { printf("FAIL %s -> %d\n", #x, (int)r_); return 1; } } while (0)

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: xrtest <openxr_loader.dll> [frames]\n"); return 2; }
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    HMODULE ldr = LoadLibraryA(argv[1]);
    if (!ldr) { printf("cannot load %s\n", argv[1]); return 2; }
    gipa = (PFN_xrGetInstanceProcAddr)GetProcAddress(ldr, "xrGetInstanceProcAddr");
    auto enumExt = Fn<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    CHECK(enumExt(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> ep(n, { XR_TYPE_EXTENSION_PROPERTIES });
    CHECK(enumExt(nullptr, n, &n, ep.data()));
    printf("%u extensions\n", n);
    const char* exts[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "UEVR_xrtest");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    CHECK(Fn<PFN_xrCreateInstance>("xrCreateInstance")(&ici, &inst));
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CHECK(Fn<PFN_xrGetSystem>("xrGetSystem")(inst, &sgi, &sys));
    XrGraphicsRequirementsD3D12KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    CHECK(Fn<PFN_xrGetD3D12GraphicsRequirementsKHR>("xrGetD3D12GraphicsRequirementsKHR")(inst, sys, &req));
    IDXGIFactory4* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&fac);
    IDXGIAdapter1* ad = nullptr; fac->EnumAdapterByLuid(req.adapterLuid, __uuidof(IDXGIAdapter1), (void**)&ad);
    ID3D12Device* dev; if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev))) { printf("no device\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q; dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q);
    ID3D12CommandAllocator* al; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al);
    ID3D12GraphicsCommandList* cl; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl); cl->Close();
    ID3D12Fence* fence; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence); uint64_t fv = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    XrGraphicsBindingD3D12KHR gb{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR }; gb.device = dev; gb.queue = q;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = sys;
    XrSession ses; CHECK(Fn<PFN_xrCreateSession>("xrCreateSession")(inst, &sci, &ses));
    XrReferenceSpaceCreateInfo rsi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rsi.poseInReferenceSpace.orientation.w = 1;
    XrSpace local, view;
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; CHECK(Fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(ses, &rsi, &local));
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; CHECK(Fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(ses, &rsi, &view));
    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    CHECK(Fn<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews")(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv));
    uint32_t W = vcv[0].recommendedImageRectWidth, H = vcv[0].recommendedImageRectHeight;
    printf("eye %ux%u\n", W, H);
    XrSwapchainCreateInfo swi{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swi.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swi.format = DXGI_FORMAT_R8G8B8A8_UNORM; swi.sampleCount = 1; swi.width = W * 2; swi.height = H; swi.faceCount = 1; swi.arraySize = 1; swi.mipCount = 1;
    XrSwapchain sc; CHECK(Fn<PFN_xrCreateSwapchain>("xrCreateSwapchain")(ses, &swi, &sc));
    swi.width = 64; swi.height = 32; XrSwapchain dummy; CHECK(Fn<PFN_xrCreateSwapchain>("xrCreateSwapchain")(ses, &swi, &dummy));
    auto enumImg = Fn<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    auto images = [&](XrSwapchain s) { uint32_t c = 0; enumImg(s, 0, &c, nullptr); std::vector<XrSwapchainImageD3D12KHR> v(c, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR }); enumImg(s, c, &c, (XrSwapchainImageBaseHeader*)v.data()); return v; };
    auto img = images(sc), dimg = images(dummy);
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8 };
    ID3D12DescriptorHeap* heap; dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&heap);
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc; return h; };
    D3D12_RENDER_TARGET_VIEW_DESC rvd{}; rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (size_t i = 0; i < img.size(); ++i) dev->CreateRenderTargetView(img[i].texture, &rvd, rtv((int)i));
    for (size_t i = 0; i < dimg.size(); ++i) dev->CreateRenderTargetView(dimg[i].texture, &rvd, rtv(4 + (int)i));

    auto poll = Fn<PFN_xrPollEvent>("xrPollEvent");
    auto waitF = Fn<PFN_xrWaitFrame>("xrWaitFrame"); auto beginF = Fn<PFN_xrBeginFrame>("xrBeginFrame"); auto endF = Fn<PFN_xrEndFrame>("xrEndFrame");
    auto locV = Fn<PFN_xrLocateViews>("xrLocateViews"); auto locS = Fn<PFN_xrLocateSpace>("xrLocateSpace");
    auto acq = Fn<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage"); auto wimg = Fn<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage");
    auto rel = Fn<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
    bool running = false;
    int frame = 0;
    for (int iter = 0; frame < frames && iter < frames * 20; ++iter) {
        XrEventDataBuffer e{ XR_TYPE_EVENT_DATA_BUFFER };
        while (poll(inst, &e) == XR_SUCCESS) {
            if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto st = ((XrEventDataSessionStateChanged*)&e)->state;
                if (st == XR_SESSION_STATE_READY) { XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; CHECK(Fn<PFN_xrBeginSession>("xrBeginSession")(ses, &bi)); running = true; }
                if (st == XR_SESSION_STATE_STOPPING) { Fn<PFN_xrEndSession>("xrEndSession")(ses); running = false; frames = frame; }
            }
            e = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (!running) { Sleep(5); continue; }
        XrFrameState fs{ XR_TYPE_FRAME_STATE }; CHECK(waitF(ses, nullptr, &fs)); CHECK(beginF(ses, nullptr));
        XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO }; vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = fs.predictedDisplayTime;
        XrViewState vs{ XR_TYPE_VIEW_STATE }; XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }, vv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        vli.space = view; CHECK(locV(ses, &vli, &vs, 2, &n, vv));
        vli.space = local; CHECK(locV(ses, &vli, &vs, 2, &n, v));
        XrSpaceLocation hl{ XR_TYPE_SPACE_LOCATION }; CHECK(locS(view, local, fs.predictedDisplayTime, &hl));
        uint32_t idx, didx; XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
        CHECK(acq(sc, nullptr, &idx)); CHECK(wimg(sc, &wi)); CHECK(acq(dummy, nullptr, &didx)); CHECK(wimg(dummy, &wi));
        al->Reset(); cl->Reset(al, nullptr);
        float bg[4] = { 0.15f, 0.15f, 0.15f, 1 };
        cl->ClearRenderTargetView(rtv(idx), bg, 0, nullptr);
        float zero[4] = { 0, 0, 0, 0 }; cl->ClearRenderTargetView(rtv(4 + didx), zero, 0, nullptr);
        for (int eye = 0; eye < 2; ++eye) {
            // world square: centre (0, 0, -6) in LOCAL, 1 m wide, facing +Z. Assumes the eye looks along -Z (the window layer
            // reports identity orientation); otherwise it is drawn where it would be for an unrotated eye.
            const auto& p = v[eye].pose.position; const auto& f = v[eye].fov;
            float D = p.z + 6.0f;
            float tL = std::tan(f.angleLeft), tR = std::tan(f.angleRight), tU = std::tan(f.angleUp), tD = std::tan(f.angleDown);
            auto px = [&](float x) { return (int)(((x - p.x) / D - tL) / (tR - tL) * W); };
            auto py = [&](float y) { return (int)((tU - (y - p.y) / D) / (tU - tD) * H); };
            D3D12_RECT r{ px(-0.5f), py(0.5f), px(0.5f), py(-0.5f) };
            r.left = std::max<LONG>(r.left, 0); r.right = std::min<LONG>(r.right, W); r.top = std::max<LONG>(r.top, 0); r.bottom = std::min<LONG>(r.bottom, H);
            r.left += eye * W; r.right += eye * W;
            float col[4] = { eye ? 0.1f : 0.9f, 0.1f, eye ? 0.9f : 0.1f, 1 };
            if (r.right > r.left && r.bottom > r.top) cl->ClearRenderTargetView(rtv(idx), col, 1, &r);
        }
        cl->Close(); ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); q->Signal(fence, ++fv);
        fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 1000);
        CHECK(rel(sc, nullptr)); CHECK(rel(dummy, nullptr));
        XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerProjectionView dv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        for (int eye = 0; eye < 2; ++eye) {
            pv[eye].pose = v[eye].pose; pv[eye].fov = v[eye].fov; pv[eye].subImage.swapchain = sc;
            pv[eye].subImage.imageRect = { { (int32_t)(eye * W), 0 }, { (int32_t)W, (int32_t)H } };
            dv[eye] = pv[eye]; dv[eye].subImage.swapchain = dummy; dv[eye].subImage.imageRect = { { eye * 32, 0 }, { 32, 32 } };
        }
        XrCompositionLayerProjection dl{ XR_TYPE_COMPOSITION_LAYER_PROJECTION }; dl.space = local; dl.viewCount = 2; dl.views = dv; dl.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        XrCompositionLayerProjection pl{ XR_TYPE_COMPOSITION_LAYER_PROJECTION }; pl.space = local; pl.viewCount = 2; pl.views = pv;
        const XrCompositionLayerBaseHeader* layers[] = { (XrCompositionLayerBaseHeader*)&dl, (XrCompositionLayerBaseHeader*)&pl };
        XrFrameEndInfo fe{ XR_TYPE_FRAME_END_INFO }; fe.displayTime = fs.predictedDisplayTime; fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = fs.shouldRender ? 2 : 0; fe.layers = layers;
        CHECK(endF(ses, &fe));
        if (frame % 100 == 0)
            printf("frame %d head [%.3f %.3f %.3f] q[%.3f %.3f %.3f %.3f]  L eye [%.3f %.3f %.3f] fov %.1f/%.1f/%.1f/%.1f\n", frame, hl.pose.position.x, hl.pose.position.y, hl.pose.position.z,
                   hl.pose.orientation.x, hl.pose.orientation.y, hl.pose.orientation.z, hl.pose.orientation.w, v[0].pose.position.x, v[0].pose.position.y, v[0].pose.position.z,
                   v[0].fov.angleLeft * 57.3f, v[0].fov.angleRight * 57.3f, v[0].fov.angleUp * 57.3f, v[0].fov.angleDown * 57.3f);
        ++frame;
    }
    Fn<PFN_xrDestroyInstance>("xrDestroyInstance")(inst);
    printf("done, %d frames\n", frame);
    return 0;
}
