// test_au_offline.cpp — offline spike test for AUPluginAdapter.
// Guards everything with __APPLE__ so the file compiles on all platforms
// but only exercises AU code on macOS.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#ifdef __APPLE__

#include "engine/plugin/AUPluginAdapter.h"
#include "engine/plugin/ProcessContext.h"

#include <cmath>
#include <cstdint>
#include <vector>

// Apple Peak Limiter four-char codes.
// type: 'aufx' (kAudioUnitType_Effect)
// sub : 'lmtr' (kAudioUnitSubType_PeakLimiter)
// mfr : 'appl' (kAudioUnitManufacturer_Apple)
static constexpr uint32_t kFCC(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) <<  8) |
            static_cast<uint32_t>(d);
}

static mcp::plugin::AUPluginAdapter::Descriptor peakLimiterDesc() {
    return { kFCC('a','u','f','x'),
             kFCC('l','m','t','r'),
             kFCC('a','p','p','l'),
             2 };  // stereo
}

// Create an audio block view over pre-allocated planar buffers.
struct TestBlock {
    static constexpr int kCh    = 2;
    static constexpr int kFrames = 512;

    float inData [kCh][kFrames]{};
    float outData[kCh][kFrames]{};

    const float* inPtrs [kCh]{};
    float*       outPtrs[kCh]{};

    mcp::plugin::AudioBlock block{};

    TestBlock() {
        for (int c = 0; c < kCh; ++c) {
            inPtrs [c] = inData [c];
            outPtrs[c] = outData[c];
        }
        block.inputs            = inPtrs;
        block.outputs           = outPtrs;
        block.numInputChannels  = kCh;
        block.numOutputChannels = kCh;
        block.numSamples        = kFrames;
    }

    void fillSine(float amplitude = 0.5f, float freqHz = 440.0f,
                  float sampleRate = 48000.0f) {
        for (int n = 0; n < kFrames; ++n) {
            const float s = amplitude * std::sin(2.0f * 3.14159265f * freqHz *
                                                  static_cast<float>(n) / sampleRate);
            for (int c = 0; c < kCh; ++c) inData[c][n] = s;
        }
    }

    float rms(int ch) const {
        float sum = 0.0f;
        for (int n = 0; n < kFrames; ++n)
            sum += outData[ch][n] * outData[ch][n];
        return std::sqrt(sum / static_cast<float>(kFrames));
    }
};

// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("AUPluginAdapter — create and identity", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    REQUIRE_FALSE(adapter->pluginId().empty());
    REQUIRE_FALSE(adapter->displayName().empty());

    // ID should encode the four-char codes
    CHECK(adapter->pluginId().rfind("au:", 0) == 0);

    INFO("Plugin ID:   " << adapter->pluginId());
    INFO("Plugin name: " << adapter->displayName());
}

TEST_CASE("AUPluginAdapter — parameter list", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    const auto& params = adapter->getParameters();
    REQUIRE_FALSE(params.empty());

    for (const auto& p : params) {
        INFO("  id=" << p.id << "  name=" << p.name
             << "  range=[" << p.minValue << ", " << p.maxValue << "]"
             << "  unit=" << p.unit);
        CHECK_FALSE(p.id.empty());
        CHECK_FALSE(p.name.empty());
        CHECK(p.minValue <= p.maxValue);
    }
}

TEST_CASE("AUPluginAdapter — prepare and process stereo sine", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    mcp::plugin::ProcessContext ctx;
    ctx.sampleRate    = 48000.0;
    ctx.maxBlockSize  = TestBlock::kFrames;
    ctx.inputChannels = TestBlock::kCh;
    ctx.outputChannels= TestBlock::kCh;
    adapter->prepare(ctx);

    TestBlock tb;
    tb.fillSine(0.3f);

    const mcp::plugin::EventBlock noEvents{nullptr, 0};
    adapter->process(tb.block, noEvents);

    // Output should contain non-zero audio (limiter passes signal, just limits)
    CHECK(tb.rms(0) > 0.0f);
    CHECK(tb.rms(1) > 0.0f);
}

TEST_CASE("AUPluginAdapter — parameter get/set round-trip", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    const auto& params = adapter->getParameters();
    REQUIRE_FALSE(params.empty());

    const auto& first = params.front();
    // Clamp a test value to the valid range
    const float testVal = first.minValue + 0.5f * (first.maxValue - first.minValue);
    adapter->setParameterValue(first.id, testVal);

    const float got = adapter->getParameterValue(first.id);
    CHECK_THAT(got, Catch::Matchers::WithinRel(testVal, 0.01f));
}

TEST_CASE("AUPluginAdapter — normalized parameter round-trip", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    const auto& params = adapter->getParameters();
    REQUIRE_FALSE(params.empty());

    const std::string& id = params.front().id;
    adapter->setNormalizedParameterValue(id, 0.25f);
    const float n = adapter->getNormalizedParameterValue(id);
    CHECK_THAT(n, Catch::Matchers::WithinAbs(0.25f, 0.02f));
}

TEST_CASE("AUPluginAdapter — state save/restore round-trip", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    mcp::plugin::ProcessContext ctx;
    ctx.sampleRate     = 48000.0;
    ctx.maxBlockSize   = 512;
    ctx.inputChannels  = 2;
    ctx.outputChannels = 2;
    adapter->prepare(ctx);

    // Tweak a parameter before saving
    const auto& params = adapter->getParameters();
    REQUIRE_FALSE(params.empty());
    const std::string& pid = params.front().id;
    const float savedVal = params.front().minValue +
                           0.3f * (params.front().maxValue - params.front().minValue);
    adapter->setParameterValue(pid, savedVal);

    const mcp::plugin::PluginState state = adapter->getState();
    CHECK(state.backend == mcp::plugin::PluginBackend::AU);
    CHECK_FALSE(state.stateData.empty());
    CHECK_FALSE(state.parameters.empty());

    // Reset to defaults, then restore
    adapter->reset();
    adapter->setState(state);

    const float restored = adapter->getParameterValue(pid);
    CHECK_THAT(restored, Catch::Matchers::WithinRel(savedVal, 0.05f));
}

TEST_CASE("AUPluginAdapter — latency is non-negative", "[au_spike]") {
    auto adapter = mcp::plugin::AUPluginAdapter::create(peakLimiterDesc());
    REQUIRE(adapter != nullptr);

    mcp::plugin::ProcessContext ctx;
    ctx.sampleRate = 48000.0; ctx.maxBlockSize = 512;
    ctx.inputChannels = 2; ctx.outputChannels = 2;
    adapter->prepare(ctx);

    CHECK(adapter->getLatencySamples() >= 0);
    CHECK(adapter->getTailSamples()    >= 0);

    INFO("Latency: " << adapter->getLatencySamples() << " samples");
    INFO("Tail:    " << adapter->getTailSamples()    << " samples");
}

TEST_CASE("AUPluginAdapter — unknown component returns nullptr", "[au_spike]") {
    // Use an obviously invalid manufacturer to verify graceful failure.
    mcp::plugin::AUPluginAdapter::Descriptor bad{
        kFCC('a','u','f','x'),
        kFCC('x','x','x','x'),
        kFCC('x','x','x','x'),
        2
    };
    auto adapter = mcp::plugin::AUPluginAdapter::create(bad);
    CHECK(adapter == nullptr);
}

#else

TEST_CASE("AUPluginAdapter — skipped on non-Apple platform", "[au_spike]") {
    SUCCEED("AU adapter only available on macOS — test skipped.");
}

#endif // __APPLE__
