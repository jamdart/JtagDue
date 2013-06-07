
// Copyright (C) 2012 R. Diez
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the Affero GNU General Public License version 3
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Affero GNU General Public License version 3 for more details.
//
// You should have received a copy of the Affero GNU General Public License version 3
// along with this program. If not, see http://www.gnu.org/licenses/ .


#include "BusPirateConnection.h"  // The include file for this module should come first.

#include <BareMetalSupport/Uptime.h>
#include <BareMetalSupport/BusyWait.h>
#include <BareMetalSupport/Miscellaneous.h>
#include <BareMetalSupport/AssertionUtils.h>
#include <BareMetalSupport/DebugConsole.h>
#include <BareMetalSupport/MainLoopSleep.h>
#include <BareMetalSupport/StackCheck.h>
#include <BareMetalSupport/TextParsingUtils.h>

#include <stdexcept>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include <udi_cdc.h>
#include <rstc.h>

#include "Globals.h"
#include "BusPirateBinaryMode.h"
#include "BusPirateOpenOcdMode.h"
#include "UsbConnection.h"
#include "SerialConsole.h"


static const char SPACE_AND_TAB[] = " \t";

static unsigned s_binaryModeCount;

static bool s_simulateProcolError;

enum UsbSpeedTestEnum
{
  stNone,
  stTxSimpleWithTimestamps,
  stTxSimpleLoop,
  stTxFastLoopCircularBuffer,
  stTxFastLoopRawUsb,
  stRxWithCircularBuffer
};

static uint8_t s_usbSpeedTestBuffer[ 1000 ];
static uint64_t s_usbSpeedTestEndTime;
static UsbSpeedTestEnum s_usbSpeedTestType;

// These symbols are defined in the linker script file.
extern uint32_t _end;

static CSerialConsole s_console;


static void WritePrompt ( CUsbTxBuffer * const txBuffer )
{
  UsbPrint( txBuffer, ">" );
}


static bool DoesStrMatch ( const char * const strBegin,
                           const char * const strEnd,
                           const char * const match,
                           const bool isCaseSensitive )
{
  const size_t len = strEnd - strBegin;
  assert( len > 0 );

  size_t i;
  for ( i = 0; i < len; ++i )
  {
    const char m = match[ i ];
    if ( m == 0 )
      return false;

    const char c = strBegin[ i ];
    assert( c != 0 );

    // Otherwise, toupper() may not be reliable.
    assert( IsPrintableAscii( m ) );

    if ( c == m )
      continue;

    if ( !isCaseSensitive && toupper( c ) == toupper( m ) )
      continue;

    return false;
  }

  if ( match[ i ] != 0 )
    return false;

  return true;
}


static bool IsCmd ( const char * const cmdBegin,
                    const char * const cmdEnd,   // One character beyond the end, like the STL does.
                    const char * const cmdName,  // Must be printable ASCII and NULL-character terminated.
                    const bool isCaseSensitive,
                    const bool allowExtraParams,
                    bool * const extraParamsFound )
{
  if ( !DoesStrMatch( cmdBegin, cmdEnd, cmdName, isCaseSensitive ) )
    return false;

  if ( !allowExtraParams )
  {
    const char * const paramBegin = SkipCharsInSet( cmdEnd, SPACE_AND_TAB );

    if ( *paramBegin != 0 )
    {
      *extraParamsFound = true;
      return false;
    }
  }

  return true;
}



// This routine could be improved in many ways:
// - Make it faster by building a complete line and sending it at once.
// - Provide memory addresses and/or offsets on the left.
// - Provide an ASCII dump on the right.
// - Use different data sizes (8 bits, 16 bits, 32 bits).
//
//  There is a similar routine in this project called DbgconHexDump().

