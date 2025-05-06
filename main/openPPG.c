#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "driver/dac_oneshot.h"

#define I2C_MAIN_SDA_IO 21      // SDA pin
#define I2C_MAIN_SCL_IO 22      // SCL pin
#define I2C_MAIN_NUM I2C_NUM_0  // Use I2C port 0
#define MAX30102_PORT 0x57

void app_main(void) {

    dac_oneshot_handle_t dac_handle;

    dac_oneshot_config_t dac_cfg = {
            .chan_id = DAC_CHAN_0,  // GPIO25
    };

    ESP_ERROR_CHECK(dac_oneshot_new_channel(&dac_cfg, &dac_handle));

    // Initializing the main and device bus handles with the esp-idf i2c library
    i2c_master_bus_config_t i2c_main_config = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = I2C_MAIN_NUM,
            .scl_io_num = I2C_MAIN_SCL_IO,
            .sda_io_num = I2C_MAIN_SDA_IO,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true
    };

    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_main_config, &bus_handle));

    i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MAX30102_PORT,
            .scl_speed_hz = 100000,
    };

    i2c_master_dev_handle_t dev_handle;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));

    // Initialize the MAX30102: Note that the MAX30102 expects writes in {[register address], [data]}
    // Set the reset bit first then wait
    // TODO: define aliases for all of the below hex values
    uint8_t modeConfig = 0x40;
    i2c_master_transmit(dev_handle, (uint8_t[]) {0x09, modeConfig}, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Set the HR mode
    uint8_t hrMode = 0x02;
    i2c_master_transmit(dev_handle, (uint8_t[]) {0x09, hrMode}, 2, -1);

    // Set the sampling info: 4 samples in the avg, fifo rolls over, fifo issues interrupt on 15 samples remaining (got these numbers from chatGPT will update as needed)
    uint8_t sampleConfig = 0x5F;
    i2c_master_transmit(dev_handle, (uint8_t[]) {0x08, sampleConfig}, 2, -1);

    // Set the power levels for the adc (it says it's for SpO2, but it seems like it sets the HR too, look into this)
    uint8_t adcRange = 0x40 | 0x08 | 0x03;
    i2c_master_transmit(dev_handle, (uint8_t[]) {0x0A, adcRange}, 2, -1);

    // Setting pulse amplitudes
    uint8_t redAmp = 0x7F;
    uint8_t irAmp = 0x24;
    i2c_master_transmit(dev_handle, (uint8_t[]) {0x0C, redAmp}, 2, -1);
    i2c_master_transmit(dev_handle, (uint8_t[]) {0x0D, irAmp}, 2, -1);

    // Let's read the data now! Implementing the pseudo code in the datasheet pg 16: https://www.analog.com/media/en/technical-documentation/data-sheets/max30102.pdf


    while (1) {
        // Read the address of the write and read pointers from the MAX30102
        uint8_t writeGetFifoWrite[1] = {0x04};
        uint8_t fifoWritePointer = 0;

        uint8_t writeGetFifoRead[1] = {0x06};
        uint8_t fifoReadPointer = 0;

        // Get the fifo_rd_ptr
        i2c_master_transmit_receive(dev_handle, writeGetFifoWrite, sizeof(writeGetFifoWrite), &fifoWritePointer,
                                    sizeof(fifoWritePointer), -1);
        i2c_master_transmit_receive(dev_handle, writeGetFifoRead, sizeof(writeGetFifoRead), &fifoReadPointer,
                                    sizeof(fifoReadPointer), -1);

        int numAvailableSamples = (fifoWritePointer - fifoReadPointer) & 0x1F;

        if (numAvailableSamples == 0 || numAvailableSamples > 32) continue;

        uint8_t sizeOfSample = 6;   // Make this a global variable in the define up top

        // read the data, yay
        uint8_t samples[numAvailableSamples * sizeOfSample];
        uint8_t fifoDataRegister[1] = {0x07};
        i2c_master_transmit_receive(dev_handle, fifoDataRegister, sizeof(fifoDataRegister), samples, sizeof(samples), -1);

        // Do some bit manipulation to get the red led data
        for (int i = 0; i < numAvailableSamples; i++) {
            uint32_t redLed = ((samples[i * sizeOfSample] << 16) | (samples[i * sizeOfSample + 1] << 8) | (samples[i * sizeOfSample + 2])) & 0x3FFFF;
            
            // Ugly dac code but excited to see it run
            // Clamp if needed
            if (redLed > 262143) redLed = 262143;

            // Normalize to 0–255
            uint8_t dacValue = (uint8_t)(((redLed - 222000) * 255UL) / 2000UL);
            printf("Here is the red LED Data: %d \n", dacValue);

            dac_oneshot_output_voltage(dac_handle, dacValue);
        }
    }
}
