// SPDX-License-Identifier: GPL-3.0-only
// Native integration adapted from SDRPlusPlus_SwissKnifeEdition's ADS-B
// module (SDR++ Community). See ../THIRD_PARTY.md.
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>

#include "adsb/frame_detector.h"
#include "aircraft_tracker.h"
#include "iq_envelope.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:         */ "adsb_decoder",
    /* Description:  */ "Native 1090 MHz ADS-B aircraft receiver",
    /* Author:       */ "SDR++ Community; joker4224 fork contributors",
    /* Version:      */ 0, 1, 0,
    /* Max instances */ 1
};

namespace {
constexpr double FREQUENCY = 1090000000.0;
constexpr double SAMPLE_RATE = 2000000.0;
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

class ADSBDecoderModule : public ModuleManager::Instance {
public:
    explicit ADSBDecoderModule(std::string instanceName) : name(std::move(instanceName)) {
        detector.onFrame = [this](const adsb::RawFrame& frame) {
            // Called synchronously by receive() while rxMutex is held.
            ++validFrames;
            adsb::Message message;
            if (adsb::parseAdsb(frame.bytes, message, frame.len)) {
                tracker.update(message, blockTime, frame.sampleOffset / SAMPLE_RATE);
            }
        };
        playHandler = {[](bool, void* ctx) { static_cast<ADSBDecoderModule*>(ctx)->invalidate(); }, this};
        retuneHandler = {[](double, void* ctx) { static_cast<ADSBDecoderModule*>(ctx)->invalidate(); }, this};
        drawHandler = {[](ImGui::WaterFall::FFTRedrawArgs, void* ctx) {
            auto* self = static_cast<ADSBDecoderModule*>(ctx);
            self->refreshGate();
            if (self->showAircraft) { self->drawAircraftWindow(); }
        }, this};
        gui::mainWindow.onPlayStateChange.bindHandler(&playHandler);
        sigpath::sourceManager.onRetune.bindHandler(&retuneHandler);
        gui::waterfall.onFFTRedraw.bindHandler(&drawHandler);
        gui::menu.registerEntry(name, menuHandler, this, this);
        enable();
    }

    ~ADSBDecoderModule() override {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&drawHandler);
        sigpath::sourceManager.onRetune.unbindHandler(&retuneHandler);
        gui::mainWindow.onPlayStateChange.unbindHandler(&playHandler);
        disable();
    }

    void postInit() override {}

    void enable() override {
        if (enabled) { return; }
        enabled = true;
        invalidate();
        refreshGate();
    }

    void disable() override {
        if (!enabled) { return; }
        enabled = false;
        stopReceiver();
        invalidate();
    }