static void HexDump ( const void * const ptr,
                      const size_t byteCount,
                      const char * const endOfLineChars,
                      CUsbTxBuffer * const txBuffer )
{
  assert( byteCount > 0 );

  const unsigned LINE_BYTE_COUNT = 32;
  const size_t eolLen = strlen( endOfLineChars );
  const size_t lineCount = ( byteCount + LINE_BYTE_COUNT - 1 ) / LINE_BYTE_COUNT;
  const size_t expectedOutputLen = byteCount * 3 + lineCount * eolLen;

  if ( expectedOutputLen > txBuffer->GetFreeCount() )
    throw std::runtime_error( "Not enough room in the Tx buffer for the hex dump." );

  const uint8_t * const bytePtr = static_cast< const uint8_t * >( ptr );

  unsigned lineElemCount = 0;
  size_t actualOutputLen = 0;

  for ( size_t i = 0; i < byteCount; ++i )
  {
    if ( lineElemCount == LINE_BYTE_COUNT )
    {
      lineElemCount = 0;
      UsbPrint( txBuffer, "%s", endOfLineChars );
      actualOutputLen += eolLen;
    }
    const uint8_t b = bytePtr[ i ];

    UsbPrint( txBuffer, "%02X ", b );
    actualOutputLen += 3;

    ++lineElemCount;
  }

  UsbPrint( txBuffer, "%s", endOfLineChars );
  actualOutputLen += eolLen;

  assert( actualOutputLen == expectedOutputLen );
}


static unsigned int ParseUnsignedIntArg ( const char * const begin )
{
  const char ERR_MSG[] = "Invalid unsigned integer value.";

  int base = 10;
  const char * p = begin;

  // Prefix "0x" means that the number is in hexadecimal.

  if ( *p == '0' && *(p+1) == 'x' )
  {
    p += 2;
    base = 16;
  }

  // strtoul() interprets a leading '-', but we always want an unsigned positive value
  // and the user should not be allowed to enter a negative value.
  if ( *p == '-' )
    throw std::runtime_error( ERR_MSG );

  char * end2;
  errno = 0;
  const unsigned long val = strtoul( p, &end2, base );

  if ( errno != 0 || ( *end2 != 0 && !IsCharInSet( *end2, SPACE_AND_TAB ) ) )
  {
    throw std::runtime_error( ERR_MSG );
  }

  STATIC_ASSERT( sizeof(unsigned int) == sizeof(unsigned long), "You may want to rethink this routine's data types." );
  return (unsigned int) val;
}


static void PrintMemory ( const char * const paramBegin,
                          CUsbTxBuffer * const txBuffer )
{
  const char * const addrEnd       = SkipCharsNotInSet( paramBegin, SPACE_AND_TAB );
  const char * const countBegin    = SkipCharsInSet   ( addrEnd,    SPACE_AND_TAB );
  const char * const countEnd      = SkipCharsNotInSet( countBegin, SPACE_AND_TAB );
  const char * const extraArgBegin = SkipCharsInSet   ( countEnd,   SPACE_AND_TAB );

  if ( *paramBegin == 0 || *countBegin == 0 || *extraArgBegin != 0 )
  {
    UsbPrintStr( txBuffer, "Invalid arguments." EOL );
    return;
  }

  assert( countBegin > paramBegin );

  const unsigned addr  = ParseUnsignedIntArg( paramBegin );
  const unsigned count = ParseUnsignedIntArg( countBegin );

  // DbgconPrint( "Addr : %u" EOL, unsigned(addr ) );
  // DbgconPrint( "Count: %u" EOL, unsigned(count) );

  if ( count == 0 )
  {
    UsbPrintStr( txBuffer, "Invalid arguments." EOL );
    return;
  }

  HexDump( (const void *) addr, size_t( count ), EOL, txBuffer );
}


static void BusyWait ( const char * const paramBegin,
                       CUsbTxBuffer * const txBuffer )
{
  const char * const delayEnd      = SkipCharsNotInSet( paramBegin, SPACE_AND_TAB );
  const char * const extraArgBegin = SkipCharsInSet   ( delayEnd,   SPACE_AND_TAB );

  if ( *paramBegin == 0 || *extraArgBegin != 0 )
  {
    UsbPrintStr( txBuffer, "Invalid arguments." EOL );
    return;
  }

  const unsigned delayMs = ParseUnsignedIntArg( paramBegin );

  if ( delayMs == 0 || delayMs > 60 * 1000 )
  {
    UsbPrintStr( txBuffer, "Invalid arguments." EOL );
    return;
  }

  const uint32_t oneMsIterationCount = GetBusyWaitLoopIterationCountFromUs( 1000 );

  for ( uint32_t i = 0; i < delayMs; ++i )
  {
    BusyWaitLoop( oneMsIterationCount );
  }

  UsbPrint( txBuffer, "Waited %u ms." EOL, unsigned( delayMs ) );
}


