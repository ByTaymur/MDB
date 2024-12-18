// mdb.c

#include "mdb.h"

// External declarations
extern UART_HandleTypeDef huart6;  // MDB UART interface

// Private variables
static MDB_Config_t mdbConfig;
static MDB_Session_t mdbSession;
static MDB_MessageQueue_t messageQueue;
static MDB_TransactionLog_t transactionLog[MDB_TRANSACTION_LOG_SIZE];
static MDB_ErrorLog_t errorLog[MDB_ERROR_LOG_SIZE];

static uint8_t transactionLogIndex = 0;
static uint8_t errorLogIndex = 0;
static uint32_t lastPollTime = 0;
static MDB_LogLevel_t currentLogLevel = LOG_INFO;

static uint8_t txBuffer[MDB_MAX_MESSAGE_LENGTH];
static uint8_t rxBuffer[MDB_MAX_MESSAGE_LENGTH];
static uint8_t lastCommand[MDB_MAX_MESSAGE_LENGTH];
static uint8_t lastCommandLength = 0;
static uint8_t retryCount = 0;

// Private function declarations
static uint8_t CalculateChecksum(uint8_t* data, uint8_t length);
static bool SendCommand(uint8_t* data, uint8_t length);
static bool WaitForResponse(uint8_t* response, uint8_t* length);
static void HandleStateChange(MDB_State_t newState);
static bool HandleJustReset(void);
static bool HandleBeginSession(uint8_t* msg, uint8_t len);
static bool HandleVendApproved(uint8_t* msg, uint8_t len);
static bool HandleVendDenied(void);
static bool HandleEndSession(void);
static bool HandleRevalueDenied(void);

bool MDB_Initialize(void) {
    // Reset internal state
    memset(&mdbConfig, 0, sizeof(MDB_Config_t));
    memset(&mdbSession, 0, sizeof(MDB_Session_t));
    memset(&messageQueue, 0, sizeof(MDB_MessageQueue_t));
    
    MDB_LogMessage(LOG_INFO, "Initializing MDB interface...");
    
    // Set initial state
    mdbSession.state = MDB_STATE_INACTIVE;
    
    // Perform reset sequence
    if(!MDB_Reset()) {
        MDB_LogMessage(LOG_ERROR, "Reset failed");
        return false;
    }
    
    // Send SETUP command
    uint8_t setupCmd[] = {MDB_CMD_SETUP, 0x00};
    if(!SendCommand(setupCmd, 2)) {
        MDB_LogMessage(LOG_ERROR, "Setup command failed");
        return false;
    }
    
    // Wait for configuration response
    uint8_t respLen;
    if(!WaitForResponse(rxBuffer, &respLen)) {
        MDB_LogMessage(LOG_ERROR, "No response to setup command");
        return false;
    }
    
    // Parse configuration
    if(!ParseConfiguration(rxBuffer, respLen)) {
        MDB_LogMessage(LOG_ERROR, "Failed to parse configuration");
        return false;
    }
    
    // Enable reader
    if(!MDB_EnableReader()) {
        MDB_LogMessage(LOG_ERROR, "Failed to enable reader");
        return false;
    }
    
    MDB_LogMessage(LOG_INFO, "MDB initialization complete");
    return true;
}

bool MDB_Reset(void) {
    MDB_LogMessage(LOG_INFO, "Performing reset...");
    
    // Send reset command
    uint8_t resetCmd = MDB_CMD_RESET;
    if(!SendCommand(&resetCmd, 1)) {
        MDB_LogError(MDB_ERR_COMMUNICATION);
        return false;
    }
    
    // Wait for JUST RESET response
    uint8_t respLen;
    if(!WaitForResponse(rxBuffer, &respLen)) {
        MDB_LogError(MDB_ERR_TIMEOUT);
        return false;
    }
    
    if(rxBuffer[0] != 0x00) { // Check for JUST RESET
        MDB_LogError(MDB_ERR_SEQUENCE);
        return false;
    }
    
    MDB_SetState(MDB_STATE_INACTIVE);
    MDB_LogMessage(LOG_INFO, "Reset complete");
    return true;
}

