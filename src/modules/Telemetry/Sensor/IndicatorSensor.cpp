#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && defined(SENSECAP_INDICATOR)

#include "IndicatorSensor.h"
#include "IndicatorSerial.h"
#include "TelemetrySensor.h"
#include <Adafruit_Sensor.h>
#include <driver/uart.h>

IndicatorSensor::IndicatorSensor() : TelemetrySensor(meshtastic_TelemetrySensorType_SENSOR_UNSET, "Indicator") {}

static int cmd_send(meshtastic_MessageType cmd, uint32_t value)
{
    meshtastic_InterdeviceMessage message = meshtastic_InterdeviceMessage_init_zero;

    message.data.sensor.type = cmd;
    message.data.sensor.data.uint32_value = value;
    return sensecapIndicator.send_uplink(message);
}

bool IndicatorSensor::initDevice(TwoWire *bus, ScanI2C::FoundDevice *dev)
{
    LOG_INFO("%s: init", sensorName);
    setup();
    return true;
}

void IndicatorSensor::setup()
{
    cmd_send(meshtastic_MessageType_POWER_ON, 0);
    // measure and send only once every minute, for the phone API
    cmd_send(meshtastic_MessageType_COLLECT_INTERVAL, 60000);
}

bool IndicatorSensor::getMetrics(meshtastic_Telemetry *measurement)
{
    meshtastic_SensorData data = meshtastic_SensorData_init_zero;

    if (ringBuf.pop(data)) {

        switch (data.type) {
        case meshtastic_MessageType_SCD41_CO2: {

            // LOG_DEBUG("CO2: %.1f", value);

            break;
        }

        case meshtastic_MessageType_AHT20_TEMP: {

            // LOG_DEBUG("Temp: %.1f", value);

            measurement->variant.environment_metrics.has_temperature = true;
            measurement->variant.environment_metrics.temperature = data.data.float_value;
            break;
        }

        case meshtastic_MessageType_AHT20_HUMIDITY: {

            // LOG_DEBUG("Humidity: %.1f", value);

            measurement->variant.environment_metrics.has_relative_humidity = true;
            measurement->variant.environment_metrics.relative_humidity = data.data.float_value;
            break;
        }

        case meshtastic_MessageType_TVOC_INDEX: {

            // LOG_DEBUG("Tvoc: %.1f", value);

            measurement->variant.environment_metrics.has_iaq = true;
            measurement->variant.environment_metrics.iaq = data.data.float_value;
            break;
        }
        default:
            break;
        }

        return true;
    }
    return false;
}

size_t IndicatorSensor::stuff_buffer(meshtastic_SensorData message)
{

    return ringBuf.push(message) ? 1 : 0;
}

#endif