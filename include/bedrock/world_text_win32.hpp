#pragma once
#if !defined(_WIN32) || !defined(_MSC_VER)
#error Native world text requires Windows MSVC.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bedrock/game_api.hpp>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// CPU-only Unicode glyph geometry, extracted from the HitBox font pipeline.
// Own the mesher on a worker; submit only immutable copied vertices to render.
namespace bedrock::world_text {
using Clock = std::chrono::steady_clock;
struct TextPoint { float x{},y{}; };
struct NameMesh {
    std::string text;
    std::vector<TextPoint> triangles; // Three points per triangle, normalized to unit height.
    float width{};
    std::vector<std::array<TextPoint,4>> quads; // Convex pairs; unmatched triangles use a repeated corner.
};

// Run once per acquired text on a worker. Merge only convex adjacent triangles;
// holes, concave outlines and unpaired triangles retain their original coverage.
inline void packNameQuads(NameMesh& mesh) {
    mesh.quads.clear();
    if(mesh.triangles.size()%3) return;
    struct Edge { TextPoint a,b,tip; std::size_t triangle; };
    const auto less=[](TextPoint a,TextPoint b) { return std::tie(a.x,a.y)<std::tie(b.x,b.y); };
    const auto equal=[](TextPoint a,TextPoint b) { return a.x==b.x && a.y==b.y; };
    const auto cross=[](TextPoint a,TextPoint b,TextPoint c) {
        return (b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
    };
    const auto count=mesh.triangles.size()/3;
    std::vector<Edge> edges; edges.reserve(count*3);
    std::vector<bool> used(count);
    mesh.quads.reserve(count);
    for(std::size_t i=0;i<count;++i) for(std::size_t j=0;j<3;++j) {
        auto a=mesh.triangles[i*3+j],b=mesh.triangles[i*3+(j+1)%3];
        if(less(b,a)) std::swap(a,b);
        edges.push_back({a,b,mesh.triangles[i*3+(j+2)%3],i});
    }
    std::sort(edges.begin(),edges.end(),[&](const Edge& a,const Edge& b) {
        if(!equal(a.a,b.a)) return less(a.a,b.a);
        return less(a.b,b.b);
    });
    for(std::size_t i=0;i<edges.size();) {
        auto end=i+1;
        while(end<edges.size() && equal(edges[i].a,edges[end].a) && equal(edges[i].b,edges[end].b)) ++end;
        if(end-i==2) {
            const auto& a=edges[i]; const auto& b=edges[i+1];
            if(a.triangle!=b.triangle && !used[a.triangle] && !used[b.triangle]) {
                const std::array<TextPoint,4> quad{a.tip,a.a,b.tip,a.b};
                const float first=cross(quad[0],quad[1],quad[2]);
                bool convex=std::abs(first)>1e-10f;
                for(std::size_t j=1;j<4;++j)
                    convex=convex && cross(quad[j],quad[(j+1)%4],quad[(j+2)%4])*first>0;
                if(convex) { mesh.quads.push_back(quad); used[a.triangle]=used[b.triangle]=true; }
            }
        }
        i=end;
    }
    for(std::size_t i=0;i<count;++i) if(!used[i])
        mesh.quads.push_back({mesh.triangles[i*3],mesh.triangles[i*3+1],mesh.triangles[i*3+2],mesh.triangles[i*3+2]});
}
template<class T> class NameComPtr {
public:
    NameComPtr()=default;
    NameComPtr(const NameComPtr&)=delete;
    NameComPtr& operator=(const NameComPtr&)=delete;
    ~NameComPtr() { if(pointer_) pointer_->Release(); }
    T** put() { if(pointer_) pointer_->Release(); pointer_=nullptr; return &pointer_; }
    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
private:
    T* pointer_{};
};
class NameTriangleSink final : public ID2D1TessellationSink {
public:
    static constexpr std::size_t maximumTriangles=4096;
    NameTriangleSink(NameMesh& mesh,float x,float y) : mesh_(mesh),x_(x),y_(y) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result) noexcept override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(id!=__uuidof(IUnknown) && id!=__uuidof(ID2D1TessellationSink)) return E_NOINTERFACE;
        *result=static_cast<ID2D1TessellationSink*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return --references_; } // Synchronous stack-owned sink.
    void STDMETHODCALLTYPE AddTriangles(const D2D1_TRIANGLE* triangles,UINT count) noexcept override {
        if(failed_ || count>maximumTriangles || mesh_.triangles.size()+std::size_t(count)*3>maximumTriangles*3) {
            failed_=true; return;
        }
        try {
            for(UINT i=0;i<count;++i) {
                for(const auto p : {triangles[i].point1,triangles[i].point2,triangles[i].point3}) {
                    if(!std::isfinite(p.x) || !std::isfinite(p.y)) { failed_=true; return; }
                    mesh_.triangles.push_back({p.x+x_,p.y+y_});
                }
            }
        } catch(...) { failed_=true; }
    }
    HRESULT STDMETHODCALLTYPE Close() noexcept override { return failed_ ? E_FAIL : S_OK; }
private:
    NameMesh& mesh_;
    float x_,y_;
    ULONG references_{1};
    bool failed_{};
};
class NameTextRenderer final : public IDWriteTextRenderer {
public:
    NameTextRenderer(ID2D1Factory* factory,NameMesh& mesh) : factory_(factory),mesh_(mesh) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result) noexcept override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(id!=__uuidof(IUnknown) && id!=__uuidof(IDWriteTextRenderer) &&
           id!=__uuidof(IDWritePixelSnapping)) return E_NOINTERFACE;
        *result=static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return --references_; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) noexcept override {
        if(!disabled) return E_POINTER; *disabled=TRUE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* matrix) noexcept override {
        if(!matrix) return E_POINTER; *matrix={1,0,0,1,0,0}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* pixels) noexcept override {
        if(!pixels) return E_POINTER; *pixels=1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) noexcept override {
        if(!run || !run->fontFace) return E_INVALIDARG;
        NameComPtr<ID2D1PathGeometry> geometry;
        NameComPtr<ID2D1GeometrySink> outline;
        auto hr=factory_->CreatePathGeometry(geometry.put());
        if(FAILED(hr)) return hr;
        hr=geometry->Open(outline.put());
        if(FAILED(hr)) return hr;
        hr=run->fontFace->GetGlyphRunOutline(run->fontEmSize,run->glyphIndices,
            run->glyphAdvances,run->glyphOffsets,run->glyphCount,run->isSideways,
            (run->bidiLevel&1)!=0,outline.get());
        const auto closed=outline->Close();
        if(FAILED(hr)) return hr;
        if(FAILED(closed)) return closed;
        NameTriangleSink sink(mesh_,x,y);
        hr=geometry->Tessellate(nullptr,0.5f,&sink);
        if(FAILED(hr)) return hr;
        return sink.Close();
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) noexcept override { return E_NOTIMPL; }
private:
    ID2D1Factory* factory_;
    NameMesh& mesh_;
    ULONG references_{1};
};
// Worker-owned CPU font geometry. DirectWrite handles Unicode/font fallback;
// Direct2D tessellates without creating a device, swapchain, or overlay.
class NameComSession {
public:
    NameComSession() : result_(CoInitializeEx(nullptr,COINIT_MULTITHREADED)) {}
    ~NameComSession() { if(SUCCEEDED(result_)) CoUninitialize(); }
private:
    HRESULT result_;
};
class NameMesher {
public:
    const char* lastStage() const noexcept { return stage_; }
    HRESULT lastResult() const noexcept { return result_; }
    std::shared_ptr<const NameMesh> make(std::string name) {
        if(!initialize()) return {};
        stage_="input"; result_=S_OK;
        if(name.empty()) { result_=E_INVALIDARG; return {}; }
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0);
        if(count<=0 || count>192) { stage_="utf8_length"; result_=E_INVALIDARG; return {}; }
        std::wstring wide(count,L'\0');
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),wide.data(),count)!=count) { stage_="utf8_convert"; result_=E_INVALIDARG; return {}; }
        NameComPtr<IDWriteTextLayout> layout;
        if(!check(write_->CreateTextLayout(wide.data(),static_cast<UINT32>(wide.size()),format_.get(),4096,128,layout.put()),"CreateTextLayout")) return {};
        DWRITE_TEXT_METRICS metrics{};
        if(!check(layout->GetMetrics(&metrics),"GetMetrics")) return {};
        if(!std::isfinite(metrics.height) || !std::isfinite(metrics.width) || metrics.height<=0 || metrics.width<=0) {
            stage_="invalid_metrics"; result_=E_FAIL; return {};
        }
        auto mesh=std::make_shared<NameMesh>();
        mesh->text=std::move(name);
        NameTextRenderer renderer(factory_.get(),*mesh);
        if(!check(layout->Draw(nullptr,&renderer,0,0),"DrawGlyphOutline")) return {};
        if(mesh->triangles.empty()) { stage_="empty_triangles"; result_=E_FAIL; return {}; }
        mesh->width=metrics.width/metrics.height;
        for(auto& p:mesh->triangles) {
            p.x=(p.x-metrics.width*0.5f)/metrics.height;
            p.y=(metrics.height-p.y)/metrics.height;
        }
        packNameQuads(*mesh);
        stage_="ready"; result_=S_OK;
        return mesh;
    }