bool MDB_ProcessMessage(uint8_t* msg, uint8_t len) {
    if(len == 0 || msg == NULL) {
        MDB_LogError(MDB_ERR_PARAMETER);
        return false;
    }

    uint8_t command = msg[0];
    bool success = true;

    MDB_LogMessage(LOG_DEBUG, "Processing message: command=0x%02X", command);

    switch(command) {
        case MDBRxCashlessJustReset:
            success = HandleJustReset();
            break;

        case MDBRxCashlessBeginSession:
            success = HandleBeginSession(msg, len);
            break;

        case MDBRxCashlessVendApproved:
            success = HandleVendApproved(msg, len);
            break;

        case MDBRxCashlessVendDenied:
            success = HandleVendDenied();
            break;

        case MDBRxCashlessEndSession:
            success = HandleEndSession();
            break;

        // ... Diğer komutlar için case'ler eklenecek

        default:
            MDB_LogMessage(LOG_WARNING, "Unknown command received: 0x%02X", command);
            success = false;
            break;
    }

    if(!success) {
        MDB_LogError(MDB_ERR_SEQUENCE);
    }

    return success;
}

void MDB_Poll(void) {
    uint32_t currentTime = HAL_GetTick();
    
    // Only poll at defined interval
    if(currentTime - lastPollTime < MDB_POLL_INTERVAL) {
        return;
    }
    
    lastPollTime = currentTime;
    
    // Process any queued messages first
    MDB_ProcessMessageQueue();
    
    // Send POLL command
    uint8_t pollCmd = MDB_CMD_POLL;
    if(!SendCommand(&pollCmd, 1)) {
        MDB_HandleError(MDB_ERR_COMMUNICATION);
        return;
    }
    
    // Wait for response
    uint8_t respLen;
    if(!WaitForResponse(rxBuffer, &respLen)) {
        return; // Just ACK is fine
    }
    
    // Process response if any
    if(respLen > 0) {
        MDB_ProcessMessage(rxBuffer, respLen);
    }
    
    // Check session timeout
    if(mdbSession.state == MDB_STATE_SESSION_IDLE) {
        if(currentTime - mdbSession.sessionTimeout > 30000) { // 30 second timeout
            MDB_LogMessage(LOG_WARNING, "Session timeout");
            MDB_SessionComplete();
        }
    }
}

static bool SendCommand(uint8_t* data, uint8_t length) {
    if(length > MDB_MAX_MESSAGE_LENGTH - 1) { // Leave room for checksum
        MDB_LogError(MDB_ERR_PARAMETER);
        return false;
    }
    
    // Save command for potential retry
    memcpy(lastCommand, data, length);
    lastCommandLength = length;
    
    // Copy data to tx buffer
    memcpy(txBuffer, data, length);
    
    // Add checksum
    txBuffer[length] = CalculateChecksum(data, length);
    
    // Send data
    if(HAL_UART_Transmit(&huart6, txBuffer, length + 1, 100) != HAL_OK) {
        MDB_LogError(MDB_ERR_COMMUNICATION);
        return false;
    }
    
    return true;
}

static bool WaitForResponse(uint8_t* response, uint8_t* length) {
    uint32_t startTime = HAL_GetTick();
    
    while(HAL_GetTick() - startTime < MDB_RESPONSE_TIMEOUT) {
        if(HAL_UART_Receive(&huart6, response, 1, 1) == HAL_OK) {
            *length = 1;
            
            // Check if more data is coming (mode bit not set)
            while(!(response[*length-1] & 0x100)) {
                if(HAL_UART_Receive(&huart6, &response[*length], 1, MDB_INTERBYTE_TIMEOUT) != HAL_OK) {
                    MDB_LogError(MDB_ERR_COMMUNICATION);
                    return false;
                }
                (*length)++;
                
                if(*length >= MDB_MAX_MESSAGE_LENGTH) {
                    MDB_LogError(MDB_ERR_PARAMETER);
                    return false;
                }
            }
            
            // Validate checksum if more than just ACK/NAK
            if(*length > 1) {
                uint8_t checksum = CalculateChecksum(response, *length - 1);
                if(checksum != response[*length - 1]) {
                    MDB_LogError(MDB_ERR_CHECKSUM);
                    return false;
                }
            }
            
            return true;
        }
    }
    
    MDB_LogError(MDB_ERR_TIMEOUT);
    return false;
}

