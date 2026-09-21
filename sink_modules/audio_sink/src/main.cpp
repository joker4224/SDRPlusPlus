#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <dsp/convert/stereo_to_mono.h>
#include <utils/flog.h>
#include <RtAudio.h>
#include <config.h>
#include <core.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_sink",
    /* Description:     */ "Audio sink module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class AudioSink : SinkManager::Sink {
public:
    AudioSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;
        s2m.init(_stream->sinkOut);
        monoPacker.init(&s2m.out, 512);
        stereoPacker.init(_stream->sinkOut, 512);

#if RTAUDIO_VERSION_MAJOR >= 6
        audio.setErrorCallback([this](RtAudioErrorType type, const std::string& errorText) noexcept {
            handleError(type, errorText);
        });
#endif

        bool created = false;
        std::string device = "";
        config.acquire();
        if (!config.conf.contains(_streamName)) {
            created = true;
            config.conf[_streamName]["device"] = "";
            config.conf[_streamName]["devices"] = json({});
        }
        device = config.conf[_streamName]["device"];
        config.release(created);

        RtAudio::DeviceInfo info;
#if RTAUDIO_VERSION_MAJOR >= 6
        for (int i : audio.getDeviceIds()) {
#else
        int count = audio.getDeviceCount();
        for (int i = 0; i < count; i++) {
#endif
            try {
                info = audio.getDeviceInfo(i);
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
                if (!info.probed) { continue; }
#endif
                if (info.outputChannels == 0) { continue; }
                if (info.isDefaultOutput) { defaultDevId = devList.size(); }
                devList.push_back(info);
                deviceIds.push_back(i);
                txtDevList += info.name;
                txtDevList += '\0';
            }
            catch (const std::exception& e) {
                flog::error("AudioSinkModule Error getting audio device ({}) info: {}", i, e.what());
            }
        }
        selectByName(device);
        recoveryRequested.store(false);
        recoveryThread = std::thread(&AudioSink::recoveryWorker, this);
    }

    ~AudioSink() {
        recoveryStop.store(true);
        recoveryWake.notify_all();
        if (recoveryThread.joinable()) { recoveryThread.join(); }

        std::lock_guard<std::mutex> lck(controlMtx);
        running = false;
        doStop();
    }

    void start() {
        std::lock_guard<std::mutex> lck(controlMtx);
        if (running) { return; }
        running = true;
        if (!doStart()) {
            flog::warn("Audio output is unavailable; automatic recovery has been scheduled");
            recoveryRequested.store(true);
            recoveryWake.notify_all();
        }
    }

    void stop() {
        std::lock_guard<std::mutex> lck(controlMtx);
        if (!running) { return; }
        running = false;
        recoveryRequested.store(false);
        doStop();
    }

    void selectFirst() {
        selectById(defaultDevId);
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devList.size(); i++) {
            if (devList[i].name == name) {
                selectById(i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        std::lock_guard<std::mutex> lck(controlMtx);
        if (id < 0 || id >= devList.size()) { return; }

        devId = id;
        bool created = false;
        config.acquire();
        if (!config.conf[_streamName]["devices"].contains(devList[id].name)) {
            created = true;
            config.conf[_streamName]["devices"][devList[id].name] = devList[id].preferredSampleRate;
        }
        sampleRate = config.conf[_streamName]["devices"][devList[id].name];
        config.release(created);

        sampleRates = devList[id].sampleRates;
        sampleRatesTxt = "";
        char buf[256];
        bool found = false;
        unsigned int defaultId = 0;
        unsigned int defaultSr = devList[id].preferredSampleRate;
        for (int i = 0; i < sampleRates.size(); i++) {
            if (sampleRates[i] == sampleRate) {
                found = true;
                srId = i;
            }
            if (sampleRates[i] == defaultSr) {
                defaultId = i;
            }
            sprintf(buf, "%d", sampleRates[i]);
            sampleRatesTxt += buf;
            sampleRatesTxt += '\0';
        }
        if (!found) {
            sampleRate = defaultSr;
            srId = defaultId;
        }

        _stream->setSampleRate(sampleRate);

        if (running) {
            doStop();
            if (!doStart()) {
                recoveryRequested.store(true);
                recoveryWake.notify_all();
            }
        }
    }

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        ImGui::SetNextItemWidth(menuWidth);
        int selectedDevId;
        {
            std::lock_guard<std::mutex> lck(controlMtx);
            selectedDevId = devId;
        }
        if (ImGui::Combo(("##_audio_sink_dev_" + _streamName).c_str(), &selectedDevId, txtDevList.c_str())) {
            selectById(selectedDevId);
            std::string selectedDeviceName;
            {
                std::lock_guard<std::mutex> lck(controlMtx);
                selectedDeviceName = devList[selectedDevId].name;
            }
            config.acquire();
            config.conf[_streamName]["device"] = selectedDeviceName;
            config.release(true);
        }

        ImGui::SetNextItemWidth(menuWidth);
        int selectedSrId;
        {
            std::lock_guard<std::mutex> lck(controlMtx);
            selectedSrId = srId;
        }
        if (ImGui::Combo(("##_audio_sink_sr_" + _streamName).c_str(), &selectedSrId, sampleRatesTxt.c_str())) {
            unsigned int selectedSampleRate;
            std::string selectedDeviceName;
            {
                std::lock_guard<std::mutex> lck(controlMtx);
                srId = selectedSrId;
                sampleRate = sampleRates[srId];
                selectedSampleRate = sampleRate;
                selectedDeviceName = devList[devId].name;
                _stream->setSampleRate(sampleRate);
                if (running) {
                    doStop();
                    if (!doStart()) {
                        recoveryRequested.store(true);
                        recoveryWake.notify_all();
                    }
                }
            }
            config.acquire();
            config.conf[_streamName]["devices"][selectedDeviceName] = selectedSampleRate;
            config.release(true);
        }
    }

#if RTAUDIO_VERSION_MAJOR >= 6
    void handleError(RtAudioErrorType type, const std::string& errorText) noexcept {
        switch (type) {
        case RtAudioErrorType::RTAUDIO_NO_ERROR:
            return;
        case RtAudioErrorType::RTAUDIO_WARNING:
            flog::warn("AudioSinkModule Warning: {} ({})", errorText, (int)type);
            break;
        case RtAudioErrorType::RTAUDIO_NO_DEVICES_FOUND:
        case RtAudioErrorType::RTAUDIO_DEVICE_DISCONNECT:
            recoveryRequested.store(true);
            flog::warn("AudioSinkModule Warning: {} ({})", errorText, (int)type);
            break;
        default:
            // RtAudio can invoke this callback from its realtime worker thread.
            // Letting an exception escape that thread calls std::terminate(), which
            // appears on Windows as the fast-fail status 0xC0000409.  Device state
            // changes (for example a HDMI/DP endpoint disappearing when a display
            // powers down) must therefore be reported without throwing here.
            recoveryRequested.store(true);
            flog::error("AudioSinkModule Error: {} ({})", errorText, (int)type);
            break;
        }
    }
#endif

private:
    bool refreshSelectedDevice() {
        if (devId < 0 || devId >= devList.size()) { return false; }

        const std::string selectedName = devList[devId].name;
        try {
#if RTAUDIO_VERSION_MAJOR >= 6
            for (int id : audio.getDeviceIds()) {
#else
            int count = audio.getDeviceCount();
            for (int id = 0; id < count; id++) {
#endif
                RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
                if (!info.probed) { continue; }
#endif
                if (info.outputChannels == 0 || info.name != selectedName) { continue; }

                deviceIds[devId] = id;
                devList[devId] = info;
                return true;
            }
        }
        catch (const std::exception& e) {
            flog::warn("Could not refresh audio devices: {}", e.what());
        }
        return false;
    }

    bool doStart() {
        streamActive = false;
        audioOpened = false;
        if (!refreshSelectedDevice()) { return false; }

        RtAudio::StreamParameters parameters;
        parameters.deviceId = deviceIds[devId];
        parameters.nChannels = 2;
        unsigned int bufferFrames = sampleRate / 60;
        RtAudio::StreamOptions opts;
        opts.flags = RTAUDIO_MINIMIZE_LATENCY;
        opts.streamName = _streamName;

        try {
            audio.openStream(&parameters, NULL, RTAUDIO_FLOAT32, sampleRate, &bufferFrames, &callback, this, &opts);
            audioOpened = audio.isStreamOpen();
            if (!audioOpened) {
                flog::error("Could not open audio device: RtAudio did not open a stream");
                return false;
            }
            stereoPacker.setSampleCount(bufferFrames);
            audio.startStream();
            if (!audio.isStreamRunning()) {
                flog::error("Could not start audio device: RtAudio stream is not running");
                audio.closeStream();
                audioOpened = false;
                return false;
            }
            stereoPacker.start();
            streamActive = true;
        }
        catch (const std::exception& e) {
            flog::error("Could not open audio device {0}", e.what());
            if (audioOpened) { audio.closeStream(); }
            audioOpened = false;
            return false;
        }

        flog::info("RtAudio stream open");
        return true;
    }

    void doStop() {
        if (streamActive) {
            s2m.stop();
            monoPacker.stop();
            stereoPacker.stop();
            monoPacker.out.stopReader();
            stereoPacker.out.stopReader();
        }

        try {
            // closeStream() stops a running stream itself.  Avoid querying the
            // RtAudio state from this thread while its WASAPI worker is changing
            // that state after a device-disconnect notification.
            if (audioOpened) { audio.closeStream(); }
        }
        catch (const std::exception& e) {
            flog::error("Could not close audio device: {}", e.what());
        }
        audioOpened = false;

        if (streamActive) {
            monoPacker.out.clearReadStop();
            stereoPacker.out.clearReadStop();
        }
        streamActive = false;
    }

    void recoveryWorker() {
#if defined(_WIN32)
        // RtAudio's WASAPI backend uses COM while enumerating and opening
        // endpoints.  Recovery runs on this worker, so initialize COM here as
        // well (the original RtAudio object was constructed on the UI thread).
        HRESULT comResult = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        bool comInitialized = SUCCEEDED(comResult);
#endif

        bool recovering = false;
        std::unique_lock<std::mutex> waitLock(recoveryWaitMtx);

        while (!recoveryStop.load()) {
            recoveryWake.wait_for(waitLock, std::chrono::seconds(1));
            if (recoveryStop.load()) { break; }
            if (!recoveryRequested.exchange(false)) { continue; }

            waitLock.unlock();
            {
                std::lock_guard<std::mutex> lck(controlMtx);
                if (!running) {
                    recovering = false;
                }
                else {
                    if (!recovering) {
                        flog::warn("Audio output stream stopped; waiting for the device to return");
                        recovering = true;
                    }

                    doStop();
                    if (doStart()) {
                        flog::info("Audio output stream recovered");
                        recovering = false;
                    }
                    else {
                        recoveryRequested.store(true);
                    }
                }
            }
            waitLock.lock();
        }

#if defined(_WIN32)
        if (comInitialized) { CoUninitialize(); }
#endif
    }

    static int callback(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames, double streamTime, RtAudioStreamStatus status, void* userData) {
        AudioSink* _this = (AudioSink*)userData;
        int count = _this->stereoPacker.out.read();
        if (count < 0) { return 0; }

        memcpy(outputBuffer, _this->stereoPacker.out.readBuf, nBufferFrames * sizeof(dsp::stereo_t));
        _this->stereoPacker.out.flush();
        return 0;
    }

    SinkManager::Stream* _stream;
    dsp::convert::StereoToMono s2m;
    dsp::buffer::Packer<float> monoPacker;
    dsp::buffer::Packer<dsp::stereo_t> stereoPacker;

    std::string _streamName;

    int srId = 0;
    int devCount;
    int devId = 0;
    bool running = false;
    bool streamActive = false;
    bool audioOpened = false;

    std::mutex controlMtx;
    std::mutex recoveryWaitMtx;
    std::condition_variable recoveryWake;
    std::atomic<bool> recoveryStop = false;
    std::thread recoveryThread;
    std::atomic<bool> recoveryRequested = false;

    unsigned int defaultDevId = 0;

    std::vector<RtAudio::DeviceInfo> devList;
    std::vector<unsigned int> deviceIds;
    std::string txtDevList;

    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;
    unsigned int sampleRate = 48000;

    RtAudio audio;
};

class AudioSinkModule : public ModuleManager::Instance {
public:
    AudioSinkModule(std::string name) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;

        sigpath::sinkManager.registerSinkProvider("Audio", provider);
    }

    ~AudioSinkModule() {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Audio");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new AudioSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/audio_sink_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    AudioSinkModule* instance = new AudioSinkModule(name);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (AudioSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
