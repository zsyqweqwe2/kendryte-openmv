/*
 * This file is part of the OpenMV project.
 * Copyright (c) 2013-2018 Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * Lepton driver.
 *
 */

#include STM32_HAL_H
#include "mp.h"
#include "irq.h"
#include "cambus.h"
#include "sensor.h"
#include "systick.h"
#include "framebuffer.h"
#include "omv_boardconfig.h"
#include "common.h"

#if defined(OMV_ENABLE_LEPTON)
#include "crc16.h"
#include "LEPTON_SDK.h"
#include "LEPTON_AGC.h"
#include "LEPTON_SYS.h"
#include "LEPTON_VID.h"
#include "LEPTON_OEM.h"
#include "LEPTON_RAD.h"
#include "LEPTON_I2C_Reg.h"

#define VOSPI_LINE_PIXELS       (80)
#define VOSPI_NUMBER_PACKETS    (60)
#define VOSPI_SPECIAL_PACKET    (20)
#define VOSPI_LINE_SIZE         (80 * 2)
#define VOSPI_HEADER_SIZE       (4)
#define VOSPI_PACKET_SIZE       (VOSPI_HEADER_SIZE + VOSPI_LINE_SIZE)
#define VOSPI_HEADER_SEG(buf)   (((buf[0] >> 4) & 0x7))
#define VOSPI_HEADER_PID(buf)   (((buf[0] << 8) | (buf[1] << 0)) & 0x0FFF)
#define VOSPI_HEADER_CRC(buf)   (((buf[2] << 8) | (buf[3] << 0)))
#define VOSPI_FIRST_PACKET      (0)
#define VOSPI_FIRST_SEGMENT     (1)
#define LEPTON_TIMEOUT          (1000)

static int h_res = 0;
static int v_res = 0;
static bool h_mirror = false;
static bool v_flip = false;

static SPI_HandleTypeDef SPIHandle;
static DMA_HandleTypeDef DMAHandle;

extern uint8_t _line_buf;
extern uint8_t _vospi_buf;
extern const uint16_t rainbow_table[256];

static bool vospi_resync = true;
static uint8_t *vospi_packet = &_line_buf;
static uint8_t *vospi_buffer = &_vospi_buf;
static volatile uint32_t vospi_pid = 0;
static volatile uint32_t vospi_seg = 1;
static uint32_t vospi_packets = 60;

void LEPTON_SPI_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&SPIHandle);
}

void LEPTON_SPI_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(SPIHandle.hdmarx);
}

static void lepton_sync()
{
    HAL_SPI_Abort(&SPIHandle);

    // Disable DMA IRQ
    HAL_NVIC_DisableIRQ(LEPTON_SPI_DMA_IRQn);

    debug_printf("resync...\n");
    systick_sleep(200);

    vospi_resync = false;
    vospi_pid = VOSPI_FIRST_PACKET;
    vospi_seg = VOSPI_FIRST_SEGMENT;

    HAL_NVIC_EnableIRQ(LEPTON_SPI_DMA_IRQn);
    HAL_SPI_Receive_DMA(&SPIHandle, vospi_packet, VOSPI_PACKET_SIZE);
}

static uint16_t lepton_calc_crc(uint8_t *buf)
{
    buf[0] &= 0x0F;
    buf[1] &= 0xFF;
    buf[2] = 0;
    buf[3] = 0;
    return CalcCRC16Bytes(VOSPI_LINE_SIZE, (char *) buf);
}

static int sleep(sensor_t *sensor, int enable)
{
    if (enable) {
        DCMI_PWDN_LOW();
        systick_sleep(100);
    } else {
        DCMI_PWDN_HIGH();
        systick_sleep(100);
    }

    return 0;
}

static int read_reg(sensor_t *sensor, uint8_t reg_addr)
{
    uint16_t reg_data;
    if (cambus_readw2(sensor->slv_addr, reg_addr, &reg_data)) {
        return -1;
    }
    return reg_data;
}

static int write_reg(sensor_t *sensor, uint8_t reg_addr, uint16_t reg_data)
{
    return cambus_writew2(sensor->slv_addr, reg_addr, reg_data);
}

static int set_pixformat(sensor_t *sensor, pixformat_t pixformat)
{
    return 0;
}

static int set_framesize(sensor_t *sensor, framesize_t framesize)
{
    return 0;
}

static int set_framerate(sensor_t *sensor, framerate_t framerate)
{
    return 0;
}

static int set_contrast(sensor_t *sensor, int level)
{
    return 0;
}