void MDB_HandleError(MDB_Error_t error) {
   MDB_LogError(error);

   switch(error) {
       case MDB_ERR_NAK:
           // Retry the last command up to 3 times
           if(retryCount < 3) {
               retryCount++;
               MDB_LogMessage(LOG_WARNING, "Retrying command, attempt %d", retryCount);
               if(lastCommandLength > 0) {
                   SendCommand(lastCommand, lastCommandLength);
               }
           } else {
               MDB_LogMessage(LOG_ERROR, "Max retries exceeded");
               retryCount = 0;
               MDB_Reset();
           }
           break;

       case MDB_ERR_TIMEOUT:
           MDB_LogMessage(LOG_ERROR, "Communication timeout");
           if(mdbSession.state != MDB_STATE_INACTIVE) {
               MDB_Reset();
           }
           break;

       case MDB_ERR_CHECKSUM:
           MDB_LogMessage(LOG_ERROR, "Checksum error");
           // Request retransmission
           uint8_t ret = MDB_RET;
           SendCommand(&ret, 1);
           break;

       case MDB_ERR_STATE:
           MDB_LogMessage(LOG_ERROR, "Invalid state transition");
           // Try to recover by completing current session
           if(mdbSession.state > MDB_STATE_ENABLED) {
               MDB_SessionComplete();
           }
           break;

       case MDB_ERR_SEQUENCE:
           MDB_LogMessage(LOG_ERROR, "Command sequence error");
           // Try to recover by resetting to known state
           if(mdbSession.state > MDB_STATE_ENABLED) {
               MDB_SessionComplete();
           } else {
               MDB_Reset();
           }
           break;

       case MDB_ERR_FUNDS:
           MDB_LogMessage(LOG_ERROR, "Insufficient funds");
           // Cancel current transaction
           if(mdbSession.state == MDB_STATE_VEND) {
               MDB_VendFailure();
           }
           break;

       case MDB_ERR_HARDWARE:
           MDB_LogMessage(LOG_ERROR, "Hardware error detected");
           // Disable reader and try full reset
           MDB_DisableReader();
           HAL_Delay(100);
           MDB_Reset();
           break;

       case MDB_ERR_COMMUNICATION:
           MDB_LogMessage(LOG_ERROR, "Communication error");
           // Try to re-establish communication
           MDB_DisableReader();
           HAL_Delay(100);
           MDB_Reset();
           HAL_Delay(100); 
           MDB_EnableReader();
           break;

       default:
           MDB_LogMessage(LOG_ERROR, "Unknown error: %d", error);
           // Try full reset for unknown errors
           MDB_Reset();
           break;
   }

   // Update error statistics
   static uint32_t lastErrorTime = 0;
   uint32_t currentTime = HAL_GetTick();
   
   // If errors are happening too frequently, disable reader
   if(currentTime - lastErrorTime < 5000) { // Less than 5 seconds between errors
       static uint8_t rapidErrorCount = 0;
       rapidErrorCount++;
       
       if(rapidErrorCount > 5) { // More than 5 errors in 5 seconds
           MDB_LogMessage(LOG_ERROR, "Too many errors, disabling reader");
           MDB_DisableReader();
           rapidErrorCount = 0;
       }
   } else {
       rapidErrorCount = 0;
   }
   
   lastErrorTime = currentTime;

   // Log extended error information
   MDB_ErrorLog_t errorEntry = {
       .timestamp = currentTime,
       .error = error,
       .state = mdbSession.state,
       .lastCommand = lastCommand[0],
       .lastResponse = rxBuffer[0]
   };
   
   // Add to error log array
   memcpy(&errorLog[errorLogIndex], &errorEntry, sizeof(MDB_ErrorLog_t));
   errorLogIndex = (errorLogIndex + 1) % MDB_ERROR_LOG_SIZE;

   // If we have serious errors, consider dumping logs
   static uint8_t seriousErrorCount = 0;
   if(error == MDB_ERR_HARDWARE || error == MDB_ERR_COMMUNICATION) {
       seriousErrorCount++;
       if(seriousErrorCount >= 3) {
           MDB_DumpLogs();
           seriousErrorCount = 0;
       }
   }
}

// Yardımcı fonksiyon - Toplu hata bilgisi yazdırma
void MDB_DumpErrorStats(void) {
   uint32_t errorCounts[MDB_ERR_HARDWARE + 1] = {0};
   uint32_t totalErrors = 0;
   
   // Hata sayılarını hesapla
   for(int i = 0; i < MDB_ERROR_LOG_SIZE; i++) {
       if(errorLog[i].timestamp != 0) {
           errorCounts[errorLog[i].error]++;
           totalErrors++;
       }
   }

   // İstatistikleri yazdır
   MDB_LogMessage(LOG_INFO, "=== Error Statistics ===");
   MDB_LogMessage(LOG_INFO, "Total Errors: %lu", totalErrors);
   
   for(int i = 0; i <= MDB_ERR_HARDWARE; i++) {
       if(errorCounts[i] > 0) {
           float percentage = (float)errorCounts[i] / totalErrors * 100.0f;
           MDB_LogMessage(LOG_INFO, "Error %d: Count=%lu (%.1f%%)", 
                         i, errorCounts[i], percentage);
       }
   }
}
}