static void ProcessUsbSpeedTestCmd ( const char * const paramBegin,
                                     CUsbTxBuffer * const txBuffer,
                                     const uint64_t currentTime )
{
  // Examples about how to automate the speed test from the bash command line:
  //   Tests where the Arduino Due is sending:
  //     echo "UsbSpeedTest TxFastLoopRawUsb" | socat - /dev/jtagdue1,b115200,raw,echo=0,crnl | pv -pertb >/dev/null
  //   Tests where the Arduino Due is receiving:
  //     (echo "UsbSpeedTest RxWithCircularBuffer" && yes ".") | pv -pertb - | socat - /dev/jtagdue1,b115200,raw,echo=0,crnl >/dev/null

  const uint32_t TEST_TIME_IN_MS = 5000;  // We could make a user parameter out of this value.

  if ( *paramBegin == 0 )
  {
    UsbPrintStr( txBuffer, "Please specify the test type as an argument:" EOL );
    UsbPrintStr( txBuffer, "  TxSimpleWithTimestamps" EOL );
    UsbPrintStr( txBuffer, "  TxSimpleLoop" EOL );
    UsbPrintStr( txBuffer, "  TxFastLoopCircularBuffer" EOL );
    UsbPrintStr( txBuffer, "  TxFastLoopRawUsb" EOL );
    UsbPrintStr( txBuffer, "  RxWithCircularBuffer" EOL );

    return;
  }

  const char * const paramEnd = SkipCharsNotInSet( paramBegin, SPACE_AND_TAB );

  assert( s_usbSpeedTestType == stNone );
  UsbSpeedTestEnum testType = stNone;

  bool extraParamsFound = false;

  if ( IsCmd( paramBegin, paramEnd, "TxSimpleWithTimestamps", false, false, &extraParamsFound ) )
    testType = stTxSimpleWithTimestamps;
  else if ( IsCmd( paramBegin, paramEnd, "TxSimpleLoop", false, false, &extraParamsFound ) )
    testType = stTxSimpleLoop;
  else if ( IsCmd( paramBegin, paramEnd, "TxFastLoopCircularBuffer", false, false, &extraParamsFound ) )
    testType = stTxFastLoopCircularBuffer;
  else if ( IsCmd( paramBegin, paramEnd, "TxFastLoopRawUsb", false, false, &extraParamsFound ) )
    testType = stTxFastLoopRawUsb;
  else if ( IsCmd( paramBegin, paramEnd, "RxWithCircularBuffer", false, false, &extraParamsFound ) )
    testType = stRxWithCircularBuffer;

  if ( testType != stNone )
  {
    for ( size_t i = 0; i < sizeof( s_usbSpeedTestBuffer ); ++i )
      s_usbSpeedTestBuffer[ i ] = '.';

    s_usbSpeedTestEndTime = currentTime + TEST_TIME_IN_MS;
    s_usbSpeedTestType = testType;

    // This message may not make it to the console, depending on the test type.
    UsbPrintStr( txBuffer, "Starting USB speed test..." EOL );

    WakeFromMainLoopSleep();

    return;
  }

  if ( extraParamsFound )
    UsbPrint( txBuffer, "No parameters are allowed after test type \"%.*s\"." EOL, paramEnd - paramBegin, paramBegin );
  else
    UsbPrint( txBuffer, "Unknown test type \"%.*s\"." EOL, paramEnd - paramBegin, paramBegin );
}


static void DisplayResetCause ( CUsbTxBuffer * const txBuffer )
{
  UsbPrint( txBuffer, "Reset cause: " );

  const uint32_t resetCause = rstc_get_reset_cause( RSTC );

  switch ( resetCause )
  {
  case RSTC_GENERAL_RESET:
    UsbPrint( txBuffer, "General" );
    break;

  case RSTC_BACKUP_RESET:
    UsbPrint( txBuffer, "Backup" );
    break;

  case RSTC_WATCHDOG_RESET:
    UsbPrint( txBuffer, "Watchdog" );
    break;

  case RSTC_SOFTWARE_RESET:
    UsbPrint( txBuffer, "Software" );
    break;

  case RSTC_USER_RESET:
    UsbPrint( txBuffer, "User" );
    break;

  default:
    UsbPrint( txBuffer, "<unknown>" );
    assert( false );
    break;
  }

  UsbPrint( txBuffer, EOL );
}


