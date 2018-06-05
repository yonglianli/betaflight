/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/crc.h"
#include "common/maths.h"
#include "common/streambuf.h"

#include "drivers/time.h"

#include "io/serial.h"

#include "rcdevice.h"

#include "fc/config.h"
#include "config/feature.h"

#ifdef USE_RCDEVICE

typedef struct runcamDeviceExpectedResponseLength_s {
    uint8_t command;
    uint8_t reponseLength;
} runcamDeviceExpectedResponseLength_t;

static runcamDeviceExpectedResponseLength_t expectedResponsesLength[] = {
    { RCDEVICE_PROTOCOL_COMMAND_GET_DEVICE_INFO,            5},
    { RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_PRESS,      2},
    { RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_RELEASE,    2},
    { RCDEVICE_PROTOCOL_COMMAND_5KEY_CONNECTION,            3},
};

rcdeviceWaitingResponseQueue watingResponseQueue;

static uint8_t runcamDeviceGetRespLen(uint8_t command)
{
    for (unsigned int i = 0; i < ARRAYLEN(expectedResponsesLength); i++) {
        if (expectedResponsesLength[i].command == command) {
            return expectedResponsesLength[i].reponseLength;
        }
    }

    return 0;
}

static bool rcdeviceRespCtxQueuePushRespCtx(rcdeviceWaitingResponseQueue *queue, rcdeviceResponseParseContext_t *respCtx)
{
    if (queue == NULL || (queue->itemCount + 1) > MAX_WAITING_RESPONSES || queue->tailPos >= MAX_WAITING_RESPONSES) {
        return false;
    }
    
    queue->buffer[queue->tailPos] = *respCtx;

    int newTailPos = queue->tailPos + 1;
    if (newTailPos >= MAX_WAITING_RESPONSES) {
        newTailPos = 0;
    }
    queue->itemCount += 1;
    queue->tailPos = newTailPos;
    
    return true;
}

static rcdeviceResponseParseContext_t* rcdeviceRespCtxQueuePeekFront(rcdeviceWaitingResponseQueue *queue)
{
    if (queue == NULL || queue->itemCount == 0 || queue->headPos >= MAX_WAITING_RESPONSES) {
        return NULL;
    }

    rcdeviceResponseParseContext_t *ctx = &queue->buffer[queue->headPos];
    return ctx;
}

static rcdeviceResponseParseContext_t* rcdeviceRespCtxQueueShift(rcdeviceWaitingResponseQueue *queue)
{
    if (queue == NULL || queue->itemCount == 0 || queue->headPos >= MAX_WAITING_RESPONSES) {
        return NULL;
    }
    
    rcdeviceResponseParseContext_t *ctx = &queue->buffer[queue->headPos];
    int newHeadPos = queue->headPos + 1;
    if (newHeadPos >= MAX_WAITING_RESPONSES) {
        newHeadPos = 0;
    }
    queue->itemCount -= 1;
    queue->headPos = newHeadPos;

    return ctx;
}

// every time send packet to device, and want to get something from device,
// it'd better call the method to clear the rx buffer before the packet send,
// else may be the useless data in rx buffer will cause the response decoding
// failed.
static void runcamDeviceFlushRxBuffer(runcamDevice_t *device)
{
    while (serialRxBytesWaiting(device->serialPort) > 0) {
        serialRead(device->serialPort);
    }
}

// a common way to send packet to device
static void runcamDeviceSendPacket(runcamDevice_t *device, uint8_t command, uint8_t *paramData, int paramDataLen)
{
    // is this device open?
    if (!device->serialPort) {
        return;
    }

    sbuf_t buf;
    // prepare pointer
    buf.ptr = device->buffer;
    buf.end = ARRAYEND(device->buffer);

    sbufWriteU8(&buf, RCDEVICE_PROTOCOL_HEADER);
    sbufWriteU8(&buf, command);

    if (paramData) {
        sbufWriteData(&buf, paramData, paramDataLen);
    }

    // add crc over (all) data
    crc8_dvb_s2_sbuf_append(&buf, device->buffer);

    // switch to reader
    sbufSwitchToReader(&buf, device->buffer);

    // send data if possible
    serialWriteBuf(device->serialPort, sbufPtr(&buf), sbufBytesRemaining(&buf));
}