private:
    bool check(HRESULT hr,const char* stage) noexcept { stage_=stage; result_=hr; return SUCCEEDED(hr); }
    const char* stage_="not_initialized";
    HRESULT result_=S_OK;
    bool initialize() {
        if(initialized_) {
            if(ready_ || Clock::now()<retryInitialization_) return ready_;
            // Factory initialization can fail transiently. Do not permanently
            // strand a successfully captured name behind the first failure.
            format_.put(); factory_.put(); write_.put();
        }
        initialized_=true;
        retryInitialization_=Clock::now()+std::chrono::seconds(5);
        if(!check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_.put())),"DWriteCreateFactory")) return false;
        D2D1_FACTORY_OPTIONS options{};
        if(!check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory),
            &options,reinterpret_cast<void**>(factory_.put())),"D2D1CreateFactory")) return false;
        if(!check(write_->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,20,L"ja-jp",format_.put()),"CreateTextFormat")) return false;
        if(!check(format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP),"SetWordWrapping")) return false;
        ready_=true; return true;
    }
    NameComSession com_;
    NameComPtr<IDWriteFactory> write_;
    NameComPtr<ID2D1Factory> factory_;
    NameComPtr<IDWriteTextFormat> format_;
    bool initialized_{},ready_{};
    Clock::time_point retryInitialization_{};
};

} // namespace bedrock::world_text
