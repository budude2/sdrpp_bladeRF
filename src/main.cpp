#include <imgui.h>
#include <spdlog/spdlog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <options.h>
#include <libbladeRF.h>
#include <gui/widgets/stepped_slider.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO {
    /* Name:            */ "bladerf_source",
    /* Description:     */ "bladeRF source module for SDR++",
    /* Author:          */ "txjacob",
    /* Version:         */ 0, 0, 1,
    /* Max instances    */ 1
};

ConfigManager config;

const char* AGG_MODES_STR = "Off\0Low\0High\0";

class bladeRFSourceModule : public ModuleManager::Instance {
public:
    bladeRFSourceModule(std::string name) {
        this->name = name;

        sampleRate = 300000;

        handler.ctx                 = this;
        handler.selectHandler       = menuSelected;
        handler.deselectHandler     = menuDeselected;
        handler.menuHandler         = menuHandler;
        handler.startHandler        = start;
        handler.stopHandler         = stop;
        handler.tuneHandler         = tune;
        handler.stream              = &stream;

        refresh();

        config.aquire();
        std::string devSerial = config.conf["device"];
        config.release();
        selectFirst();
        core::setInputSampleRate(sampleRate);

        sigpath::sourceManager.registerSource("BladeRF", &handler);
    }

    ~bladeRFSourceModule() {
        
    }

    enum AGCMode {
        AGC_MODE_OFF,
        AGC_MODE_LOW,
        AGC_MODE_HIGG
    };

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {
        std::string devSerial;
        devList.clear();
        devListTxt = "";

        struct bladerf_devinfo *dev_info;
        int n = bladerf_get_device_list(&dev_info);

        for (int i = 0; i < n; i++) {
            devSerial = dev_info[i].serial;
            devList.push_back(devSerial);
            devListTxt += devSerial;
        }
    }

    void selectFirst() {
        if (devList.size() != 0) {
            selectBySerial(devList[0]);
        }
    }