// a common way to send a packet to device, and get response from the device.
static void runcamDeviceSendRequestAndWaitingResp(runcamDevice_t *device, uint8_t commandID, uint8_t *paramData, uint8_t paramDataLen, timeUs_t tiemout, int maxRetryTimes, void *userInfo, rcdeviceRespParseFunc parseFunc)
{
    runcamDeviceFlushRxBuffer(device);

    rcdeviceResponseParseContext_t responseCtx;
    memset(&responseCtx, 0, sizeof(rcdeviceResponseParseContext_t));
    responseCtx.command = commandID;
    responseCtx.maxRetryTimes = maxRetryTimes;
    responseCtx.expectedRespLen = runcamDeviceGetRespLen(commandID);
    responseCtx.timeout = tiemout;
    responseCtx.timeoutTimestamp = millis() + tiemout;
    responseCtx.parserFunc = parseFunc;
    responseCtx.device = device;
    responseCtx.protocolVer = RCDEVICE_PROTOCOL_VERSION_1_0;
    memcpy(responseCtx.paramData, paramData, paramDataLen);
    responseCtx.paramDataLen = paramDataLen;
    responseCtx.userInfo = userInfo;
    rcdeviceRespCtxQueuePushRespCtx(&watingResponseQueue, &responseCtx);

    // send packet
    runcamDeviceSendPacket(device, commandID, paramData, paramDataLen);
}

static uint8_t calcCRCFromData(uint8_t *ptr, uint8_t len)
{
    uint8_t i;
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *ptr++;
        for (i = 8; i > 0; --i) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

static void runcamDeviceParseV2DeviceInfo(rcdeviceResponseParseContext_t *ctx)
{
    if (ctx->result != RCDEVICE_RESP_SUCCESS) {
        ctx->device->isReady = false;
        return;
    }
    runcamDevice_t *device = ctx->device;
    device->info.protocolVersion = ctx->recvBuf[1];

    uint8_t featureLowBits = ctx->recvBuf[2];
    uint8_t featureHighBits = ctx->recvBuf[3];
    device->info.features = (featureHighBits << 8) | featureLowBits;
    device->isReady = true;
}

// get the device info(firmware version, protocol version and features, see the
// definition of runcamDeviceInfo_t to know more)
static void runcamDeviceGetDeviceInfo(runcamDevice_t *device)
{
    runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_GET_DEVICE_INFO, NULL, 0, 5000, 0, NULL, runcamDeviceParseV2DeviceInfo);
}

static void runcamDeviceSend5KeyOSDCableConnectionEvent(runcamDevice_t *device, uint8_t operation, rcdeviceRespParseFunc parseFunc)
{
    runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_5KEY_CONNECTION, &operation, sizeof(uint8_t), 200, 1, NULL, parseFunc);
}

// init the runcam device, it'll search the UART port with FUNCTION_RCDEVICE id
// this function will delay 400ms in the first loop to wait the device prepared,
// as we know, there are has some camera need about 200~400ms to initialization,
// and then we can send/receive from it.
void runcamDeviceInit(runcamDevice_t *device)
{
    device->isReady = false;
    serialPortFunction_e portID = FUNCTION_RCDEVICE;
    serialPortConfig_t *portConfig = findSerialPortConfig(portID);
    if (portConfig != NULL) {
        device->serialPort = openSerialPort(portConfig->identifier, portID, NULL, NULL, 115200, MODE_RXTX, SERIAL_NOT_INVERTED);

        if (device->serialPort != NULL) {
            // send RCDEVICE_PROTOCOL_COMMAND_GET_DEVICE_INFO to device to retrive
            // device info, e.g protocol version, supported features
            runcamDeviceGetDeviceInfo(device);
        }
    }
}

bool runcamDeviceSimulateCameraButton(runcamDevice_t *device, uint8_t operation)
{
    if (device->info.protocolVersion == RCDEVICE_PROTOCOL_VERSION_1_0) {
        runcamDeviceSendPacket(device, RCDEVICE_PROTOCOL_COMMAND_CAMERA_CONTROL, &operation, sizeof(operation));
    } else {
        return false;
    }

    return true;
}

// every time start to control the OSD menu of camera, must call this method to
// camera
void runcamDeviceOpen5KeyOSDCableConnection(runcamDevice_t *device, rcdeviceRespParseFunc parseFunc)
{
    runcamDeviceSend5KeyOSDCableConnectionEvent(device, RCDEVICE_PROTOCOL_5KEY_CONNECTION_OPEN, parseFunc);
}