static void DisplayCpuLoad ( CUsbTxBuffer * const txBuffer )
{
  const uint8_t * lastMinute;
        uint8_t   lastMinuteIndex;

  const uint8_t * lastSecond;
        uint8_t   lastSecondIndex;

  GetCpuLoadStats( &lastMinute, &lastMinuteIndex,
                   &lastSecond, &lastSecondIndex );

  uint32_t minuteAverage = 0;

  for ( unsigned j = 0; j < CPU_LOAD_MINUTE_SLOT_COUNT; ++j )
  {
    const unsigned index = ( lastMinuteIndex + j ) % CPU_LOAD_MINUTE_SLOT_COUNT;

    minuteAverage += lastMinute[ index ];
  }

  minuteAverage = minuteAverage * 100 / ( CPU_LOAD_MINUTE_SLOT_COUNT * 255 );
  assert( minuteAverage <= 100 );

  UsbPrint( txBuffer, "CPU load in the last 60 seconds (1 second intervals, oldest to newest):" EOL );

  for ( unsigned j = 0; j < CPU_LOAD_MINUTE_SLOT_COUNT; ++j )
  {
    const unsigned index = ( lastMinuteIndex + j ) % CPU_LOAD_MINUTE_SLOT_COUNT;

    const uint32_t val = lastMinute[ index ] * 100 / 255;

    assert( val <= 100 );

    UsbPrint( txBuffer, "%3u %%" EOL, unsigned( val ) );
  }


  uint32_t secondAverage = 0;

  for ( unsigned j = 0; j < CPU_LOAD_SECOND_SLOT_COUNT; ++j )
  {
    const unsigned index = ( lastSecondIndex + j ) % CPU_LOAD_SECOND_SLOT_COUNT;

    secondAverage += lastSecond[ index ];
  }

  secondAverage = secondAverage * 100 / ( CPU_LOAD_SECOND_SLOT_COUNT * 255 );
  assert( secondAverage <= 100 );


  UsbPrint( txBuffer, "CPU load in the last second (50 ms intervals, oldest to newest):" EOL );

  for ( unsigned j = 0; j < CPU_LOAD_SECOND_SLOT_COUNT; ++j )
  {
    const unsigned index = ( lastSecondIndex + j ) % CPU_LOAD_SECOND_SLOT_COUNT;

    const uint32_t val = lastSecond[ index ] * 100 / 255;

    assert( val <= 100 );

    UsbPrint( txBuffer, "%2u %%" EOL, unsigned( val ) );
  }

  UsbPrint( txBuffer, "Average CPU load in the last 60 seconds: %2u %%" EOL, unsigned( minuteAverage ) );
  UsbPrint( txBuffer, "Average CPU load in the last    second : %2u %%" EOL, unsigned( secondAverage ) );
}


static void SimulateError ( const char * const paramBegin,
                            CUsbTxBuffer * const txBuffer )
{
  if ( *paramBegin == 0 )
  {
    UsbPrintStr( txBuffer, "Please specify the error type as an argument: 'command' or 'protocol'" EOL );
    return;
  }

  const char * const paramEnd      = SkipCharsNotInSet( paramBegin, SPACE_AND_TAB );
  const char * const extraArgBegin = SkipCharsInSet   ( paramEnd,   SPACE_AND_TAB );

  if ( *extraArgBegin != 0 )
  {
    UsbPrintStr( txBuffer, "Invalid arguments." EOL );
    return;
  }

  if ( DoesStrMatch( paramBegin, paramEnd, "command", false ) )
  {
    throw std::runtime_error( "Simulated command error." );
  }

  if ( DoesStrMatch( paramBegin, paramEnd, "protocol", false ) )
  {
    s_simulateProcolError = true;
    return;
  }

  UsbPrint( txBuffer, "Unknown error type \"%.*s\"." EOL, paramEnd - paramBegin, paramBegin );
}