static int set_brightness(sensor_t *sensor, int level)
{
    return 0;
}

static int set_saturation(sensor_t *sensor, int level)
{
    return 0;
}

static int set_gainceiling(sensor_t *sensor, gainceiling_t gainceiling)
{
    return 0;
}

static int set_quality(sensor_t *sensor, int quality)
{
    return 0;
}

static int set_colorbar(sensor_t *sensor, int enable)
{
    return 0;
}

static int set_special_effect(sensor_t *sensor, sde_t sde)
{
    return 0;
}

static int set_auto_gain(sensor_t *sensor, int enable, float gain_db, float gain_db_ceiling)
{
    return 0;
}

static int get_gain_db(sensor_t *sensor, float *gain_db)
{
    return 0;
}

static int set_auto_exposure(sensor_t *sensor, int enable, int exposure_us)
{
    return 0;
}

static int get_exposure_us(sensor_t *sensor, int *exposure_us)
{
    return 0;
}

static int set_auto_whitebal(sensor_t *sensor, int enable, float r_gain_db, float g_gain_db, float b_gain_db)
{
    return 0;
}

static int get_rgb_gain_db(sensor_t *sensor, float *r_gain_db, float *g_gain_db, float *b_gain_db)
{
    return 0;
}

static int set_hmirror(sensor_t *sensor, int enable)
{
    h_mirror = enable;
    return 0;
}

static int set_vflip(sensor_t *sensor, int enable)
{
    v_flip = enable;
    return 0;
}

static int set_lens_correction(sensor_t *sensor, int enable, int radi, int coef)
{
    return 0;
}

static int reset(sensor_t *sensor)
{
    DCMI_PWDN_LOW();
    systick_sleep(10);

    DCMI_PWDN_HIGH();
    systick_sleep(10);

    DCMI_RESET_LOW();
    systick_sleep(10);

    DCMI_RESET_HIGH();
    systick_sleep(1000);

    LEP_AGC_ROI_T roi;
    LEP_CAMERA_PORT_DESC_T handle = {0};
    h_res = v_res = h_mirror = v_flip = 0;

    for (uint32_t start = HAL_GetTick(); ;systick_sleep(1)) {
        if (LEP_OpenPort(0, LEP_CCI_TWI, 0, &handle) == LEP_OK) {
            break;
        }
        if (HAL_GetTick() - start >= LEPTON_TIMEOUT) {
            return -1;
        }
    }

    for (uint32_t start = HAL_GetTick(); ;systick_sleep(1)) {
        LEP_SDK_BOOT_STATUS_E status;
        if (LEP_GetCameraBootStatus(&handle, &status) != LEP_OK) {
            return -1;
        }
        if (status == LEP_BOOT_STATUS_BOOTED) {
            break;
        }
        if (HAL_GetTick() - start >= LEPTON_TIMEOUT) {
            return -1;
        }
    }

    for (uint32_t start = HAL_GetTick(); ;systick_sleep(1)) {
        LEP_UINT16 status;
        if (LEP_DirectReadRegister(&handle, LEP_I2C_STATUS_REG, &status) != LEP_OK) {
            return -1;
        }
        if (!(status & LEP_I2C_STATUS_BUSY_BIT_MASK)) {
            break;
        }
        if (HAL_GetTick() - start >= LEPTON_TIMEOUT) {
            return -1;
        }
    }

    for (uint32_t start = HAL_GetTick(); ;systick_sleep(1)) {
        LEP_SYS_STATUS_E status;
        if (LEP_GetSysFFCStatus(&handle, &status) != LEP_OK) {
            return -1;
        }
        if (status == LEP_SYS_STATUS_READY) {
            break;
        }
        if (HAL_GetTick() - start >= (LEPTON_TIMEOUT * 5)) {
            return -1;
        }
    }

    if (LEP_SetRadEnableState(&handle, LEP_RAD_DISABLE) != LEP_OK
        || LEP_GetAgcROI(&handle, &roi) != LEP_OK
        || LEP_SetAgcEnableState(&handle, LEP_AGC_ENABLE) != LEP_OK
        || LEP_SetAgcCalcEnableState(&handle, LEP_AGC_ENABLE) != LEP_OK) {
        return -1;
    }

    h_res = roi.endCol + 1;
    v_res = roi.endRow + 1;

    if (v_res > 60) {
        vospi_packets = 240;
    } else {
        vospi_packets = 60;
    }

    // resync and enable DMA before the first snapshot.
    vospi_resync = true;
    return 0;
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    (void) lepton_calc_crc; // to shut the compiler up.

    if (vospi_resync == true) {
        return; // nothing to do here
    }

    if (vospi_pid < vospi_packets && (vospi_packet[0] & 0xF) != 0xF) {
        uint32_t pid = VOSPI_HEADER_PID(vospi_packet);
        uint32_t seg = VOSPI_HEADER_SEG(vospi_packet);
        if (pid != (vospi_pid % VOSPI_NUMBER_PACKETS)) {
            if (vospi_pid == VOSPI_FIRST_PACKET) {
                // Wait for the first packet of the first segement.
                vospi_pid = VOSPI_FIRST_PACKET;
                vospi_seg = VOSPI_FIRST_SEGMENT;
            } else { // lost sync
                vospi_resync = true;
                debug_printf("lost sync, packet id:%lu expected id:%lu \n", pid, vospi_pid);
            }
        } else if (vospi_packets > 60 && pid == VOSPI_SPECIAL_PACKET && seg != vospi_seg ) {
            if (vospi_seg == VOSPI_FIRST_SEGMENT) {
                // Wait for the first packet of the first segement.
                vospi_pid = VOSPI_FIRST_PACKET;
                vospi_seg = VOSPI_FIRST_SEGMENT;
            } else { // lost sync
                vospi_resync = true;
                debug_printf("lost sync, segment id:%lu expected id:%lu\n", seg, vospi_seg);
            }
        } else {
            memcpy(vospi_buffer + vospi_pid * VOSPI_LINE_SIZE,
                    vospi_packet + VOSPI_HEADER_SIZE, VOSPI_LINE_SIZE);
            if ((++vospi_pid % VOSPI_NUMBER_PACKETS) == 0) {
                vospi_seg++;
            }
        }
    }

}

