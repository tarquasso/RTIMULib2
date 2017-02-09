////////////////////////////////////////////////////////////////////////////
//
//  This file is part of RTIMULib
//
//  Copyright (c) 2014-2015, richards-tech, LLC
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of
//  this software and associated documentation files (the "Software"), to deal in
//  the Software without restriction, including without limitation the rights to use,
//  copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//  Software, and to permit persons to whom the Software is furnished to do so,
//  subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//  INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//  PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
//  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
//  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "RTPressureMS5837.h"

RTPressureMS5837::RTPressureMS5837(RTIMUSettings *settings) : RTPressure(settings)
{
    m_validReadings = false;
}

RTPressureMS5837::~RTPressureMS5837()
{
}

bool RTPressureMS5837::pressureInit()
{
    unsigned char cmd = MS5611_CMD_PROM + 2;
    unsigned char data[2];

    m_pressureAddr = m_settings->m_I2CPressureAddress;

    // get calibration data

    for (int i = 0; i < 6; i++) {
        if (!m_settings->HALRead(m_pressureAddr, cmd, 2, data, "Failed to read MS5611 calibration data"))
            return false;
        m_calData[i] = (((uint16_t)data[0]) << 8) | ((uint16_t)data[1]);
        // printf("Cal index: %d, data: %d\n", i, m_calData[i]);
        cmd += 2;
    }

    m_state = MS5837_STATE_IDLE;
    return true;
}

bool RTPressureMS5837::pressureRead(RTIMU_DATA& data)
{
    data.pressureValid = false;
    data.temperatureValid = false;
    data.temperature = 0;
    data.pressure = 0;

    if (m_state == MS5837_STATE_IDLE) {
        // start pressure conversion
        if (!m_settings->HALWrite(m_pressureAddr, MS5611_CMD_CONV_D1, 0, 0, "Failed to start MS5611 pressure conversion")) {
            return false;
        } else {
            m_state = MS5837_STATE_PRESSURE;
            m_timer = RTMath::currentUSecsSinceEpoch();
        }
    }

    pressureBackground();

    if (m_validReadings) {
        data.pressureValid = true;
        data.temperatureValid = true;
        data.temperature = m_temperature;
        data.pressure = m_pressure;
    }
    return true;
}


void RTPressureMS5837::pressureBackground()
{
    uint8_t data[3];

    switch (m_state) {
        case MS5837_STATE_IDLE:
        break;

        case MS5837_STATE_PRESSURE:
        if ((RTMath::currentUSecsSinceEpoch() - m_timer) < 10000)
            break;                                          // not time yet
        if (!m_settings->HALRead(m_pressureAddr, MS5837_CMD_ADC, 3, data, "Failed to read MS5837 pressure")) {
            break;
        }
        m_D1 = (((uint32_t)data[0]) << 16) | (((uint32_t)data[1]) << 8) | ((uint32_t)data[2]);

        // start temperature conversion

        if (!m_settings->HALWrite(m_pressureAddr, MS5837_CMD_CONV_D2, 0, 0, "Failed to start MS5837 temperature conversion")) {
            break;
        } else {
            m_state = MS5837_STATE_TEMPERATURE;
            m_timer = RTMath::currentUSecsSinceEpoch();
        }
        break;

        case MS5837_STATE_TEMPERATURE:
        if ((RTMath::currentUSecsSinceEpoch() - m_timer) < 10000)
            break;                                          // not time yet
        if (!m_settings->HALRead(m_pressureAddr, MS5837_CMD_ADC, 3, data, "Failed to read MS5837 temperature")) {
            break;
        }
        m_D2 = (((uint32_t)data[0]) << 16) | (((uint32_t)data[1]) << 8) | ((uint32_t)data[2]);

        //  call this function for testing only
        //  should give T = 2000 (20.00C) and pressure 39982 (3999.8hPa)

        setTestData();

        //  now calculate the real values

        int64_t deltaT = (int32_t)m_D2 - (((int32_t)m_calData[4]) << 8); //dT = D2 - C5 *2^8

        int32_t temperature = 2000 + ((deltaT * (int64_t)m_calData[5]) >> 23); // note - this still needs to be divided by 100

        int64_t offset = (((int64_t)m_calData[1]) << 16) + ((m_calData[3] * deltaT) >> 7);
        int64_t sens = (((int64_t)m_calData[0]) << 15) + ((m_calData[2] * deltaT) >> 8);
        //RTFLOAT p = (RTFLOAT)(((((int64_t)m_D1 * sens) >> 21) - offset) >> 13) / (RTFLOAT)10.0; //mbar

        //  do second order temperature compensation
        int64_t Ti;
        int64_t offseti, offset2;
        int64_t sensi, sens2;
        if (temperature < 2000) { //Low Temperature
            Ti = (3 * (deltaT * deltaT)) >> 33;
            offseti = 3 * ((temperature - 2000) * (temperature - 2000)) / 2;
            sensi = 5 * ((temperature - 2000) * (temperature - 2000)) / 8;
            if (temperature < -1500) {
                offseti += 7 * (temperature + 1500) * (temperature + 1500);
                sensi += 4 * ((temperature + 1500) * (temperature + 1500));
            }
        } else { //High Temperature
            Ti = (2 * (deltaT * deltaT)) >> 37;
            offseti = 1 * ((temperature - 2000) * (temperature - 2000)) / 16;
            sensi = 0;
        }
        offset2 = offset - offseti;
        sens2 = sens - sensi;

        RTFLOAT temp2 = (temperature - Ti)/(RTFLOAT)100.0; //celsius
        RTFLOAT p2 = (RTFLOAT)(((((int64_t)m_D1 * sens2) >> 21) - offset2) >> 13) / (RTFLOAT)10.0; //mbar

        m_temperature = temp2; //celsius
        m_pressure = p2; //mbar

        //printf("Temp: %f, pressure: %f\n", m_temperature, m_pressure);

        m_validReadings = true;
        m_state = MS5837_STATE_IDLE;
        break;
    }
}

void RTPressureMS5837::setTestData()
{
    m_calData[0] = 34982;
    m_calData[1] = 36352;
    m_calData[2] = 20328;
    m_calData[3] = 22354;
    m_calData[4] = 26646;
    m_calData[5] = 26146;

    m_D1 = 4958179;
    m_D2 = 6815414;
}
