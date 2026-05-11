#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/plugin/AudioProcessor.h"
#include "engine/plugin/ParameterInfo.h"
#include "engine/plugin/PluginFactory.h"
#include "engine/plugin/PluginState.h"
#include "engine/plugin/PluginWrapper.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace mcp::plugin;
using Catch::Approx;

// ── Helpers ──────────────────────────────────────────────────────────────────

static ProcessContext makeCtx(double sr = 48000.0, int block = 256,
                               int inCh = 2, int outCh = 2) {
    return {sr, block, inCh, outCh, LatencyMode::Live};
}

// Fill a planar buffer with a constant value and return channel pointers.
struct TestBuffer {
    int chans, samps;
    std::vector<float>  data;
    std::vector<float*> ptrs;

    TestBuffer(int c, int s, float fill = 0.0f)
        : chans(c), samps(s),
          data(static_cast<size_t>(c * s), fill),
          ptrs(static_cast<size_t>(c))
    {
        for (int i = 0; i < c; ++i)
            ptrs[static_cast<size_t>(i)] = data.data() + i * s;
    }

    const float** cptrs() {
        m_cptrs.resize(static_cast<size_t>(chans));
        for (int i = 0; i < chans; ++i)
            m_cptrs[static_cast<size_t>(i)] = ptrs[static_cast<size_t>(i)];
        return m_cptrs.data();
    }

    float at(int ch, int s) const {
        return data[static_cast<size_t>(ch * samps + s)];
    }

private:
    std::vector<const float*> m_cptrs;
};

// ── ParameterInfo / makeLinearParam ──────────────────────────────────────────

TEST_CASE("makeLinearParam toNormalized/fromNormalized roundtrip", "[plugin][param]") {
    auto p = makeLinearParam("gain_db", "Gain", "dB", -96.0f, 12.0f, 0.0f);

    REQUIRE(p.toNormalized(-96.0f) == Approx(0.0f));
    REQUIRE(p.toNormalized( 12.0f) == Approx(1.0f));
    REQUIRE(p.toNormalized(  0.0f) == Approx(96.0f / 108.0f).epsilon(1e-5));

    REQUIRE(p.fromNormalized(0.0f) == Approx(-96.0f));
    REQUIRE(p.fromNormalized(1.0f) == Approx( 12.0f));

    const float v = -20.0f;
    REQUIRE(p.fromNormalized(p.toNormalized(v)) == Approx(v).epsilon(1e-4f));
}

TEST_CASE("ParameterSet value clamping", "[plugin][param]") {
    ParameterSet ps({ makeLinearParam("x", "X", "", 0.0f, 1.0f, 0.5f) });

    ps.setValue("x", 2.0f);
    REQUIRE(ps.getValue("x") == Approx(1.0f));

    ps.setValue("x", -5.0f);
    REQUIRE(ps.getValue("x") == Approx(0.0f));
}

TEST_CASE("ParameterSet indexFor / getValueAt / setValueAt", "[plugin][param]") {
    ParameterSet ps({ makeLinearParam("a", "A", "", 0.0f, 10.0f, 5.0f) });

    const int idx = ps.indexFor("a");
    REQUIRE(idx == 0);
    REQUIRE(ps.indexFor("nonexistent") == -1);

    REQUIRE(ps.getValueAt(idx) == Approx(5.0f));
    ps.setValueAt(idx, 7.0f);
    REQUIRE(ps.getValueAt(idx) == Approx(7.0f));
}

TEST_CASE("ParameterSet normalized get/set", "[plugin][param]") {
    ParameterSet ps({ makeLinearParam("v", "V", "", 0.0f, 100.0f, 50.0f) });

    REQUIRE(ps.getNormalized("v") == Approx(0.5f));
    ps.setNormalized("v", 0.25f);
    REQUIRE(ps.getValue("v") == Approx(25.0f));
}

// ── TrimProcessor basic processing ───────────────────────────────────────────

TEST_CASE("TrimProcessor: unity gain passes signal through", "[plugin][trim]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.trim.stereo");
    REQUIRE(proc != nullptr);

    proc->prepare(makeCtx());

    TestBuffer in (2, 64, 1.0f);
    TestBuffer out(2, 64, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 2, 2, 64 };

    proc->process(blk, noEvt);

    // 0 dB gain → output == input
    for (int s = 0; s < 64; ++s) {
        REQUIRE(out.at(0, s) == Approx(1.0f));
        REQUIRE(out.at(1, s) == Approx(1.0f));
    }
}