static int snapshot(sensor_t *sensor, image_t *image, streaming_cb_t cb)
{
    if ((!h_res) || (!v_res) || (!sensor->framesize) || (!sensor->pixformat)) {
        return -1;
    }

    fb_update_jpeg_buffer();

    vospi_pid = VOSPI_FIRST_PACKET;
    vospi_seg = VOSPI_FIRST_SEGMENT;
    do {
        if (vospi_resync == true) {
            lepton_sync();
        }
        __WFI();
    } while (vospi_pid < vospi_packets);

    image->w    = MAIN_FB()->w;
    image->h    = MAIN_FB()->h;
    image->bpp  = MAIN_FB()->bpp; // invalid
    image->data = MAIN_FB()->pixels; // valid

    uint16_t *src = (uint16_t*) vospi_buffer;

    for (int y=0; y<v_res; y++) {
        for (int x=0; x<h_res; x++) {
            // Value is the 14-bit value from the FLIR IR camera.
            // However, with AGC enabled only the bottom 8-bits are non-zero.
            uint8_t val = src[y*h_res+x]>>8;
            switch (sensor->pixformat) {
                case PIXFORMAT_RGB565: {
                    IMAGE_PUT_RGB565_PIXEL(image, x, y, rainbow_table[val]);
                    break;
                }
                case PIXFORMAT_GRAYSCALE: {
                    IMAGE_PUT_GRAYSCALE_PIXEL(image, x, y, val);
                    break;
                }
                default: {
                    break;
                }
            }
        }
    }

    switch (sensor->pixformat) {
        case PIXFORMAT_GRAYSCALE: {
            MAIN_FB()->bpp = 1;
            break;
        }
        case PIXFORMAT_RGB565: {
            MAIN_FB()->bpp = 2;
            break;
        }
        default: {
            break;
        }
    }

    image->bpp  = MAIN_FB()->bpp;
    return 0;
}