    void selectBySerial(std::string serial) {
        struct bladerf_devinfo dev_info;

        /* Initialize the information used to identify the desired device
         * to all wildcard (i.e., "any device") values */
        bladerf_init_devinfo(&dev_info);
        strncpy(dev_info.serial, serial.c_str(), sizeof(dev_info.serial) - 1);
        try {
            int err = bladerf_open_with_devinfo(&dev, &dev_info);
            if (err != 0) {
                spdlog::error("Could not open bladeRF {0}", serial);
                return;
            }
        }
        catch (std::exception e) {
            spdlog::error("Could not open bladeRF {0}", serial);
        }

        selectedSerial = serial;
        const struct bladerf_range* range;

        // Original bladeRF only has 1 input channel, will need to change argument 2 to a variable for the new bladeRF
        bladerf_get_sample_rate_range(dev, BLADERF_CHANNEL_RX(0), &range);

        sampleRateList.clear();
        sampleRateListTxt = "";
        for (int i = 0; i < range->max / (range->min * 10); i++) {
            sampleRateList.push_back((range->min + range->min * i) * 10);
            sampleRateListTxt += getBandwdithScaled((range->min + range->min * i)*10);
            sampleRateListTxt += '\0';
        }

        // Load config here
        config.aquire();
        bool created = false;
        if (!config.conf["devices"].contains(selectedSerial)) {
            created = true;
            config.conf["devices"][selectedSerial]["sampleRate"] = 800000;
            config.conf["devices"][selectedSerial]["agcMode"] = 0;
            config.conf["devices"][selectedSerial]["lna"] = false;
            config.conf["devices"][selectedSerial]["lnaGain"] = 0;
            config.conf["devices"][selectedSerial]["rxvga1"] = 5;
            config.conf["devices"][selectedSerial]["rxvga2"] = 0;
        }

        // Load sample rate
        srId = 0;
        sampleRate = sampleRateList[0];
        if (config.conf["devices"][selectedSerial].contains("sampleRate")) {
            int selectedSr = config.conf["devices"][selectedSerial]["sampleRate"];
            for (int i = 0; i < sampleRateList.size(); i++) {
                if (sampleRateList[i] == selectedSr) {
                    srId = i;
                    sampleRate = selectedSr;
                    break;
                }
            }
        }

        // Load Gains
        if (config.conf["devices"][selectedSerial].contains("agcMode")) {
            agcMode = config.conf["devices"][selectedSerial]["agcMode"];
        }
        if (config.conf["devices"][selectedSerial].contains("lna")) {
            hfLNA = config.conf["devices"][selectedSerial]["lna"];
        }
        if (config.conf["devices"][selectedSerial].contains("lnaGain")) {
            LNAgain = config.conf["devices"][selectedSerial]["lnaGain"];
        }
        if (config.conf["devices"][selectedSerial].contains("rxvga1")) {
            rxvga1 = config.conf["devices"][selectedSerial]["rxvga1"];
        }
        if (config.conf["devices"][selectedSerial].contains("rxvga2")) {
            rxvga2 = config.conf["devices"][selectedSerial]["rxvga2"];
        }

        config.release(created);

        bladerf_close(dev);
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        spdlog::info("bladeRFSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;
        spdlog::info("bladeRFSourceModule '{0}': Menu Deselect!", _this->name);
    }
    
    static void start(void* ctx)
    {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;
        if (_this->running) {
            return;
        }
        if (_this->selectedSerial.empty()) {
            spdlog::error("Tried to start bladeRF source with null serial");
            return;
        }

        struct bladerf_devinfo dev_info;
        int status;

        struct bladerf_stream *bladerfStream;
        _this->bladerfStream = bladerfStream;

        
        /* Initialize the information used to identify the desired device
         * to all wildcard (i.e., "any device") values */
        bladerf_init_devinfo(&dev_info);
        strncpy(dev_info.serial, _this->selectedSerial.c_str(), sizeof(dev_info.serial) - 1);

        status = bladerf_open_with_devinfo(&(_this->dev), &dev_info);
        if (status != 0) {
            spdlog::error("Could not open bladeRF {0}", _this->selectedSerial);
            spdlog::error(bladerf_strerror(status));
            return;
        }

        bladerf_sample_rate actualRate;

        status = bladerf_set_sample_rate(_this->dev, BLADERF_CHANNEL_RX(0), _this->sampleRateList[_this->srId], &actualRate);
        if (status != 0) {
            spdlog::error("Could not set sample rate on bladeRF {0}", _this->selectedSerial);
            spdlog::error(bladerf_strerror(status));
            return;
        }

        spdlog::info("Sample rate set to {0}", actualRate);

        status = bladerf_set_bandwidth(_this->dev, BLADERF_CHANNEL_RX(0), 28000000, NULL);
        if (status != 0) {
            fprintf(stderr, "Failed to set bandwidth = %u: %s\n", 28000000,
            bladerf_strerror(status));
            return;
        }

        status = bladerf_set_frequency(_this->dev, BLADERF_CHANNEL_RX(0), _this->freq);
        if (status != 0) {
            spdlog::error("Could not set frequency rate on bladeRF {0}", _this->selectedSerial);
            spdlog::error(bladerf_strerror(status));
            return;
        }

        _this->channel_layout   = BLADERF_RX_X1;
        _this->format           = BLADERF_FORMAT_SC16_Q11;
        _this->num_buffers      = 8;
        _this->buffer_size      = roundf(((float)_this->sampleRateList[_this->srId]/200.0) / 1024) * 1024; // 1024 Samples
        _this->num_transfers    = 4;
        _this->stream_timeout   = 3500; // Milliseconds

        bladerf_sync_config(
            _this->dev,
            _this->channel_layout,
            _this->format,
            _this->num_buffers,
            _this->buffer_size,
            _this->num_transfers,
            _this->stream_timeout);

        status = bladerf_enable_module(_this->dev, BLADERF_CHANNEL_RX(0), true);
        if (status < 0) {
            fprintf(stderr, "Failed to enable module: %s\n",
            bladerf_strerror(status));
            bladerf_deinit_stream(bladerfStream);
            bladerf_close(_this->dev);
            return;
        }

        _this->running = true;
        _this->workerThread = std::thread(worker, _this);
        spdlog::info("bladeRFSourceModule '{0}': Start!", _this->name);
    }
    
    static void stop(void* ctx) {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;
        if (!_this->running) {
            return;
        }
        _this->stream.stopWriter();
        _this->workerThread.join();
        _this->running = false;
        bladerf_close(_this->dev);
        _this->stream.clearWriteStop();
        spdlog::info("bladeRFSourceModule '{0}': Stop!", _this->name);
    }
    
    static void tune(double freq, void* ctx) {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;
        _this->freq = freq;
        int status;
        if (_this->running) {
            status = bladerf_set_frequency(_this->dev, 0, _this->freq);
            if (status != 0) {
                spdlog::error("Could not set frequency rate on bladeRF {0}", _this->selectedSerial);
                spdlog::error(bladerf_strerror(status));
                return;
            }
        }
        spdlog::info("bladeRFSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }
    
    static void menuHandler(void* ctx) {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvailWidth();

        if (_this->running) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(CONCAT("##_bladeRF_dev_sel_", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectBySerial(_this->devList[_this->devId]);
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerial != "") {
                config.aquire();
                config.conf["device"] = _this->selectedSerial;
                config.release(true);
            }
        }

        if (ImGui::Combo(CONCAT("##_bladeRF_sr_sel_", _this->name), &_this->srId, _this->sampleRateListTxt.c_str())) {
            _this->sampleRate = _this->sampleRateList[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerial != "") {
                config.aquire();
                config.conf["devices"][_this->selectedSerial]["sampleRate"] = _this->sampleRate;
                config.release(true);
            }
        }

        ImGui::SameLine();
        float refreshBtnWdith = menuWidth - ImGui::GetCursorPosX();
        if (ImGui::Button(CONCAT("Refresh##_bladeRF_refr_", _this->name), ImVec2(refreshBtnWdith, 0))) {
            _this->refresh();
            config.aquire();
            std::string devSerial = config.conf["device"];
            config.release();
            _this->selectFirst();;
            core::setInputSampleRate(_this->sampleRate);
        }

        if (_this->running) { style::endDisabled(); }

        ImGui::Text("LNA Gain");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloatWithSteps(CONCAT("##_bladeRF_lnaGain_", _this->name), &_this->LNAgain, 0, 6, 3, "%.0f dB")) {
            if (_this->running) {
                bladerf_set_gain_stage(_this->dev, 0, "lna", _this->LNAgain);
            }
            if (_this->selectedSerial != "") {
                config.aquire();
                config.conf["devices"][_this->selectedSerial]["lnaGain"] = _this->LNAgain;
                config.release(true);
            }
        }
        
        ImGui::Text("rxVGA1 Gain");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloatWithSteps(CONCAT("##_bladeRF_rxvga1_", _this->name), &_this->rxvga1, 5, 30, 1, "%.0f dB")) {
            if (_this->running) {
                bladerf_set_gain_stage(_this->dev, 0, "rxvga1", _this->rxvga1);
            }
            if (_this->selectedSerial != "") {
                config.aquire();
                config.conf["devices"][_this->selectedSerial]["rxvga1"] = _this->rxvga1;
                config.release(true);
            }
        }

        ImGui::Text("rxVGA2 Gain");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloatWithSteps(CONCAT("##_bladeRF_rxvga2_", _this->name), &_this->rxvga2, 0, 30, 1, "%.0f dB")) {
            if (_this->running) {
                bladerf_set_gain_stage(_this->dev, 0, "rxvga2", _this->rxvga2);
            }
            if (_this->selectedSerial != "") {
                config.aquire();
                config.conf["devices"][_this->selectedSerial]["rxvga2"] = _this->rxvga2;
                config.release(true);
            }
        }  
    }

