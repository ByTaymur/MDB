// mdb.h
#ifndef __MDB_h
#define __MDB_h

#include "stm32f7xx_hal.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// MDB States
typedef enum {
    MDB_STATE_INACTIVE,
    MDB_STATE_DISABLED,
    MDB_STATE_ENABLED,
    MDB_STATE_SESSION_IDLE,
    MDB_STATE_VEND,
    MDB_STATE_REVALUE,
    MDB_STATE_NEGATIVE_VEND
} MDB_State_t;

// Log Levels
typedef enum {
    LOG_NONE = 0,
    LOG_ERROR,
    LOG_WARNING, 
    LOG_INFO,
    LOG_DEBUG
} MDB_LogLevel_t;

// Error Codes
typedef enum {
    MDB_ERR_NONE = 0,
    MDB_ERR_NAK,
    MDB_ERR_TIMEOUT,
    MDB_ERR_CHECKSUM,
    MDB_ERR_STATE,
    MDB_ERR_PARAMETER,
    MDB_ERR_COMMUNICATION,
    MDB_ERR_SEQUENCE,
    MDB_ERR_FUNDS,
    MDB_ERR_HARDWARE
} MDB_Error_t;

// MDB Commands
#define MDB_ACK                  0x00
#define MDB_NAK                  0xFF
#define MDB_RET                  0xAA

#define MDB_CMD_RESET           0x10
#define MDB_CMD_SETUP           0x11
#define MDB_CMD_POLL            0x12
#define MDB_CMD_VEND            0x13
#define MDB_CMD_READER          0x14
#define MDB_CMD_REVALUE         0x15
#define MDB_CMD_EXPANSION       0x17

// Timing Constants
#define MDB_RESPONSE_TIMEOUT     5    // 5ms
#define MDB_INTERBYTE_TIMEOUT    1    // 1ms
#define MDB_NON_RESPONSE_TIMEOUT 5000 // 5sec
#define MDB_RESET_HOLD_TIME      100  // 100ms
#define MDB_POLL_INTERVAL        200  // 200ms

// Buffer Sizes
#define MDB_MAX_MESSAGE_LENGTH   36
#define MDB_QUEUE_SIZE          10
#define MDB_TRANSACTION_LOG_SIZE 50
#define MDB_ERROR_LOG_SIZE      50

// Transaction Types
typedef enum {
    TRANS_PAID_VEND,
    TRANS_FREE_VEND,
    TRANS_TEST_VEND,
    TRANS_REVALUE,
    TRANS_NEGATIVE_VEND
} MDB_TransactionType_t;

// Structure Definitions
typedef struct {
    uint8_t featureLevel;
    uint16_t countryCode;
    uint8_t scaleFactor;
    uint8_t decimalPlaces;
    uint16_t maxPrice;
    uint16_t minPrice;
    uint8_t miscOptions;
} MDB_Config_t;

typedef struct {
    MDB_State_t state;
    uint32_t availableFunds;
    uint32_t vendAmount;
    uint16_t itemNumber;
    bool multivend;
    bool refundable;
    uint32_t sessionTimeout;
    MDB_TransactionType_t transType;
} MDB_Session_t;

typedef struct {
    uint8_t data[MDB_MAX_MESSAGE_LENGTH];
    uint8_t length;
    uint32_t timestamp;
} MDB_Message_t;

typedef struct {
    MDB_Message_t messages[MDB_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} MDB_MessageQueue_t;

typedef struct {
    uint32_t timestamp;
    MDB_TransactionType_t type;
    uint32_t amount;
    uint16_t itemNumber;
    bool success;
    MDB_Error_t error;
} MDB_TransactionLog_t;

typedef struct {
    uint32_t timestamp;
    MDB_Error_t error;
    MDB_State_t state;
    uint8_t lastCommand;
    uint8_t lastResponse;
} MDB_ErrorLog_t;

// Function Declarations
bool MDB_Initialize(void);
bool MDB_Reset(void);
bool MDB_BeginSession(uint32_t funds);
bool MDB_VendRequest(uint16_t itemNumber, uint32_t amount);
bool MDB_VendSuccess(uint16_t itemNumber);
bool MDB_VendFailure(void);
bool MDB_SessionComplete(void);
bool MDB_Revalue(uint32_t amount);
void MDB_Poll(void);
bool MDB_EnableReader(void);
bool MDB_DisableReader(void);
void MDB_HandleError(MDB_Error_t error);

// Message Processing Functions
bool MDB_ProcessMessage(uint8_t* msg, uint8_t len);
bool MDB_QueueMessage(uint8_t* data, uint8_t length);
bool MDB_ProcessMessageQueue(void);

// Logging Functions
void MDB_LogMessage(MDB_LogLevel_t level, const char* format, ...);
void MDB_LogTransaction(MDB_TransactionLog_t* transaction);
void MDB_LogError(MDB_Error_t error);
void MDB_DumpLogs(void);

// State Management
void MDB_SetState(MDB_State_t newState);

#endif