static const char * const CMDNAME_QUESTION_MARK = "?";
static const char * const CMDNAME_HELP = "help";
static const char * const CMDNAME_I = "i";
static const char * const CMDNAME_USBSPEEDTEST = "UsbSpeedTest";
static const char * const CMDNAME_JTAGPINS = "JtagPins";
static const char * const CMDNAME_JTAGSHIFTSPEEDTEST = "JtagShiftSpeedTest";
static const char * const CMDNAME_MALLOCTEST = "MallocTest";
static const char * const CMDNAME_CPP_EXCEPTION_TEST = "ExceptionTest";
static const char * const CMDNAME_MEMORY_USAGE = "MemoryUsage";
static const char * const CMDNAME_SIMULATE_ERROR = "SimulateError";
static const char * const CMDNAME_RESET = "Reset";
static const char * const CMDNAME_CPU_LOAD = "CpuLoad";
static const char * const CMDNAME_RESET_CAUSE = "ResetCause";
static const char * const CMDNAME_PRINT_MEMORY = "PrintMemory";
static const char * const CMDNAME_BUSY_WAIT = "BusyWait";
static const char * const CMDNAME_UPTIME = "Uptime";


static void ProcessCommand ( const char * const cmdBegin,
                             CUsbRxBuffer * const rxBuffer,
                             CUsbTxBuffer * const txBuffer,
                             const uint64_t currentTime )
{
  const char * const cmdEnd = SkipCharsNotInSet( cmdBegin, SPACE_AND_TAB );
  assert( cmdBegin != cmdEnd );

  const char * const paramBegin = SkipCharsInSet( cmdEnd, SPACE_AND_TAB );

  bool extraParamsFound = false;

  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_QUESTION_MARK, true, false, &extraParamsFound ) ||
       IsCmd( cmdBegin, cmdEnd, CMDNAME_HELP, false, false, &extraParamsFound ) )
  {
    UsbPrintStr( txBuffer, "This console is similar to the Bus Pirate console." EOL );
    UsbPrintStr( txBuffer, "Commands longer than 1 character are case insensitive." EOL );
    UsbPrintStr( txBuffer, "Commands are:" EOL );

    UsbPrint( txBuffer, "  %s, %s: Show this help text." EOL, CMDNAME_QUESTION_MARK, CMDNAME_HELP );
    UsbPrint( txBuffer, "  %s: Show version information." EOL, CMDNAME_I );
    UsbPrint( txBuffer, "  %s: Test USB transfer speed." EOL, CMDNAME_USBSPEEDTEST );
    UsbPrint( txBuffer, "  %s: Show JTAG pin status (read as inputs)." EOL, CMDNAME_JTAGPINS );
    UsbPrint( txBuffer, "  %s: Test JTAG shift speed. WARNING: Do NOT connect any JTAG device." EOL, CMDNAME_JTAGSHIFTSPEEDTEST );
    UsbPrint( txBuffer, "  %s: Exercises malloc()." EOL, CMDNAME_MALLOCTEST );
    UsbPrint( txBuffer, "  %s: Exercises C++ exceptions." EOL, CMDNAME_CPP_EXCEPTION_TEST );
    UsbPrint( txBuffer, "  %s: Shows memory usage." EOL, CMDNAME_MEMORY_USAGE );
    UsbPrint( txBuffer, "  %s" EOL, CMDNAME_CPU_LOAD );
    UsbPrint( txBuffer, "  %s" EOL, CMDNAME_UPTIME );
    UsbPrint( txBuffer, "  %s" EOL, CMDNAME_RESET );
    UsbPrint( txBuffer, "  %s" EOL, CMDNAME_RESET_CAUSE );
    UsbPrint( txBuffer, "  %s <addr> <byte count>" EOL, CMDNAME_PRINT_MEMORY );
    UsbPrint( txBuffer, "  %s <milliseconds>" EOL, CMDNAME_BUSY_WAIT );
    UsbPrint( txBuffer, "  %s <command|protocol>" EOL, CMDNAME_SIMULATE_ERROR );

    return;
  }

  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_I, true, false, &extraParamsFound ) )
  {
    #ifndef NDEBUG
      const char buildType[] = "Debug build";
    #else
      const char buildType[] = "Release build";
    #endif

    UsbPrint( txBuffer, "JtagDue %s" EOL, PACKAGE_VERSION );
    UsbPrint( txBuffer, "%s, compiler version %s" EOL, buildType, __VERSION__ );
    UsbPrint( txBuffer, "Watchdog %s" EOL, ENABLE_WDT ? "enabled" : "disabled" );

    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_RESET, false, false, &extraParamsFound ) )
  {
    // This message does not reach the other side, we would need to add some delay.
    //   UsbPrint( txBuffer, "Resetting the board..." EOL );
    __disable_irq();
    DbgconPrintStr( "Resetting the board..." EOL );
    DbgconWaitForDataSent();
    ResetBoard( ENABLE_WDT );
    assert( false );  // We should never reach this point.
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_CPU_LOAD, false, false, &extraParamsFound ) )
  {
    if ( ENABLE_CPU_SLEEP )
      UsbPrint( txBuffer, "CPU load statistics not available." EOL );
    else
      DisplayCpuLoad( txBuffer );

    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_UPTIME, false, false, &extraParamsFound ) )
  {
    UsbPrint( txBuffer, "Uptime: %llu seconds." EOL, (long long)(GetUptime() / 1000) );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_RESET_CAUSE, false, false, &extraParamsFound ) )
  {
    DisplayResetCause( txBuffer );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_PRINT_MEMORY, false, true, &extraParamsFound ) )
  {
    PrintMemory( paramBegin, txBuffer );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_BUSY_WAIT, false, true, &extraParamsFound ) )
  {
    BusyWait( paramBegin, txBuffer );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_USBSPEEDTEST, false, true, &extraParamsFound ) )
  {
    ProcessUsbSpeedTestCmd( paramBegin, txBuffer, currentTime );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_JTAGPINS, false, false, &extraParamsFound ) )
  {
    PrintJtagPinStatus( txBuffer );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_JTAGSHIFTSPEEDTEST, false, false, &extraParamsFound ) )
  {
    // Fill the Rx buffer with some test data.
    rxBuffer->Reset();
    for ( uint32_t i = 0; !rxBuffer->IsFull(); ++i )
    {
      rxBuffer->WriteElem( CUsbRxBuffer::ElemType( i ) );
    }


    // If the mode is set to MODE_HIZ, you cannot see the generated signal with the oscilloscope.
    // Note also that the built-in pull-ups on the Atmel ATSAM3X8 are too weak (between 50 and 100 KOhm,
    // yields too slow a rising time) to be of any use.

    const bool oldPullUps = GetJtagPullups();
    SetJtagPullups( false );

    const JtagPinModeEnum oldMode = GetJtagPinMode();
    SetJtagPinMode ( MODE_JTAG );


    // Each JTAG transfer needs 2 bits in the Rx buffer, TMS and TDI,
    // but produces only 1 bit, TDO.
    const uint32_t jtagByteCount = rxBuffer->GetElemCount() / 2;

    const uint16_t bitCount = jtagByteCount * 8;

    // Shift all JTAG data through several times.

    const uint64_t startTime = GetUptime();
    const uint32_t iterCount = 50;

    for ( uint32_t i = 0; i < iterCount; ++i )
    {
      // We hope that this will not clear the buffer contents.
      rxBuffer->Reset();
      rxBuffer->CommitWrittenElements( jtagByteCount * 2 );

      txBuffer->Reset();

      ShiftJtagData( rxBuffer,
                     txBuffer,
                     bitCount );

      assert( txBuffer->GetElemCount() == jtagByteCount );
    }

    const uint64_t finishTime = GetUptime();
    const uint32_t elapsedTime = uint32_t( finishTime - startTime );

    rxBuffer->Reset();
    txBuffer->Reset();
    const unsigned kBitsPerSec = unsigned( uint64_t(bitCount) * iterCount * 1000 / elapsedTime / 1024 );

    SetJtagPinMode( oldMode );
    SetJtagPullups( oldPullUps );

    // I am getting 221 KiB/s with GCC 4.7.3 and optimisation level "-O3".
    UsbPrint( txBuffer, EOL "Finished JTAG shift speed test, throughput %u Kbits/s (%u KiB/s)." EOL,
              kBitsPerSec, kBitsPerSec / 8 );

    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_MALLOCTEST, false, false, &extraParamsFound ) )
  {
    UsbPrintStr( txBuffer, "Allocalling memory..." EOL );

    volatile uint32_t * const volatile mallocTest = (volatile uint32_t *) malloc(123);
    *mallocTest = 123;

    UsbPrintStr( txBuffer, "Releasing memory..." EOL );

    free( const_cast< uint32_t * >( mallocTest ) );

    UsbPrintStr( txBuffer, "Test finished." EOL );

    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_CPP_EXCEPTION_TEST, false, false, &extraParamsFound ) )
  {
    try
    {
      UsbPrintStr( txBuffer, "Throwing integer exception..." EOL );
      throw 123;
      UsbPrintStr( txBuffer, "Throw did not work." EOL );
      assert( false );
    }
    catch ( ... )
    {
      UsbPrintStr( txBuffer, "Caught integer exception." EOL );
    }
    UsbPrintStr( txBuffer, "Test finished." EOL );

    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_SIMULATE_ERROR, false, true, &extraParamsFound ) )
  {
    SimulateError( paramBegin, txBuffer );
    return;
  }


  if ( IsCmd( cmdBegin, cmdEnd, CMDNAME_MEMORY_USAGE, false, false, &extraParamsFound ) )
  {
    const unsigned heapSize = unsigned( GetHeapEndAddr() - uintptr_t( &_end ) );

    UsbPrint( txBuffer, "Partitions: malloc heap: %u bytes, free: %u bytes, stack: %u bytes." EOL,
              heapSize,
              GetStackStartAddr() - GetHeapEndAddr(),
              STACK_SIZE );

    UsbPrint( txBuffer, "Used stack (estimated): %u from %u bytes." EOL,
              unsigned( GetStackSizeUsageEstimate() ),
              STACK_SIZE );

    const struct mallinfo mi = mallinfo();
    const unsigned heapSizeAccordingToNewlib = unsigned( mi.arena );

    UsbPrint( txBuffer, "Heap: %u allocated from %u bytes." EOL,
              unsigned( mi.uordblks ),
              unsigned( mi.arena ) );

    assert( heapSize == heapSizeAccordingToNewlib );
    UNUSED_IN_RELEASE( heapSizeAccordingToNewlib );

    return;
  }

  if ( extraParamsFound )
    UsbPrint( txBuffer, "Command \"%.*s\" does not take any parameters." EOL, cmdEnd - cmdBegin, cmdBegin );
  else
    UsbPrint( txBuffer, "Unknown command \"%.*s\"." EOL, cmdEnd - cmdBegin, cmdBegin );
}


