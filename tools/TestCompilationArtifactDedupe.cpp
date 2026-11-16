// Compiler-input deduplication: permutations whose preprocessed text is identical
// share one D3DCompile call, while every stage keeps its own disk artifact and
// receives exactly the bytecode an isolated compile of that permutation produces.
#define NOMINMAX
#include "ShaderCache/CompilationArtifact.h"
#include <future>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using Blob = Microsoft::WRL::ComPtr<ID3DBlob>;
static void Require(bool test, const char* message) { if (!test) throw std::runtime_error(message); }
static void Write(const fs::path& path, std::string_view text) { std::ofstream f(path, std::ios::binary); f << text; }
// Under the Linux harness's fake compiler, FXC can be slowed down so duplicate
// permutations are guaranteed to overlap in flight; with the real compiler the
// same assertions hold whether duplicates overlap (shared) or not (reused).
#ifdef PIXL_STUB_D3DCOMPILER
static void SetCompileDelay(int ms) { g_stubCompileMs = ms; }
#else
static void SetCompileDelay(int) {}
#endif
static std::string Bytes(ID3DBlob* blob) { return blob ? std::string(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize()) : std::string{}; }

struct Include : ID3DInclude
{
    fs::path root;
    explicit Include(fs::path path) : root(std::move(path)) {}
    HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
    {
        std::ifstream file(root / name, std::ios::binary);
        if (!file) return E_FAIL;
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        auto memory = new char[bytes.size() + 1];
        memcpy(memory, bytes.data(), bytes.size());
        *data = memory; *size = static_cast<UINT>(bytes.size());
        return S_OK;
    }
    HRESULT Close(LPCVOID data) override { delete[] static_cast<const char*>(data); return S_OK; }
};

// Five descriptors. A/B/C are not referenced by the source, so their
// preprocessed text is identical; USED selects a different body.
static const D3D_SHADER_MACRO kA[]{ {"PSHADER", nullptr}, {"A", nullptr}, {nullptr, nullptr} };
static const D3D_SHADER_MACRO kB[]{ {"PSHADER", nullptr}, {"B", nullptr}, {nullptr, nullptr} };
static const D3D_SHADER_MACRO kC[]{ {"PSHADER", nullptr}, {"C", "7"}, {nullptr, nullptr} };
static const D3D_SHADER_MACRO kU[]{ {"PSHADER", nullptr}, {"USED", nullptr}, {nullptr, nullptr} };
static const D3D_SHADER_MACRO kUA[]{ {"PSHADER", nullptr}, {"USED", nullptr}, {"A", nullptr}, {nullptr, nullptr} };
static const D3D_SHADER_MACRO* kSets[]{ kA, kB, kC, kU, kUA };
static const char* kNames[]{ "A", "B", "C", "U", "UA" };