    static void worker(void* ctx) {
        bladeRFSourceModule* _this = (bladeRFSourceModule*)ctx;

        int16_t* inBuf = new int16_t[2 * _this->buffer_size * sizeof(int16_t)];
        int status;

        while(true)
        {
            status = bladerf_sync_rx(_this->dev, inBuf, _this->buffer_size, NULL, _this->stream_timeout);
            if (status != 0) {
                spdlog::error(bladerf_strerror(status));
            }
            for (int i = 0; i < _this->buffer_size; i++) {
                _this->stream.writeBuf[i].q = (float)inBuf[i * 2] / (float)65536;
                _this->stream.writeBuf[i].i = (float)inBuf[(i * 2) + 1] / (float)65536;
            }
            if (!_this->stream.swap(_this->buffer_size)) { break; };
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    std::string selectedSerial = "";
    int devId       = 0;
    int srId        = 0;
    int agcMode     = AGC_MODE_OFF;
    bool hfLNA      = false;
    float LNAgain   = 0;
    float rxvga1    = 5;
    float rxvga2    = 0;

    bladerf_channel_layout  channel_layout;
    bladerf_format          format;
    unsigned int            num_buffers;
    unsigned int            buffer_size;
    unsigned int            num_transfers;
    unsigned int            stream_timeout;

    std::vector<std::string> devList;
    std::string devListTxt;
    std::vector<uint32_t> sampleRateList;
    std::string sampleRateListTxt;
    
    /** [Opening a device] */
    struct bladerf *dev = NULL;
    struct bladerf_stream *bladerfStream;

    /** [struct channel_config] */
    /* The RX and TX channels are configured independently for these parameters */
    struct channel_config {
        bladerf_channel channel;
        unsigned int frequency;
        unsigned int bandwidth;
        unsigned int samplerate;
        int gain;
    };
    /** [struct channel_config] */

    std::thread workerThread;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(options::opts.root + "/bladeRF_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new bladeRFSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (bladeRFSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
