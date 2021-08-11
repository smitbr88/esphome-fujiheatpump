#include "fujihp.h"
using namespace esphome;

/**
 * Create a new FujiAirCon object
 *
 * Args:
 *   hw_serial: pointer to an Arduino HardwareSerial instance
 *   poll_interval: polling interval in milliseconds
 */
FujiAirCon::FujiAirCon(
        HardwareSerial* hw_serial,
        uint32_t poll_interval
) :
    PollingComponent{poll_interval}, // member initializers list
    hw_serial_{hw_serial}
{
    this->traits_.set_supports_action(true);
    this->traits_.set_supports_current_temperature(true);
    this->traits_.set_supports_two_point_target_temperature(false);
    this->traits_.set_supports_away(false);
    this->traits_.set_visual_min_temperature(FUJIHP_MIN_TEMPERATURE);
    this->traits_.set_visual_max_temperature(FUJIHP_MAX_TEMPERATURE);
    this->traits_.set_visual_temperature_step(FUJIHP_TEMPERATURE_STEP);
}

void FujiAirCon::check_logger_conflict_() {
#ifdef USE_LOGGER
    if (this->get_hw_serial_() == logger::global_logger->get_hw_serial()) {
        ESP_LOGW(TAG, "  You're using the same serial port for logging"
                " and the FujiAirCon component. Please disable"
                " logging over the serial port by setting"
                " logger:baud_rate to 0.");
    }
#endif
}

void FujiAirCon::update() {
    // This will be called every "update_interval" milliseconds.
    //this->dump_config();
      hp.waitForFrame();     // attempt to read state from bus and place a reply frame in the buffer
      hp.sendPendingFrame(); // send any frame waiting in the buffer
#ifndef USE_CALLBACKS
    this->hpSettingsChanged();
    heatpumpStatus currentStatus = hp->getStatus();
    this->hpStatusChanged(currentStatus);
#endif
      hp.getOnOff();
      hp.getMode();
      hp.getFanMode();
      hp.getTemp();
}

/**
 * Get our supported traits.
 *
 * Note:
 * Many of the following traits are only available in the 1.5.0 dev train of
 * ESPHome, particularly the Dry operation mode, and several of the fan modes.
 *
 * Returns:
 *   This class' supported climate::ClimateTraits.
 */
climate::ClimateTraits FujiAirCon::traits() {
    return traits_;
}

/**
 * Modify our supported traits.
 *
 * Returns:
 *   A reference to this class' supported climate::ClimateTraits.
 */
climate::ClimateTraits& FujiAirCon::config_traits() {
    return traits_;
}

/**
 * Implement control of a FujiAirCon.
 *
 * Maps HomeAssistant/ESPHome modes to Mitsubishi modes.
 */