int main()
{
    const auto root = fs::temp_directory_path() / ("PIXL-Dedupe-Test-" + std::to_string(GetCurrentProcessId()));
    try {
        fs::remove_all(root);
        fs::create_directories(root / "lib");
        const auto source = root / "Lighting.hlsl";
        Write(root / "common.hlsli", "#define BASE 0.25\n");
        Write(source, "#include \"common.hlsli\"\n#ifdef USED\nfloat4 main():SV_Target{return BASE*2;}\n#else\nfloat4 main():SV_Target{return BASE;}\n#endif\n");
        auto stage = [&](int i) { return root / "lib" / (std::string(kNames[i]) + ".pixlbin"); };

        // Reference: each permutation compiled in isolation, no sharing possible.
        std::string reference[5];
        for (int i = 0; i < 5; ++i) {
            SIE::CompilationArtifacts isolated; Include inc(root); Blob code, errors;
            Require(SUCCEEDED(isolated.Compile(source, kSets[i], inc, "main", "ps_5_0", 0, {}, false,
                code.GetAddressOf(), errors.GetAddressOf())), "isolated compile");
            reference[i] = Bytes(code.Get());
        }
        Require(reference[0] == reference[1] && reference[1] == reference[2], "unused macros give identical bytecode");
        Require(reference[3] == reference[4] && reference[0] != reference[3], "used macro changes bytecode");

        // Concurrent cold compile of all five stages with disk enabled.
        SetCompileDelay(300);
        SIE::CompilationArtifacts cache;
        std::atomic<int> observed{};
        std::atomic<int> peakWaiters{};
        cache.SetInputWaitObserver([&] {
            ++observed;
            int w = cache.InputWaiters(), p = peakWaiters.load();
            while (w > p && !peakWaiters.compare_exchange_weak(p, w)) {}
        });
        std::vector<std::future<std::pair<HRESULT, std::string>>> jobs;
        for (int i = 0; i < 5; ++i) jobs.emplace_back(std::async(std::launch::async, [&, i] {
            Include inc(root); Blob code, errors; bool hit = false;
            const auto hr = cache.Compile(source, kSets[i], inc, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, stage(i), true,
                code.GetAddressOf(), errors.GetAddressOf(), &hit, true);
            return std::make_pair(hr, Bytes(code.Get()));
        }));
        std::string cold[5];
        for (int i = 0; i < 5; ++i) { auto [hr, bytes] = jobs[i].get(); Require(SUCCEEDED(hr), "concurrent dedupe compile"); cold[i] = bytes; }
        // The flags differ from the isolated reference run, so compare within the run and against a flag-matched isolated compile.
        Require(cache.GetTimingSnapshot().actualCompiles == 2, "five permutations with two distinct inputs ran FXC exactly twice");
        Require(cold[0] == cold[1] && cold[1] == cold[2] && cold[3] == cold[4] && cold[0] != cold[3], "shared results map to the right permutations");
        for (int i = 0; i < 5; ++i) {
            SIE::CompilationArtifacts isolated; Include inc(root); Blob code, errors;
            Require(SUCCEEDED(isolated.Compile(source, kSets[i], inc, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, {}, false,
                code.GetAddressOf(), errors.GetAddressOf())), "flag-matched isolated compile");
            Require(Bytes(code.Get()) == cold[i], "deduplicated bytecode is byte-identical to an isolated compile");
        }
        const auto stats = cache.GetTimingSnapshot();
        Require(stats.inputCoalesced + stats.inputReuseHits == 3, "three permutations were served by input identity");
        Require(cache.InputWaiters() == 0, "no waiter left behind");
        Require(stats.inputCoalesced == 0 || observed.load() == static_cast<int>(stats.inputCoalesced), "observer called once per waiting caller");
        for (int i = 0; i < 5; ++i) {
            Require(fs::exists(stage(i)) && fs::exists(stage(i).wstring() + L".identity") && fs::exists(stage(i).wstring() + L".fast"),
                "every permutation persisted its own artifact, identity and fast index");
        }

        // Next launch: every stage is a fast disk hit; FXC never runs.
        {
            SIE::CompilationArtifacts relaunch;
            for (int i = 0; i < 5; ++i) {
                Include inc(root); Blob code, errors; bool hit = false;
                Require(SUCCEEDED(relaunch.Compile(source, kSets[i], inc, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, stage(i), true,
                    code.GetAddressOf(), errors.GetAddressOf(), &hit, true)) && hit, "relaunch disk hit");
                Require(Bytes(code.Get()) == cold[i], "relaunch returns the same bytecode");
            }
            Require(relaunch.GetTimingSnapshot().fastArtifactHits == 5, "all relaunch hits use the fast index");
            Require(relaunch.GetTimingSnapshot().actualCompiles == 0, "relaunch compiles nothing");
        }

        // Sequential reuse (no overlap): later duplicates reuse the retained blob.
        SetCompileDelay(0);
        {
            SIE::CompilationArtifacts sequential;
            for (int i = 0; i < 5; ++i) {
                Include inc(root); Blob code, errors;
                Require(SUCCEEDED(sequential.Compile(source, kSets[i], inc, "main", "ps_5_0", D3DCOMPILE_SKIP_VALIDATION, {}, false,
                    code.GetAddressOf(), errors.GetAddressOf())), "sequential compile");
            }
            Require(sequential.GetTimingSnapshot().actualCompiles == 2 && sequential.GetTimingSnapshot().inputReuseHits == 3, "sequential duplicates reuse retained blobs");
        }

        // A leader failure reaches every follower with diagnostics; nothing hangs.
        Write(source, "#ifdef USED\ninvalid\n#else\ninvalid\n#endif\nfloat4 main(){return 0;}\n");
        SetCompileDelay(300);
        {
            SIE::CompilationArtifacts failing;
            std::vector<std::future<std::pair<HRESULT, bool>>> f;
            for (int i = 0; i < 3; ++i) f.emplace_back(std::async(std::launch::async, [&, i] {
                Include inc(root); Blob code, errors;
                const auto hr = failing.Compile(source, kSets[i], inc, "main", "ps_5_0", 0, {}, false, code.GetAddressOf(), errors.GetAddressOf());
                return std::make_pair(hr, errors && !code);
            }));
            for (auto& j : f) { auto [hr, diag] = j.get(); Require(FAILED(hr) && diag, "failure and diagnostics propagate"); }
            Require(failing.GetTimingSnapshot().actualCompiles == 1, "failing input compiled once");
            Require(failing.InputWaiters() == 0, "no waiter left after failure");
        }

#ifdef PIXL_STUB_D3DCOMPILER
        // Invalidation while followers wait: everyone returns, superseded work aborts.
        // Needs a compile slow enough to invalidate mid-flight, so it runs on the stub only.
        Write(source, "float4 main():SV_Target{return 1;}\n");
        SetCompileDelay(600);
        {
            SIE::CompilationArtifacts invalidating;
            std::vector<std::future<HRESULT>> f;
            for (int i = 0; i < 3; ++i) f.emplace_back(std::async(std::launch::async, [&, i] {
                Include inc(root); Blob code, errors;
                return invalidating.Compile(source, kSets[i], inc, "main", "ps_5_0", 0, {}, false, code.GetAddressOf(), errors.GetAddressOf());
            }));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            invalidating.Invalidate();
            for (auto& j : f) Require(j.get() == E_ABORT, "superseded duplicates abort");
            Require(invalidating.InputWaiters() == 0, "no waiter left after invalidation");
            SetCompileDelay(0);
            Include inc(root); Blob code, errors;
            Require(SUCCEEDED(invalidating.Compile(source, kA, inc, "main", "ps_5_0", 0, {}, false, code.GetAddressOf(), errors.GetAddressOf())) && code,
                "new generation compiles normally");
        }
#endif

        fs::remove_all(root);
        std::cout << "Compilation dedupe: shared FXC runs, byte-identical results, per-stage persistence, relaunch fast hits, failure and invalidation cases passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
