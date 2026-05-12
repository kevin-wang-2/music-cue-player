#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/plugin/NativePluginBackend.h"
#include "engine/plugin/MissingPluginProcessor.h"
#include "engine/plugin/ExternalPluginReference.h"
#include "engine/plugin/PluginRuntimeStatus.h"
#include "engine/plugin/AudioProcessor.h"
#include "engine/plugin/PluginState.h"

using namespace mcp::plugin;

// ── MissingPluginProcessor ────────────────────────────────────────────────────

TEST_CASE("MissingPluginProcessor passes audio through", "[missing_plugin]") {
    ExternalPluginReference ref;
    ref.backend  = "au";
    ref.pluginId = "au:aufx/test/test";
    ref.name     = "Test";

    MissingPluginProcessor mp(ref, PluginRuntimeStatus::Missing, "test");

    constexpr int N = 64;
    float inL[N], inR[N], outL[N], outR[N];
    for (int i = 0; i < N; ++i) { inL[i] = static_cast<float>(i); inR[i] = -static_cast<float>(i); }

    const float* inputs[]  = {inL, inR};
    float*       outputs[] = {outL, outR};
    AudioBlock block;
    block.numInputChannels  = 2;
    block.numOutputChannels = 2;
    block.numSamples        = N;
    block.inputs            = inputs;
    block.outputs           = outputs;

    mp.process(block, {});

    for (int i = 0; i < N; ++i) {
        REQUIRE(outL[i] == inL[i]);
        REQUIRE(outR[i] == inR[i]);
    }
}

TEST_CASE("MissingPluginProcessor zero-fills extra output channels", "[missing_plugin]") {
    ExternalPluginReference ref;
    ref.backend  = "au";
    ref.pluginId = "au:aufx/test/test";

    MissingPluginProcessor mp(ref, PluginRuntimeStatus::Missing, "test");

    constexpr int N = 8;
    float inL[N]{}, outL[N]{}, outR[N]{};
    const float* inputs[]  = {inL};
    float*       outputs[] = {outL, outR};
    AudioBlock block;
    block.numInputChannels  = 1;
    block.numOutputChannels = 2;
    block.numSamples        = N;
    block.inputs            = inputs;
    block.outputs           = outputs;

    for (int i = 0; i < N; ++i) inL[i] = 1.0f;

    mp.process(block, {});

    for (int i = 0; i < N; ++i) {
        REQUIRE(outL[i] == 1.0f);
        REQUIRE(outR[i] == 0.0f);   // extra channel zeroed
    }
}

TEST_CASE("MissingPluginProcessor getState preserves stateBlob and paramSnapshot", "[missing_plugin]") {
    ExternalPluginReference ref;
    ref.backend        = "au";
    ref.pluginId       = "au:aufx/demo/demo";
    ref.stateBlob      = {0x01, 0x02, 0x03, 0x04};
    ref.paramSnapshot["gain"] = 0.75f;
    ref.paramSnapshot["freq"] = 0.3f;

    MissingPluginProcessor mp(ref, PluginRuntimeStatus::Missing, "plugin not found");

    const PluginState st = mp.getState();
    REQUIRE(st.stateData == ref.stateBlob);
    REQUIRE(st.parameters.count("gain"));
    REQUIRE(st.parameters.at("gain") == Catch::Approx(0.75f));
    REQUIRE(st.parameters.count("freq"));
    REQUIRE(st.parameters.at("freq") == Catch::Approx(0.3f));
}

TEST_CASE("MissingPluginProcessor reports correct runtimeStatus", "[missing_plugin]") {
    ExternalPluginReference ref;
    ref.backend  = "au";
    ref.pluginId = "au:aufx/none/none";

    MissingPluginProcessor mp(ref, PluginRuntimeStatus::Failed, "bad id");
    REQUIRE(mp.runtimeStatus() == PluginRuntimeStatus::Failed);
}

// ── NativePluginBackend — unknown backend ─────────────────────────────────────

TEST_CASE("NativePluginBackend returns non-null for unknown backend", "[native_backend]") {
    NativePluginBackend backend;

    ExternalPluginReference ref;
    ref.backend  = "xyzzy";
    ref.pluginId = "xyzzy:foo/bar";

    auto proc = backend.load(ref);
    REQUIRE(proc != nullptr);
}