void FujiAirCon::control(const climate::ClimateCall &call) {
    ESP_LOGV(TAG, "Control called.");

    bool updated = false;
    bool has_mode = call.get_mode().has_value();
    bool has_temp = call.get_target_temperature().has_value();
    if (has_mode){
        this->mode = *call.get_mode();
    }
    switch (this->mode) {
        case climate::CLIMATE_MODE_COOL:
            hp.setMode("COOL");
            hp.setOnOff(1);

            if (has_mode){
                if (cool_setpoint.has_value() && !has_temp) {
                    hp.setTemp(cool_setpoint.value());
                    this->target_temperature = cool_setpoint.value();
                }
                this->action = climate::CLIMATE_ACTION_IDLE;
                updated = true;
            }
            break;
        case climate::CLIMATE_MODE_HEAT:
            hp.setMode("HEAT");
            hp.setOnOff(1);
            if (has_mode){
                if (heat_setpoint.has_value() && !has_temp) {
                    hp.setTemp(heat_setpoint.value());
                    this->target_temperature = heat_setpoint.value();
                }
                this->action = climate::CLIMATE_ACTION_IDLE;
                updated = true;
            }
            break;
        case climate::CLIMATE_MODE_DRY:
            hp.setMode("DRY");
            hp.setOnOff(1);
            if (has_mode){
                this->action = climate::CLIMATE_ACTION_DRYING;
                updated = true;
            }
            break;
        case climate::CLIMATE_MODE_HEAT_COOL:
            hp.setMode("AUTO");
            hp.setOnOff(1);
            if (has_mode){
                if (auto_setpoint.has_value() && !has_temp) {
                    hp.setTemp(auto_setpoint.value());
                    this->target_temperature = auto_setpoint.value();
                }
                this->action = climate::CLIMATE_ACTION_IDLE;
            }
            updated = true;
            break;
        case climate::CLIMATE_MODE_FAN_ONLY:
            hp.setMode("FAN");
            hp.setOnOff(1);
            if (has_mode){
                this->action = climate::CLIMATE_ACTION_FAN;
                updated = true;
            }
            break;
        case climate::CLIMATE_MODE_OFF:
        default:
            if (has_mode){
                hp.setOnOff(0);
                this->action = climate::CLIMATE_ACTION_OFF;
                updated = true;
            }
            break;
    }

    if (has_temp){
        ESP_LOGV(
            "control", "Sending target temp: %.1f",
            *call.get_target_temperature()
        );
        hp.setTemp(*call.get_target_temperature());
        this->target_temperature = *call.get_target_temperature();
        updated = true;
    }

    //const char* FAN_MAP[6]         = {"AUTO", "QUIET", "1", "2", "3", "4"};
    if (call.get_fan_mode().has_value()) {
        ESP_LOGV("control", "Requested fan mode is %s", *call.get_fan_mode());
        this->fan_mode = *call.get_fan_mode();
        switch(*call.get_fan_mode()) {
            case climate::CLIMATE_FAN_OFF:
                hp.setOnOff(0);
                updated = true;
                break;
            case climate::CLIMATE_FAN_LOW:
                hp.setFanMode(2);
                updated = true;
                break;
            case climate::CLIMATE_FAN_MEDIUM:
                hp.setFanMode(3);
                updated = true;
                break;
            case climate::CLIMATE_FAN_HIGH:
                hp.setFanMode(4);
                updated = true;
                break;
            case climate::CLIMATE_FAN_ON:
            case climate::CLIMATE_FAN_AUTO:
            default:
                hp.setFanMode(0);
                updated = true;
                break;
        }
    }


    ESP_LOGD(TAG, "control - Was HeatPump updated? %s", YESNO(updated));

    // send the update back to esphome:
    this->publish_state();
    // and the heat pump:
    hp.update();
}