TEST_CASE("TrimProcessor: -6 dB gain halves amplitude", "[plugin][trim]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.trim.mono");
    REQUIRE(proc != nullptr);

    proc->setParameterValue("gain_db", -20.0f * std::log10(2.0f));  // ≈ -6.02 dB
    proc->prepare(makeCtx(48000.0, 256, 1, 1));

    TestBuffer in (1, 64, 1.0f);
    TestBuffer out(1, 64, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 1, 1, 64 };
    proc->process(blk, noEvt);

    for (int s = 0; s < 64; ++s)
        REQUIRE(out.at(0, s) == Approx(0.5f).epsilon(1e-4f));
}

// ── Sample-accurate automation ────────────────────────────────────────────────

TEST_CASE("TrimProcessor: sample-accurate gain automation", "[plugin][trim][automation]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.trim.stereo");
    proc->setParameterValue("gain_db", 0.0f);   // start at unity
    proc->prepare(makeCtx());

    TestBuffer in (2, 64, 1.0f);
    TestBuffer out(2, 64, 0.0f);

    // Event: switch to -∞ dB (silence) at sample 32
    const ParameterEvent ev{"gain_db", -96.0f, 32};
    const EventBlock evBlk{&ev, 1};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 2, 2, 64 };
    proc->process(blk, evBlk);

    // Samples 0–31: gain == unity (1.0)
    for (int s = 0; s < 32; ++s)
        REQUIRE(out.at(0, s) == Approx(1.0f).epsilon(1e-5f));

    // Samples 32–63: gain == -96 dB (≈ 0)
    for (int s = 32; s < 64; ++s)
        REQUIRE(out.at(0, s) == Approx(0.0f).margin(1e-4f));
}

// ── PluginState get / set roundtrip ──────────────────────────────────────────

TEST_CASE("TrimProcessor: getState / setState roundtrip", "[plugin][state]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.trim.stereo");
    proc->setParameterValue("gain_db", -12.0f);

    const PluginState saved = proc->getState();
    REQUIRE(saved.pluginId == "internal.trim.stereo");
    REQUIRE(saved.backend  == PluginBackend::Internal);
    REQUIRE(!saved.stateData.empty());

    auto proc2 = f.create("internal.trim.stereo");
    proc2->setState(saved);
    REQUIRE(proc2->getParameterValue("gain_db") == Approx(-12.0f).epsilon(1e-4f));
}

TEST_CASE("TrimProcessor: setState with unknown keys does not crash", "[plugin][state]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.trim.stereo");

    PluginState s;
    s.pluginId  = "internal.trim.stereo";
    s.backend   = PluginBackend::Internal;
    s.version   = 1;
    const std::string badJson = R"({"gain_db":-3.0,"unknown_key":42})";
    s.stateData.assign(badJson.begin(), badJson.end());
    s.parameters["another_unknown"] = 99.0f;

    REQUIRE_NOTHROW(proc->setState(s));
    REQUIRE(proc->getParameterValue("gain_db") == Approx(-3.0f).epsilon(1e-4f));
}

// ── InternalPluginFactory scan / create ──────────────────────────────────────

TEST_CASE("InternalPluginFactory scan returns 4 descriptors", "[plugin][factory]") {
    InternalPluginFactory f;
    const auto list = f.scan();
    REQUIRE(list.size() == 4);

    const std::vector<std::string> expectedIds{
        "internal.trim.mono", "internal.trim.stereo",
        "internal.delay.mono", "internal.delay.stereo"
    };
    for (const auto& id : expectedIds) {
        const bool found = std::any_of(list.begin(), list.end(),
                                       [&](const PluginDescriptor& d){ return d.id == id; });
        REQUIRE(found);
    }
}

TEST_CASE("InternalPluginFactory create returns nullptr for unknown id", "[plugin][factory]") {
    InternalPluginFactory f;
    REQUIRE(f.create("vst3.acme.reverb") == nullptr);
    REQUIRE(f.create("") == nullptr);
}

TEST_CASE("InternalPluginFactory descriptors have correct backend and layout", "[plugin][factory]") {
    InternalPluginFactory f;
    for (const auto& d : f.scan()) {
        REQUIRE(d.backend == PluginBackend::Internal);
        REQUIRE(!d.supportedLayouts.empty());
        REQUIRE(!d.supportedLayouts.front().outputs.empty());
        REQUIRE(d.supportsAutomation);
        REQUIRE(!d.isInstrument);
    }
}

// ── PluginWrapper ─────────────────────────────────────────────────────────────

TEST_CASE("PluginWrapper: output gain applied after processor", "[plugin][wrapper]") {
    InternalPluginFactory f;
    PluginWrapper w(f.create("internal.trim.stereo"));
    w.prepare(makeCtx());
    w.setOutputGainDb(-6.0206f);   // ≈ ×0.5

    TestBuffer in (2, 32, 1.0f);
    TestBuffer out(2, 32, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 2, 2, 32 };
    w.process(blk, noEvt);

    for (int s = 0; s < 32; ++s)
        REQUIRE(out.at(0, s) == Approx(0.5f).epsilon(2e-3f));
}

