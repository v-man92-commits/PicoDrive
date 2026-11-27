#include "common.h"
#include "libtwl/dma/dmaNitro.h"
#include "tusb.h"
#include "device/dcd.h"
#include "DSPicoUsb.h"
#include "DSPicoUsbOutEndpoint.h"

// Diagnostics: OUT completions reported by the DSpico while no transfer was
// armed on the DS side. Previously these were passed to TinyUSB as if data
// had arrived (without copying anything), which made it consume whatever was
// still in its buffer - e.g. writing the command block itself to disk.
volatile u32 gDSPicoUsbSpuriousOutCompletions = 0;

void DSPicoUsbOutEndpoint::BeginTransfer(u8* buffer, u32 length)
{
    rtos_lockMutex(&_mutex);
    {
        _buffer = buffer;
        _bufferLength = length;
        _bufferOffset = 0;
        _totalReceived = 0;
        _currentDSPicoBuffer = 0;
        _transferActive = true;
        u32 firstReceiveLength = _bufferLength;
        if (firstReceiveLength > 512)
        {
            firstReceiveLength = 512;
        }
        rtos_lockMutex(&gCardMutex);
        {
            card_romSetCmd(DSPICO_CMD_USB_COMMAND_BEGIN_TRANSFER(_endpointAddress, _currentDSPicoBuffer, firstReceiveLength));
            card_romStartXfer(DSPICO_USB_DEFAULT_COMMAND_SETTINGS, false);
            card_romWaitBusy();
        }
        rtos_unlockMutex(&gCardMutex);
    }
    rtos_unlockMutex(&_mutex);
}

void DSPicoUsbOutEndpoint::ProcessTransferCompleteEvent(u32 transferredBytes)
{
    rtos_lockMutex(&_mutex);
    {
        if (_transferActive)
        {
            _totalReceived += transferredBytes;
            int length = _bufferLength - _bufferOffset - 512;
            if (length > 512)
            {
                length = 512;
            }
            if (transferredBytes == 512)
            {
                if (length > 0)
                {
                    rtos_lockMutex(&gCardMutex);
                    {
                        card_romSetCmd(DSPICO_CMD_USB_COMMAND_BEGIN_TRANSFER(_endpointAddress, 1 - _currentDSPicoBuffer, length));
                        card_romStartXfer(DSPICO_USB_DEFAULT_COMMAND_SETTINGS, false);
                        card_romWaitBusy();
                    }
                    rtos_unlockMutex(&gCardMutex);
                }
            }

            ReceiveUsbBlock();

            if (transferredBytes < 512 || length == 0)
            {
                dcd_event_xfer_complete(0, _endpointAddress, _bufferOffset, XFER_RESULT_SUCCESS, true);
                _transferActive = false;
            }
        }
        else if (_endpointAddress == 0)
        {
            // Control endpoint: keep the original behaviour (status stages)
            dcd_event_xfer_complete(0, _endpointAddress, transferredBytes, XFER_RESULT_SUCCESS, true);
        }
        else
        {
            // Data endpoint with no transfer armed: never report a completion
            // for data we didn't copy. Count it; the host will time out and
            // retry the command instead of us writing stale buffer contents.
            gDSPicoUsbSpuriousOutCompletions = gDSPicoUsbSpuriousOutCompletions + 1;
        }
    }
    rtos_unlockMutex(&_mutex);
}

void DSPicoUsbOutEndpoint::ReceiveUsbBlock()
{
    u32 length = _totalReceived - _bufferOffset;
    if (length > 0)
    {
        if (length > 512)
        {
            length = 512;
        }
        rtos_lockMutex(&gCardMutex);
        {
            // 512-byte blocks are read by the CPU (not DMA) so the copy is
            // guaranteed to have completed before the transfer is reported.
            card_romSetCmd(DSPICO_CMD_USB_READ_DATA(_currentDSPicoBuffer, _endpointAddress));
            card_romStartXfer(MCCNT1_DIR_READ | MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_LEN_512 | MCCNT1_CMD_SCRAMBLE |
                MCCNT1_LATENCY2(4) | MCCNT1_CLOCK_SCRAMBLER | MCCNT1_LATENCY1(0), false);
            if (length & 3)
            {
                u8* target = _buffer + _bufferOffset + length;
                u8* dst = _buffer + _bufferOffset;
                do
                {
                    // Read data if available
                    if (card_romIsDataReady())
                    {
                        u32 data = card_romGetData();
                        if (dst + 3 < target)
                        {
                            *(u32*)dst = data;
                            dst += 4;
                        }
                        else if (dst < target)
                        {
                            *dst++ = data & 0xFF;
                            if (dst < target)
                            {
                                *dst++ = (data >> 8) & 0xFF;
                                if (dst < target)
                                {
                                    *dst++ = (data >> 16) & 0xFF;
                                }
                            }
                        }
                    }
                } while (card_romIsBusy());
            }
            else
            {
                card_romCpuRead((u32*)(_buffer + _bufferOffset), length >> 2);
            }
        }
        rtos_unlockMutex(&gCardMutex);
        _bufferOffset += length;
        _currentDSPicoBuffer = 1 - _currentDSPicoBuffer;
    }
}