void FujiAirCon::hpSettingsChanged() {
    heatpumpSettings currentSettings = hp.getSettings();

    if (currentSettings.power == NULL) {
        /*
         * We should always get a valid pointer here once the HeatPump
         * component fully initializes. If HeatPump hasn't read the settings
         * from the unit yet (hp.connect() doesn't do this, sadly), we'll need
         * to punt on the update. Likely not an issue when run in callback
         * mode, but that isn't working right yet.
         */
        ESP_LOGW(TAG, "Waiting for HeatPump to read the settings the first time.");
        delay(10);
        return;
    }

    /*
     * ************ HANDLE POWER AND MODE CHANGES ***********
     * https://github.com/geoffdavis/HeatPump/blob/stream/src/HeatPump.h#L125
     * const char* POWER_MAP[2]       = {"OFF", "ON"};
     * const char* MODE_MAP[5]        = {"HEAT", "DRY", "COOL", "FAN", "AUTO"};
     */
    if (strcmp(currentSettings.power, 1) == 0) {
        if (strcmp(currentSettings.mode, "HEAT") == 0) {
            this->mode = climate::CLIMATE_MODE_HEAT;
            if (heat_setpoint != currentSettings.temperature) {
                heat_setpoint = currentSettings.temperature;
                save(currentSettings.temperature, heat_storage);
            }
            this->action = climate::CLIMATE_ACTION_IDLE;
        } else if (strcmp(currentSettings.mode, "DRY") == 0) {
            this->mode = climate::CLIMATE_MODE_DRY;
            this->action = climate::CLIMATE_ACTION_DRYING;
        } else if (strcmp(currentSettings.mode, "COOL") == 0) {
            this->mode = climate::CLIMATE_MODE_COOL;
            if (cool_setpoint != currentSettings.temperature) {
                cool_setpoint = currentSettings.temperature;
                save(currentSettings.temperature, cool_storage);
            }
            this->action = climate::CLIMATE_ACTION_IDLE;
        } else if (strcmp(currentSettings.mode, "FAN") == 0) {
            this->mode = climate::CLIMATE_MODE_FAN_ONLY;
            this->action = climate::CLIMATE_ACTION_FAN;
        } else if (strcmp(currentSettings.mode, "AUTO") == 0) {
            this->mode = climate::CLIMATE_MODE_HEAT_COOL;
            if (auto_setpoint != currentSettings.temperature) {
                auto_setpoint = currentSettings.temperature;
                save(currentSettings.temperature, auto_storage);
            }
            this->action = climate::CLIMATE_ACTION_IDLE;
        } else {
            ESP_LOGW(
                    TAG,
                    "Unknown climate mode value %s received from HeatPump",
                    currentSettings.mode
            );
        }
    } else {
        this->mode = climate::CLIMATE_MODE_OFF;
        this->action = climate::CLIMATE_ACTION_OFF;
    }

    ESP_LOGI(TAG, "Climate mode is: %i", this->mode);

    /*
     * ******* HANDLE FAN CHANGES ********
     *
     * const char* FAN_MAP[6]         = {"AUTO", "QUIET", "1", "2", "3", "4"};
     */
    if (strcmp(currentSettings.fan, "2") == 0) {
            this->fan_mode = climate::CLIMATE_FAN_LOW;
    } else if (strcmp(currentSettings.fan, "3") == 0) {
            this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
    } else if (strcmp(currentSettings.fan, "4") == 0) {
            this->fan_mode = climate::CLIMATE_FAN_HIGH;
    } else { //case "AUTO" or default:
        this->fan_mode = climate::CLIMATE_FAN_AUTO;
    }
    ESP_LOGI(TAG, "Fan mode is: %i", this->fan_mode);

    /*
     * ******** HANDLE TARGET TEMPERATURE CHANGES ********
     */
    this->target_temperature = currentSettings.temperature;
    ESP_LOGI(TAG, "Target temp is: %f", this->target_temperature);

    /*
     * ******** Publish state back to ESPHome. ********
     */
    this->publish_state();
}

/**
 * Report changes in the current temperature sensed by the HeatPump.
 */
void FujiAirCon::hpStatusChanged(heatpumpStatus currentStatus) {
    this->current_temperature = currentStatus.roomTemperature;
    switch (this->mode) {
        case climate::CLIMATE_MODE_HEAT:
            if (currentStatus.operating) {
                this->action = climate::CLIMATE_ACTION_HEATING;
            }
            else {
                this->action = climate::CLIMATE_ACTION_IDLE;
            }
            break;
        case climate::CLIMATE_MODE_COOL:
            if (currentStatus.operating) {
                this->action = climate::CLIMATE_ACTION_COOLING;
            }
            else {
                this->action = climate::CLIMATE_ACTION_IDLE;
            }
            break;
        case climate::CLIMATE_MODE_HEAT_COOL:
            this->action = climate::CLIMATE_ACTION_IDLE;
            if (currentStatus.operating) {
              if (this->current_temperature > this->target_temperature) {
                  this->action = climate::CLIMATE_ACTION_COOLING;
              } else if (this->current_temperature < this->target_temperature) {
                  this->action = climate::CLIMATE_ACTION_HEATING;
              }
            }
            break;
        case climate::CLIMATE_MODE_DRY:
            if (currentStatus.operating) {
                this->action = climate::CLIMATE_ACTION_DRYING;
            }
            else {
                this->action = climate::CLIMATE_ACTION_IDLE;
            }
            break;
        case climate::CLIMATE_MODE_FAN_ONLY:
            this->action = climate::CLIMATE_ACTION_FAN;
            break;
        default:
            this->action = climate::CLIMATE_ACTION_OFF;
    }

    this->publish_state();
}

