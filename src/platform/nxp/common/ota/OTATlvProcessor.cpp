/*
 *
 *    Copyright (c) 2023, 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <lib/core/TLV.h>
#include <lib/support/BufferReader.h>
#include <lib/support/BytesToHex.h>
#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <platform/nxp/common/ota/OTAImageProcessorImpl.h>
#include <platform/nxp/common/ota/OTATlvProcessor.h>
#if OTA_ENCRYPTION_ENABLE
#include "mbedtls/aes.h"
#endif
namespace chip {

#if OTA_ENCRYPTION_ENABLE
constexpr uint8_t au8Iv[] = { 0x00, 0x00, 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x00, 0x00, 0x00, 0x00 };
#endif

CHIP_ERROR OTATlvProcessor::ApplyAction()
{
    return mApplyState == ApplyState::kApply ? CHIP_NO_ERROR : CHIP_ERROR_OTA_PROCESSOR_DO_NOT_APPLY;
}

CHIP_ERROR OTATlvProcessor::Process(ByteSpan & block)
{
    CHIP_ERROR status     = CHIP_NO_ERROR;
    uint32_t bytes        = std::min(mLength - mProcessedLength, static_cast<uint32_t>(block.size()));
    ByteSpan relevantData = block.SubSpan(0, bytes);

    status = ProcessInternal(relevantData);
    if (!IsError(status))
    {
        mProcessedLength += bytes;
        block = block.SubSpan(bytes);
        if (mProcessedLength == mLength)
        {
            status = ExitAction();
            if (!IsError(status))
            {
                // If current block was processed fully and the block still contains data, it
                // means that the block contains another TLV's data and the current processor
                // should be changed by OTAImageProcessorImpl.
                return CHIP_ERROR_OTA_CHANGE_PROCESSOR;
            }
        }
    }

    return status;
}

void OTATlvProcessor::ClearInternal()
{
    mLength          = 0;
    mProcessedLength = 0;
    mWasSelected     = false;
    mApplyState      = ApplyState::kApply;
#if OTA_ENCRYPTION_ENABLE
    mIVOffset = 0;
#endif
}

bool OTATlvProcessor::IsError(CHIP_ERROR & status)
{
    return status != CHIP_NO_ERROR && status != CHIP_ERROR_BUFFER_TOO_SMALL && status != CHIP_ERROR_OTA_FETCH_ALREADY_SCHEDULED;
}

void OTADataAccumulator::Init(uint32_t threshold)
{
    mThreshold    = threshold;
    mBufferOffset = 0;
    mBuffer.Alloc(mThreshold);
}

void OTADataAccumulator::Clear()
{
    mThreshold    = 0;
    mBufferOffset = 0;
    mBuffer.Free();
}

CHIP_ERROR OTADataAccumulator::Accumulate(ByteSpan & block)
{
    uint32_t numBytes = std::min(mThreshold - mBufferOffset, static_cast<uint32_t>(block.size()));
    memcpy(&mBuffer[mBufferOffset], block.data(), numBytes);
    mBufferOffset += numBytes;
    block = block.SubSpan(numBytes);

    if (mBufferOffset < mThreshold)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }

    return CHIP_NO_ERROR;
}

#if OTA_ENCRYPTION_ENABLE
CHIP_ERROR OTATlvProcessor::vOtaProcessInternalEncryption(MutableByteSpan & block)
{
    /*
     * This method decrypts an encrypted OTA block with AES CTR mode
     */

    uint8_t iv[16];
    uint8_t key[kOTAEncryptionKeyLength];
    uint8_t keystream[16] = { 0 };
    uint32_t u32IVCount;
    uint32_t Offset = 0;
    uint8_t data;
    mbedtls_aes_context aesCtx;

    // Init the AES context
    mbedtls_aes_init(&aesCtx);

    memcpy(iv, au8Iv, sizeof(au8Iv));

    u32IVCount = (((uint32_t) iv[12]) << 24) | (((uint32_t) iv[13]) << 16) | (((uint32_t) iv[14]) << 8) | (iv[15]);
    u32IVCount += (mIVOffset >> 4);

    iv[12] = (uint8_t) ((u32IVCount >> 24) & 0xff);
    iv[13] = (uint8_t) ((u32IVCount >> 16) & 0xff);
    iv[14] = (uint8_t) ((u32IVCount >> 8) & 0xff);
    iv[15] = (uint8_t) (u32IVCount & 0xff);

    // Convert the encryption key from hexadecimal to bytes
    if (Encoding::HexToBytes(OTA_ENCRYPTION_KEY, strlen(OTA_ENCRYPTION_KEY), key, kOTAEncryptionKeyLength) !=
        kOTAEncryptionKeyLength)
    {
        mbedtls_aes_free(&aesCtx);
        return CHIP_ERROR_INVALID_STRING_LENGTH;
    }

    // Set the AES encryption key
    if (mbedtls_aes_setkey_dec(&aesCtx, key, kOTAEncryptionKeyLength * 8) != 0)
    {
        mbedtls_aes_free(&aesCtx);
        return CHIP_ERROR_INTERNAL;
    }

    // Process the block in 16 bytes chunks
    while (Offset + 16 <= block.size())
    {
        /*Encrypt the IV*/
        if (mbedtls_aes_crypt_ecb(&aesCtx, MBEDTLS_AES_ENCRYPT, iv, keystream) != 0)
        {
            mbedtls_aes_free(&aesCtx);
            return CHIP_ERROR_INTERNAL;
        }

        /* Decrypt a block of the buffer */
        for (uint8_t i = 0; i < 16; i++)
        {
            // XOR with ciphertext to get plaintext
            data = block[Offset + i] ^ keystream[i];
            memcpy(&block[Offset + i], &data, sizeof(uint8_t));
        }

        /* increment the IV counter for the next block  */
        u32IVCount++;

        iv[12] = (uint8_t) ((u32IVCount >> 24) & 0xff);
        iv[13] = (uint8_t) ((u32IVCount >> 16) & 0xff);
        iv[14] = (uint8_t) ((u32IVCount >> 8) & 0xff);
        iv[15] = (uint8_t) (u32IVCount & 0xff);

        Offset += 16; /* increment the buffer offset */
        mIVOffset += 16;
    }

    // Cleanup AES context
    mbedtls_aes_free(&aesCtx);

    return CHIP_NO_ERROR;
}

#endif
} // namespace chip