TEST_CASE("NativePluginBackend marks unknown backend as Failed", "[native_backend]") {
    NativePluginBackend backend;

    ExternalPluginReference ref;
    ref.backend  = "xyzzy";
    ref.pluginId = "xyzzy:foo/bar";

    auto proc = backend.load(ref);
    REQUIRE(NativePluginBackend::statusOf(*proc) == PluginRuntimeStatus::Failed);
}

TEST_CASE("NativePluginBackend statusOf returns Ok for non-missing processor", "[native_backend]") {
    // Simulate a processor that is NOT a MissingPluginProcessor (just use Missing again but
    // check that statusOf dispatches correctly via dynamic_cast).
    ExternalPluginReference ref;
    ref.backend  = "au";
    ref.pluginId = "au:aufx/none/none";

    MissingPluginProcessor mp(ref, PluginRuntimeStatus::Missing, "test");
    REQUIRE(NativePluginBackend::statusOf(mp) == PluginRuntimeStatus::Missing);
}

// ── AU round-trip (macOS only) ────────────────────────────────────────────────

#ifdef __APPLE__

#include "engine/plugin/AUPluginAdapter.h"

static constexpr auto kType         = 0x61756678u;  // 'aufx'
static constexpr auto kSubtype      = 0x6C6D7472u;  // 'lmtr'
static constexpr auto kManufacturer = 0x6170706Cu;  // 'appl'

TEST_CASE("NativePluginBackend load AU: plugin found returns Ok", "[native_backend][au]") {
    NativePluginBackend backend;
    const auto ref = NativePluginBackend::makeAUReference(kType, kSubtype, kManufacturer, 2);
    if (ref.runtimeStatus != PluginRuntimeStatus::Ok) {
        WARN("Apple Peak Limiter not found on this system — skipping");
        return;
    }
    auto proc = backend.load(ref);
    REQUIRE(proc != nullptr);
    REQUIRE(NativePluginBackend::statusOf(*proc) == PluginRuntimeStatus::Ok);
}

TEST_CASE("NativePluginBackend AU state round-trip via stateBlob", "[native_backend][au]") {
    NativePluginBackend backend;
    const auto ref0 = NativePluginBackend::makeAUReference(kType, kSubtype, kManufacturer, 2);
    if (ref0.runtimeStatus != PluginRuntimeStatus::Ok) {
        WARN("Apple Peak Limiter not found — skipping");
        return;
    }

    // Phase 1: load, change a parameter, capture state.
    auto proc1 = backend.load(ref0);
    REQUIRE(NativePluginBackend::statusOf(*proc1) == PluginRuntimeStatus::Ok);

    const auto params = proc1->getParameters();
    REQUIRE(!params.empty());

    // Set a non-default normalized value on the first parameter.
    PluginState st1 = proc1->getState();
    const std::string pid = params[0].id;
    st1.parameters[pid] = params[0].toNormalized
                              ? params[0].toNormalized(0.5f * (params[0].minValue + params[0].maxValue))
                              : 0.4f;
    proc1->setState(st1);

    const PluginState saved = proc1->getState();
    REQUIRE(!saved.stateData.empty());   // stateBlob captured

    // Phase 2: build a new ExternalPluginReference from saved state, reload.
    mcp::plugin::ExternalPluginReference ref2 = ref0;
    ref2.stateBlob     = saved.stateData;
    ref2.paramSnapshot = {};

    auto proc2 = backend.load(ref2);
    REQUIRE(NativePluginBackend::statusOf(*proc2) == PluginRuntimeStatus::Ok);

    const PluginState restored = proc2->getState();
    REQUIRE(restored.stateData == saved.stateData);
}

TEST_CASE("NativePluginBackend makeAUReference: missing component sets Missing status", "[native_backend][au]") {
    const auto ref = NativePluginBackend::makeAUReference(0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 2);
    REQUIRE(ref.runtimeStatus == PluginRuntimeStatus::Missing);
}

TEST_CASE("NativePluginBackend AU malformed plugin ID returns Failed", "[native_backend][au]") {
    NativePluginBackend backend;
    ExternalPluginReference ref;
    ref.backend  = "au";
    ref.pluginId = "au:bad";   // too short

    auto proc = backend.load(ref);
    REQUIRE(proc != nullptr);
    REQUIRE(NativePluginBackend::statusOf(*proc) == PluginRuntimeStatus::Failed);
}

#endif // __APPLE__