void FujiAirCon::set_remote_temperature(float temp) {
    ESP_LOGD(TAG, "Setting remote temp: %.1f", temp);
    this->hp.setRemoteTemperature(temp);
}

void FujiAirCon::setup() {
    // This will be called by App.setup()
    hp.connect(&Serial2, true); // second parameter is whether to init as a secondary controller
    this->banner();
    ESP_LOGCONFIG(TAG, "Setting up UART...");
    if (!this->get_hw_serial_()) {
        ESP_LOGCONFIG(
                TAG,
                "No HardwareSerial was provided. "
                "Software serial ports are unsupported by this component."
        );
        this->mark_failed();
        return;
    }
    this->check_logger_conflict_();

    ESP_LOGCONFIG(TAG, "Intializing new HeatPump object.");
    this->hp = new HeatPump();
    this->current_temperature = NAN;
    this->target_temperature = NAN;
    this->fan_mode = climate::CLIMATE_FAN_OFF;
    this->swing_mode = climate::CLIMATE_SWING_OFF;

#ifdef USE_CALLBACKS
    hp.setSettingsChangedCallback(
            [this]() {
                this->hpSettingsChanged();
            }
    );

    hp.setStatusChangedCallback(
            [this](heatpumpStatus currentStatus) {
                this->hpStatusChanged(currentStatus);
            }
    );
#endif

    ESP_LOGCONFIG(
            TAG,
            "hw_serial(%p) is &Serial(%p)? %s",
            this->get_hw_serial_(),
            &Serial,
            YESNO(this->get_hw_serial_() == &Serial)
    );

    ESP_LOGCONFIG(TAG, "Calling hp.connect(%p)", this->get_hw_serial_());

    if (hp.connect(this->get_hw_serial_(), this->baud_)) {
        hp.sync();
    }
    else {
        ESP_LOGCONFIG(
                TAG,
                "Connection to HeatPump failed."
                " Marking FujiAirCon component as failed."
        );
        this->mark_failed();
    }

    // create various setpoint persistence:
    cool_storage = global_preferences.make_preference<uint8_t>(this->get_object_id_hash() + 1);
    heat_storage = global_preferences.make_preference<uint8_t>(this->get_object_id_hash() + 2);
    auto_storage = global_preferences.make_preference<uint8_t>(this->get_object_id_hash() + 3);

    // load values from storage:
    cool_setpoint = load(cool_storage);
    heat_setpoint = load(heat_storage);
    auto_setpoint = load(auto_storage);

    this->dump_config();
}

/**
 * The ESP only has a few bytes of rtc storage, so instead
 * of storing floats directly, we'll store the number of
 * TEMPERATURE_STEPs from MIN_TEMPERATURE.
 **/
void FujiAirCon::save(float value, ESPPreferenceObject& storage) {
    uint8_t steps = (value - FUJIHP_MIN_TEMPERATURE) / FUJIHP_TEMPERATURE_STEP;
    storage.save(&steps);
}

optional<float> FujiAirCon::load(ESPPreferenceObject& storage) {
    uint8_t steps = 0;
    if (!storage.load(&steps)) {
        return {};
    }
    return FUJIHP_MIN_TEMPERATURE + (steps * FUJIHP_TEMPERATURE_STEP);
}

void FujiAirCon::dump_config() {
    this->banner();
    ESP_LOGI(TAG, "  Supports HEAT: %s", YESNO(true));
    ESP_LOGI(TAG, "  Supports COOL: %s", YESNO(true));
    ESP_LOGI(TAG, "  Supports AWAY mode: %s", YESNO(false));
    ESP_LOGI(TAG, "  Saved heat: %.1f", heat_setpoint.value_or(-1));
    ESP_LOGI(TAG, "  Saved cool: %.1f", cool_setpoint.value_or(-1));
    ESP_LOGI(TAG, "  Saved auto: %.1f", auto_setpoint.value_or(-1));
}

void FujiAirCon::dump_state() {
    LOG_CLIMATE("", "FujiAirCon Climate", this);
    ESP_LOGI(TAG, "HELLO");
}