// when the control was stop, must call this method to the camera to disconnect
// with camera.
void runcamDeviceClose5KeyOSDCableConnection(runcamDevice_t *device, rcdeviceRespParseFunc parseFunc)
{
    runcamDeviceSend5KeyOSDCableConnectionEvent(device, RCDEVICE_PROTOCOL_5KEY_CONNECTION_CLOSE, parseFunc);
}

// simulate button press event of 5 key osd cable with special button
void runcamDeviceSimulate5KeyOSDCableButtonPress(runcamDevice_t *device, uint8_t operation, rcdeviceRespParseFunc parseFunc)
{
    if (operation == RCDEVICE_PROTOCOL_5KEY_SIMULATION_NONE) {
        return;
    }

    runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_PRESS, &operation, sizeof(uint8_t), 200, 1, NULL, parseFunc);
}

// simulate button release event of 5 key osd cable
void runcamDeviceSimulate5KeyOSDCableButtonRelease(runcamDevice_t *device, rcdeviceRespParseFunc parseFunc)
{
    runcamDeviceSendRequestAndWaitingResp(device, RCDEVICE_PROTOCOL_COMMAND_5KEY_SIMULATION_RELEASE, NULL, 0, 200, 1, NULL, parseFunc);
}

static rcdeviceResponseParseContext_t* getWaitingResponse(timeUs_t currentTimeUs)
{
    rcdeviceResponseParseContext_t *respCtx = rcdeviceRespCtxQueuePeekFront(&watingResponseQueue);
    while (respCtx != NULL && respCtx->timeoutTimestamp != 0 && currentTimeUs > respCtx->timeoutTimestamp) {
        if (respCtx->timeoutTimestamp != 0 && currentTimeUs > respCtx->timeoutTimestamp) {
            if (respCtx->maxRetryTimes > 0) {
                runcamDeviceSendPacket(respCtx->device, respCtx->command, respCtx->paramData, respCtx->paramDataLen);
                respCtx->timeoutTimestamp = currentTimeUs + respCtx->timeout;
                respCtx->maxRetryTimes -= 1;
                respCtx = NULL;
                break;
            } else {
                respCtx->result = RCDEVICE_RESP_TIMEOUT;
                if (respCtx->parserFunc != NULL) {
                    respCtx->parserFunc(respCtx);
                }

                // dequeue and get next waiting response context
                rcdeviceRespCtxQueueShift(&watingResponseQueue);
                respCtx = rcdeviceRespCtxQueuePeekFront(&watingResponseQueue);
            }
        }
    }

    return respCtx;
}

void rcdeviceReceive(timeUs_t currentTimeUs) 
{
    UNUSED(currentTimeUs);
    rcdeviceResponseParseContext_t *respCtx = NULL;
    while ((respCtx = getWaitingResponse(millis())) != NULL && serialRxBytesWaiting(respCtx->device->serialPort)) {
        const uint8_t c = serialRead(respCtx->device->serialPort);
        respCtx->recvBuf[respCtx->recvRespLen] = c;
        respCtx->recvRespLen += 1;
        
        // if data received done, trigger callback to parse response data, and update rcdevice state
        if (respCtx->recvRespLen == respCtx->expectedRespLen) {
            // verify the crc value
            if (respCtx->protocolVer == RCDEVICE_PROTOCOL_RCSPLIT_VERSION) {
                uint8_t crcFromPacket = respCtx->recvBuf[3];
                respCtx->recvBuf[3] = respCtx->recvBuf[4]; // move packet tail field to crc field, and calc crc with first 4 bytes
                uint8_t crc = calcCRCFromData(respCtx->recvBuf, 4);
                respCtx->result = (crc == crcFromPacket) ? RCDEVICE_RESP_SUCCESS : RCDEVICE_RESP_INCORRECT_CRC;
                
            } else if (respCtx->protocolVer == RCDEVICE_PROTOCOL_VERSION_1_0) {
                uint8_t crc = 0;
                for (int i = 0; i < respCtx->recvRespLen; i++) {
                    crc = crc8_dvb_s2(crc, respCtx->recvBuf[i]);
                }
                respCtx->result = (crc == 0) ? RCDEVICE_RESP_SUCCESS : RCDEVICE_RESP_INCORRECT_CRC;
            }

            if (respCtx->parserFunc != NULL) {
                respCtx->parserFunc(respCtx);
            }

            // dequeue current response context
            rcdeviceRespCtxQueueShift(&watingResponseQueue);
        }
    }
}

#endif