int lepton_init(sensor_t *sensor)
{
    sensor->gs_bpp              = sizeof(uint8_t);
    sensor->reset               = reset;
    sensor->sleep               = sleep;
    sensor->snapshot            = snapshot;
    sensor->read_reg            = read_reg;
    sensor->write_reg           = write_reg;
    sensor->set_pixformat       = set_pixformat;
    sensor->set_framesize       = set_framesize;
    sensor->set_framerate       = set_framerate;
    sensor->set_contrast        = set_contrast;
    sensor->set_brightness      = set_brightness;
    sensor->set_saturation      = set_saturation;
    sensor->set_gainceiling     = set_gainceiling;
    sensor->set_quality         = set_quality;
    sensor->set_colorbar        = set_colorbar;
    sensor->set_special_effect  = set_special_effect;
    sensor->set_auto_gain       = set_auto_gain;
    sensor->get_gain_db         = get_gain_db;
    sensor->set_auto_exposure   = set_auto_exposure;
    sensor->get_exposure_us     = get_exposure_us;
    sensor->set_auto_whitebal   = set_auto_whitebal;
    sensor->get_rgb_gain_db     = get_rgb_gain_db;
    sensor->set_hmirror         = set_hmirror;
    sensor->set_vflip           = set_vflip;
    sensor->set_lens_correction = set_lens_correction;

    SENSOR_HW_FLAGS_SET(sensor, SENSOR_HW_FLAGS_VSYNC, 1);
    SENSOR_HW_FLAGS_SET(sensor, SENSOR_HW_FLAGS_HSYNC, 0);
    SENSOR_HW_FLAGS_SET(sensor, SENSOR_HW_FLAGS_PIXCK, 0);
    SENSOR_HW_FLAGS_SET(sensor, SENSOR_HW_FLAGS_FSYNC, 0);
    SENSOR_HW_FLAGS_SET(sensor, SENSOR_HW_FLAGS_JPEGE, 0);

    // Configure the DMA handler for Transmission process
    DMAHandle.Instance                 = LEPTON_SPI_DMA_STREAM;
    DMAHandle.Init.Request             = LEPTON_SPI_DMA_REQUEST;
    DMAHandle.Init.Mode                = DMA_CIRCULAR;
    DMAHandle.Init.Priority            = DMA_PRIORITY_HIGH;
    DMAHandle.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    // When the DMA is configured in direct mode (the FIFO is disabled), the source and
    // destination transfer widths are equal, and both defined by PSIZE (MSIZE is ignored).
    // Additionally, burst transfers are not possible (MBURST and PBURST are both ignored).
    DMAHandle.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    DMAHandle.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    // Note MBURST and PBURST are ignored.
    DMAHandle.Init.MemBurst            = DMA_MBURST_INC4;
    DMAHandle.Init.PeriphBurst         = DMA_PBURST_INC4;
    DMAHandle.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    DMAHandle.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    DMAHandle.Init.MemInc              = DMA_MINC_ENABLE;
    DMAHandle.Init.PeriphInc           = DMA_PINC_DISABLE;

    // NVIC configuration for DMA transfer complete interrupt
    NVIC_SetPriority(LEPTON_SPI_DMA_IRQn, IRQ_PRI_DMA21);
    HAL_NVIC_DisableIRQ(LEPTON_SPI_DMA_IRQn);

    HAL_DMA_DeInit(&DMAHandle);
    if (HAL_DMA_Init(&DMAHandle) != HAL_OK) {
        // Initialization Error
        return -1;
    }

    memset(&SPIHandle, 0, sizeof(SPIHandle));
    SPIHandle.Instance               = LEPTON_SPI;
    SPIHandle.Init.NSS               = SPI_NSS_HARD_OUTPUT;
    SPIHandle.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
    SPIHandle.Init.NSSPolarity       = SPI_NSS_POLARITY_LOW;
    SPIHandle.Init.Mode              = SPI_MODE_MASTER;
    SPIHandle.Init.TIMode            = SPI_TIMODE_DISABLE;
    SPIHandle.Init.Direction         = SPI_DIRECTION_2LINES_RXONLY;
    SPIHandle.Init.DataSize          = SPI_DATASIZE_8BIT;
    SPIHandle.Init.FifoThreshold     = SPI_FIFO_THRESHOLD_04DATA;
    SPIHandle.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    SPIHandle.Init.CLKPhase          = SPI_PHASE_2EDGE;
    SPIHandle.Init.CLKPolarity       = SPI_POLARITY_HIGH;
    SPIHandle.Init.BaudRatePrescaler = LEPTON_SPI_PRESCALER;
    // Recommanded setting to avoid glitches
    SPIHandle.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_SPI_Init(&SPIHandle) != HAL_OK) {
        LEPTON_SPI_RESET();
        LEPTON_SPI_RELEASE();
        LEPTON_SPI_CLK_DISABLE();
        return -1;
    }

    // Associate the initialized DMA handle to the the SPI handle
    __HAL_LINKDMA(&SPIHandle, hdmarx, DMAHandle);

    // NVIC configuration for SPI transfer complete interrupt
    NVIC_SetPriority(LEPTON_SPI_IRQn, IRQ_PRI_DCMI);
    HAL_NVIC_EnableIRQ(LEPTON_SPI_IRQn);

    return 0;
}
#else
int lepton_init(sensor_t *sensor)
{
    return -1;
}
#endif //defined(OMV_ENABLE_LEPTON)
