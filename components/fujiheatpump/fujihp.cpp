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
    this->traits_.set_supports_action(false);
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

    ESP_LOGCONFIG(
            TAG,
            "hw_serial(%p) is &Serial(%p)? %s",
            this->get_hw_serial_(),
            &Serial,
            YESNO(this->get_hw_serial_() == &Serial)
    );
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