static void ParseCommand ( const char * const cmdStr,
                           CUsbRxBuffer * const rxBuffer,
                           CUsbTxBuffer * const txBuffer,
                           const uint64_t currentTime )
{
  const char * const s = SkipCharsInSet( cmdStr, SPACE_AND_TAB );

  if ( *s != 0 )
  {
    ProcessCommand( s, rxBuffer, txBuffer, currentTime );
  }
}


static void SpeedTest ( CUsbRxBuffer * const rxBuffer,
                        CUsbTxBuffer * const txBuffer,
                        const uint64_t currentTime )
{
  if ( currentTime >= s_usbSpeedTestEndTime )
  {
    // This message may not make it to the console, depending on the test type.
    UsbPrintStr( txBuffer, EOL "USB speed test finished." EOL );
    WritePrompt( txBuffer );

    s_usbSpeedTestType = stNone;
    return;
  }


  switch ( s_usbSpeedTestType )
  {
  case stTxSimpleWithTimestamps:
    // Simple loop with the timestamps
    for ( uint32_t i = 0; i < 100; ++i )
    {
      if ( txBuffer->GetFreeCount() < 40 )
        break;

      UsbPrint( txBuffer, "%u - %u" EOL, unsigned(currentTime), unsigned(s_usbSpeedTestEndTime) );
    }

    break;

  case stTxSimpleLoop:
   {
    // Simple loop with a dot.
    const uint32_t freeCount = txBuffer->GetFreeCount();

    for ( uint32_t i = 0; i < freeCount; ++i )
      txBuffer->WriteElem( '.' );

    break;
   }

  case stTxFastLoopCircularBuffer:

    // Performance loop with the Circular Buffer, which is the normal way in this firmware.
    // I am getting a throughput of 4.4 MB/s with this method.

    for ( ; ; )
    {
      CUsbTxBuffer::SizeType maxChunkElemCount;
      CUsbTxBuffer::ElemType * const writePtr = txBuffer->GetWritePtr( &maxChunkElemCount );

      if ( maxChunkElemCount == 0 )
        break;

      memset( writePtr, '.', maxChunkElemCount );

      txBuffer->CommitWrittenElements( maxChunkElemCount );
    }

    break;

  case stTxFastLoopRawUsb:
    // This method uses the udi_cdc_write_buf() routine directly.
    // I am getting a throughput of 6.2 MB/s with this method.
    for ( uint32_t i = 0; i < 1000; ++i )
    {
      const uint32_t remainingCount = udi_cdc_write_buf( s_usbSpeedTestBuffer, sizeof( s_usbSpeedTestBuffer ) );

      if ( remainingCount == 0 )
        break;
    }

    // If we do not trigger the main loop iteration manually, we will have idle time between transfers.
    WakeFromMainLoopSleep();
    break;

  case stRxWithCircularBuffer:
   {
    // This test does NOT read the data off the Circular Buffer, it just discards it.
    // I am getting a throughput of 4.5 MB/s with this method.

    const CUsbTxBuffer::SizeType elemCount = rxBuffer->GetElemCount();
    if ( elemCount != 0 )
    {
      if ( false )
      {
        if ( txBuffer->GetFreeCount() >= 80 )
          UsbPrint( txBuffer, "Discarded %u read bytes." EOL, unsigned(elemCount) );
      }

      rxBuffer->ConsumeReadElements( elemCount );
    }
    break;
   }

  default:
    assert( false );
    break;
  }
}