    bool isEnabled() override { return enabled; }

private:
    void startReceiver() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
            0, SAMPLE_RATE, SAMPLE_RATE, SAMPLE_RATE, SAMPLE_RATE, true);
        if (!vfo) { return; }
        vfo->setSnapInterval(1000);
        invalidate();
        sink = std::make_unique<dsp::sink::Handler<dsp::complex_t>>(vfo->output, receive, this);
        sink->start();
    }

    void stopReceiver() {
        if (!vfo) { return; }
        // Join the consumer before destroying its input or captured state.
        sink->stop();
        sink.reset();
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = nullptr;
    }
    void resetLocked() {
        detector.reset();
        tracker.clear();
        validFrames = 0;
        lastDataTime = 0;
    }

    void invalidate() {
        std::lock_guard<std::mutex> lock(rxMutex);
        receiving = false;
        readyAfter = nowSeconds() + 0.25; // Drain buffered samples after a retune/restart.
        resetLocked();
    }

    // This is called only on the GUI thread, including when the menu is folded.
    // The DSP worker never reads GUI objects or receiver configuration.
    void refreshGate() {
        if (!enabled) { return; }
        double rate = sigpath::iqFrontEnd.getEffectiveSamplerate();
        // Do not construct a wideband upsampling VFO for a narrowband source.
        // Delay it until the user selects a suitable source rate.
        if (!std::isfinite(rate) || rate < SAMPLE_RATE) {
            stopReceiver();
            invalidate();
            std::lock_guard<std::mutex> lock(rxMutex);
            status = "Need >= 2 MS/s after decimation";
            return;
        }
        if (!vfo) { startReceiver(); }
        if (!vfo) {
            std::lock_guard<std::mutex> lock(rxMutex);
            status = "Could not create ADS-B VFO";
            return;
        }
        double center = gui::waterfall.getCenterFrequency();
        double offset = vfo->getOffset();
        bool covered = rate >= SAMPLE_RATE && std::fabs(offset) + SAMPLE_RATE / 2 <= rate / 2 + 1;
        bool tuned = std::fabs(center + offset - FREQUENCY) <= 1;
        bool playing = gui::mainWindow.isPlaying();
        std::lock_guard<std::mutex> lock(rxMutex);
        if (center != lastCenter || offset != lastOffset || rate != lastRate) {
            lastCenter = center;
            lastOffset = offset;
            lastRate = rate;
            readyAfter = nowSeconds() + 0.25;
            resetLocked();
        }
        bool next = playing && covered && tuned && nowSeconds() >= readyAfter;
        if (receiving && !next) { resetLocked(); }
        receiving = next;
        status = !playing ? "Source stopped" : !covered ? "Need >= 2 MS/s covering this VFO"
            : !tuned ? "Tune this VFO to 1090 MHz" : !next ? "Settling after tune/start" : "Receiving 1090 MHz";
    }

    static void receive(dsp::complex_t* data, int count, void* ctx) {
        auto* self = static_cast<ADSBDecoderModule*>(ctx);
        std::lock_guard<std::mutex> lock(self->rxMutex);
        if (!self->receiving || count <= 0) { return; }
        double now = nowSeconds();
        // Do not stitch a partial burst across a stalled stream or source gap.
        if (self->lastDataTime != 0 && now - self->lastDataTime > 0.5) { self->resetLocked(); }
        self->lastDataTime = now;
        self->blockTime = now;
        self->magnitude.resize(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            self->magnitude[i] = adsb::iqEnvelope(data[i].re, data[i].im);
        }
        self->detector.process(self->magnitude.data(), count);
    }

    static void menuHandler(void* ctx) {
        auto* self = static_cast<ADSBDecoderModule*>(ctx);
        self->refreshGate();
        ImGui::PushID(self);
        if (!self->enabled) { style::beginDisabled(); }
        if (ImGui::Button("Tune to 1090 MHz", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            self->invalidate();
            tuner::tune(tuner::TUNER_MODE_CENTER, self->vfo ? self->name : "", FREQUENCY);
        }
        if (!self->enabled) { style::endDisabled(); }
        ImGui::TextWrapped("Use a source rate of at least 2 MS/s after decimation, then press Play.");
        {
            std::lock_guard<std::mutex> lock(self->rxMutex);
            ImGui::TextWrapped("%s", self->enabled ? self->status.c_str() : "Module disabled");
            auto aircraft = self->tracker.snapshot(nowSeconds());
            ImGui::Text("CRC-valid DF17: %llu", static_cast<unsigned long long>(self->validFrames));
            ImGui::Text("Aircraft: %zu", aircraft.size());
        }
        ImGui::Checkbox("Show aircraft list", &self->showAircraft);
        if (ImGui::Button("Clear aircraft and counters")) {
            std::lock_guard<std::mutex> lock(self->rxMutex);
            self->resetLocked();
        }
        ImGui::TextWrapped("Airborne positions require a fresh even/odd CPR pair. Unknown or stale fields show --.");
        ImGui::PopID();
    }

    void drawAircraftWindow() {
        double now;
        std::vector<adsb::Aircraft> aircraft;
        {
            std::lock_guard<std::mutex> lock(rxMutex);
            now = nowSeconds();
            aircraft = tracker.snapshot(now);
        }
        ImGui::SetNextWindowSize(ImVec2(1080 * style::uiScale, 400 * style::uiScale), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(("ADS-B aircraft##" + name).c_str(), &showAircraft)) {
            ImGui::TextWrapped("Callsign is broadcast identification, not a verified commercial flight number. Altitude is barometric ft. Speed is kt (GS / IAS / TAS).");
            ImGui::TextDisabled("Dynamic fields expire after 15 s; aircraft after 60 s. Surface position is not decoded.");
            auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("aircraft", 9, flags, ImVec2(0, 0))) {
                ImGui::TableSetupScrollFreeze(1, 1);
                for (const char* column : {"ICAO", "Callsign", "Latitude", "Longitude", "Alt ft (baro)", "Speed kt", "Track / heading", "V.rate ft/min", "Seen s"}) {
                    ImGui::TableSetupColumn(column);
                }
                ImGui::TableHeadersRow();
                for (const auto& ac : aircraft) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%06X", ac.icao);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.callsign.empty() ? "--" : ac.callsign.c_str());
                    ImGui::TableNextColumn(); if (ac.havePos) ImGui::Text("%.5f", ac.lat); else ImGui::TextUnformatted("--");
                    ImGui::TableNextColumn(); if (ac.havePos) ImGui::Text("%.5f", ac.lon); else ImGui::TextUnformatted("--");
                    ImGui::TableNextColumn(); if (ac.haveAlt) ImGui::Text("%d", ac.altFt); else ImGui::TextUnformatted(ac.surface ? "Surface" : "--");
                    ImGui::TableNextColumn(); if (ac.haveSpeed) ImGui::Text("%.1f %s", ac.speedKt, !ac.airspeed ? "GS" : ac.trueAirspeed ? "TAS" : "IAS"); else ImGui::TextUnformatted("--");
                    ImGui::TableNextColumn(); if (ac.haveHeading) ImGui::Text("%.1f %s", ac.headingDeg, ac.airspeed ? "HDG" : "TRK"); else ImGui::TextUnformatted("--");
                    ImGui::TableNextColumn(); if (ac.haveVrate) ImGui::Text("%d %s", ac.vrateFpm, ac.verticalRateBarometric ? "baro" : "GNSS"); else ImGui::TextUnformatted("--");
                    ImGui::TableNextColumn(); ImGui::Text("%.1f", now - ac.lastSeen);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    std::string name;
    bool enabled = false, showAircraft = true;
    VFOManager::VFO* vfo = nullptr;
    std::unique_ptr<dsp::sink::Handler<dsp::complex_t>> sink;
    EventHandler<bool> playHandler;
    EventHandler<double> retuneHandler;
    EventHandler<ImGui::WaterFall::FFTRedrawArgs> drawHandler;
    double lastCenter = -1, lastOffset = -1, lastRate = -1;

    // Every member below is protected by rxMutex, including callback state.
    std::mutex rxMutex;
    adsb::FrameDetector detector;
    adsb::AircraftTracker tracker;
    std::vector<float> magnitude;
    bool receiving = false;
    double readyAfter = 0, blockTime = 0, lastDataTime = 0;
    uint64_t validFrames = 0;
    std::string status = "Source stopped";
};

MOD_EXPORT void _INIT_() {}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new ADSBDecoderModule(std::move(name)); }
MOD_EXPORT void _DELETE_INSTANCE_(void* instance) { delete static_cast<ADSBDecoderModule*>(instance); }
MOD_EXPORT void _END_() {}