TEST_CASE("PluginWrapper: bypass copies input to output without processing", "[plugin][wrapper]") {
    InternalPluginFactory f;
    PluginWrapper w(f.create("internal.trim.stereo"));
    w.prepare(makeCtx());

    // Set gain to silence so we can tell if processor ran
    w.getProcessor()->setParameterValue("gain_db", -96.0f);
    w.setBypassed(true);

    TestBuffer in (2, 32, 0.7f);
    TestBuffer out(2, 32, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 2, 2, 32 };
    w.process(blk, noEvt);

    // Bypass → input copied to output regardless of processor gain
    for (int s = 0; s < 32; ++s)
        REQUIRE(out.at(0, s) == Approx(0.7f).epsilon(1e-5f));
}

TEST_CASE("PluginWrapper: latency passthrough", "[plugin][wrapper]") {
    InternalPluginFactory f;
    PluginWrapper w(f.create("internal.trim.stereo"));
    w.prepare(makeCtx());
    REQUIRE(w.getLatencySamples() == 0);
}

// ── DelayProcessor ────────────────────────────────────────────────────────────

TEST_CASE("DelayProcessor: prepare and process without crash", "[plugin][delay]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.delay.stereo");
    REQUIRE(proc != nullptr);
    REQUIRE_NOTHROW(proc->prepare(makeCtx()));

    TestBuffer in (2, 128, 0.5f);
    TestBuffer out(2, 128, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 2, 2, 128 };
    REQUIRE_NOTHROW(proc->process(blk, noEvt));
}

TEST_CASE("DelayProcessor: mix=0 outputs dry signal", "[plugin][delay]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.delay.mono");
    proc->setParameterValue("delay_ms", 100.0f);
    proc->setParameterValue("mix", 0.0f);
    proc->prepare(makeCtx(48000.0, 512, 1, 1));

    TestBuffer in (1, 64, 1.0f);
    TestBuffer out(1, 64, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 1, 1, 64 };
    proc->process(blk, noEvt);

    // mix == 0: output == dry == input
    for (int s = 0; s < 64; ++s)
        REQUIRE(out.at(0, s) == Approx(1.0f).epsilon(1e-5f));
}

TEST_CASE("DelayProcessor: delay shifts signal by correct sample count", "[plugin][delay]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.delay.mono");

    const double sr = 48000.0;
    // delay_ms = 100 → 4800 samples; use a smaller block to test it.
    // Use delay = 0 ms initially, switch to 5 samples worth.
    // Instead test: after the delay, the delayed output matches the input.
    const int delayMs  = 10;  // 480 samples at 48 kHz
    const int delaySmp = static_cast<int>(delayMs / 1000.0 * sr);  // 480

    proc->setParameterValue("delay_ms", static_cast<float>(delayMs));
    proc->setParameterValue("mix", 1.0f);
    proc->prepare(makeCtx(sr, 2048, 1, 1));

    // Feed delaySmp + 100 samples; input = constant 1.0
    const int totalSmp = delaySmp + 100;
    TestBuffer in (1, totalSmp, 1.0f);
    TestBuffer out(1, totalSmp, 0.0f);
    const EventBlock noEvt{};
    AudioBlock blk{ in.cptrs(), out.ptrs.data(), 1, 1, totalSmp };
    proc->process(blk, noEvt);

    // First delaySmp samples should be 0 (buffer was zeroed in prepare)
    for (int s = 0; s < delaySmp; ++s)
        REQUIRE(out.at(0, s) == Approx(0.0f).margin(1e-5f));

    // Samples after delaySmp should be 1.0 (the delayed input)
    for (int s = delaySmp; s < totalSmp; ++s)
        REQUIRE(out.at(0, s) == Approx(1.0f).epsilon(1e-5f));
}

TEST_CASE("DelayProcessor: getState / setState roundtrip", "[plugin][state][delay]") {
    InternalPluginFactory f;
    auto proc = f.create("internal.delay.stereo");
    proc->setParameterValue("delay_ms", 250.0f);
    proc->setParameterValue("mix",      0.75f);

    const PluginState s = proc->getState();
    REQUIRE(s.pluginId == "internal.delay.stereo");

    auto proc2 = f.create("internal.delay.stereo");
    proc2->setState(s);
    REQUIRE(proc2->getParameterValue("delay_ms") == Approx(250.0f).epsilon(1e-3f));
    REQUIRE(proc2->getParameterValue("mix")      == Approx(0.75f) .epsilon(1e-4f));
}