void BusPirateConsole_ProcessData ( CUsbRxBuffer * const rxBuffer,
                                    CUsbTxBuffer * const txBuffer,
                                    const uint64_t currentTime )
{
  // If we are in speed test mode, and we have not finished testing yet, do nothing else.

  if ( s_usbSpeedTestType != stNone )
  {
    SpeedTest( rxBuffer, txBuffer, currentTime );

    if ( s_usbSpeedTestType != stNone )
      return;
  }


  // Speed is not important here, so we favor simplicity. We only process one command at a time.
  // There is also a limit on the number of bytes consumed, so that the main loop does not get
  // blocked for a long time if we keep getting garbage.

  for ( uint32_t i = 0; i < 100; ++i )
  {
    if ( rxBuffer->IsEmpty() || ! txBuffer->IsEmpty() )
      break;

    const uint8_t byte = rxBuffer->ReadElement();
    bool endLoop = false;

    if ( byte == BIN_MODE_CHAR )
    {
      // For more information about entering binary mode, see here:
      //   http://dangerousprototypes.com/2009/10/09/bus-pirate-raw-bitbang-mode/
      ++s_binaryModeCount;

       if ( s_binaryModeCount == 20 )
       {
         ChangeBusPirateMode( bpBinMode, txBuffer );
         endLoop = true;
       }
    }
    else
    {
      s_binaryModeCount = 0;

      uint32_t cmdLen;
      const char * const cmd = s_console.AddChar( byte, txBuffer, &cmdLen );

      if ( cmd != NULL )
      {
        UsbPrintStr( txBuffer, EOL );

        s_simulateProcolError = false;

        try
        {
          ParseCommand( cmd, rxBuffer, txBuffer, currentTime );
        }
        catch ( const std::exception & e )
        {
          UsbPrint( txBuffer, "Error processing command: %s" EOL, e.what() );
        }

        if ( s_simulateProcolError )
        {
          throw std::runtime_error( "Simulated protocol error." );
        }

        WritePrompt( txBuffer );

        endLoop = true;
      }
    }

    if ( endLoop )
      break;
  }
}


static void ResetBusPirateConsole ( void )
{
  s_binaryModeCount = 0;
  s_usbSpeedTestType = stNone;
  s_console.Reset();
}


void BusPirateConsole_Init ( CUsbTxBuffer * const txBufferForWelcomeMsg )
{
  ResetBusPirateConsole();

  // Unfortunately, we cannot print here a welcome banner, because OpenOCD will abort when it sees the "Welcome..." text.
  // This may change in the future though, I am planning to submit a patch that would make OpenOCD discard
  // all available input right after establishing the connection.

  if ( false )
  {
    UsbPrint( txBufferForWelcomeMsg, "Welcome to the Arduino Due's native USB serial port." EOL );
    UsbPrint( txBufferForWelcomeMsg, "Type '?' for help." EOL );
    // Not even a short prompt alone is tolerated:
    WritePrompt( txBufferForWelcomeMsg );
  }
}


void BusPirateConsole_Terminate ( void )
{
  ResetBusPirateConsole();
}
